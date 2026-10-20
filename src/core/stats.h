#pragma once
// Adherence statistics and supply forecasting. Pure functions over the dose log.
#include <cstdint>
#include <vector>

#include "core/dose_log.h"
#include "core/model.h"
#include "core/tz.h"

namespace core {

struct DayStats {
  int32_t day = 0;  // local day number
  int onTime = 0;
  int late = 0;
  int skipped = 0;
  int missed = 0;
  int total() const { return onTime + late + skipped + missed; }
  bool perfect() const { return total() > 0 && skipped == 0 && missed == 0; }
};

struct AdherenceStats {
  int onTime = 0;
  int late = 0;
  int skipped = 0;
  int missed = 0;
  int prn = 0;
  int currentStreakDays = 0;  // consecutive days (ending at toDay) where every dose was taken
  int longestStreakDays = 0;
  double adherencePercent = 0;  // (onTime + late) / scheduled outcomes
  std::vector<DayStats> days;   // one per day in [fromDay, toDay]
};

// Only resolved scheduled doses are counted; PRN takes are counted separately.
// Days without any scheduled dose neither extend nor break a streak.
AdherenceStats computeAdherence(const std::vector<DoseLogEntry>& log, const PosixTz& tz, int32_t fromDay,
                                int32_t toDay, int profileFilter = -1, int medFilter = -1);

struct WeeklySummary {
  int32_t weekStartDay = 0;  // Monday
  DayStats days[7];
  int onTime = 0, late = 0, skipped = 0, missed = 0;
  double adherencePercent = 0;
};
// The ISO week (Mon..Sun) containing `day`.
WeeklySummary weeklySummary(const std::vector<DoseLogEntry>& log, const PosixTz& tz, int32_t day,
                            int profileFilter = -1);

struct SupplyForecast {
  bool tracked = false;
  bool lowStock = false;
  double dosesPerDay = 0;
  int32_t runOutDay = kNoDate;   // first day the supply no longer covers the schedule
  int32_t refillByDay = kNoDate; // runOutDay - leadDays, or the explicit refill date if earlier
};
SupplyForecast forecastSupply(const Medication& m, const Profile& p, int32_t today, int leadDays = 3);

}  // namespace core
