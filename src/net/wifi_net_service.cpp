#include "net/wifi_net_service.h"

#include <Arduino.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_random.h>
#include <lwip/sockets.h>

#include <algorithm>
#include <cstring>

#include "board/log.h"
#include "core/json.h"
#include "net_core/api_json.h"
#include "net_core/web_assets.h"
#include "net_core/wifi_creds.h"

namespace net {
namespace {

constexpr uint32_t kConnectTimeoutMs = 12000;
constexpr uint32_t kPortalIdleMs = 10UL * 60UL * 1000UL;
constexpr uint32_t kProvIdleMs = 15UL * 60UL * 1000UL;
constexpr const char* kHostname = "rx-nudge";
constexpr const char* kNs = "net";
const IPAddress kApIp(192, 168, 4, 1);
DNSServer g_dns;

uint64_t rng64() { return (static_cast<uint64_t>(esp_random()) << 32) | esp_random(); }

// ESP-IDF esp_ota_* behind the pure verification session.
class EspOtaTarget : public IOtaTarget {
 public:
  size_t capacity() const override {
    const esp_partition_t* p = esp_ota_get_next_update_partition(nullptr);
    return p ? p->size : 0;
  }
  bool begin(size_t size, std::string* err) override {
    part_ = esp_ota_get_next_update_partition(nullptr);
    if (!part_) return fail("no update partition", err);
    const esp_err_t e = esp_ota_begin(part_, size, &h_);
    if (e != ESP_OK) return fail(esp_err_to_name(e), err);
    open_ = true;
    MT_LOGF("ota: writing to %s (%u bytes)", part_->label, static_cast<unsigned>(size));
    return true;
  }
  bool write(const uint8_t* p, size_t n, std::string* err) override {
    const esp_err_t e = esp_ota_write(h_, p, n);
    return e == ESP_OK || fail(esp_err_to_name(e), err);
  }
  bool commit(std::string* err) override {
    open_ = false;
    esp_err_t e = esp_ota_end(h_);  // checks the image structure and its appended digest
    if (e != ESP_OK) return fail(std::string("image check: ") + esp_err_to_name(e), err);
    e = esp_ota_set_boot_partition(part_);
    if (e != ESP_OK) return fail(esp_err_to_name(e), err);
    MT_LOGF("ota: %s set as boot partition (pending verify)", part_->label);
    return true;
  }
  void abort() override {
    if (open_) esp_ota_abort(h_);
    open_ = false;
  }

 private:
  static bool fail(const std::string& m, std::string* err) {
    if (err) *err = m;
    return false;
  }
  const esp_partition_t* part_ = nullptr;
  esp_ota_handle_t h_ = 0;
  bool open_ = false;
};

std::string header(httpd_req_t* r, const char* name) {
  const size_t n = httpd_req_get_hdr_value_len(r, name);
  if (!n) return "";
  std::string s(n + 1, '\0');
  if (httpd_req_get_hdr_value_str(r, name, &s[0], n + 1) != ESP_OK) return "";
  s.resize(n);
  return s;
}

std::string peerIp(httpd_req_t* r) {
  struct sockaddr_storage a;
  socklen_t len = sizeof a;
  if (getpeername(httpd_req_to_sockfd(r), reinterpret_cast<struct sockaddr*>(&a), &len) != 0) return "";
  uint8_t ip[4];
  if (a.ss_family == AF_INET) {
    memcpy(ip, &reinterpret_cast<struct sockaddr_in*>(&a)->sin_addr.s_addr, 4);
  } else if (a.ss_family == AF_INET6) {
    uint8_t v6[16];
    memcpy(v6, &reinterpret_cast<struct sockaddr_in6*>(&a)->sin6_addr, 16);
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (memcmp(v6, mapped, 12) != 0) return "";  // real IPv6 peers are not accepted (LAN check is IPv4)
    memcpy(ip, v6 + 12, 4);
  } else {
    return "";
  }
  char b[16];
  snprintf(b, sizeof b, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  return b;
}

const char* methodName(int m) {
  switch (m) {
    case HTTP_GET: return "GET";
    case HTTP_POST: return "POST";
    case HTTP_PUT: return "PUT";
    case HTTP_DELETE: return "DELETE";
    case HTTP_HEAD: return "HEAD";
    default: return "OTHER";
  }
}

HttpRequest buildRequest(httpd_req_t* r) {
  HttpRequest q;
  const std::string uri = r->uri;
  const size_t qm = uri.find('?');
  q.path = uri.substr(0, qm);
  if (qm != std::string::npos) q.query = uri.substr(qm + 1);
  q.meta.method = methodName(r->method);
  q.meta.ip = peerIp(r);
  q.meta.host = header(r, "Host");
  q.meta.cookie = header(r, "Cookie");
  q.meta.csrf = header(r, "X-CSRF-Token");
  q.meta.origin = header(r, "Origin");
  q.sha256 = header(r, "X-Firmware-SHA256");
  return q;
}

// Reads the body into out; false (with a response already sent) when too big or the client vanished.
bool readBody(httpd_req_t* r, size_t maxBytes, std::string& out) {
  if (r->content_len > maxBytes) {
    httpd_resp_set_status(r, "413 Payload Too Large");
    httpd_resp_set_type(r, "application/json");
    httpd_resp_sendstr(r, "{\"error\":\"request too large\"}");
    return false;
  }
  out.resize(r->content_len);
  size_t got = 0;
  int timeouts = 0;
  while (got < r->content_len) {
    const int n = httpd_req_recv(r, &out[got], r->content_len - got);
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;
    if (n <= 0) return false;
    got += static_cast<size_t>(n);
  }
  return true;
}

esp_err_t sendResponse(httpd_req_t* r, const HttpResponse& resp) {
  char status[48];
  snprintf(status, sizeof status, "%d %s", resp.status, statusText(resp.status));
  httpd_resp_set_status(r, status);
  httpd_resp_set_type(r, resp.contentType.c_str());
  if (resp.gzip) httpd_resp_set_hdr(r, "Content-Encoding", "gzip");
  for (const auto& h : resp.headers) httpd_resp_set_hdr(r, h.first.c_str(), h.second.c_str());
  return httpd_resp_send(r, resp.body.data(), static_cast<ssize_t>(resp.body.size()));
}

std::string randomApPassword() {
  static const char kAlpha[] = "abcdefghjkmnpqrstuvwxyz23456789";  // no 0/o/1/l/i
  std::string s;
  for (int i = 0; i < 8; i++) s.push_back(kAlpha[esp_random() % (sizeof kAlpha - 1)]);
  return s;
}

}  // namespace

// ------------------------------------------------------------------ lifecycle
WifiNetService::WifiNetService(NetHooks h) : hooks_(std::move(h)) {}

bool WifiNetService::begin() {
  esp_log_level_set("wifi", ESP_LOG_WARN);  // the driver logs SSIDs at INFO level; keep the console clean
  esp_log_level_set("wifi_init", ESP_LOG_WARN);
  WiFi.persistent(false);  // the Arduino layer must not copy credentials into its own NVS area
  WiFi.mode(WIFI_OFF);
  loadState();
  sec_.reset(new PortalSecurity(rng64));
  ota_.reset(new EspOtaTarget());
  available_ = xTaskCreatePinnedToCore(workerEntry, "net", 8192, this, 1, &task_, 0) == pdPASS;
  MT_LOGF("net: ready (credentials %s, last sync %lld)", ssid_.empty() ? "none" : "stored",
          static_cast<long long>(sync_.lastSyncUtc));
  return available_;
}

void WifiNetService::attachAdmin(app::AdminService* admin) {
  admin_ = admin;
  const esp_app_desc_t* d = esp_ota_get_app_description();
  api_.reset(new PortalApi(*admin, *sec_, ota_.get(), d ? d->project_name : "rxnudge"));
}

void WifiNetService::loadState() {
  Preferences p;
  if (!p.begin(kNs, true)) return;
  ssid_ = p.getString("ssid", "").c_str();
  pass_ = p.getString("pass", "").c_str();
  sync_.lastSyncUtc = p.getLong64("tsLast", 0);
  sync_.lastAttemptUtc = p.getLong64("tsAtt", 0);
  sync_.failures = p.getInt("tsFail", 0);
  sync_.lastCorrectionSec = p.getInt("tsCorr", 0);
  p.end();
  hasCreds_ = !ssid_.empty();
  lastSync_ = sync_.lastSyncUtc;
}

void WifiNetService::saveState() {
  Preferences p;
  if (!p.begin(kNs, false)) return;
  p.putLong64("tsLast", sync_.lastSyncUtc);
  p.putLong64("tsAtt", sync_.lastAttemptUtc);
  p.putInt("tsFail", sync_.failures);
  p.putInt("tsCorr", sync_.lastCorrectionSec);
  p.end();
  lastSync_ = sync_.lastSyncUtc;
}

bool WifiNetService::wifiEnabledSetting() { return admin_ && admin_->database().settings.wifiEnabled; }

void WifiNetService::enableWifiSetting() {
  if (!admin_) return;
  core::Settings s = admin_->database().settings;
  if (s.wifiEnabled) return;
  s.wifiEnabled = true;
  admin_->saveSettings(s);
}

bool WifiNetService::configured() { return hasCreds_.load() && wifiEnabledSetting(); }

bool WifiNetService::busy() const {
  if (radioOn_.load() || jobRunning_.load() || provisioning_.load() || portal_.load()) return true;
  std::lock_guard<std::mutex> g(jobMx_);
  return !jobs_.empty();
}

bool WifiNetService::clockStale() {
  TimeSyncState s;
  s.lastSyncUtc = lastSync_.load();
  return syncTrust(s, hooks_.nowUtc(), configured()) == SyncTrust::Stale;
}

// ------------------------------------------------------------------ worker
void WifiNetService::workerEntry(void* self) { static_cast<WifiNetService*>(self)->worker(); }

void WifiNetService::push(Job j) {
  {
    std::lock_guard<std::mutex> g(jobMx_);
    if (j.kind == JobKind::Notify && !jobs_.empty() && jobs_.back().kind == JobKind::Notify) {
      jobs_.back().missed.insert(jobs_.back().missed.end(), j.missed.begin(), j.missed.end());  // one message per batch
    } else {
      jobs_.push_back(std::move(j));
    }
  }
  if (task_) xTaskNotifyGive(task_);
}

void WifiNetService::worker() {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(provisioning_ ? 10 : 250));
    for (;;) {
      Job j;
      {
        std::lock_guard<std::mutex> g(jobMx_);
        if (jobs_.empty()) break;
        j = std::move(jobs_.front());
        jobs_.pop_front();
        jobRunning_ = true;
      }
      runJob(j);
      jobRunning_ = false;
    }
    maintain();
  }
}

void WifiNetService::maintain() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  const uint32_t now = millis();
  if (provisioning_) {
    g_dns.processNextRequest();
    if (provStopAtMs_ && static_cast<int32_t>(now - provStopAtMs_) >= 0) {
      MT_LOGF("net: hotspot closed after successful setup");
      stopProvisioningLocked();
    } else if (now - provActivityMs_ > kProvIdleMs) {
      MT_LOGF("net: hotspot closed (idle)");
      stopProvisioningLocked();
    }
  }
  if (portal_) {
    uint32_t last;
    {
      std::lock_guard<std::mutex> s(secMx_);
      last = sec_->lastActivityMs();
    }
    if (api_ && api_->rebootRequested()) {
      MT_LOGF("ota: restarting into the new firmware");
      delay(1500);  // let the browser receive the answer
      stopPortalLocked("update installed");
      if (hooks_.restart) hooks_.restart();
      esp_restart();
    }
    if (now - last > kPortalIdleMs) stopPortalLocked("idle");
  }
}

void WifiNetService::runJob(Job& j) {
  std::lock_guard<std::recursive_mutex> g(mx_);
  switch (j.kind) {
    case JobKind::AutoSync: {
      const std::string e = doSync(j.reason, nullptr);
      if (!e.empty()) MT_LOGF("net: time sync (%s) failed: %s", syncReasonName(j.reason), e.c_str());
      break;
    }
    case JobKind::Notify: {
      if (!admin_ || ssid_.empty()) break;
      const core::Settings st = admin_->database().settings;
      if (!st.notifyEnabled || st.notifyUrl.empty() || !st.wifiEnabled) break;
      std::string err;
      if (!ensureSta(&err)) {
        MT_LOGF("net: notification not sent: %s", err.c_str());
        releaseRadio();
        break;
      }
      const OutboundRequest r = buildMissedNotification(st.notifyUrl, st.notifyNames, j.missed, hooks_.tz());
      if (!httpSend(r, &err)) MT_LOGF("net: notification failed: %s", err.c_str());
      else MT_LOGF("net: caregiver notified (%u missed)", static_cast<unsigned>(j.missed.size()));
      releaseRadio();
      break;
    }
    case JobKind::Manual: {
      std::string err;
      int64_t utc = 0;
      if (j.manual == app::NetJob::SyncTime) {
        const std::string e = doSync(SyncReason::Manual, &utc);
        if (!e.empty()) err = "Time sync failed: " + e;
      } else {
        std::string e;
        const bool ok = ensureSta(&e);
        if (ok) MT_LOGF("net: test ok, rssi %d", WiFi.RSSI());
        releaseRadio();
        lastError_ = ok ? "" : "Last test failed: " + e;
        if (!ok) err = "Could not connect: " + e;
      }
      std::lock_guard<std::mutex> g(jobStateMx_);
      jstate_.running = false;
      jstate_.error = err;
      jstate_.utc = utc;
      ++jstate_.seq;
      break;
    }
    case JobKind::TestNewCredentials: {
      provMessage_ = "Testing the connection…";
      std::string err;
      if (ensureSta(&err)) {
        provState_ = ProvState::Ok;
        provMessage_ = "Connected to " + ssid_ + ". You can close this page.";
        MT_LOGF("net: new Wi-Fi network works");
        enableWifiSetting();
        if (admin_ && admin_->database().settings.autoTimeSync) doSync(SyncReason::Manual, nullptr);
        provStopAtMs_ = millis() + 8000;
      } else {
        // put the previous (working) credentials back
        ssid_ = prevSsid_;
        pass_ = prevPass_;
        hasCreds_ = !ssid_.empty();
        Preferences p;
        if (p.begin(kNs, false)) {
          if (ssid_.empty()) {
            p.remove("ssid");
            p.remove("pass");
          } else {
            p.putString("ssid", ssid_.c_str());
            p.putString("pass", pass_.c_str());
          }
          p.end();
        }
        WiFi.disconnect(false);
        provState_ = ProvState::Failed;
        provMessage_ = "Could not connect (" + err + "). Check the password; only 2.4 GHz networks work.";
      }
      break;
    }
  }
}

// ------------------------------------------------------------------ radio
bool WifiNetService::ensureSta(std::string* err) {
  if (ssid_.empty()) {
    if (err) *err = "Wi-Fi is not set up";
    return false;
  }
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == String(ssid_.c_str())) return true;
  radioOn_ = true;
  WiFi.persistent(false);
  WiFi.mode(provisioning_ ? WIFI_AP_STA : WIFI_STA);
  WiFi.setHostname(kHostname);
  WiFi.begin(ssid_.c_str(), pass_.empty() ? nullptr : pass_.c_str());
  const uint32_t t0 = millis();
  wl_status_t st = WiFi.status();
  while (st != WL_CONNECTED && millis() - t0 < kConnectTimeoutMs) {
    if (st == WL_CONNECT_FAILED) break;
    delay(100);
    st = WiFi.status();
  }
  if (st == WL_CONNECTED) {
    MT_LOGF("net: connected in %lu ms, rssi %d", static_cast<unsigned long>(millis() - t0), WiFi.RSSI());
    return true;
  }
  if (err) *err = st == WL_NO_SSID_AVAIL ? "network not found" : st == WL_CONNECT_FAILED ? "wrong password?" : "timed out";
  return false;
}

void WifiNetService::releaseRadio() {
  if (provisioning_ || portal_) return;  // still needed
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  radioOn_ = false;
}

bool WifiNetService::ntpQuery(int64_t* utc, std::string* err) {
  static const char* const kServers[] = {"pool.ntp.org", "time.cloudflare.com", "time.google.com"};
  WiFiUDP udp;
  if (!udp.begin(12123)) {
    if (err) *err = "no UDP socket";
    return false;
  }
  std::string last = "no answer from time servers";
  for (const char* host : kServers) {
    IPAddress ip;
    if (!WiFi.hostByName(host, ip)) {
      last = "DNS lookup failed";
      continue;
    }
    uint8_t pkt[kNtpPacketSize];
    buildNtpRequest(pkt);
    while (udp.parsePacket() > 0) udp.flush();
    const uint32_t t0 = millis();
    udp.beginPacket(ip, 123);
    udp.write(pkt, sizeof pkt);
    udp.endPacket();
    while (millis() - t0 < 2000) {
      if (udp.parsePacket() >= static_cast<int>(kNtpPacketSize)) {
        udp.read(pkt, sizeof pkt);
        int64_t sec;
        uint32_t ms;
        if (parseNtpResponse(pkt, sizeof pkt, &sec, &ms, &last)) {
          const uint32_t rtt = millis() - t0;
          *utc = sec + (static_cast<int64_t>(ms) + rtt / 2 + 500) / 1000;
          udp.stop();
          return true;
        }
        break;
      }
      delay(10);
    }
  }
  udp.stop();
  if (err) *err = last;
  return false;
}

std::string WifiNetService::doSync(SyncReason r, int64_t* utcOut) {
  const int64_t before = hooks_.nowUtc();
  std::string err;
  int64_t utc = 0;
  const bool ok = ensureSta(&err) && ntpQuery(&utc, &err);
  if (ok && !hooks_.setClockUtc(utc)) err = "clock chip did not accept the time";
  if (ok && err.empty()) {
    recordSyncSuccess(sync_, utc, before);
    MT_LOGF("net: time synced (%s), correction %ld s", syncReasonName(r), static_cast<long>(sync_.lastCorrectionSec));
    if (utcOut) *utcOut = utc;
  } else {
    recordSyncFailure(sync_, before);
  }
  saveState();
  releaseRadio();
  return ok && err.empty() ? "" : err;
}

bool WifiNetService::httpSend(const OutboundRequest& r, std::string* err) {
  esp_http_client_config_t c = {};
  c.url = r.url.c_str();
  c.method = HTTP_METHOD_POST;
  c.timeout_ms = 10000;
  c.crt_bundle_attach = esp_crt_bundle_attach;  // https with the built-in CA bundle
  esp_http_client_handle_t h = esp_http_client_init(&c);
  if (!h) {
    if (err) *err = "bad URL";
    return false;
  }
  esp_http_client_set_header(h, "Content-Type", r.contentType.c_str());
  for (const auto& kv : r.headers) esp_http_client_set_header(h, kv.first.c_str(), kv.second.c_str());
  esp_http_client_set_post_field(h, r.body.data(), static_cast<int>(r.body.size()));
  const esp_err_t e = esp_http_client_perform(h);
  const int code = esp_http_client_get_status_code(h);
  esp_http_client_cleanup(h);
  if (e != ESP_OK) {
    if (err) *err = esp_err_to_name(e);
    return false;
  }
  if (code < 200 || code > 299) {
    if (err) *err = "server answered " + std::to_string(code);
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ INetService
app::NetStatus WifiNetService::status() {
  std::unique_lock<std::recursive_mutex> g(mx_, std::try_to_lock);
  if (!g.owns_lock()) {  // a radio job is running: never block the UI for it
    std::lock_guard<std::mutex> c(cacheMx_);
    app::NetStatus s = cache_;
    s.busy = true;
    if (!provisioning_) s.text = "Working…";
    return s;
  }
  app::NetStatus s;
  s.available = available_;
  s.configured = !ssid_.empty();
  s.busy = busy();
  s.provisioning = provisioning_;
  s.portal = portal_;
  s.ssid = ssid_;
  s.lastSyncUtc = sync_.lastSyncUtc;
  if (provisioning_) {
    s.apSsid = apSsid_;
    s.apPass = apPass_;
    s.portalUrl = "http://192.168.4.1";
    s.text = provState_ == ProvState::Idle ? "Hotspot on: join it with your phone" : provMessage_;
  } else if (portal_) {
    {
      std::lock_guard<std::mutex> sg(secMx_);
      s.portalCode = sec_->code();
      const uint32_t idle = millis() - sec_->lastActivityMs();
      s.portalMinutesLeft = idle >= kPortalIdleMs ? 0 : static_cast<int>((kPortalIdleMs - idle + 59999) / 60000);
    }
    s.portalUrl = std::string("http://") + kHostname + ".local";
    s.portalIp = portalIp_;
    s.text = "Web portal running on " + ssid_;
  } else if (!available_) {
    s.text = "Wi-Fi hardware not available";
  } else if (ssid_.empty()) {
    s.text = "Not set up";
  } else {
    s.text = lastError_.empty() ? "Saved: " + ssid_ + " (radio off)" : lastError_;
  }
  {
    std::lock_guard<std::mutex> c(cacheMx_);
    cache_ = s;
  }
  return s;
}

std::string WifiNetService::saveCredentials(const std::string& ssid, const std::string& pass) {
  const std::string e = validateCredentials(ssid, pass);
  if (!e.empty()) return e;
  std::lock_guard<std::recursive_mutex> g(mx_);
  Preferences p;
  if (!p.begin(kNs, false)) return "Could not save (NVS)";
  const bool ok = p.putString("ssid", ssid.c_str()) > 0 && (pass.empty() ? (p.remove("pass"), true) : p.putString("pass", pass.c_str()) > 0);
  p.end();
  if (!ok) return "Could not save (NVS)";
  ssid_ = ssid;
  pass_ = pass;
  hasCreds_ = true;
  lastError_.clear();
  MT_LOGF("net: credentials saved");  // never the values
  return "";
}

std::string WifiNetService::forgetCredentials() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  if (portal_) stopPortalLocked("credentials removed");
  Preferences p;
  if (p.begin(kNs, false)) {
    p.remove("ssid");
    p.remove("pass");
    p.end();
  }
  ssid_.clear();
  pass_.clear();
  hasCreds_ = false;
  lastError_.clear();
  releaseRadio();
  MT_LOGF("net: credentials removed");
  return "";
}

std::string WifiNetService::startJob(app::NetJob job) {
  if (!available_) return app::RadioOffNetService::kMsg;
  if (!hasCreds_) return "Set up Wi-Fi first";
  {
    std::lock_guard<std::mutex> g(jobStateMx_);
    if (jstate_.running) return "Already working";
    jstate_.kind = job;
    jstate_.running = true;
    jstate_.error.clear();
    jobStartMs_ = millis();
  }
  Job j;
  j.kind = JobKind::Manual;
  j.manual = job;
  push(std::move(j));
  return "";
}

app::NetJobState WifiNetService::jobState() {
  std::lock_guard<std::mutex> g(jobStateMx_);
  app::NetJobState s = jstate_;
  if (s.running) s.elapsedSec = static_cast<int>((millis() - jobStartMs_) / 1000);
  return s;
}

void WifiNetService::autoSync(SyncReason r) {
  std::lock_guard<std::recursive_mutex> g(mx_);
  const bool auto_ = admin_ && admin_->database().settings.autoTimeSync;
  if (!available_ || !syncDue(sync_, r, hooks_.nowUtc(), !ssid_.empty() && wifiEnabledSetting(), auto_)) return;
  Job j;
  j.kind = JobKind::AutoSync;
  j.reason = r;
  push(std::move(j));
}

void WifiNetService::notifyMissed(const std::vector<MissedDose>& m) {
  if (!available_ || m.empty() || !admin_) return;
  const core::Settings st = admin_->database().settings;
  if (!st.notifyEnabled || st.notifyUrl.empty() || !st.wifiEnabled) return;
  Job j;
  j.kind = JobKind::Notify;
  j.missed = m;
  push(std::move(j));
}

std::string WifiNetService::sendTestNotification() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  if (!available_) return app::RadioOffNetService::kMsg;
  if (!admin_) return "Not ready";
  const core::Settings st = admin_->database().settings;
  if (st.notifyUrl.empty()) return "Enter the URL first";
  std::string err;
  if (!ensureSta(&err)) {
    releaseRadio();
    return "Could not connect: " + err;
  }
  const bool ok = httpSend(buildTestNotification(st.notifyUrl), &err);
  releaseRadio();
  return ok ? "" : "Not delivered: " + err;
}

// ------------------------------------------------------------------ provisioning (hotspot)
void WifiNetService::scanNetworks() {
  networks_.clear();
  const int n = WiFi.scanNetworks(false, true);
  for (int i = 0; i < n; i++) {
    const std::string s = WiFi.SSID(i).c_str();
    if (s.empty()) continue;
    auto it = std::find_if(networks_.begin(), networks_.end(), [&](const ScanEntry& e) { return e.ssid == s; });
    if (it != networks_.end()) {
      it->rssi = std::max(it->rssi, static_cast<int>(WiFi.RSSI(i)));
      continue;
    }
    networks_.push_back({s, static_cast<int>(WiFi.RSSI(i)), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(networks_.begin(), networks_.end(), [](const ScanEntry& a, const ScanEntry& b) { return a.rssi > b.rssi; });
  if (networks_.size() > 20) networks_.resize(20);
}

std::string WifiNetService::startProvisioning() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  if (!available_) return app::RadioOffNetService::kMsg;
  if (provisioning_) return "";
  if (portal_) stopPortalLocked("hotspot started");
  radioOn_ = true;
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false);
  scanNetworks();
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[24];
  snprintf(name, sizeof name, "StickyMed-%02X%02X", mac[4], mac[5]);
  apSsid_ = name;
  apPass_ = randomApPassword();
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAPConfig(kApIp, kApIp, IPAddress(255, 255, 255, 0));
  if (!WiFi.softAP(apSsid_.c_str(), apPass_.c_str(), 1, 0, 2)) {
    releaseRadio();
    return "Could not start the hotspot";
  }
  g_dns.setErrorReplyCode(DNSReplyCode::NoError);
  g_dns.start(53, "*", kApIp);  // every name resolves to us: phones open the setup page
  provisioning_ = true;
  provState_ = ProvState::Idle;
  provMessage_.clear();
  provActivityMs_ = millis();
  provStopAtMs_ = 0;
  if (!startCaptiveServer()) {
    stopProvisioningLocked();
    return "Could not start the setup page";
  }
  MT_LOGF("net: hotspot %s on, %u networks found", apSsid_.c_str(), static_cast<unsigned>(networks_.size()));
  if (task_) xTaskNotifyGive(task_);
  return "";
}

std::string WifiNetService::stopProvisioning() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  stopProvisioningLocked();
  return "";
}

void WifiNetService::stopProvisioningLocked() {
  if (!provisioning_) return;
  if (captive_) httpd_stop(static_cast<httpd_handle_t>(captive_));
  captive_ = nullptr;
  g_dns.stop();
  WiFi.softAPdisconnect(true);
  provisioning_ = false;
  provStopAtMs_ = 0;
  apPass_.clear();
  releaseRadio();
}

bool WifiNetService::startCaptiveServer() {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80;
  c.ctrl_port = 32769;
  c.stack_size = 6144;
  c.max_uri_handlers = 6;
  c.max_open_sockets = 4;
  c.lru_purge_enable = true;
  c.uri_match_fn = httpd_uri_match_wildcard;
  httpd_handle_t h = nullptr;
  if (httpd_start(&h, &c) != ESP_OK) return false;
  for (httpd_method_t m : {HTTP_GET, HTTP_POST}) {
    httpd_uri_t u = {};
    u.uri = "/*";
    u.method = m;
    u.handler = [](httpd_req_t* r) -> esp_err_t { return WifiNetService::captiveHandler(r); };
    u.user_ctx = this;
    httpd_register_uri_handler(h, &u);
  }
  captive_ = h;
  return true;
}

int WifiNetService::captiveHandler(void* rv) {
  httpd_req_t* r = static_cast<httpd_req_t*>(rv);
  WifiNetService* self = static_cast<WifiNetService*>(r->user_ctx);
  std::string path = r->uri;
  path = path.substr(0, path.find('?'));
  self->provActivityMs_ = millis();
  HttpResponse resp;
  if (r->method == HTTP_GET && (path == "/" || path == "/setup.html")) {
    const WebAsset* a = findWebAsset("/setup.html");
    resp.contentType = a->contentType;
    resp.body.assign(reinterpret_cast<const char*>(a->data), a->size);
    resp.gzip = true;
    resp.headers.push_back({"Cache-Control", "no-store"});
  } else if (r->method == HTTP_GET && path == "/api/networks") {
    std::lock_guard<std::recursive_mutex> g(self->mx_);
    core::JsonValue arr = core::JsonValue::array();
    for (const auto& n : self->networks_) {
      core::JsonValue o = core::JsonValue::object();
      o.set("ssid", core::JsonValue::string(n.ssid));
      o.set("rssi", core::JsonValue::integer(n.rssi));
      o.set("secure", core::JsonValue::boolean(n.secure));
      arr.push(o);
    }
    core::JsonValue j = core::JsonValue::object();
    j.set("networks", arr);
    resp = jsonResponse(200, j);
  } else if (r->method == HTTP_POST && path == "/api/scan") {
    std::lock_guard<std::recursive_mutex> g(self->mx_);
    self->scanNetworks();
    resp = jsonResponse(200, core::JsonValue::object());
  } else if (r->method == HTTP_GET && path == "/api/status") {
    core::JsonValue j = core::JsonValue::object();
    static const char* const kStates[] = {"idle", "testing", "ok", "failed"};
    std::lock_guard<std::recursive_mutex> g(self->mx_);
    j.set("state", core::JsonValue::string(kStates[static_cast<int>(self->provState_.load())]));
    j.set("message", core::JsonValue::string(self->provMessage_));
    resp = jsonResponse(200, j);
  } else if (r->method == HTTP_POST && path == "/api/wifi") {
    std::string body;
    if (!readBody(r, 512, body)) return ESP_OK;
    core::JsonValue j;
    std::string err;
    if (!parseBody(body, 512, j, &err) || !j["ssid"].isString() || (j.has("pass") && !j["pass"].isString())) {
      resp = errorResponse(400, "send ssid and pass");
    } else {
      const std::string ssid = j["ssid"].asString(), pass = j["pass"].asString();
      err = validateCredentials(ssid, pass);
      if (!err.empty()) {
        resp = errorResponse(422, err);
      } else if (self->provState_ == ProvState::Testing) {
        resp = errorResponse(409, "a test is already running");
      } else {
        std::lock_guard<std::recursive_mutex> g(self->mx_);
        self->prevSsid_ = self->ssid_;
        self->prevPass_ = self->pass_;
        err = self->saveCredentials(ssid, pass);
        if (!err.empty()) {
          resp = errorResponse(500, err);
        } else {
          self->provState_ = ProvState::Testing;
          self->provMessage_ = "Testing the connection…";
          Job job;
          job.kind = JobKind::TestNewCredentials;
          self->push(std::move(job));
          resp = jsonResponse(200, core::JsonValue::object());
        }
      }
    }
    // the body held the password: wipe our copy
    std::fill(body.begin(), body.end(), '\0');
  } else if (r->method == HTTP_GET) {
    // OS captive-portal probes (generate_204, hotspot-detect.html, ncsi.txt, ...) land here
    resp.status = 302;
    resp.contentType = "text/plain";
    resp.headers.push_back({"Location", "http://192.168.4.1/"});
  } else {
    resp = errorResponse(404, "not found");
  }
  return sendResponse(r, resp);
}

// ------------------------------------------------------------------ LAN portal
std::string WifiNetService::startPortal() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  if (!available_) return app::RadioOffNetService::kMsg;
  if (portal_) return "";
  if (!api_) return "Not ready";
  if (ssid_.empty()) return "Set up Wi-Fi first";
  if (!wifiEnabledSetting()) return "Wi-Fi is switched off in Settings";
  if (provisioning_) stopProvisioningLocked();
  std::string err;
  if (!ensureSta(&err)) {
    releaseRadio();
    return "Could not connect: " + err;
  }
  WiFi.setSleep(false);  // responsive web pages; the portal is short-lived
  portalIp_ = WiFi.localIP().toString().c_str();
  if (MDNS.begin(kHostname)) MDNS.addService("http", "tcp", 80);
  {
    std::lock_guard<std::mutex> s(secMx_);
    sec_->setDeviceIp(portalIp_);
    sec_->start(millis());
  }
  portal_ = true;
  if (!startPortalServer()) {
    stopPortalLocked("server failed");
    return "Could not start the web server";
  }
  MT_LOGF("net: web portal on http://%s.local (%s)", kHostname, portalIp_.c_str());
  return "";
}

std::string WifiNetService::stopPortal() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  stopPortalLocked("stopped");
  return "";
}

void WifiNetService::stopPortalLocked(const char* why) {
  if (!portal_) return;
  if (portalHttpd_) httpd_stop(static_cast<httpd_handle_t>(portalHttpd_));  // waits for the server task
  portalHttpd_ = nullptr;
  {
    std::lock_guard<std::mutex> s(secMx_);
    if (api_) api_->otaAbort();
    sec_->stop();
  }
  MDNS.end();
  portal_ = false;
  releaseRadio();
  MT_LOGF("net: web portal off (%s)", why);
}

bool WifiNetService::startPortalServer() {
  httpd_config_t c = HTTPD_DEFAULT_CONFIG();
  c.server_port = 80;
  c.ctrl_port = 32768;
  c.stack_size = 12288;  // JSON + admin service + restore
  c.max_uri_handlers = 8;
  c.max_open_sockets = 5;
  c.max_resp_headers = 12;
  c.lru_purge_enable = true;
  c.recv_wait_timeout = 10;
  c.send_wait_timeout = 10;
  c.uri_match_fn = httpd_uri_match_wildcard;
  httpd_handle_t h = nullptr;
  if (httpd_start(&h, &c) != ESP_OK) return false;
  httpd_uri_t ota = {};
  ota.uri = "/api/ota";
  ota.method = HTTP_POST;
  ota.handler = [](httpd_req_t* r) -> esp_err_t { return WifiNetService::otaHandler(r); };
  ota.user_ctx = this;
  httpd_register_uri_handler(h, &ota);  // registered first: matched before the generic POST
  for (httpd_method_t m : {HTTP_GET, HTTP_POST, HTTP_PUT, HTTP_DELETE}) {
    httpd_uri_t u = {};
    u.uri = "/*";
    u.method = m;
    u.handler = [](httpd_req_t* r) -> esp_err_t { return WifiNetService::portalHandler(r); };
    u.user_ctx = this;
    httpd_register_uri_handler(h, &u);
  }
  portalHttpd_ = h;
  return true;
}

int WifiNetService::portalHandler(void* rv) {
  httpd_req_t* r = static_cast<httpd_req_t*>(rv);
  WifiNetService* self = static_cast<WifiNetService*>(r->user_ctx);
  HttpRequest q = buildRequest(r);
  const size_t limit = q.path == "/api/restore" ? kMaxRestoreBody : kMaxJsonBody;
  if (!readBody(r, limit, q.body)) return ESP_OK;
  HttpResponse resp;
  {
    std::lock_guard<std::mutex> g(self->secMx_);
    resp = self->api_->handle(q, millis());
  }
  return sendResponse(r, resp);
}

int WifiNetService::otaHandler(void* rv) {
  httpd_req_t* r = static_cast<httpd_req_t*>(rv);
  WifiNetService* self = static_cast<WifiNetService*>(r->user_ctx);
  HttpRequest q = buildRequest(r);
  bool ok = false;
  HttpResponse resp;
  {
    std::lock_guard<std::mutex> g(self->secMx_);
    resp = self->api_->otaBegin(q, r->content_len, millis(), &ok);
  }
  if (!ok) {
    httpd_resp_set_hdr(r, "Connection", "close");  // the unread body is dropped with the socket
    return sendResponse(r, resp);
  }
  MT_LOGF("ota: upload started (%u bytes)", static_cast<unsigned>(r->content_len));
  std::unique_ptr<uint8_t[]> buf(new uint8_t[4096]);
  size_t left = r->content_len;
  int timeouts = 0;
  std::string err;
  while (left > 0) {
    const int n = httpd_req_recv(r, reinterpret_cast<char*>(buf.get()), std::min<size_t>(left, 4096));
    if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 4) continue;
    if (n <= 0) {
      err = "connection lost";
      break;
    }
    timeouts = 0;
    if (!self->api_->otaChunk(buf.get(), static_cast<size_t>(n), &err)) break;
    left -= static_cast<size_t>(n);
    std::lock_guard<std::mutex> g(self->secMx_);
    self->sec_->touch(millis());  // a long upload is activity
  }
  if (left > 0) {
    std::lock_guard<std::mutex> g(self->secMx_);
    self->api_->otaAbort();
    MT_LOGF("ota: upload failed: %s", err.c_str());
    httpd_resp_set_hdr(r, "Connection", "close");
    return sendResponse(r, errorResponse(422, err));
  }
  {
    std::lock_guard<std::mutex> g(self->secMx_);
    resp = self->api_->otaFinish(millis());
  }
  MT_LOGF("ota: upload %s", resp.status == 200 ? "verified" : "rejected");
  esp_err_t e = sendResponse(r, resp);
  if (self->task_) xTaskNotifyGive(self->task_);
  return e;
}

// ------------------------------------------------------------------ sleep hooks
void WifiNetService::stopInteractive() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  stopPortalLocked("sleep requested");
  stopProvisioningLocked();
}

void WifiNetService::shutdownForSleep() {
  std::lock_guard<std::recursive_mutex> g(mx_);
  stopPortalLocked("sleep");
  stopProvisioningLocked();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  radioOn_ = false;
}

// ------------------------------------------------------------------ rollback
void otaConfirmBoot(bool healthy) {
  const esp_partition_t* run = esp_ota_get_running_partition();
  esp_ota_img_states_t st;
  if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK || st != ESP_OTA_IMG_PENDING_VERIFY) return;
  if (healthy) {
    esp_ota_mark_app_valid_cancel_rollback();
    MT_LOGF("ota: new firmware on %s confirmed", run->label);
  } else {
    MT_LOGF("ota: new firmware on %s failed its start checks, rolling back", run->label);
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}

const char* otaRunningPartition() {
  const esp_partition_t* run = esp_ota_get_running_partition();
  return run ? run->label : "?";
}

}  // namespace net
