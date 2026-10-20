#pragma once
// Full-screen RxNudge logo shown while the device is off after a power-button
// hold. One 800x480 SSD1677 frame (bit 1 = white), stored in flash.
#include <cstdint>

namespace ui {

constexpr int kSleepLogoBytes = 800 / 8 * 480;
extern const uint8_t kSleepLogoFrame[kSleepLogoBytes];

}  // namespace ui
