#pragma once
// Demo dataset (2 profiles) used by host previews and, with -DMT_DEMO_DATA=1,
// seeded on the device so every screen can be checked before the editors exist.
#include <vector>

#include "core/dose_log.h"
#include "core/model.h"
#include "core/tz.h"

namespace core {

Database makeDemoDatabase(int32_t today);
// Plausible history for the `days` days before nowUtc (deterministic): mostly taken, some late/skipped/missed.
std::vector<DoseLogEntry> makeDemoLog(const Database& db, const PosixTz& tz, int64_t nowUtc, int days);

}  // namespace core
