#include "net_core/ota_verify.h"

#include <cstring>

namespace net {

static std::string cstr(const uint8_t* p, size_t n) {
  size_t len = 0;
  while (len < n && p[len]) len++;
  std::string s(reinterpret_cast<const char*>(p), len);
  for (char& c : s)
    if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) c = '?';
  return s;
}

bool parseImageHead(const uint8_t* p, size_t n, uint16_t* chipId, AppDescriptor* desc, std::string* err) {
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  if (n < kImageHeadNeeded) return fail("image too small");
  if (p[0] != kEspImageMagic) return fail("not an ESP32 firmware image");
  const uint16_t chip = static_cast<uint16_t>(p[12] | (p[13] << 8));
  if (chipId) *chipId = chip;
  // esp_app_desc_t follows the 24-byte image header and the 8-byte first segment header
  const uint8_t* d = p + 32;
  const uint32_t magic = uint32_t(d[0]) | (uint32_t(d[1]) << 8) | (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
  if (magic != kAppDescMagic) return fail("no app descriptor");
  if (desc) {
    desc->valid = true;
    desc->version = cstr(d + 16, 32);
    desc->project = cstr(d + 48, 32);
    desc->idfVersion = cstr(d + 112, 32);
  }
  return true;
}

bool OtaSession::fail(const std::string& e, std::string* err) {
  if (st_ == State::Receiving) t_.abort();
  st_ = State::Failed;
  err_ = e;
  if (err) *err = e;
  return false;
}

bool OtaSession::begin(size_t size, const std::string& shaHex, std::string* err) {
  st_ = State::Idle;
  got_ = 0;
  head_.clear();
  desc_ = AppDescriptor();
  sha_.reset();
  err_.clear();
  if (!parseSha256Hex(shaHex, want_)) return fail("SHA-256 missing or not 64 hex digits", err);
  if (size < kImageHeadNeeded) return fail("file too small", err);
  if (size > t_.capacity()) return fail("file larger than the update slot", err);
  size_ = size;
  std::string e;
  if (!t_.begin(size, &e)) return fail("cannot open update slot: " + e, err);
  st_ = State::Receiving;
  return true;
}

bool OtaSession::write(const uint8_t* p, size_t n, std::string* err) {
  if (st_ != State::Receiving) return fail("no upload in progress", err);
  if (got_ + n > size_) return fail("more data than announced", err);
  if (head_.size() < kImageHeadNeeded) {
    const size_t take = n < kImageHeadNeeded - head_.size() ? n : kImageHeadNeeded - head_.size();
    head_.append(reinterpret_cast<const char*>(p), take);
    if (head_.size() == kImageHeadNeeded) {
      uint16_t chip = 0;
      std::string e;
      if (!parseImageHead(reinterpret_cast<const uint8_t*>(head_.data()), head_.size(), &chip, &desc_, &e)) return fail(e, err);
      if (chip != kChipIdEsp32S3) return fail("firmware is not for ESP32-S3", err);
      if (!project_.empty() && desc_.project != project_) return fail("firmware is for '" + desc_.project + "'", err);
    }
  }
  sha_.update(p, n);
  std::string e;
  if (!t_.write(p, n, &e)) return fail("flash write failed: " + e, err);
  got_ += n;
  return true;
}

bool OtaSession::finish(std::string* err) {
  if (st_ != State::Receiving) return fail("no upload in progress", err);
  if (got_ != size_) return fail("upload incomplete", err);
  uint8_t d[32];
  sha_.finish(d);
  if (!constantTimeEqual(d, want_, 32)) return fail("SHA-256 mismatch: file damaged or wrong checksum", err);
  std::string e;
  if (!t_.commit(&e)) {
    st_ = State::Failed;
    err_ = "image rejected: " + e;
    if (err) *err = err_;
    return false;
  }
  st_ = State::Done;
  return true;
}

void OtaSession::abort() {
  if (st_ == State::Receiving) t_.abort();
  st_ = State::Idle;
}

}  // namespace net
