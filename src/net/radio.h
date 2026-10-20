#pragma once

namespace net {

// Boot state: Wi-Fi off (Bluetooth is compiled out in sdkconfig.defaults).
// WifiNetService turns the radio on only for a job and off again afterwards.
void radiosOff();

}  // namespace net
