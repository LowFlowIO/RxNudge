#include "app/app_storage.h"

#include "board/log.h"
#include "storage/arduino_fs.h"

namespace app {
namespace {
storage::LittleFsAdapter g_internal;
storage::SdAdapter g_sd;
storage::FsRepository* g_repo = nullptr;
storage::FsDoseLog* g_log = nullptr;
core::Database g_db;
StorageSummary g_sum;
}  // namespace

const StorageSummary& storageBegin() {
  g_sum = StorageSummary();
  g_sum.internalMounted = g_internal.begin();
  g_sum.sdMounted = g_sd.begin();
  if (!g_sum.internalMounted) {
    g_sum.message = "ERROR: internal storage not mounted";
    MT_LOGF("storage: LittleFS mount failed");
    return g_sum;
  }
  static storage::FsRepository repo(g_internal);
  static storage::FsDoseLog log(g_internal);
  g_repo = &repo;
  g_log = &log;
  g_sum.dbStatus = repo.loadDatabase(g_db);
  if (g_sum.dbStatus == storage::LoadStatus::NotFound) {
    g_db = core::Database();  // fresh device: empty, valid database
    repo.saveDatabase(g_db);
  }
  g_sum.profiles = static_cast<int>(g_db.profiles.size());
  g_sum.meds = static_cast<int>(g_db.meds.size());
  g_sum.healthy = repo.health().healthy();
  g_sum.message = repo.health().lastError.empty() ? storage::loadStatusName(g_sum.dbStatus) : repo.health().lastError;
  // ids and counts only: never medication names (privacy rule)
  MT_LOGF("storage: db=%s profiles=%d meds=%d sd=%d migrations=%d", storage::loadStatusName(g_sum.dbStatus),
          g_sum.profiles, g_sum.meds, g_sum.sdMounted ? 1 : 0, repo.health().migrationsRun);
  if (g_sum.sdMounted) g_sd.mkdirs(storage::kRootDir);
  return g_sum;
}

const StorageSummary& storageSummary() { return g_sum; }
core::Database& database() { return g_db; }
storage::IRepository* repository() { return g_repo; }
storage::FsDoseLog* doseLog() { return g_log; }
storage::IFileSystem* internalFs() { return &g_internal; }
storage::IFileSystem* sdFs() { return &g_sd; }
bool sdRemount() {
  g_sum.sdMounted = g_sd.begin();
  return g_sum.sdMounted;
}

}  // namespace app
