#pragma once
// The LAN web portal: static assets + JSON API. Pure C++; the device adapter
// (src/net/web_portal.cpp) only moves bytes between esp_http_server and this.
#include <memory>
#include <string>

#include "app_core/admin_service.h"
#include "core/json.h"
#include "net_core/http.h"
#include "net_core/ota_verify.h"
#include "net_core/portal_security.h"

namespace net {

class PortalApi {
 public:
  PortalApi(app::AdminService& admin, PortalSecurity& sec, IOtaTarget* ota, std::string project);

  HttpResponse handle(const HttpRequest& r, uint32_t nowMs);

  // Streaming OTA upload (POST /api/ota): begin checks session + CSRF + SHA header.
  HttpResponse otaBegin(const HttpRequest& r, size_t size, uint32_t nowMs, bool* ok);
  bool otaChunk(const uint8_t* p, size_t n, std::string* err);
  HttpResponse otaFinish(uint32_t nowMs);
  void otaAbort();
  bool rebootRequested() const { return reboot_; }

 private:
  HttpResponse route(const HttpRequest& r, uint32_t nowMs);
  HttpResponse asset(const std::string& path);
  HttpResponse login(const HttpRequest& r, uint32_t nowMs);
  HttpResponse state();
  HttpResponse saveProfile(const HttpRequest& r, int id);
  HttpResponse deleteProfile(int id);
  HttpResponse saveMed(const HttpRequest& r, int id);
  HttpResponse deleteMed(int id);
  HttpResponse archiveMed(const HttpRequest& r, int id);
  HttpResponse saveSchedule(const HttpRequest& r, int id);
  HttpResponse history(const HttpRequest& r);
  HttpResponse exportData(const HttpRequest& r, bool csv);
  HttpResponse backup();
  HttpResponse restore(const HttpRequest& r);
  HttpResponse notifyGet();
  HttpResponse notifyPut(const HttpRequest& r);

  app::AdminService& admin_;
  PortalSecurity& sec_;
  IOtaTarget* ota_;
  std::unique_ptr<OtaSession> otaSession_;
  std::string project_;
  bool reboot_ = false;
};

HttpResponse jsonResponse(int status, const core::JsonValue& j);
HttpResponse errorResponse(int status, const std::string& message);

}  // namespace net
