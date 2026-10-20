#pragma once
// Streaming SHA-256 (FIPS 180-4). Pure C++ so OTA verification is host-tested
// with exactly the code that runs on the device.
#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

class Sha256 {
 public:
  Sha256() { reset(); }
  void reset();
  void update(const uint8_t* data, size_t len);
  void update(const std::string& s) { update(reinterpret_cast<const uint8_t*>(s.data()), s.size()); }
  void finish(uint8_t out[32]);
  std::string hexDigest();  // finishes; lowercase hex

  static std::string hex(const std::string& data);

 private:
  void block(const uint8_t* p);
  uint32_t h_[8];
  uint8_t buf_[64];
  size_t bufLen_;
  uint64_t total_;
};

std::string toHex(const uint8_t* p, size_t n);
// Accepts 64 hex digits (any case, surrounding whitespace ignored); false otherwise.
bool parseSha256Hex(const std::string& in, uint8_t out[32]);
// Comparison time does not depend on where the first difference is.
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n);
bool constantTimeEqual(const std::string& a, const std::string& b);

}  // namespace net
