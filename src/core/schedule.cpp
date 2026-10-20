#include "core/schedule.h"

#include <algorithm>

namespace core {

uint64_t makeEventId(uint8_t profileId, uint16_t medId, int64_t scheduledUtc, bool prn) {
  const uint64_t minute = static_cast<uint64_t>(floorDiv(scheduledUtc, 60)) & 0xFFFFFFFFFFULL;
  const uint64_t prof = static_cast<uint64_t>(profileId & 0x7F) | (prn ? 0x80u : 0u);
  return (prof << 56) | (static_cast<uint64_t>(medId) << 40) | minute;
}

bool eventIdIsPrn(uint64_t id) { return (id >> 63) != 0; }

static bool dayMatches(const ScheduleRule& r, int32_t day, int32_t anchor) {
  switch (r.type) {
    case ScheduleType::FixedTimes:
    case ScheduleType::Routine:
      return true;
    case ScheduleType::Weekdays:
      return (r.weekdayMask >> weekdayFromDays(day)) & 1;
    case ScheduleType::EveryN: {
      const int64_t period = static_cast<int64_t>(r.intervalN) * (r.intervalWeeks ? 7 : 1);
      return floorMod(static_cast<int64_t>(day) - anchor, period) == 0;
    }
    case ScheduleType::SpecificDates:
      return std::find(r.dates.begin(), r.dates.end(), day) != r.dates.end();
    case ScheduleType::Cycle: {
      const int64_t period = static_cast<int64_t>(r.cycleOn) + r.cycleOff;
      return floorMod(static_cast<int64_t>(day) - anchor, period) < r.cycleOn;
    }
    case ScheduleType::Prn:
    default:
      return false;
  }
}

int scheduledPhaseOn(const Medication& m, int32_t day) {
  if (!m.active) return -1;
  const Phase* ph = phaseOn(m, day);
  if (!ph) return -1;
  const int32_t anchor = ph->fromDay == kNoDate ? m.startDay : ph->fromDay;
  if (!dayMatches(ph->rule, day, anchor)) return -1;
  return static_cast<int>(ph - m.phases.data());
}

int16_t resolveMinute(const DoseTime& t, const Profile& p) {
  int v = t.minute;
  if (t.routine != Routine::None) {
    v = p.routineMin[static_cast<size_t>(t.routine) - 1] + t.offsetMin;
  }
  if (v < 0) v = 0;
  if (v > 1439) v = 1439;
  return static_cast<int16_t>(v);
}

void expandMedication(const Medication& m, const Profile& p, const PosixTz& tz, int64_t fromUtc, int64_t toUtc,
                      std::vector<Occurrence>& out) {
  if (toUtc <= fromUtc) return;
  // Local day range covering the UTC window, with one day of slack each side for offsets.
  const int32_t d0 = static_cast<int32_t>(floorDiv(fromUtc, 86400)) - 1;
  const int32_t d1 = static_cast<int32_t>(floorDiv(toUtc, 86400)) + 1;
  const size_t before = out.size();
  for (int32_t day = d0; day <= d1; day++) {
    const int pi = scheduledPhaseOn(m, day);
    if (pi < 0) continue;
    const ScheduleRule& r = m.phases[static_cast<size_t>(pi)].rule;
    if (r.type == ScheduleType::Prn) continue;
    for (const DoseTime& t : r.times) {
      const int16_t minute = resolveMinute(t, p);
      const int64_t utc = localToUtc(tz, day, minute * 60);
      if (utc <= fromUtc || utc > toUtc) continue;
      Occurrence o{makeEventId(m.profileId, m.id, utc), m.profileId, m.id, utc, day, minute, static_cast<uint8_t>(pi)};
      out.push_back(o);
    }
  }
  // Merge duplicates produced by DST gaps (two local times -> same instant) or repeated times.
  std::sort(out.begin() + static_cast<long>(before), out.end(),
            [](const Occurrence& a, const Occurrence& b) { return a.scheduledUtc < b.scheduledUtc; });
  out.erase(std::unique(out.begin() + static_cast<long>(before), out.end(),
                        [](const Occurrence& a, const Occurrence& b) { return a.eventId == b.eventId; }),
            out.end());
}

void expandOccurrences(const Database& db, const PosixTz& tz, int64_t fromUtc, int64_t toUtc,
                       std::vector<Occurrence>& out, int profileFilter) {
  out.clear();
  for (const Medication& m : db.meds) {
    if (profileFilter >= 0 && m.profileId != profileFilter) continue;
    const Profile* p = db.profile(m.profileId);
    if (!p) continue;
    expandMedication(m, *p, tz, fromUtc, toUtc, out);
  }
  std::sort(out.begin(), out.end(), [](const Occurrence& a, const Occurrence& b) {
    if (a.scheduledUtc != b.scheduledUtc) return a.scheduledUtc < b.scheduledUtc;
    if (a.profileId != b.profileId) return a.profileId < b.profileId;
    return a.medId < b.medId;
  });
}

double dosesPerDay(const Medication& m, const Profile& p, int32_t fromDay, int days) {
  if (days <= 0) return 0;
  long count = 0;
  for (int32_t d = fromDay; d < fromDay + days; d++) {
    const int pi = scheduledPhaseOn(m, d);
    if (pi < 0) continue;
    const ScheduleRule& r = m.phases[static_cast<size_t>(pi)].rule;
    if (r.type == ScheduleType::Prn) continue;
    // count distinct minutes
    std::vector<int16_t> mins;
    for (const auto& t : r.times) mins.push_back(resolveMinute(t, p));
    std::sort(mins.begin(), mins.end());
    count += std::unique(mins.begin(), mins.end()) - mins.begin();
  }
  return static_cast<double>(count) / days;
}

}  // namespace core
