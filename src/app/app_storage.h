#pragma once
// Boot-time storage bring-up for the firmware: mounts internal LittleFS (primary)
// and the SD card (exports/backups), loads the database and keeps a visible health summary.
#include <string>

#include "core/model.h"
#include "storage/log_store.h"
#include "storage/repository.h"

namespace app {

struct StorageSummary {
  bool internalMounted = false;
  bool sdMounted = false;
  storage::LoadStatus dbStatus = storage::LoadStatus::NotFound;
  int profiles = 0;
  int meds = 0;
  bool healthy = false;
  std::string message;  // short text for the home screen
};

// Call once from setup() after board::begin().
const StorageSummary& storageBegin();
const StorageSummary& storageSummary();
core::Database& database();
storage::IRepository* repository();  // nullptr if internal flash failed to mount
storage::FsDoseLog* doseLog();
storage::IFileSystem* internalFs();  // raw LittleFS adapter (callers serialise writes)
storage::IFileSystem* sdFs();        // raw SD adapter
bool sdRemount();                    // re-runs the SD mount (card inserted after boot)

}  // namespace app
