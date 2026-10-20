#include "storage/record_store.h"

#include "core/selftest_logic.h"

namespace storage {

const char* loadStatusName(LoadStatus s) {
  switch (s) {
    case LoadStatus::Ok: return "ok";
    case LoadStatus::NotFound: return "empty";
    case LoadStatus::RecoveredFromTemp: return "recovered(tmp)";
    case LoadStatus::RecoveredFromBackup: return "recovered(backup)";
    case LoadStatus::Corrupt: return "CORRUPT";
  }
  return "?";
}

static void put16(std::string& s, uint16_t v) {
  s += static_cast<char>(v & 0xFF);
  s += static_cast<char>(v >> 8);
}
static void put32(std::string& s, uint32_t v) {
  for (int i = 0; i < 4; i++) s += static_cast<char>((v >> (8 * i)) & 0xFF);
}
static uint32_t get32(const std::string& s, size_t o) {
  uint32_t v = 0;
  for (int i = 0; i < 4; i++) v |= static_cast<uint32_t>(static_cast<uint8_t>(s[o + i])) << (8 * i);
  return v;
}

std::string encodeRecord(uint16_t schema, const std::string& payload) {
  std::string out = "MTR1";
  put16(out, schema);
  put16(out, 0);
  put32(out, static_cast<uint32_t>(payload.size()));
  put32(out, core::crc32(reinterpret_cast<const uint8_t*>(payload.data()), payload.size()));
  out += payload;
  return out;
}

bool decodeRecord(const std::string& raw, uint16_t& schema, std::string& payload) {
  if (raw.size() < kRecordHeaderSize || raw.compare(0, 4, "MTR1") != 0) return false;
  schema = static_cast<uint16_t>(static_cast<uint8_t>(raw[4]) | (static_cast<uint8_t>(raw[5]) << 8));
  const uint32_t len = get32(raw, 8);
  if (raw.size() != kRecordHeaderSize + len) return false;
  const uint32_t crc = get32(raw, 12);
  if (core::crc32(reinterpret_cast<const uint8_t*>(raw.data() + kRecordHeaderSize), len) != crc) return false;
  payload = raw.substr(kRecordHeaderSize);
  return true;
}

bool RecordStore::write(const std::string& path, uint16_t schema, const std::string& payload) {
  const std::string tmp = path + ".tmp", bak = path + ".bak";
  const std::string rec = encodeRecord(schema, payload);
  fs_.mkdirs(parentDir(path));
  if (fs_.exists(tmp)) fs_.remove(tmp);
  if (!fs_.writeFile(tmp, rec)) {
    fs_.remove(tmp);
    return false;
  }
  std::string verify;  // read back: catches silent media write errors before we touch the good copy
  if (!fs_.readFile(tmp, verify) || verify != rec) {
    fs_.remove(tmp);
    return false;
  }
  if (fs_.exists(path)) {
    // Only rotate a main file that is itself valid, so .bak is always "last known good".
    std::string cur, curPayload;
    uint16_t curSchema;
    const bool curValid = fs_.readFile(path, cur) && decodeRecord(cur, curSchema, curPayload);
    if (curValid) {
      if (fs_.exists(bak) && !fs_.remove(bak)) return false;
      if (!fs_.rename(path, bak)) return false;
    } else if (!fs_.remove(path)) {
      return false;
    }
  }
  return fs_.rename(tmp, path);
}

LoadStatus RecordStore::read(const std::string& path, uint16_t& schema, std::string& payload) {
  const std::string tmp = path + ".tmp", bak = path + ".bak";
  std::string raw;
  const bool haveMain = fs_.readFile(path, raw);
  if (haveMain && decodeRecord(raw, schema, payload)) {
    if (fs_.exists(tmp)) fs_.remove(tmp);  // stale temp from a commit that never got past step 1
    return LoadStatus::Ok;
  }
  const bool haveTmp = fs_.readFile(tmp, raw);
  // A temp file is only trustworthy if the main file is gone (crash between the two renames).
  if (!haveMain && haveTmp && decodeRecord(raw, schema, payload)) {
    fs_.rename(tmp, path);
    return LoadStatus::RecoveredFromTemp;
  }
  const bool haveBak = fs_.readFile(bak, raw);
  if (haveBak && decodeRecord(raw, schema, payload)) {
    // Repair main from the backup, keep the corrupt main aside for diagnosis.
    if (haveMain) {
      fs_.remove(path + ".bad");
      fs_.rename(path, path + ".bad");
      if (fs_.exists(path)) fs_.remove(path);
    }
    fs_.writeFile(path, raw);
    return LoadStatus::RecoveredFromBackup;
  }
  if (!haveMain && !haveTmp && !haveBak) return LoadStatus::NotFound;
  return LoadStatus::Corrupt;
}

}  // namespace storage
