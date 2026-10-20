#include "app_core/app_model.h"

#include "core/alarm.h"

#include <algorithm>
#include <map>

#include "app_core/format.h"
#include "core/schedule.h"
#include "core/stats.h"
#include "core/tz.h"
#include "ui_core/strings.h"

namespace app {
using namespace core;
using ui::S;
using ui::tr;

BannerKind topBanner(const StatusFlags& s, int* more) {
  const bool flags[] = {s.clockUnset, s.veryLowBattery, s.storageError, s.alarmHwFault, s.clockUntrusted && !s.clockUnset,
                        s.lowBattery && !s.veryLowBattery, s.sdError, s.backupFailed && !s.sdError,
                        !s.sdPresent && !s.sdError && s.autoBackupOn};
  const BannerKind kinds[] = {BannerKind::ClockUnset, BannerKind::VeryLowBattery, BannerKind::StorageError, BannerKind::AlarmHw, BannerKind::ClockUntrusted,
                              BannerKind::LowBattery, BannerKind::SdError, BannerKind::BackupFailed, BannerKind::SdMissing};
  BannerKind top = BannerKind::None;
  int count = 0;
  for (size_t i = 0; i < sizeof(flags); i++) {
    if (!flags[i]) continue;
    if (top == BannerKind::None) top = kinds[i];
    count++;
  }
  if (more) *more = count > 0 ? count - 1 : 0;
  return top;
}

RowState rowStateFrom(DoseState s) {
  switch (s) {
    case DoseState::Pending: return RowState::Pending;
    case DoseState::Due: return RowState::Due;
    case DoseState::Snoozed: return RowState::Snoozed;
    case DoseState::Taken: return RowState::Taken;
    case DoseState::TakenLate: return RowState::TakenLate;
    case DoseState::Skipped: return RowState::Skipped;
    case DoseState::Missed: return RowState::Missed;
  }
  return RowState::Untracked;
}

static RowState rowStateFromAction(DoseAction a) {
  switch (a) {
    case DoseAction::Taken:
    case DoseAction::PrnTaken: return RowState::Taken;
    case DoseAction::TakenLate: return RowState::TakenLate;
    case DoseAction::Skipped: return RowState::Skipped;
    case DoseAction::Missed: return RowState::Missed;
    default: return RowState::Untracked;
  }
}

static std::string doseTextFor(const Medication& m, int32_t day) {
  const Phase* ph = phaseOn(m, day);
  return (ph && !ph->doseText.empty()) ? ph->doseText : m.doseText;
}

AppModel::AppModel(const IClock& clock, Database& db, IDoseLogSink* persist)
    : clock_(clock), db_(db), persist_(persist), engine_(clock, db, this) {}

int32_t AppModel::today() const { return utcToLocal(tz(), now()).days; }

bool AppModel::append(const DoseLogEntry& e) {
  if (recent_.size() >= kRecentLogCap) recent_.erase(recent_.begin(), recent_.begin() + kRecentLogCap / 8);
  recent_.push_back(e);
  return persist_ ? persist_->append(e) : true;
}

void AppModel::setRecentLog(const std::vector<DoseLogEntry>& entries) {
  recent_ = entries;
  if (recent_.size() > kRecentLogCap) recent_.erase(recent_.begin(), recent_.end() - static_cast<long>(kRecentLogCap));
}

void AppModel::startEngine(int64_t nowUtc) {
  engine_.begin(nowUtc);
  running_ = true;
}

void AppModel::restoreEngine(const EngineSnapshot& s) {
  engine_.restore(s);
  running_ = true;
}

std::vector<EngineEvent> AppModel::process() {
  if (!running_) return {};
  return engine_.process();
}

int64_t AppModel::lastTakenUtc(uint16_t medId) const {
  int64_t t = engine_.lastTaken(medId);
  // fall back to the RAM log (e.g. right after a fresh engine start)
  std::vector<DoseLogEntry> mine;
  for (const auto& e : recent_)
    if (e.medId == medId) mine.push_back(e);
  for (const auto& kv : resolveLog(mine)) {
    const ResolvedDose& r = kv.second;
    if (r.resolved && (r.action == DoseAction::Taken || r.action == DoseAction::TakenLate || r.action == DoseAction::PrnTaken))
      t = std::max(t, r.actionUtc);
  }
  return t;
}

std::string AppModel::lastTakenText(uint16_t medId) const {
  const int64_t t = lastTakenUtc(medId);
  if (t <= 0) return "";
  const LocalTime lt = utcToLocal(tz(), t);
  const std::string hm = fmtHm(lt.secOfDay / 60);
  if (lt.days == today()) return fmt(tr(S::LastTaken), hm.c_str());
  return fmt(tr(S::LastTaken), (std::string(weekdayShort(weekdayFromDays(lt.days))) + " " + hm).c_str());
}

TodayVM AppModel::today(uint8_t profileId) const {
  TodayVM vm;
  vm.profileId = profileId;
  const Profile* p = db_.profile(profileId);
  if (!p) return vm;
  vm.person = p->name;
  const int32_t d = today();
  const int64_t from = localToUtc(tz(), d, 0) - 1, to = localToUtc(tz(), d + 1, 0) - 1;
  std::vector<Occurrence> occ;
  expandOccurrences(db_, tz(), from, to, occ, profileId);
  std::vector<DoseLogEntry> dayLog;
  for (const auto& e : recent_)
    if (e.profileId == profileId && e.scheduledUtc > from && e.scheduledUtc <= to) dayLog.push_back(e);
  const auto resolved = resolveLog(dayLog);
  std::map<uint16_t, std::string> lastCache;
  auto lastFor = [&](uint16_t med) {
    auto it = lastCache.find(med);
    if (it != lastCache.end()) return it->second;
    return lastCache[med] = lastTakenText(med);
  };
  for (const Occurrence& o : occ) {
    const Medication* m = db_.med(o.medId);
    if (!m) continue;
    TimelineRow r;
    r.eventId = o.eventId;
    r.medId = o.medId;
    r.minute = o.localMinute;
    r.time = fmtHm(o.localMinute);
    r.med = m->name;
    r.dose = doseTextFor(*m, d);
    r.form = m->form;
    r.critical = m->critical;
    if (const DoseInstance* inst = engine_.find(o.eventId)) r.state = rowStateFrom(inst->state);
    else {
      auto it = resolved.find(o.eventId);
      r.state = (it != resolved.end() && it->second.resolved) ? rowStateFromAction(it->second.action) : RowState::Untracked;
    }
    r.lastTaken = lastFor(o.medId);
    vm.total++;
    if (r.state == RowState::Taken || r.state == RowState::TakenLate) vm.taken++;
    vm.rows.push_back(r);
  }
  for (const auto& kv : resolved) {  // PRN takes today
    const ResolvedDose& rd = kv.second;
    if (!eventIdIsPrn(rd.eventId) || !rd.resolved || rd.action != DoseAction::PrnTaken) continue;
    const Medication* m = db_.med(rd.medId);
    if (!m) continue;
    TimelineRow r;
    r.eventId = rd.eventId;
    r.medId = rd.medId;
    r.minute = utcToLocal(tz(), rd.actionUtc).secOfDay / 60;
    r.time = fmtHm(r.minute);
    r.med = m->name;
    r.dose = doseTextFor(*m, d);
    r.form = m->form;
    r.state = RowState::Taken;
    r.prn = true;
    r.lastTaken = lastFor(rd.medId);
    vm.rows.push_back(r);
  }
  std::stable_sort(vm.rows.begin(), vm.rows.end(), [](const TimelineRow& a, const TimelineRow& b) { return a.minute < b.minute; });
  for (const Medication& m : db_.meds) {
    const Phase* ph = phaseOn(m, d);
    if (m.profileId == profileId && m.active && ph && ph->rule.type == ScheduleType::Prn) vm.hasPrn = true;
  }
  return vm;
}

HomeVM AppModel::home(int page, int lanesPerPage) const {
  HomeVM vm;
  const int64_t t = now();
  const LocalTime lt = utcToLocal(tz(), t);
  vm.clock = fmtHm(lt.secOfDay / 60);
  vm.date = fmtLongDate(lt.days);
  vm.status = status;
  vm.profileCount = static_cast<int>(db_.profiles.size());
  if (lanesPerPage < 1) lanesPerPage = 1;
  vm.pageCount = std::max(1, (vm.profileCount + lanesPerPage - 1) / lanesPerPage);
  vm.page = std::min(std::max(page, 0), vm.pageCount - 1);
  for (int i = vm.page * lanesPerPage; i < std::min(vm.profileCount, (vm.page + 1) * lanesPerPage); i++) {
    const Profile& p = db_.profiles[static_cast<size_t>(i)];
    const TodayVM td = today(p.id);
    LaneVM lane;
    lane.profileId = p.id;
    lane.privacy = p.privacy;
    lane.title = p.privacy ? p.initials : p.name;
    lane.taken = td.taken;
    lane.total = td.total;
    for (const auto& r : td.rows) {
      if (r.state == RowState::Due || r.state == RowState::Snoozed) lane.overdue++;
    }
    for (const auto& r : td.rows) {
      if (r.prn) continue;
      if (r.state == RowState::Due || r.state == RowState::Snoozed) {
        lane.nextTime = tr(S::Now);
        lane.nextLabel = p.privacy ? "" : r.med + " " + r.dose;
        break;
      }
      if (r.state == RowState::Pending && lane.nextTime.empty()) {
        lane.nextTime = r.time;
        lane.nextLabel = p.privacy ? "" : r.med + " " + r.dose;
      }
    }
    vm.lanes.push_back(lane);
  }
  return vm;
}

DoseDetailVM AppModel::detail(uint64_t id) const {
  DoseDetailVM vm;
  const DoseInstance* inst = engine_.find(id);
  int64_t sched = 0;
  uint16_t medId = 0;
  RowState st = RowState::Untracked;
  if (inst) {
    sched = inst->scheduledUtc;
    medId = inst->medId;
    st = rowStateFrom(inst->state);
  } else {
    const auto res = resolveLog(recent_);
    auto it = res.find(id);
    if (it == res.end()) return vm;
    sched = it->second.scheduledUtc;
    medId = it->second.medId;
    st = it->second.resolved ? rowStateFromAction(it->second.action) : RowState::Untracked;
  }
  const Medication* m = db_.med(medId);
  if (!m) return vm;
  const Profile* p = db_.profile(m->profileId);
  const int32_t d = utcToLocal(tz(), sched).days;
  vm.found = true;
  vm.eventId = id;
  vm.profileId = m->profileId;
  vm.person = p ? p->name : "";
  vm.med = m->name;
  vm.dose = doseTextFor(*m, d);
  vm.instructions = m->instructions;
  vm.notes = m->notes;
  vm.form = m->form;
  vm.critical = m->critical;
  vm.scheduledUtc = sched;
  vm.scheduled = fmt(tr(S::Scheduled), fmtHm(utcToLocal(tz(), sched).secOfDay / 60).c_str());
  vm.state = st;
  vm.lastTaken = lastTakenText(medId);
  if (m->supply >= 0 && p) {
    const SupplyForecast f = forecastSupply(*m, *p, today());
    vm.supply = fmt(tr(S::Supply), m->supply);
    vm.lowStock = f.lowStock;
    if (f.refillByDay != kNoDate) vm.refillBy = fmt(tr(S::RefillBy), fmtShortDate(f.refillByDay).c_str());
  }
  const bool prn = eventIdIsPrn(id);
  vm.canTake = inst && !prn &&
               (st == RowState::Due || st == RowState::Snoozed ||
                (st == RowState::Pending && now() >= sched - kEarlyTakeWindowSec));
  vm.canSkip = inst && !prn && (st == RowState::Due || st == RowState::Snoozed || st == RowState::Pending);
  vm.canBackfill = inst && !prn && (st == RowState::Missed || st == RowState::Due || st == RowState::Snoozed);
  vm.canUndo = inst && inst->undoable && now() - inst->lastActionRecorded <= kUndoWindowSec;
  return vm;
}

AlarmVM AppModel::alarm() const {
  AlarmVM vm;
  std::map<uint8_t, int64_t> earliest;  // profile -> earliest due
  for (const DoseInstance& d : engine_.instances()) {
    if (d.state != DoseState::Due) continue;
    auto it = earliest.find(d.profileId);
    if (it == earliest.end() || d.scheduledUtc < it->second) earliest[d.profileId] = d.scheduledUtc;
  }
  if (earliest.empty()) return vm;
  uint8_t who = earliest.begin()->first;
  for (const auto& kv : earliest)
    if (kv.second < earliest[who]) who = kv.first;
  const Profile* p = db_.profile(who);
  vm.active = true;
  vm.profileId = who;
  vm.person = p ? p->name : "?";
  vm.earliestScheduledUtc = earliest[who];
  vm.time = fmtHm(utcToLocal(tz(), earliest[who]).secOfDay / 60);
  vm.canSnooze = true;
  const int64_t t = now();
  for (const DoseInstance& d : engine_.instances()) {
    if (d.state != DoseState::Due || d.profileId != who) continue;
    const Medication* m = db_.med(d.medId);
    if (!m) continue;
    AlarmMed am;
    am.eventId = d.eventId;
    am.med = m->name;
    am.dose = doseTextFor(*m, d.localDay);
    am.instructions = m->instructions;
    am.form = m->form;
    am.critical = m->critical;
    vm.critical = vm.critical || m->critical;
    if (d.snoozeCount >= kMaxSnoozes || t + 5 * 60 >= engine_.graceEnd(d)) vm.canSnooze = false;
    vm.meds.push_back(am);
  }
  vm.silent = !alertAudible(p, vm.critical, utcToLocal(tz(), t).secOfDay / 60);
  for (const auto& kv : earliest) {
    if (kv.first == who) continue;
    vm.othersWaiting++;
    const Profile* o = db_.profile(kv.first);
    if (!vm.othersNames.empty()) vm.othersNames += ", ";
    vm.othersNames += o ? (o->privacy ? o->initials : o->name) : "?";
  }
  return vm;
}

PrnVM AppModel::prn(uint8_t profileId) const {
  PrnVM vm;
  vm.profileId = profileId;
  const Profile* p = db_.profile(profileId);
  if (!p) return vm;
  vm.person = p->name;
  const int32_t d = today();
  for (const Medication& m : db_.meds) {
    if (m.profileId != profileId || !m.active) continue;
    const Phase* ph = phaseOn(m, d);
    if (!ph || ph->rule.type != ScheduleType::Prn) continue;
    const PrnStatus s = engine_.prnStatus(m.id);
    PrnRow r;
    r.medId = m.id;
    r.med = m.name;
    r.dose = doseTextFor(m, d);
    r.form = m.form;
    r.allowed = s.allowed && running_;
    if (!s.allowed && s.nextAllowedUtc > 0) {
      const LocalTime lt = utcToLocal(tz(), s.nextAllowedUtc);
      std::string when = fmtHm(lt.secOfDay / 60);
      if (lt.days != d) when = std::string(weekdayShort(weekdayFromDays(lt.days))) + " " + when;
      r.status = fmt(tr(S::NextAllowed), when.c_str());
    } else {
      r.status = fmt(tr(S::PrnCount), s.takenLast24h, s.maxPer24h);
    }
    r.lastTaken = lastTakenText(m.id);
    vm.rows.push_back(r);
  }
  return vm;
}

static CellState cellFor(const DayStats& ds, bool future) {
  if (future) return CellState::Future;
  if (ds.total() == 0) return CellState::NoData;
  if (ds.perfect()) return CellState::Perfect;
  if (ds.onTime + ds.late == 0) return CellState::Missed;
  return CellState::Partial;
}

HistoryVM AppModel::history(uint8_t profileId, HistoryMode mode, int32_t anchor, bool perMed) const {
  HistoryVM vm;
  vm.profileId = profileId;
  vm.mode = mode;
  const Profile* p = db_.profile(profileId);
  vm.person = p ? p->name : "";
  const int32_t td = today();
  if (mode == HistoryMode::Day) {
    vm.fromDay = vm.toDay = anchor;
    vm.periodTitle = anchor == td ? std::string(tr(S::Today)) : fmtShortDate(anchor);
  } else if (mode == HistoryMode::Week) {
    vm.fromDay = anchor - ((weekdayFromDays(anchor) + 6) % 7);
    vm.toDay = vm.fromDay + 6;
    vm.periodTitle = fmtShortDate(vm.fromDay) + " - " + fmtShortDate(vm.toDay);
  } else {
    const Ymd y = civilFromDays(anchor);
    vm.fromDay = daysFromCivil(y.y, y.m, 1);
    vm.toDay = vm.fromDay + daysInMonth(y.y, y.m) - 1;
    vm.periodTitle = fmtMonthYear(anchor);
  }
  const AdherenceStats a = computeAdherence(recent_, tz(), vm.fromDay, vm.toDay, profileId);
  vm.onTime = a.onTime;
  vm.late = a.late;
  vm.skipped = a.skipped;
  vm.missed = a.missed;
  vm.prn = a.prn;
  vm.percent = static_cast<int>(a.adherencePercent + 0.5);
  const AdherenceStats streaks = computeAdherence(recent_, tz(), td - 365, td, profileId);
  vm.streak = streaks.currentStreakDays;
  vm.longest = streaks.longestStreakDays;
  if (mode == HistoryMode::Week) {
    for (const DayStats& ds : a.days) {
      DayCell c;
      c.day = ds.day;
      c.dayOfMonth = civilFromDays(ds.day).d;
      c.state = cellFor(ds, ds.day > td);
      c.taken = ds.onTime + ds.late;
      c.total = ds.total();
      c.today = ds.day == td;
      vm.cells.push_back(c);
    }
  } else if (mode == HistoryMode::Month) {
    const int lead = (weekdayFromDays(vm.fromDay) + 6) % 7;  // Monday first
    for (int i = 0; i < 42; i++) {
      DayCell c;
      const int32_t day = vm.fromDay - lead + i;
      if (day >= vm.fromDay && day <= vm.toDay) {
        const DayStats& ds = a.days[static_cast<size_t>(day - vm.fromDay)];
        c.day = day;
        c.dayOfMonth = civilFromDays(day).d;
        c.state = cellFor(ds, day > td);
        c.taken = ds.onTime + ds.late;
        c.total = ds.total();
        c.today = day == td;
      }
      vm.cells.push_back(c);
    }
  } else {
    const int64_t from = localToUtc(tz(), anchor, 0) - 1, to = localToUtc(tz(), anchor + 1, 0) - 1;
    std::vector<DoseLogEntry> dayLog;
    for (const auto& e : recent_)
      if (e.profileId == profileId && e.scheduledUtc > from && e.scheduledUtc <= to) dayLog.push_back(e);
    std::vector<ResolvedDose> rs;
    for (const auto& kv : resolveLog(dayLog))
      if (kv.second.resolved) rs.push_back(kv.second);
    std::sort(rs.begin(), rs.end(), [](const ResolvedDose& x, const ResolvedDose& y) { return x.scheduledUtc < y.scheduledUtc; });
    for (const auto& r : rs) {
      const Medication* m = db_.med(r.medId);
      HistoryRow row;
      row.time = fmtHm(utcToLocal(tz(), r.action == DoseAction::PrnTaken ? r.actionUtc : r.scheduledUtc).secOfDay / 60);
      row.med = m ? m->name : "?";
      row.state = rowStateFromAction(r.action);
      vm.rows.push_back(row);
    }
  }
  if (perMed) {
    for (const Medication& m : db_.meds) {
      if (m.profileId != profileId) continue;
      const AdherenceStats ma = computeAdherence(recent_, tz(), vm.fromDay, vm.toDay, profileId, m.id);
      const int total = ma.onTime + ma.late + ma.skipped + ma.missed;
      if (total == 0 && ma.prn == 0) continue;
      MedAdherence x;
      x.med = m.name;
      x.taken = ma.onTime + ma.late + ma.prn;
      x.total = total + ma.prn;
      x.percent = total ? static_cast<int>(ma.adherencePercent + 0.5) : 100;
      vm.perMed.push_back(x);
    }
  }
  return vm;
}

bool AppModel::lastActionUndoable() const {
  if (last_.count == 0 || last_.type == CommandType::Undo) return false;
  return now() - lastAt_ <= kUndoWindowSec;
}

CommandResult AppModel::execute(const DoseCommand& c) {
  CommandResult r;
  if (!running_) {
    r.result = ActionResult::InvalidState;
    return r;
  }
  if (c.type == CommandType::TakePrn) {
    r.result = engine_.takePrn(c.medId, c.source, &r.prnEventId);
    if (r.result == ActionResult::Ok) {
      r.applied = 1;
      last_ = DoseCommand();
      last_.type = CommandType::TakePrn;
      last_.add(r.prnEventId);
      lastAt_ = now();
    }
    return r;
  }
  if (c.count == 0) {
    r.result = ActionResult::NotFound;
    return r;
  }
  // one person per confirm
  int profile = -1;
  for (int i = 0; i < c.count; i++) {
    const DoseInstance* d = engine_.find(c.ids[i]);
    if (!d) {
      r.result = ActionResult::NotFound;
      return r;
    }
    if (profile >= 0 && d->profileId != profile) {
      r.result = ActionResult::InvalidState;
      return r;
    }
    profile = d->profileId;
  }
  for (int i = 0; i < c.count; i++) {
    ActionResult ar = ActionResult::InvalidState;
    switch (c.type) {
      case CommandType::Take: ar = engine_.take(c.ids[i], c.source, c.takenAt); break;
      case CommandType::Skip: ar = engine_.skip(c.ids[i], c.source); break;
      case CommandType::Snooze: ar = engine_.snooze(c.ids[i], c.minutes, c.source); break;
      case CommandType::Undo: ar = engine_.undo(c.ids[i], c.source); break;
      default: break;
    }
    if (ar == ActionResult::Ok) r.applied++;
    else r.result = ar;  // report the first failure but keep going for the rest of this person's doses
  }
  if (r.applied > 0) {
    if (c.type == CommandType::Undo) {
      last_ = DoseCommand();
    } else {
      last_ = DoseCommand();
      last_.type = c.type;
      for (int i = 0; i < c.count; i++) {
        const DoseInstance* d = engine_.find(c.ids[i]);
        if (d && d->undoable) last_.add(c.ids[i]);
      }
      lastAt_ = now();
    }
    if (r.result != ActionResult::Ok && r.applied == c.count) r.result = ActionResult::Ok;
  }
  return r;
}

}  // namespace app
