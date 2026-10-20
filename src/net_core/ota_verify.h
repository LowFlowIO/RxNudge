#pragma once
// Streaming OTA verification (pure, host-tested). The upload must carry the
// SHA-256 of the image (X-Firmware-SHA256 header or a form field); bytes go to
// the inactive slot while being hashed, and the slot only becomes bootable when
// size, ESP32-S3 image header, app descriptor and hash all match.
#include <cstddef>
#include <cstdint>
#include <string>

#include "net_core/sha256.h"

namespace net {

class IOtaTarget {
 public:
  virtual ~IOtaTarget() = default;
  virtual size_t capacity() const = 0;                           // inactive slot size
  virtual bool begin(size_t size, std::string* err) = 0;         // erase / open the inactive slot
  virtual bool write(const uint8_t* p, size_t n, std::string* err) = 0;
  virtual bool commit(std::string* err) = 0;                     // validate + set boot partition (pending verify)
  virtual void abort() = 0;
};

struct AppDescriptor {
  bool valid = false;
  std::string version, project, idfVersion;
};

constexpr uint8_t kEspImageMagic = 0xE9;
constexpr uint16_t kChipIdEsp32S3 = 9;
constexpr uint32_t kAppDescMagic = 0xABCD5432;
constexpr size_t kImageHeadNeeded = 32 + 256;  // image + segment header + esp_app_desc_t
// Parses the first bytes of an ESP-IDF app image.
bool parseImageHead(const uint8_t* p, size_t n, uint16_t* chipId, AppDescriptor* desc, std::string* err);

class OtaSession {
 public:
  enum class State : uint8_t { Idle, Receiving, Done, Failed };
  OtaSession(IOtaTarget& t, std::string expectedProject) : t_(t), project_(std::move(expectedProject)) {}
  bool begin(size_t declaredSize, const std::string& sha256Hex, std::string* err);
  bool write(const uint8_t* p, size_t n, std::string* err);
  bool finish(std::string* err);
  void abort();
  State state() const { return st_; }
  size_t received() const { return got_; }
  const AppDescriptor& descriptor() const { return desc_; }
  const std::string& error() const { return err_; }

 private:
  bool fail(const std::string& e, std::string* err);
  IOtaTarget& t_;
  std::string project_;
  State st_ = State::Idle;
  size_t size_ = 0, got_ = 0;
  uint8_t want_[32] = {};
  Sha256 sha_;
  std::string head_;
  AppDescriptor desc_;
  std::string err_;
};

}  // namespace net
