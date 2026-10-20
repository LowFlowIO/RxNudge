#pragma once
// Append-only dose log, one file per UTC month: <root>/logs/YYYY-MM.log
// Each record is "\n<json>|<crc32 hex>"; the leading newline isolates a torn
// record from a power cut so later appends stay readable. Corrections are new
// entries (Undo / Backfill); nothing is ever rewritten. Entries hold ids only.
#include <string>
#include <vector>

#include "core/dose_log.h"
#include "storage/fs.h"

namespace storage {

std::string logMonthFile(int64_t utc);  // "YYYY-MM.log"
std::string encodeLogLine(const core::DoseLogEntry& e);
bool decodeLogLine(const std::string& line, core::DoseLogEntry& e);

class FsDoseLog : public core::IDoseLogSink {
 public:
  FsDoseLog(IFileSystem& fs, std::string root = "/RxNudge") : fs_(fs), dir_(root + "/logs") {}
  bool append(const core::DoseLogEntry& e) override;
  // Reads every month in chronological order. Bad (torn/corrupt) records are skipped and counted.
  std::vector<core::DoseLogEntry> readAll(size_t* badRecords = nullptr);
  std::vector<core::DoseLogEntry> readMonth(const std::string& fileName, size_t* badRecords = nullptr);
  std::vector<std::string> months();  // sorted file names
  const std::string& dir() const { return dir_; }
  size_t failedAppends() const { return failed_; }

 private:
  IFileSystem& fs_;
  std::string dir_;
  size_t failed_ = 0;
};

}  // namespace storage
