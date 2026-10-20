#include "storage/sd_selftest.h"

#include <Arduino.h>
#include <SDCardManager.h>

#include "board/board.h"
#include "board/log.h"
#include "core/selftest_logic.h"

namespace storage {
namespace {

constexpr const char* kDir = "/rxnudge";
constexpr const char* kPath = "/rxnudge/selftest.bin";
constexpr size_t kChunk = 512;
constexpr uint32_t kBytes = 16 * 1024;

}  // namespace

SdTestResult runSdSelfTest() {
  SdTestResult r;
  r.detectPin = board::sdCardDetected();
  r.bytes = kBytes;
  r.mounted = SdMan.begin();
  if (!r.mounted) {
    MT_LOGF("sd: mount failed (detect=%d)", r.detectPin);
    return r;
  }
  SdMan.ensureDirectoryExists(kDir);

  uint8_t buf[kChunk];
  uint32_t crcWritten = 0;
  const uint32_t seed = static_cast<uint32_t>(micros()) | 1u;

  uint32_t t0 = millis();
  FsFile f;
  if (SdMan.openFileForWrite("SELFTEST", kPath, f)) {
    bool ok = true;
    for (uint32_t off = 0; off < kBytes; off += kChunk) {
      core::fillPattern(buf, kChunk, seed + off);
      crcWritten = core::crc32(buf, kChunk, crcWritten);
      if (f.write(buf, kChunk) != kChunk) {
        ok = false;
        break;
      }
    }
    ok = ok && f.sync();
    f.close();
    r.written = ok;
  }
  r.writeMs = millis() - t0;

  uint32_t crcRead = 0;
  t0 = millis();
  if (r.written && SdMan.openFileForRead("SELFTEST", kPath, f)) {
    uint32_t total = 0;
    while (total < kBytes) {
      const int n = f.read(buf, kChunk);
      if (n <= 0) break;
      crcRead = core::crc32(buf, static_cast<size_t>(n), crcRead);
      total += static_cast<uint32_t>(n);
    }
    f.close();
    r.readBack = total == kBytes;
  }
  r.readMs = millis() - t0;
  r.crc = crcRead;
  r.crcMatch = r.readBack && crcRead == crcWritten;
  r.removed = SdMan.remove(kPath);
  SdMan.sleep();
  MT_LOGF("sd: mounted=%d written=%d read=%d crc=%08lx match=%d removed=%d w=%lums r=%lums", r.mounted, r.written,
          r.readBack, static_cast<unsigned long>(r.crc), r.crcMatch, r.removed, static_cast<unsigned long>(r.writeMs),
          static_cast<unsigned long>(r.readMs));
  return r;
}

}  // namespace storage
