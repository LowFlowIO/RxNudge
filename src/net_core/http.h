#pragma once
// Transport-independent HTTP request/response used by the portal API, so the
// complete API (auth, CSRF, validation, JSON) runs in host tests unchanged.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "net_core/portal_security.h"

namespace net {

struct HttpRequest {
  std::string path;   // without query
  std::string query;  // after '?', undecoded
  std::string body;
  std::string sha256; // X-Firmware-SHA256 (OTA)
  RequestMeta meta;   // method, ip, host, cookie, csrf, origin
};

struct HttpResponse {
  int status = 200;
  std::string contentType = "application/json; charset=utf-8";
  std::string body;
  bool gzip = false;  // body is gzip-compressed (Content-Encoding: gzip)
  std::vector<std::pair<std::string, std::string>> headers;
  const std::string* header(const std::string& name) const {
    for (const auto& h : headers)
      if (h.first == name) return &h.second;
    return nullptr;
  }
};

std::string queryParam(const std::string& query, const std::string& key);  // URL-decoded, "" if missing
std::string urlDecode(const std::string& s);
const char* statusText(int status);

}  // namespace net
