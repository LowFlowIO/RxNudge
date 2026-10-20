#pragma once
// Shared helpers for the native Unity suites (not a suite itself).
#include "core/civil.h"
#include "core/model.h"
#include "core/tz.h"

namespace fx {

inline int64_t utc(int y, int mo, int d, int h = 0, int mi = 0, int s = 0) {
  return static_cast<int64_t>(core::daysFromCivil(y, mo, d)) * 86400 + h * 3600 + mi * 60 + s;
}

inline const core::PosixTz& berlin() {
  static core::PosixTz tz;
  static bool init = core::parsePosixTz(core::kDefaultTz, tz);
  (void)init;
  return tz;
}

// Local Berlin wall time -> UTC (gap -> first valid instant, ambiguous -> first occurrence).
inline int64_t berlinLocal(int y, int mo, int d, int h, int mi = 0) {
  return core::localToUtc(berlin(), core::daysFromCivil(y, mo, d), h * 3600 + mi * 60);
}

inline int32_t day(int y, int mo, int d) { return core::daysFromCivil(y, mo, d); }

inline core::DoseTime at(int h, int m = 0) {
  core::DoseTime t;
  t.minute = static_cast<int16_t>(h * 60 + m);
  return t;
}

inline core::Profile profile(uint8_t id = 1) {
  core::Profile p;
  p.id = id;
  p.name = "Alex";
  p.initials = "AX";
  return p;
}

inline core::Medication med(uint16_t id, uint8_t profileId, int32_t start, core::ScheduleRule rule) {
  core::Medication m;
  m.id = id;
  m.profileId = profileId;
  m.name = "Med" + std::to_string(id);
  m.doseText = "1 tablet";
  m.startDay = start;
  core::Phase ph;
  ph.rule = rule;
  m.phases.push_back(ph);
  return m;
}

inline core::ScheduleRule fixedRule(std::initializer_list<core::DoseTime> times) {
  core::ScheduleRule r;
  r.type = core::ScheduleType::FixedTimes;
  r.times = times;
  return r;
}

inline core::Database singleMedDb(core::ScheduleRule rule, int32_t start) {
  core::Database db;
  db.profiles.push_back(profile(1));
  db.meds.push_back(med(1, 1, start, rule));
  return db;
}

}  // namespace fx
