#pragma once
// IClock backed by the ESP32 system time (survives deep sleep via the RTC timer).
// Phase 3 sets the system time from the PCF8563 at boot; until then it starts at the epoch.
#include <ctime>

#include "core/clock.h"

namespace app {

class SystemClock : public core::IClock {
 public:
  int64_t nowUtc() const override { return static_cast<int64_t>(time(nullptr)); }
};

}  // namespace app
