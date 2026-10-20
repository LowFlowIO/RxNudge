#pragma once
// Expands medication schedules into concrete dose occurrences (UTC).
#include <cstdint>
#include <vector>

#include "core/model.h"
#include "core/tz.h"

namespace core {

struct Occurrence {
  uint64_t eventId;
  uint8_t profileId;
  uint16_t medId;
  int64_t scheduledUtc;
  int32_t localDay;     // local calendar day of the dose
  int16_t localMinute;  // intended local wall-clock minute
  uint8_t phaseIndex;
};

// Deterministic event id: same (profile, med, scheduled minute) -> same id, always.
// Bit 63..56 profile (bit 63 set for PRN), 55..40 med id, 39..0 scheduled UTC minute.
uint64_t makeEventId(uint8_t profileId, uint16_t medId, int64_t scheduledUtc, bool prn = false);
bool eventIdIsPrn(uint64_t id);

// Is the medication scheduled on this local day (phase + day filter)? Returns phase index or -1.
int scheduledPhaseOn(const Medication& m, int32_t day);

// Local minute-of-day for a dose time (routine anchors resolved and clamped to the same day).
int16_t resolveMinute(const DoseTime& t, const Profile& p);

// All non-PRN occurrences with fromUtc < scheduledUtc <= toUtc, sorted by (scheduledUtc, medId).
// DST: nonexistent local times move to the first valid minute after the gap;
// ambiguous times fire once at the first occurrence; duplicates after resolution are merged.
void expandOccurrences(const Database& db, const PosixTz& tz, int64_t fromUtc, int64_t toUtc,
                       std::vector<Occurrence>& out, int profileFilter = -1);
void expandMedication(const Medication& m, const Profile& p, const PosixTz& tz, int64_t fromUtc, int64_t toUtc,
                      std::vector<Occurrence>& out);

// Average scheduled doses per day over [fromDay, fromDay + days).
double dosesPerDay(const Medication& m, const Profile& p, int32_t fromDay, int days);

}  // namespace core
