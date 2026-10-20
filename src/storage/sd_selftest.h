#pragma once
#include <cstdint>

namespace storage {

struct SdTestResult {
  bool detectPin = false;  // GPIO11 reports a card (LOW)
  bool mounted = false;
  bool written = false;
  bool readBack = false;
  bool crcMatch = false;
  bool removed = false;
  uint32_t bytes = 0;
  uint32_t writeMs = 0;
  uint32_t readMs = 0;
  uint32_t crc = 0;
  bool pass() const { return mounted && written && readBack && crcMatch && removed; }
};

// Writes a 16 KiB pseudo-random file to /rxnudge/selftest.bin with SdFat on
// the shared SPI bus, reads it back, compares the CRC32, deletes it, and
// releases the card (SdMan.sleep(); SPI.end() must never be called).
SdTestResult runSdSelfTest();

}  // namespace storage
