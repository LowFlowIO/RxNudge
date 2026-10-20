#include "core/alarm.h"

#include <algorithm>
#include <map>

namespace core {

const char* patternName(PatternId p) {
  switch (p) {
    case PatternId::Chime: return "chime";
    case PatternId::Beeps: return "beeps";
    case PatternId::Urgent: return "urgent";
    case PatternId::Gentle: return "gentle";
    default: return "inherit";
  }
}

const AlertPattern& patternFor(PatternId p) {
  static const AlertPattern kPatterns[] = {
      {{{2093, 180, 60}, {2637, 180, 60}, {3136, 300, 600}}, 3, 2},                                      // chime
      {{{2400, 150, 120}, {2400, 150, 120}, {2400, 150, 700}}, 3, 2},                                    // beeps
      {{{2700, 100, 60}, {2700, 100, 60}, {2700, 100, 60}, {2700, 100, 60}, {2700, 100, 400}}, 5, 3},    // urgent
      {{{1800, 250, 900}}, 1, 2},                                                                       // gentle
  };
  const int i = static_cast<int>(p);
  return kPatterns[(i >= 0 && i < static_cast<int>(PatternId::Count)) ? i : 0];
}

uint32_t patternDurationMs(const AlertPattern& p) {
  uint32_t t = 0;
  for (int i = 0; i < p.count; i++) t += p.steps[i].onMs + p.steps[i].offMs;
  return t * p.repeats;
}

uint16_t volumeToDuty(uint8_t percent) {
  if (percent == 0) return 0;
  if (percent > 100) percent = 100;
  // perceived loudness is roughly logarithmic in pulse width: quadratic curve, minimum audible 8/1024
  const uint32_t d = 8 + (504u * percent * percent) / 10000u;
  return static_cast<uint16_t>(d > 512 ? 512 : d);
}

bool alertAudible(const Profile* p, bool critical, int localMinute) {
  if (critical || !p) return true;
  return !inQuietHours(*p, localMinute);
}

AlertPlan planAlerts(const std::vector<EngineEvent>& events, const DoseEngine& engine, const Database& db,
                     const PosixTz& tz, int64_t nowUtc) {
  AlertPlan plan;
  struct Acc {
    PersonAlert a;
    bool anyFresh = false;
    bool anyAudible = false;
    bool medPattern = false;
    uint64_t seen[32];
    int nSeen = 0;
  };
  std::map<uint8_t, Acc> by;
  const int nowMinute = utcToLocal(tz, nowUtc).secOfDay / 60;
  for (const EngineEvent& e : events) {
    if (e.kind == EngineEventKind::BecameMissed) {
      plan.missed++;
      continue;
    }
    const DoseInstance* d = engine.find(e.eventId);
    // Only doses that are still waiting for the user alert; a dose that became due and then
    // missed during the same catch-up window is reported through `missed` instead.
    if (!d || d->state != DoseState::Due) continue;
    const Medication* m = db.med(d->medId);
    const Profile* p = db.profile(d->profileId);
    Acc& acc = by[d->profileId];
    PersonAlert& a = acc.a;
    bool dup = false;
    for (int i = 0; i < acc.nSeen; i++) dup = dup || acc.seen[i] == d->eventId;
    if (acc.nSeen == 0) {
      a.profileId = d->profileId;
      a.earliestScheduledUtc = d->scheduledUtc;
      a.pattern = p ? static_cast<PatternId>(p->alertPattern) : PatternId::Chime;
      a.volume = p ? p->alertVolume : 70;
    }
    if (!dup && acc.nSeen < 32) {
      acc.seen[acc.nSeen++] = d->eventId;
      a.doseCount++;
      if (e.stale) plan.catchUpDoses++;
    }
    a.earliestScheduledUtc = std::min(a.earliestScheduledUtc, d->scheduledUtc);
    a.alarmIndex = std::max(a.alarmIndex, e.alarmIndex);
    const bool crit = m && m->critical;
    a.critical = a.critical || crit;
    if (m && m->alertPattern != static_cast<uint8_t>(PatternId::Inherit) &&
        m->alertPattern < static_cast<uint8_t>(PatternId::Count) && (!acc.medPattern || crit)) {
      a.pattern = static_cast<PatternId>(m->alertPattern);
      acc.medPattern = true;
    }
    if (m && m->alertVolume > 0) a.volume = std::max(a.volume, m->alertVolume);
    if (!e.stale) {
      acc.anyFresh = true;
      acc.anyAudible = acc.anyAudible || e.audible;
    } else {
      acc.anyAudible = acc.anyAudible || alertAudible(p, crit, nowMinute);
    }
  }
  for (auto& kv : by) {
    Acc& acc = kv.second;
    acc.a.catchUp = !acc.anyFresh;
    acc.a.audible = acc.anyAudible || acc.a.critical;
    if (acc.a.pattern >= PatternId::Count) acc.a.pattern = PatternId::Chime;
    plan.alerts.push_back(acc.a);
  }
  std::sort(plan.alerts.begin(), plan.alerts.end(), [](const PersonAlert& x, const PersonAlert& y) {
    if (x.critical != y.critical) return x.critical;
    if (x.earliestScheduledUtc != y.earliestScheduledUtc) return x.earliestScheduledUtc < y.earliestScheduledUtc;
    return x.profileId < y.profileId;
  });
  return plan;
}

bool AlarmManager::addOutput(AlertOutput* o) {
  if (!o || n_ >= kMaxOutputs) return false;
  out_[n_++] = o;
  return true;
}

AlarmManager::Result AlarmManager::deliver(const PersonAlert& a) {
  Result r;
  for (int i = 0; i < n_; i++) {
    AlertOutput* o = out_[i];
    if (!o->available()) continue;
    if (o->audible()) {
      if (!a.audible) continue;
      o->play(a);
      r.sounded = true;
    } else {
      o->play(a);
      r.shown = true;
    }
  }
  return r;
}

bool AlarmManager::visualOnly() const {
  for (int i = 0; i < n_; i++)
    if (out_[i]->audible() && out_[i]->available()) return false;
  return true;
}

}  // namespace core
