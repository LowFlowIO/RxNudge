#pragma once
// CSV / JSON export of the dose log and full JSON backup/restore.
#include <string>
#include <vector>

#include "core/dose_log.h"
#include "core/model.h"

namespace storage {

struct ExportOptions {
  bool includeNames = false;  // explicit user choice on the export screen; default keeps exports name-free
  int profileId = 0;          // 0 = all profiles
};

std::string exportCsv(const std::vector<core::DoseLogEntry>& log, const core::Database& db, const ExportOptions& o = {});
std::string exportJson(const std::vector<core::DoseLogEntry>& log, const core::Database& db, const ExportOptions& o = {});

// Backup = {"format":"rxnudge-backup","schemaVersion":N,"database":{...},"log":[...]}
std::string makeBackup(const core::Database& db, const std::vector<core::DoseLogEntry>& log);
// Accepts backups from older schema versions (migrated). Nothing is modified on failure.
bool restoreBackup(const std::string& json, core::Database& db, std::vector<core::DoseLogEntry>& log, std::string* err,
                   int* migrationsRun = nullptr);

std::string csvEscape(const std::string& s);

}  // namespace storage
