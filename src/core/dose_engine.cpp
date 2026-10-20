#include "core/dose_engine.h"

#include <algorithm>

namespace core {

const char* doseStateName(DoseState s) {
  static const char* const n[] = {"pending", "due", "snoozed", "taken", "taken_late", "skipped", "missed"};
  const unsigned i = static_cast<unsigned>(s);
  return i < 7 ? n[i] : "?";
}

bool isFinalState(DoseState s) {
  return s == DoseState::Taken || s == DoseState::TakenLate || s == DoseState::Skipped || s == DoseState::Missed;
}

const char* actionResultName(ActionResult r) {
  static const char* const n[] = {"ok",         "not_found",      "invalid_state", "too_early",  "taken_at_in_future",
                                  "would_double_dose", "snooze_limit", "invalid_snooze", "snooze_beyond_grace",
                                  "undo_expired", "nothing_to_undo", "not_prn",     "prn_too_soon", "prn_daily_cap"};
  const unsigned i = static_cast<unsigned>(r);
  return i < sizeof(n) / sizeof(n[0]) ? n[i] : "?";
}

DoseEngine::DoseEngine(const IClock& clock, Database& db, IDoseLogSink* log) : clock_(clock), db_(db), log_(log) {
  if (!parsePosixTz(db.settings.tz, tz_)) parsePosixTz(kDefaultTz, tz_);
}

bool DoseEngine::setTimezone(const std::string& s) {
  PosixTz t;
  if (!parsePosixTz(s, t)) return false;
  tz_ = t;
  db_.settings.tz = s;
  return true;
}

void DoseEngine::begin(int64_t startUtc) {
  inst_.clear();
  lastTaken_.clear();
  generatedUntil_ = startUtc;
  processedUntil_ = startUtc;
}

void DoseEngine::restore(const EngineSnapshot& s) {
  generatedUntil_ = s.generatedUntilUtc;
  processedUntil_ = s.processedUntilUtc;
  inst_ = s.instances;
  lastTaken_ = s.lastTaken;
  std::sort(inst_.begin(), inst_.end(),
            [](const DoseInstance& a, const DoseInstance& b) { return a.scheduledUtc < b.scheduledUtc; });
}

EngineSnapshot DoseEngine::snapshot() const {
  EngineSnapshot s;
  s.generatedUntilUtc = generatedUntil_;
  s.processedUntilUtc = processedUntil_;
  s.instances = inst_;
  s.lastTaken = lastTaken_;
  return s;
}

DoseInstance* DoseEngine::findMut(uint64_t id) {
  for (auto& d : inst_)
    if (d.eventId == id) return &d;
  return nullptr;
}

const DoseInstance* DoseEngine::find(uint64_t id) const {
  for (const auto& d : inst_)
    if (d.eventId == id) return &d;
  return nullptr;
}

int64_t DoseEngine::lastTaken(uint16_t medId) const {
  for (const auto& kv : lastTaken_)
    if (kv.first == medId) return kv.second;
  return 0;
}

void DoseEngine::setLastTaken(uint16_t medId, int64_t t) {
  for (auto& kv : lastTaken_)
    if (kv.first == medId) {
      kv.second = t;
      return;
    }
  lastTaken_.emplace_back(medId, t);
}

int64_t DoseEngine::graceEnd(const DoseInstance& d) const {
  const Medication* m = db_.med(d.medId);
  const int64_t grace = m ? m->graceMin : 60;
  return d.scheduledUtc + grace * 60;
}

void DoseEngine::generate(int64_t untilUtc) {
  if (untilUtc <= generatedUntil_) return;
  std::vector<Occurrence> occ;
  expandOccurrences(db_, tz_, generatedUntil_, untilUtc, occ);
  for (const Occurrence& o : occ) {
    if (find(o.eventId)) continue;  // deterministic id: never duplicate
    DoseInstance d;
    d.eventId = o.eventId;
    d.profileId = o.profileId;
    d.medId = o.medId;
    d.scheduledUtc = o.scheduledUtc;
    d.localDay = o.localDay;
    d.localMinute = o.localMinute;
    inst_.push_back(d);
  }
  generatedUntil_ = untilUtc;
  std::stable_sort(inst_.begin(), inst_.end(), [](const DoseInstance& a, const DoseInstance& b) {
    if (a.scheduledUtc != b.scheduledUtc) return a.scheduledUtc < b.scheduledUtc;
    return a.eventId < b.eventId;
  });
}

void DoseEngine::regenerateFuture() {
  const int64_t now = clock_.nowUtc();
  inst_.erase(std::remove_if(inst_.begin(), inst_.end(),
                             [now](const DoseInstance& d) {
                               return d.state == DoseState::Pending && d.scheduledUtc > now &&
                                      !eventIdIsPrn(d.eventId);
                             }),
              inst_.end());
  if (generatedUntil_ > now) generatedUntil_ = now;
}

int64_t DoseEngine::nextTransition(const DoseInstance& d) const {
  const Medication* m = db_.med(d.medId);
  const int64_t ge = graceEnd(d);
  switch (d.state) {
    case DoseState::Pending:
      return d.scheduledUtc;
    case DoseState::Due: {
      int64_t t = ge;
      if (m && d.alarmsFired <= m->ladderCount) {
        const int64_t tick = d.ladderStartUtc + static_cast<int64_t>(d.alarmsFired) * m->ladderIntervalMin * 60;
        if (tick < t) t = tick;
      }
      return t;
    }
    case DoseState::Snoozed:
      return d.snoozeUntil < ge ? d.snoozeUntil : ge;
    default:
      return INT64_MAX;
  }
}

bool DoseEngine::audibleAt(const Medication& m, int64_t utc) const {
  if (m.critical) return true;
  const Profile* p = db_.profile(m.profileId);
  if (!p) return true;
  const LocalTime lt = utcToLocal(tz_, utc);
  return !inQuietHours(*p, lt.secOfDay / 60);
}

void DoseEngine::log(const DoseInstance& d, DoseAction a, int64_t actionUtc, ActionSource src, const char* note) {
  if (!log_) return;
  DoseLogEntry e;
  e.eventId = d.eventId;
  e.profileId = d.profileId;
  e.medId = d.medId;
  e.scheduledUtc = d.scheduledUtc;
  e.action = a;
  e.actionUtc = actionUtc;
  e.recordedUtc = clock_.nowUtc();
  e.source = src;
  e.note = note ? note : "";
  log_->append(e);
}

void DoseEngine::applyTransition(DoseInstance& d, int64_t at, int64_t now, std::vector<EngineEvent>& ev) {
  const Medication* m = db_.med(d.medId);
  const bool stale = at < now - 60;
  const int64_t ge = graceEnd(d);
  if (at >= ge && d.state != DoseState::Pending) {
    d.state = DoseState::Missed;
    d.actionUtc = ge;
    d.undoable = false;
    log(d, DoseAction::Missed, ge, ActionSource::System, "grace expired");
    ev.push_back({ge, d.eventId, d.medId, EngineEventKind::BecameMissed, 0, false, stale});
    return;
  }
  switch (d.state) {
    case DoseState::Pending:
      d.state = DoseState::Due;
      d.ladderStartUtc = d.scheduledUtc;
      d.alarmsFired = 1;
      ev.push_back({at, d.eventId, d.medId, EngineEventKind::BecameDue, 0, m ? audibleAt(*m, at) : true, stale});
      return;
    case DoseState::Due:
      ev.push_back({at, d.eventId, d.medId, EngineEventKind::AlarmTick, d.alarmsFired, m ? audibleAt(*m, at) : true,
                    stale});
      d.alarmsFired++;
      return;
    case DoseState::Snoozed:
      d.state = DoseState::Due;
      d.ladderStartUtc = at;
      d.alarmsFired = 1;
      ev.push_back({at, d.eventId, d.medId, EngineEventKind::SnoozeEnded, 0, m ? audibleAt(*m, at) : true, stale});
      return;
    default:
      return;
  }
}

std::vector<EngineEvent> DoseEngine::process() { return processUntil(clock_.nowUtc()); }

std::vector<EngineEvent> DoseEngine::processUntil(int64_t now) {
  std::vector<EngineEvent> ev;
  generate(now + kLookaheadSec);
  for (;;) {
    DoseInstance* best = nullptr;
    int64_t bestT = INT64_MAX;
    for (auto& d : inst_) {
      const int64_t t = nextTransition(d);
      if (t > now) continue;
      // chronological; ties broken by scheduled time then id for determinism
      if (!best || t < bestT || (t == bestT && (d.scheduledUtc < best->scheduledUtc ||
                                                (d.scheduledUtc == best->scheduledUtc && d.eventId < best->eventId)))) {
        best = &d;
        bestT = t;
      }
    }
    if (!best) break;
    applyTransition(*best, bestT, now, ev);
  }
  if (now > processedUntil_) processedUntil_ = now;
  prune(now);
  return ev;
}

void DoseEngine::prune(int64_t now) {
  inst_.erase(std::remove_if(inst_.begin(), inst_.end(),
                             [&](const DoseInstance& d) {
                               if (!isFinalState(d.state)) return false;
                               // keep PRN takes for 24 h lockout accounting and undo
                               const int64_t ref = d.actionUtc > d.scheduledUtc ? d.actionUtc : d.scheduledUtc;
                               return now - ref > kRetainFinalSec;
                             }),
              inst_.end());
}

std::vector<const DoseInstance*> DoseEngine::upcoming(int64_t horizonSec) const {
  std::vector<const DoseInstance*> out;
  const int64_t now = clock_.nowUtc();
  for (const auto& d : inst_)
    if (!isFinalState(d.state) && d.scheduledUtc <= now + horizonSec) out.push_back(&d);
  return out;
}

int64_t DoseEngine::nextWakeUtc() const {
  int64_t t = INT64_MAX;
  for (const auto& d : inst_) {
    const int64_t n = nextTransition(d);
    if (n < t) t = n;
  }
  if (generatedUntil_ - kLookaheadSec / 2 < t) t = generatedUntil_ - kLookaheadSec / 2;
  return t;
}

ActionResult DoseEngine::take(uint64_t id, ActionSource src, int64_t takenAt) {
  DoseInstance* d = findMut(id);
  if (!d) return ActionResult::NotFound;
  const int64_t now = clock_.nowUtc();
  if (takenAt == kNow) takenAt = now;
  if (takenAt > now) return ActionResult::TakenAtInFuture;
  if (eventIdIsPrn(id)) return ActionResult::InvalidState;
  Medication* m = db_.med(d->medId);
  if (!m) return ActionResult::NotFound;
  switch (d->state) {
    case DoseState::Pending:
      if (now < d->scheduledUtc - kEarlyTakeWindowSec) return ActionResult::TooEarly;
      break;
    case DoseState::Due:
    case DoseState::Snoozed:
      break;
    case DoseState::Missed: {
      // Backfill only: record that it WAS taken. Never allow this to become a
      // double dose: the taken-at time must precede the next dose of the same med.
      for (const auto& o : inst_) {
        if (o.medId == d->medId && o.scheduledUtc > d->scheduledUtc && takenAt >= o.scheduledUtc - kEarlyTakeWindowSec)
          return ActionResult::WouldDoubleDose;
      }
      break;
    }
    default:
      return ActionResult::InvalidState;
  }
  if (takenAt < d->scheduledUtc - kEarlyTakeWindowSec) return ActionResult::TooEarly;
  d->prevState = d->state;
  d->prevSnoozeCount = d->snoozeCount;
  d->prevSnoozeUntil = d->snoozeUntil;
  d->prevAlarmsFired = d->alarmsFired;
  d->prevLadderStart = d->ladderStartUtc;
  d->prevLastTaken = lastTaken(d->medId);
  const bool onTime = takenAt <= d->scheduledUtc + static_cast<int64_t>(m->onTimeMin) * 60;
  d->state = onTime ? DoseState::Taken : DoseState::TakenLate;
  d->actionUtc = takenAt;
  d->undoable = true;
  d->lastActionRecorded = now;
  d->supplyDecremented = false;
  if (m->supply > 0) {
    m->supply--;
    d->supplyDecremented = true;
  }
  if (takenAt > lastTaken(d->medId)) setLastTaken(d->medId, takenAt);
  const bool backfill = d->prevState == DoseState::Missed || takenAt < now - 60;
  log(*d, onTime ? DoseAction::Taken : DoseAction::TakenLate, takenAt, backfill ? ActionSource::Backfill : src,
      d->prevState == DoseState::Missed ? "correction: taken after missed" : "");
  return ActionResult::Ok;
}

ActionResult DoseEngine::skip(uint64_t id, ActionSource src) {
  DoseInstance* d = findMut(id);
  if (!d) return ActionResult::NotFound;
  if (d->state != DoseState::Pending && d->state != DoseState::Due && d->state != DoseState::Snoozed)
    return ActionResult::InvalidState;
  const int64_t now = clock_.nowUtc();
  d->prevState = d->state;
  d->prevSnoozeCount = d->snoozeCount;
  d->prevSnoozeUntil = d->snoozeUntil;
  d->prevAlarmsFired = d->alarmsFired;
  d->prevLadderStart = d->ladderStartUtc;
  d->prevLastTaken = lastTaken(d->medId);
  d->supplyDecremented = false;
  d->state = DoseState::Skipped;
  d->actionUtc = now;
  d->undoable = true;
  d->lastActionRecorded = now;
  log(*d, DoseAction::Skipped, now, src, "");
  return ActionResult::Ok;
}

ActionResult DoseEngine::snooze(uint64_t id, int minutes, ActionSource src) {
  DoseInstance* d = findMut(id);
  if (!d) return ActionResult::NotFound;
  if (minutes != 5 && minutes != 10 && minutes != 15) return ActionResult::InvalidSnooze;
  if (d->state != DoseState::Due) return ActionResult::InvalidState;
  if (d->snoozeCount >= kMaxSnoozes) return ActionResult::SnoozeLimit;
  const int64_t now = clock_.nowUtc();
  const int64_t until = now + minutes * 60;
  if (until >= graceEnd(*d)) return ActionResult::SnoozeBeyondGrace;
  d->prevState = d->state;
  d->prevSnoozeCount = d->snoozeCount;
  d->prevSnoozeUntil = d->snoozeUntil;
  d->prevAlarmsFired = d->alarmsFired;
  d->prevLadderStart = d->ladderStartUtc;
  d->prevLastTaken = lastTaken(d->medId);
  d->supplyDecremented = false;
  d->state = DoseState::Snoozed;
  d->snoozeCount++;
  d->snoozeUntil = until;
  d->undoable = true;
  d->lastActionRecorded = now;
  log(*d, DoseAction::Snoozed, now, src, minutes == 5 ? "5 min" : (minutes == 10 ? "10 min" : "15 min"));
  return ActionResult::Ok;
}

ActionResult DoseEngine::undo(uint64_t id, ActionSource src) {
  DoseInstance* d = findMut(id);
  if (!d) return ActionResult::NotFound;
  if (!d->undoable) return ActionResult::NothingToUndo;
  const int64_t now = clock_.nowUtc();
  if (now - d->lastActionRecorded > kUndoWindowSec) return ActionResult::UndoExpired;
  Medication* m = db_.med(d->medId);
  if (d->supplyDecremented && m && m->supply >= 0) m->supply++;
  d->supplyDecremented = false;
  setLastTaken(d->medId, d->prevLastTaken);
  log(*d, DoseAction::Undo, now, src, "");
  d->undoable = false;
  if (eventIdIsPrn(id)) {
    inst_.erase(inst_.begin() + (d - inst_.data()));
    return ActionResult::Ok;
  }
  d->state = d->prevState;
  d->snoozeCount = d->prevSnoozeCount;
  d->snoozeUntil = d->prevSnoozeUntil;
  d->alarmsFired = d->prevAlarmsFired;
  d->ladderStartUtc = d->prevLadderStart;
  d->actionUtc = d->prevState == DoseState::Missed ? graceEnd(*d) : 0;
  return ActionResult::Ok;
}

PrnStatus DoseEngine::prnStatus(uint16_t medId) const {
  PrnStatus s;
  const Medication* m = db_.med(medId);
  if (!m) return s;
  const int64_t now = clock_.nowUtc();
  const Phase* ph = phaseOn(*m, utcToLocal(tz_, now).days);
  if (!ph || ph->rule.type != ScheduleType::Prn || !m->active) return s;
  s.isPrn = true;
  s.maxPer24h = ph->rule.prnMaxPer24h;
  std::vector<int64_t> takes;
  for (const auto& d : inst_)
    if (d.medId == medId && eventIdIsPrn(d.eventId) && (d.state == DoseState::Taken) && d.actionUtc > now - 86400)
      takes.push_back(d.actionUtc);
  std::sort(takes.begin(), takes.end());
  s.takenLast24h = static_cast<int>(takes.size());
  s.lastTakenUtc = takes.empty() ? 0 : takes.back();
  int64_t next = 0;
  if (!takes.empty()) next = takes.back() + static_cast<int64_t>(ph->rule.prnMinIntervalMin) * 60;
  if (s.takenLast24h >= s.maxPer24h) {
    const int64_t capFree = takes[takes.size() - static_cast<size_t>(s.maxPer24h)] + 86400;
    if (capFree > next) next = capFree;
  }
  s.nextAllowedUtc = next > now ? next : 0;
  s.allowed = next <= now;
  return s;
}

ActionResult DoseEngine::takePrn(uint16_t medId, ActionSource src, uint64_t* outId) {
  const PrnStatus s = prnStatus(medId);
  if (!s.isPrn) return ActionResult::NotPrn;
  if (!s.allowed) return s.takenLast24h >= s.maxPer24h ? ActionResult::PrnDailyCap : ActionResult::PrnTooSoon;
  Medication* m = db_.med(medId);
  const int64_t now = clock_.nowUtc();
  const uint64_t id = makeEventId(m->profileId, medId, now, true);
  if (find(id)) return ActionResult::PrnTooSoon;
  DoseInstance d;
  d.eventId = id;
  d.profileId = m->profileId;
  d.medId = medId;
  d.scheduledUtc = now;
  const LocalTime lt = utcToLocal(tz_, now);
  d.localDay = lt.days;
  d.localMinute = static_cast<int16_t>(lt.secOfDay / 60);
  d.state = DoseState::Taken;
  d.actionUtc = now;
  d.undoable = true;
  d.lastActionRecorded = now;
  d.prevLastTaken = lastTaken(medId);
  if (m->supply > 0) {
    m->supply--;
    d.supplyDecremented = true;
  }
  setLastTaken(medId, now);
  inst_.push_back(d);
  log(d, DoseAction::PrnTaken, now, src, "");
  if (outId) *outId = id;
  return ActionResult::Ok;
}

}  // namespace core
