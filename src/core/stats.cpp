#include "core/stats.h"

#include <cmath>

#include "core/schedule.h"

namespace core {

AdherenceStats computeAdherence(const std::vector<DoseLogEntry>& log, const PosixTz& tz, int32_t fromDay,
                                int32_t toDay, int profileFilter, int medFilter) {
  AdherenceStats s;
  if (toDay < fromDay) return s;
  s.days.resize(static_cast<size_t>(toDay - fromDay + 1));
  for (int32_t d = fromDay; d <= toDay; d++) s.days[static_cast<size_t>(d - fromDay)].day = d;
  const auto resolved = resolveLog(log);
  for (const auto& kv : resolved) {
    const ResolvedDose& r = kv.second;
    if (!r.resolved) continue;
    if (profileFilter >= 0 && r.profileId != profileFilter) continue;
    if (medFilter >= 0 && r.medId != medFilter) continue;
    const int32_t day = utcToLocal(tz, r.scheduledUtc).days;
    if (day < fromDay || day > toDay) continue;
    DayStats& ds = s.days[static_cast<size_t>(day - fromDay)];
    switch (r.action) {
      case DoseAction::Taken: ds.onTime++; s.onTime++; break;
      case DoseAction::TakenLate: ds.late++; s.late++; break;
      case DoseAction::Skipped: ds.skipped++; s.skipped++; break;
      case DoseAction::Missed: ds.missed++; s.missed++; break;
      case DoseAction::PrnTaken: s.prn++; break;
      default: break;
    }
  }
  int run = 0;
  for (const DayStats& ds : s.days) {
    if (ds.total() == 0) continue;
    if (ds.perfect()) {
      run++;
      if (run > s.longestStreakDays) s.longestStreakDays = run;
    } else {
      run = 0;
    }
  }
  s.currentStreakDays = run;
  const int scheduled = s.onTime + s.late + s.skipped + s.missed;
  s.adherencePercent = scheduled ? 100.0 * (s.onTime + s.late) / scheduled : 0.0;
  return s;
}

WeeklySummary weeklySummary(const std::vector<DoseLogEntry>& log, const PosixTz& tz, int32_t day, int profileFilter) {
  WeeklySummary w;
  const int wd = weekdayFromDays(day);  // 0 = Sunday
  w.weekStartDay = day - ((wd + 6) % 7);
  const AdherenceStats a = computeAdherence(log, tz, w.weekStartDay, w.weekStartDay + 6, profileFilter);
  for (int i = 0; i < 7; i++) w.days[i] = a.days[static_cast<size_t>(i)];
  w.onTime = a.onTime;
  w.late = a.late;
  w.skipped = a.skipped;
  w.missed = a.missed;
  w.adherencePercent = a.adherencePercent;
  return w;
}

SupplyForecast forecastSupply(const Medication& m, const Profile& p, int32_t today, int leadDays) {
  SupplyForecast f;
  if (m.supply < 0) return f;
  f.tracked = true;
  f.lowStock = m.supply <= m.lowStockThreshold;
  f.dosesPerDay = dosesPerDay(m, p, today, 28);
  if (f.dosesPerDay > 0) {
    // Walk forward day by day so phase changes and course ends are honoured.
    int32_t remaining = m.supply;
    int32_t d = today;
    const int32_t limit = today + 3660;
    for (; d < limit; d++) {
      const int pi = scheduledPhaseOn(m, d);
      if (pi < 0) {
        const int32_t end = effectiveEndDay(m);
        if (end != kNoDate && d > end) break;  // course over before running out
        continue;
      }
      const ScheduleRule& r = m.phases[static_cast<size_t>(pi)].rule;
      if (r.type == ScheduleType::Prn) continue;
      const int32_t need = static_cast<int32_t>(std::lround(dosesPerDay(m, p, d, 1)));
      if (remaining < need) {
        f.runOutDay = d;
        break;
      }
      remaining -= need;
    }
    if (f.runOutDay != kNoDate) f.refillByDay = f.runOutDay - leadDays;
  }
  if (m.refillDay != kNoDate && (f.refillByDay == kNoDate || m.refillDay < f.refillByDay)) f.refillByDay = m.refillDay;
  return f;
}

}  // namespace core
