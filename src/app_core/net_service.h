#pragma once
// Network services used by the settings screens and the wizard: Wi-Fi
// credentials and provisioning (AP captive portal), NTP time sync, the LAN web
// portal (which also receives OTA uploads) and the caregiver notification.
// The device implementation is net::WifiNetService (src/net). Every call is
// on demand: the radio is switched off again when the job is done.
#include <cstdint>
#include <string>

namespace app {

struct NetStatus {
  bool available = false;     // this firmware has a network stack
  bool configured = false;    // credentials stored in NVS
  bool busy = false;          // radio on right now (sync, provisioning, portal, notification)
  bool provisioning = false;  // AP captive portal running
  bool portal = false;        // LAN web portal running
  std::string ssid;           // configured network (never the password)
  std::string text;           // one line for the status row
  std::string apSsid, apPass; // provisioning hotspot (shown on the device only)
  std::string portalUrl;      // e.g. http://rx-nudge.local
  std::string portalIp;
  std::string portalCode;     // one-time login code (shown on the device only)
  int64_t lastSyncUtc = 0;    // 0 = never
  int portalMinutesLeft = 0;
};

// Long-running actions run in the background so the screen never freezes;
// the settings pages poll jobState() and show progress, then the result.
enum class NetJob : uint8_t { None = 0, SyncTime, TestConnection };

struct NetJobState {
  NetJob kind = NetJob::None;  // last started job
  bool running = false;
  uint32_t seq = 0;            // incremented each time a job finishes
  std::string error;           // "" = success (last finished job)
  int64_t utc = 0;             // SyncTime: the time written to the clock
  int elapsedSec = 0;          // while running
};

class INetService {
 public:
  virtual ~INetService() = default;
  virtual NetStatus status() = 0;
  // Each call returns "" on success, otherwise a message suitable for a toast.
  virtual std::string saveCredentials(const std::string& ssid, const std::string& pass) = 0;
  virtual std::string forgetCredentials() = 0;
  // SyncTime: connect, NTP, write the RTC, radio off. TestConnection: connect, radio off.
  virtual std::string startJob(NetJob job) = 0;  // "" = started
  virtual NetJobState jobState() = 0;
  virtual std::string startProvisioning() = 0;       // hotspot + captive portal for a phone
  virtual std::string stopProvisioning() = 0;
  virtual std::string startPortal() = 0;             // LAN web portal (new one-time code)
  virtual std::string stopPortal() = 0;
  virtual std::string sendTestNotification() = 0;
};

// Used where no radio exists (host previews, or when the Wi-Fi driver failed to start).
class RadioOffNetService : public INetService {
 public:
  static constexpr const char* kMsg = "Wi-Fi is not available";
  NetStatus status() override {
    NetStatus s;
    s.text = "Wi-Fi hardware not available";
    return s;
  }
  std::string saveCredentials(const std::string&, const std::string&) override { return kMsg; }
  std::string forgetCredentials() override { return ""; }
  std::string startJob(NetJob) override { return kMsg; }
  NetJobState jobState() override { return NetJobState(); }
  std::string startProvisioning() override { return kMsg; }
  std::string stopProvisioning() override { return ""; }
  std::string startPortal() override { return kMsg; }
  std::string stopPortal() override { return ""; }
  std::string sendTestNotification() override { return kMsg; }
};

}  // namespace app
