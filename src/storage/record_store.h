#pragma once
// Crash-safe single-record files.
// Layout: "MTR1" | u16 schema | u16 flags(0) | u32 payload length | u32 CRC32(payload) | payload  (little endian)
// Write: <p>.tmp (write+flush+verify) -> remove <p>.bak -> rename <p> -> <p>.bak -> rename <p>.tmp -> <p>
// Read:  <p>, else valid <p>.tmp (interrupted commit), else <p>.bak (last known good).
#include <cstdint>
#include <string>

#include "storage/fs.h"

namespace storage {

enum class LoadStatus : uint8_t {
  Ok = 0,
  NotFound,           // no copy at all (fresh device)
  RecoveredFromTemp,  // commit was interrupted after the old file moved away; newest data recovered
  RecoveredFromBackup,  // main copy corrupt/missing: last known good (older) data restored -> visible warning
  Corrupt             // every copy unreadable -> visible error, nothing loaded
};
const char* loadStatusName(LoadStatus s);
inline bool loadOk(LoadStatus s) { return s == LoadStatus::Ok || s == LoadStatus::RecoveredFromTemp || s == LoadStatus::RecoveredFromBackup; }

constexpr size_t kRecordHeaderSize = 16;

std::string encodeRecord(uint16_t schema, const std::string& payload);
// Returns false on bad magic, length or CRC.
bool decodeRecord(const std::string& raw, uint16_t& schema, std::string& payload);

class RecordStore {
 public:
  explicit RecordStore(IFileSystem& fs) : fs_(fs) {}
  bool write(const std::string& path, uint16_t schema, const std::string& payload);
  // On recovery the main file is rewritten from the recovered copy.
  LoadStatus read(const std::string& path, uint16_t& schema, std::string& payload);

 private:
  IFileSystem& fs_;
};

}  // namespace storage
