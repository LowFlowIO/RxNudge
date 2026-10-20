#pragma once
// NTP packet handling and the time-sync policy (pure, host-tested).
#include <cstddef>
#include <cstdint>
#include <string>

namespace net {

constexpr size_t kNtpPacketSize = 48;
constexpr int64_t kNtpUnixOffset = 2208988800LL;  // 1900-01-01 -> 1970-01-01
constexpr int64_t kSyncMaxAgeSec = 24 * 3600;     // re-sync after wake when older
constexpr int64_t kTrustMaxAgeSec = 7 * 86400;    // clock marked untrusted after this without a sync

void buildNtpRequest(uint8_t out[kNtpPacketSize]);
// Validates a server reply (mode 4, leap != 3, stratum 1..15, non-zero transmit
// time, year 2024..2099) and returns its transmit time as Unix seconds + ms.
bool parseNtpResponse(const uint8_t* p, size_t n, int64_t* unixSec, uint32_t* ms, std::string* err);

struct TimeSyncState {
  int64_t lastSyncUtc = 0;     // 0 = never
  int64_t lastAttemptUtc = 0;
  int failures = 0;            // consecutive
  int32_t lastCorrectionSec = 0;
};

enum class SyncReason : uint8_t { ColdBoot, Wake, DailySlot, Manual };
const char* syncReasonName(SyncReason r);

// Should we switch the radio on for NTP now?
bool syncDue(const TimeSyncState& s, SyncReason r, int64_t nowUtc, bool wifiConfigured, bool autoSync);
// Earliest next automatic attempt after failures (15 min doubling, max 6 h).
int64_t retryAfterSec(int failures);
void recordSyncSuccess(TimeSyncState& s, int64_t ntpUtc, int64_t rtcUtcBefore);
void recordSyncFailure(TimeSyncState& s, int64_t nowUtc);

enum class SyncTrust : uint8_t { NotTracked, Fresh, Stale };
// NotTracked: no Wi-Fi configured or never synced (the RTC trust rules from Phase 3 apply alone).
SyncTrust syncTrust(const TimeSyncState& s, int64_t nowUtc, bool wifiConfigured);
const char* syncTrustName(SyncTrust t);

}  // namespace net
