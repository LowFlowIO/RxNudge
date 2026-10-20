#include "storage/log_store.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

#include "core/civil.h"
#include "core/codec.h"
#include "core/selftest_logic.h"

namespace storage {

std::string logMonthFile(int64_t utc) {
  const core::Ymd d = core::civilFromDays(static_cast<int32_t>(core::floorDiv(utc, 86400)));
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d.log", d.y, d.m);
  return buf;
}

std::string encodeLogLine(const core::DoseLogEntry& e) {
  const std::string json = core::encodeLogEntry(e).dump();
  char crc[12];
  std::snprintf(crc, sizeof(crc), "%08lx",
                static_cast<unsigned long>(core::crc32(reinterpret_cast<const uint8_t*>(json.data()), json.size())));
  return "\n" + json + "|" + crc;
}

bool decodeLogLine(const std::string& line, core::DoseLogEntry& e) {
  const size_t bar = line.rfind('|');
  if (bar == std::string::npos || line.size() - bar != 9) return false;
  const std::string json = line.substr(0, bar);
  char* end = nullptr;
  const unsigned long crc = std::strtoul(line.c_str() + bar + 1, &end, 16);
  if (*end != '\0') return false;
  if (core::crc32(reinterpret_cast<const uint8_t*>(json.data()), json.size()) != static_cast<uint32_t>(crc)) return false;
  core::JsonValue j;
  return core::parseJson(json, j) && core::decodeLogEntry(j, e);
}

bool FsDoseLog::append(const core::DoseLogEntry& e) {
  fs_.mkdirs(dir_);
  const bool ok = fs_.appendFile(dir_ + "/" + logMonthFile(e.recordedUtc), encodeLogLine(e));
  if (!ok) failed_++;
  return ok;
}

std::vector<std::string> FsDoseLog::months() {
  std::vector<std::string> out;
  for (const auto& n : fs_.list(dir_))
    if (n.size() == 11 && n.compare(7, 4, ".log") == 0) out.push_back(n);
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<core::DoseLogEntry> FsDoseLog::readMonth(const std::string& fileName, size_t* badRecords) {
  std::vector<core::DoseLogEntry> out;
  std::string data;
  if (!fs_.readFile(dir_ + "/" + fileName, data)) return out;
  size_t pos = 0;
  while (pos <= data.size()) {
    size_t nl = data.find('\n', pos);
    if (nl == std::string::npos) nl = data.size();
    const std::string line = data.substr(pos, nl - pos);
    if (!line.empty()) {
      core::DoseLogEntry e;
      if (decodeLogLine(line, e)) out.push_back(e);
      else if (badRecords) (*badRecords)++;
    }
    pos = nl + 1;
  }
  return out;
}

std::vector<core::DoseLogEntry> FsDoseLog::readAll(size_t* badRecords) {
  if (badRecords) *badRecords = 0;
  std::vector<core::DoseLogEntry> out;
  for (const auto& m : months()) {
    auto part = readMonth(m, badRecords);
    out.insert(out.end(), part.begin(), part.end());
  }
  return out;
}

}  // namespace storage
