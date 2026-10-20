#pragma once
// Host stand-in for net::WifiNetService: records calls and behaves like the
// device service (credentials kept outside the database, NTP writes the clock).
#include <functional>
#include <string>

#include "app_core/net_service.h"

struct FakeNetService : public app::INetService {
  std::string ssid, pass;
  bool provisioning = false, portal = false;
  bool connectOk = true;
  int64_t ntpUtc = 0;  // time a SyncTime job writes (0 = NTP failure)
  int64_t lastSync = 0;
  int tests = 0, notifications = 0;
  std::function<bool(int64_t)> writeClock;
  app::NetStatus status() override {
    app::NetStatus s;
    s.available = true;
    s.configured = !ssid.empty();
    s.ssid = ssid;
    s.provisioning = provisioning;
    s.portal = portal;
    s.busy = provisioning || portal;
    s.text = s.configured ? "Saved: " + ssid + " (radio off)" : "Not set up";
    s.apSsid = "StickyMed-1A2B";
    s.apPass = "k7m2p9qa";
    s.portalUrl = "http://rx-nudge.local";
    s.portalIp = "192.168.1.57";
    s.portalCode = "482913";
    s.portalMinutesLeft = 10;
    s.lastSyncUtc = lastSync;
    return s;
  }
  std::string saveCredentials(const std::string& s, const std::string& p) override {
    if (s.empty() || s.size() > 32) return "Network name: 1 to 32 bytes";
    if (!p.empty() && (p.size() < 8 || p.size() > 63)) return "Password: 8 to 63 characters";
    ssid = s;
    pass = p;
    return "";
  }
  std::string forgetCredentials() override {
    ssid.clear();
    pass.clear();
    return "";
  }
  // Jobs finish after `jobPolls` calls to jobState() (simulates the background worker).
  int jobPolls = 1;
  int pollsLeft = 0;
  app::NetJobState job;
  std::string startJob(app::NetJob kind) override {
    if (job.running) return "Already working";
    if (ssid.empty()) return "Set up Wi-Fi first";
    job.kind = kind;
    job.running = true;
    job.elapsedSec = 0;
    pollsLeft = jobPolls;
    return "";
  }
  app::NetJobState jobState() override {
    if (job.running && pollsLeft-- <= 0) {
      job.running = false;
      job.error.clear();
      if (job.kind == app::NetJob::TestConnection) {
        ++tests;
        if (!connectOk) job.error = "Could not connect";
      } else if (!ntpUtc) {
        job.error = "No answer from the time server";
      } else if (writeClock && !writeClock(ntpUtc)) {
        job.error = "Could not write the clock";
      } else {
        lastSync = ntpUtc;
        job.utc = ntpUtc;
      }
      ++job.seq;
    } else if (job.running) {
      ++job.elapsedSec;
    }
    return job;
  }
  std::string startProvisioning() override {
    provisioning = true;
    return "";
  }
  std::string stopProvisioning() override {
    provisioning = false;
    return "";
  }
  std::string startPortal() override {
    if (ssid.empty()) return "Set up Wi-Fi first";
    portal = true;
    return "";
  }
  std::string stopPortal() override {
    portal = false;
    return "";
  }
  std::string sendTestNotification() override {
    ++notifications;
    return "";
  }
};
