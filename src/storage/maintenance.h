#pragma once
// SD backups (list/write/read) and factory reset of the internal store.
#include <string>
#include <vector>

#include "core/dose_log.h"
#include "storage/fs.h"

namespace storage {

std::string backupDir();  // /RxNudge/backups
// "backup-20261018-2307.json" from local date/time parts.
std::string backupFileName(int year, int month, int day, int hour, int minute);
bool writeBackupFile(IFileSystem& sd, const std::string& name, const std::string& json);  // tmp + rename
std::vector<std::string> listBackups(IFileSystem& sd);  // manual + auto files, newest first
bool readBackupFile(IFileSystem& sd, const std::string& name, std::string& json);

// Deletes the database (all copies), the engine snapshot and every dose-log file.
// Returns the number of files that could not be removed (0 = clean).
int wipeInternalStore(IFileSystem& fs, const std::string& root = "/RxNudge");
// Replaces the dose log with `entries` (used by restore).
bool rewriteDoseLog(IFileSystem& fs, const std::vector<core::DoseLogEntry>& entries, const std::string& root = "/RxNudge");

}  // namespace storage
