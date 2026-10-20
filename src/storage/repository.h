#pragma once
// Repositories for the configuration database and the dose-engine snapshot.
#include <string>

#include "core/dose_engine.h"
#include "core/model.h"
#include "storage/fs.h"
#include "storage/record_store.h"

namespace storage {

constexpr const char* kRootDir = "/RxNudge";

struct StorageHealth {
  LoadStatus dbStatus = LoadStatus::NotFound;
  LoadStatus snapshotStatus = LoadStatus::NotFound;
  int migrationsRun = 0;
  bool lastWriteFailed = false;
  std::string lastError;  // shown on the home/status screen; empty = healthy
  bool healthy() const {
    return lastError.empty() && !lastWriteFailed && dbStatus != LoadStatus::Corrupt &&
           dbStatus != LoadStatus::RecoveredFromBackup && snapshotStatus != LoadStatus::Corrupt;
  }
};

class IRepository {
 public:
  virtual ~IRepository() = default;
  virtual LoadStatus loadDatabase(core::Database& db) = 0;
  virtual bool saveDatabase(const core::Database& db) = 0;
  virtual LoadStatus loadSnapshot(core::EngineSnapshot& s) = 0;
  virtual bool saveSnapshot(const core::EngineSnapshot& s) = 0;
  virtual const StorageHealth& health() const = 0;
};

// One implementation for every medium; the medium is the IFileSystem.
//  internal: LittleFS partition "spiffs" (Arduino adapter) / MemFs or HostFs on host
//  SD:       SdFat via SDCardManager (Arduino adapter) / HostFs on host
class FsRepository : public IRepository {
 public:
  FsRepository(IFileSystem& fs, std::string root = kRootDir) : fs_(fs), store_(fs), root_(std::move(root)) {}
  LoadStatus loadDatabase(core::Database& db) override;
  bool saveDatabase(const core::Database& db) override;
  LoadStatus loadSnapshot(core::EngineSnapshot& s) override;
  bool saveSnapshot(const core::EngineSnapshot& s) override;
  const StorageHealth& health() const override { return health_; }

  std::string dbPath() const { return root_ + "/db.rec"; }
  std::string snapshotPath() const { return root_ + "/engine.rec"; }
  IFileSystem& fs() { return fs_; }

 private:
  IFileSystem& fs_;
  RecordStore store_;
  std::string root_;
  StorageHealth health_;
};

}  // namespace storage
