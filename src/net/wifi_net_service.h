#pragma once
// Device implementation of app::INetService (ESP32-S3, Arduino WiFi + ESP-IDF
// esp_http_server). Everything is on demand: the radio is switched on for a
// job (NTP, notification, hotspot provisioning, LAN portal) and off afterwards.
// Wi-Fi credentials live in NVS namespace "net" and are never logged.
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "app_core/admin_service.h"
#include "app_core/net_service.h"
#include "core/tz.h"
#include "net_core/notify.h"
#include "net_core/ota_verify.h"
#include "net_core/portal_api.h"
#include "net_core/portal_security.h"
#include "net_core/time_sync.h"

namespace net {

struct NetHooks {
  std::function<int64_t()> nowUtc;
  std::function<bool(int64_t)> setClockUtc;  // PCF8563 + system time (+ engine restart)
  std::function<core::PosixTz()> tz;
  std::function<void()> restart;             // flush storage, then esp_restart()
};

class WifiNetService : public app::INetService {
 public:
  explicit WifiNetService(NetHooks h);
  bool begin();                               // NVS + worker task; false = no radio
  void attachAdmin(app::AdminService* admin);  // portal + settings access (after AdminService exists)

  // Called by the device app.
  void autoSync(SyncReason r);                    // async, only when due
  void notifyMissed(const std::vector<MissedDose>& m);  // async, only when opted in
  bool busy() const;                              // sleep blocker
  bool clockStale();                              // > 7 days since the last NTP sync
  bool configured();                              // credentials stored and Wi-Fi enabled (never blocks)
  void stopInteractive();                         // portal + hotspot off (user asked to sleep)
  void shutdownForSleep();                        // radio off, servers stopped

  // app::INetService
  app::NetStatus status() override;
  std::string saveCredentials(const std::string& ssid, const std::string& pass) override;
  std::string forgetCredentials() override;
  std::string startJob(app::NetJob job) override;
  app::NetJobState jobState() override;
  std::string startProvisioning() override;
  std::string stopProvisioning() override;
  std::string startPortal() override;
  std::string stopPortal() override;
  std::string sendTestNotification() override;

 private:
  enum class JobKind : uint8_t { AutoSync, Notify, TestNewCredentials, Manual };
  struct Job {
    JobKind kind;
    SyncReason reason = SyncReason::Wake;
    app::NetJob manual = app::NetJob::None;
    std::vector<MissedDose> missed;
  };
  enum class ProvState : uint8_t { Idle, Testing, Ok, Failed };

  static void workerEntry(void* self);
  void worker();
  void runJob(Job& j);
  void push(Job j);
  void maintain();

  // radio (callers hold mx_)
  bool ensureSta(std::string* err);
  void releaseRadio();
  bool ntpQuery(int64_t* utc, std::string* err);
  std::string doSync(SyncReason r, int64_t* utcOut);
  bool httpSend(const OutboundRequest& r, std::string* err);
  void scanNetworks();
  void stopProvisioningLocked();
  void stopPortalLocked(const char* why);
  bool startCaptiveServer();
  bool startPortalServer();
  void loadState();
  void saveState();
  bool wifiEnabledSetting();
  void enableWifiSetting();

  // httpd handlers
  static int captiveHandler(void* req);
  static int portalHandler(void* req);
  static int otaHandler(void* req);

  NetHooks hooks_;
  app::AdminService* admin_ = nullptr;
  std::recursive_mutex mx_;  // radio + servers + credentials
  mutable std::mutex jobMx_;
  std::mutex secMx_;         // PortalSecurity / PortalApi
  std::deque<Job> jobs_;
  std::atomic<bool> jobRunning_{false};
  std::atomic<bool> radioOn_{false};
  TaskHandle_t task_ = nullptr;
  bool available_ = false;

  std::string ssid_, pass_;  // pass_ never leaves this class except to WiFi.begin()
  TimeSyncState sync_;
  std::string lastError_;

  // provisioning
  std::atomic<bool> provisioning_{false};
  std::string apSsid_, apPass_;
  struct ScanEntry {
    std::string ssid;
    int rssi;
    bool secure;
  };
  std::vector<ScanEntry> networks_;
  std::atomic<ProvState> provState_{ProvState::Idle};
  std::string provMessage_;
  std::string prevSsid_, prevPass_;
  uint32_t provActivityMs_ = 0;
  uint32_t provStopAtMs_ = 0;
  void* captive_ = nullptr;  // httpd_handle_t

  // LAN portal
  std::atomic<bool> portal_{false};
  void* portalHttpd_ = nullptr;  // httpd_handle_t
  std::unique_ptr<PortalSecurity> sec_;
  std::unique_ptr<IOtaTarget> ota_;
  std::unique_ptr<PortalApi> api_;
  std::string portalIp_;

  // lock-free snapshots for callers that must never wait for a radio job
  std::atomic<bool> hasCreds_{false};
  std::atomic<int64_t> lastSync_{0};
  app::NetStatus cache_;
  std::mutex jobStateMx_;
  app::NetJobState jstate_;
  uint32_t jobStartMs_ = 0;
  std::mutex cacheMx_;
};

// OTA rollback support: call once the app has started healthily (before any
// deep sleep). Marks a freshly installed image valid, or rolls back when the
// start was not healthy. No effect on an image that is already valid.
void otaConfirmBoot(bool healthy);
const char* otaRunningPartition();

}  // namespace net
