#pragma once
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>

#include "board/board_config.h"

namespace board {

// Font ids registered with the renderer.
constexpr int FONT_TITLE = 1;
constexpr int FONT_BODY = 2;
constexpr int FONT_SMALL = 3;

HalDisplay& display();
GfxRenderer& gfx();
HalGPIO& io();

// Full power-up sequence. The order follows the reference (main.cpp setup()):
//   0. release deep-sleep GPIO holds while keeping PWR_HOLD/LOCK HIGH (Seeed board_init)
//   1. setCpuFrequencyMhz(80)
//   2. HalGPIO::begin(): PWR_HOLD, PWR_LOCK, EN_BAT_CHG low, EPD/SD/TOUCH power,
//      buttons, GT911, SPI bus
//   3. HalDisplay::begin(): SSD1677 reset and init
//   4. fading fix, fonts, landscape orientation
//   5. RxNudge additions: external-power/SD-detect inputs
//   6. the first refresh must be FULL (done by the caller through firstFullRefresh())
void begin();

// One FULL refresh after power-on is required before FAST refresh works (reference quirk).
void firstFullRefresh();

bool externalPowerPresent();  // GPIO9, HIGH = present [SEEED]
int chargeStateRaw();         // GPIO40 raw level (ASSUMPTION, diagnostic only)
bool sdCardDetected();        // GPIO11, LOW = present [SEEED]

}  // namespace board
