#include "net/radio.h"

#include <WiFi.h>

namespace net {

void radiosOff() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_OFF);
}

}  // namespace net
