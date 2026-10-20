#include "net_core/time_sync.h"

#include <cstring>

#include "core/civil.h"

namespace net {

void buildNtpRequest(uint8_t out[kNtpPacketSize]) {
  memset(out, 0, kNtpPacketSize);
  out[0] = (0 << 6) | (4 << 3) | 3;  // LI 0, version 4, mode 3 (client)
}

static uint32_t be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool parseNtpResponse(const uint8_t* p, size_t n, int64_t* unixSec, uint32_t* ms, std::string* err) {
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  if (!p || n < kNtpPacketSize) return fail("short packet");
  const int li = p[0] >> 6, mode = p[0] & 7;
  if (mode != 4) return fail("not a server reply");
  if (li == 3) return fail("server not synchronised");
  const int stratum = p[1];
  if (stratum < 1 || stratum > 15) return fail(stratum == 0 ? "kiss-of-death" : "bad stratum");
  const uint32_t sec = be32(p + 40), frac = be32(p + 44);
  if (sec == 0 && frac == 0) return fail("no transmit time");
  // Era 0 ends in 2036: values below 2^31 belong to era 1.
  int64_t ntp = sec;
  if (sec < 0x80000000u) ntp += 0x100000000LL;
  const int64_t unixT = ntp - kNtpUnixOffset;
  const core::Ymd d = core::civilFromDays(static_cast<int32_t>(core::floorDiv(unixT, 86400)));
  if (d.y < 2024 || d.y > 2099) return fail("implausible year");
  if (unixSec) *unixSec = unixT;
  if (ms) *ms = static_cast<uint32_t>((static_cast<uint64_t>(frac) * 1000ULL) >> 32);
  return true;
}

const char* syncReasonName(SyncReason r) {
  switch (r) {
    case SyncReason::ColdBoot: return "boot";
    case SyncReason::Wake: return "wake";
    case SyncReason::DailySlot: return "daily";
    case SyncReason::Manual: return "manual";
  }
  return "?";
}

int64_t retryAfterSec(int failures) {
  if (failures <= 0) return 0;
  int64_t s = 15 * 60;
  for (int i = 1; i < failures && s < 6 * 3600; i++) s *= 2;
  return s > 6 * 3600 ? 6 * 3600 : s;
}

bool syncDue(const TimeSyncState& s, SyncReason r, int64_t now, bool configured, bool autoSync) {
  if (!configured) return false;
  if (r == SyncReason::Manual) return true;
  if (!autoSync) return false;
  if (s.failures > 0 && now - s.lastAttemptUtc < retryAfterSec(s.failures)) return false;
  switch (r) {
    case SyncReason::ColdBoot:
    case SyncReason::DailySlot: return true;
    case SyncReason::Wake: return s.lastSyncUtc == 0 || now - s.lastSyncUtc > kSyncMaxAgeSec;
    default: return false;
  }
}

void recordSyncSuccess(TimeSyncState& s, int64_t ntpUtc, int64_t rtcBefore) {
  s.lastSyncUtc = ntpUtc;
  s.lastAttemptUtc = ntpUtc;
  s.failures = 0;
  const int64_t c = ntpUtc - rtcBefore;
  s.lastCorrectionSec = static_cast<int32_t>(c > 2000000000LL ? 2000000000LL : (c < -2000000000LL ? -2000000000LL : c));
}

void recordSyncFailure(TimeSyncState& s, int64_t now) {
  s.lastAttemptUtc = now;
  if (s.failures < 100) s.failures++;
}

SyncTrust syncTrust(const TimeSyncState& s, int64_t now, bool configured) {
  if (!configured || s.lastSyncUtc == 0) return SyncTrust::NotTracked;
  return now - s.lastSyncUtc >= kTrustMaxAgeSec ? SyncTrust::Stale : SyncTrust::Fresh;
}

const char* syncTrustName(SyncTrust t) {
  switch (t) {
    case SyncTrust::NotTracked: return "not_tracked";
    case SyncTrust::Fresh: return "fresh";
    case SyncTrust::Stale: return "stale";
  }
  return "?";
}

}  // namespace net
