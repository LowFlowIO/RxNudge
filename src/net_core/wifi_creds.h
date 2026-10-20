#pragma once
// Wi-Fi credential rules shared by the device screens, the hotspot page and tests.
#include <string>

namespace net {

// "" when acceptable. An empty password means an open network.
inline std::string validateCredentials(const std::string& ssid, const std::string& pass) {
  if (ssid.empty() || ssid.size() > 32) return "Network name: 1 to 32 bytes";
  if (!pass.empty() && (pass.size() < 8 || pass.size() > 63)) return "Password: 8 to 63 characters";
  for (char c : pass)
    if (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) return "Password contains a control character";
  return "";
}

}  // namespace net
