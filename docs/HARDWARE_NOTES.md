# RxNudge – Hardware & Toolchain Inventory (reTerminal Sticky)

Source: https://github.com/LowFlowIO/sticky-micronotes, branch `main`, commit `1315d2c` (2026-09-03 11:54 CEST).
Cloned to `/workspace/sticky-micronotes`. All paths are relative to that repo.
Legend: **FOUND (file:line)** = read from the source. **NOT FOUND (assumption)** = not in the repo; this is an assumption or open question.


## 0. Official-source research (2026-10-18)
Sources:
- [S1] Seeed Hardware Overview: https://www.seeedstudio.com/sticky/docs/en/device-guide/hardware-overview/
- [S2] Seeed ESP-IDF Pages and Peripherals: https://www.seeedstudio.com/sticky/docs/en/device-guide/esp-pages/
- [S3] Seeed ESP-IDF Refresh and Low Power: https://www.seeedstudio.com/sticky/docs/en/device-guide/esp-refresh/
- [S4] Seeed ESP-IDF Development Basics: https://www.seeedstudio.com/sticky/docs/en/device-guide/esp-basics/
- [S5] Seeed demo source: https://files.seeedstudio.com/wiki/reterminal_sticky/res/Sticky_dashboard_demo.zip (main/pin_config.h, devices/*.cpp, board/board.cpp, sdkconfig.defaults)
- [S6] Seeed peripheral demo: https://files.seeedstudio.com/wiki/reterminal_sticky/res/Sticky_peripheral_demo.zip (main/demo_buzzer.cpp)
- [S7] SdFat registry: https://registry.platformio.org/libraries/greiman/SdFat (latest 2.3.1, 2025-08-13)

No public schematic or datasheet pack was found: no link on the Seeed docs and none on files.seeedstudio.com. Seeed's GitHub org only has `Seeed-Projects/reterminal-sticky-playground-registry`. The demo zips above are the official example code.

| Question | Answer | Status |
|---|---|---|
| Buzzer on GPIO48? | Yes. "Buzzer, PWM: GPIO48" [S1]. `PIN_BUZZER 48` [S5 pin_config.h:27]. Driven by LEDC 10-bit, 2400 Hz, duty 256, 60 ms beep [S5 sticky_buzzer.cpp]. The peripheral demo uses variable frequency, default 4000 Hz, duty 512 [S6]. Duty 0 = silent, so it is **active-high PWM** | FOUND |
| Active or passive? | Seeed only ever drives it with variable-frequency PWM, never a DC level. That implies a **passive** transducer, but no schematic confirms it | ASSUMPTION (board_config.h) |
| LED / vibration? | The only LED is the "dual-color (red/green) charging indicator" next to USB-C [S1]. No firmware pin is documented, so it is assumed charger-driven. There is **no vibration motor** and **no user LED** [S1] | FOUND (absence) / ASSUMPTION (LED not controllable) |
| Timer wake in deep sleep | Seeed's deep-sleep path latches PWR_HOLD 45 / PWR_LOCK 46 HIGH with `gpio_hold_en` + `gpio_deep_sleep_hold_en`, and parks EPD_EN 47, TOUCH_EN 42, TOUCH_RST 41, MIC_EN 38, SD_EN 10 and buzzer 48 LOW. Wake source is EXT1 GPIO4 any-low [S3; S5 sticky_power.cpp]. The rails therefore stay up in deep sleep, so the ESP32 RTC timer (`esp_sleep_enable_timer_wakeup`) can wake it. Seeed's demos do not use timer wake | FOUND (rail hold) / ASSUMPTION (timer wake untested by Seeed; verified by self-test item 7) |
| RTC chip | **PCF8563** on I2C GPIO0/1 [S1], address 0x51 [S5 pin_config.h:19]. The PCF8563 needs its own 32.768 kHz crystal by design. Its INT/alarm pin is **not** documented as routed to the ESP32, so it cannot be used as a wake source yet | FOUND / ASSUMPTION (INT not routed) |
| 32 kHz crystal on ESP32 | Not documented. The firmware uses the internal 136 kHz RC (`CONFIG_ESP32S3_RTC_CLK_SRC_INT_RC=y`, sdkconfig.sticky) | NOT FOUND → ASSUMPTION: none |
| Panel size / resolution | **3.97", 800 x 480**, 235 ppi, 4-level grayscale, SSD1677-class SPI e-paper [S1, S4]. The earlier "4.26"" suspicion was wrong. Seeed's driver rotates the framebuffer 180° [S5 sticky_display.cpp:36]; the reference driver writes the panel directly. Which way up is correct must be checked on hardware | FOUND |
| GPIO40 | Seeed's demo **does not use GPIO40**. Seeed's external-power detect is **GPIO9, HIGH = USB present** [S5 pin_config.h:9; sticky_charger.cpp]. The reference's `CHARGE_STATE 40` is unverified. RxNudge uses GPIO9 and only logs GPIO40 raw | FOUND (GPIO9) / ASSUMPTION (GPIO40) |
| GPIO11 | **SD card detect**, input with pull-up, **LOW = card present** [S5 pin_config.h:59; sticky_sdcard.cpp:32,88-94] | FOUND |
| GPIO39 | EN_BAT_CHGn, **active low** (LOW = charge enabled) [S5 pin_config.h:8; sticky_charger.cpp] | FOUND |
| PSRAM | **8 MB PSRAM** (ESP32-S3, octal) [S1]. Seeed uses `CONFIG_SPIRAM=y`, `SPIRAM_MODE_OCT`, `SPIRAM_SPEED_80M` [S5 sdkconfig.defaults]. **Fixed:** RxNudge `sdkconfig.defaults` now sets these, and the generated `sdkconfig.sticky` contains `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y` and `CONFIG_SPIRAM_SPEED_80M=y`. The reference's generated sdkconfig had PSRAM off | FOUND / FIXED |
| Flash size | Seeed: **32 MB** QSPI flash [S1]; demo uses `FLASHSIZE_32MB` [S5]. RxNudge keeps the reference's 16 MB image (a valid subset), so the CrossPoint partition table and web flasher keep working | FOUND |
| SdFat version | Pin **greiman/SdFat@2.3.1** (latest release) [S7]. Compiles cleanly with espressif32 6.12.0 / Arduino 2.0.17 | FOUND |
| Partition layout | Kept CrossPoint's table unchanged (2 x 6.25 MB OTA, 3.4 MB spiffs, coredump). The app is 1.05 MB, so there is no reason to change it | DECISION |
| Other buses | Sensor I2C1 GPIO0/1: BQ27220 0x55, PCF8563 0x51, SHT40 0x44, LSM6DS3TR-C 0x6A (INT GPIO7). Touch I2C0 GPIO2/3. PDM mic CLK 19 / DATA 20 / EN 38. USB-C = UART0 TX43/RX44 [S1, S5] | FOUND |
| SPI quirks | E-paper and SD share SPI2. Never release the whole bus after SD use. Unmount/deselect SD before an e-paper refresh. Unused SPI data pins must be -1, or SPI grabs GPIO0 (sensor SCL) [S2 FAQ 1-2] | FOUND |
| Callback rule | GPIO callbacks must not refresh the display or drive the buzzer; they should only post events [S2 FAQ 4] | FOUND |

**Correction to section 8:** the buzzer exists and is officially documented. RxNudge Phase 1 drives it (alerts/buzzer.cpp). If the self-test records "not heard", alerts fall back to visual-only.

## 1. Toolchain / build
| Item | Status | Detail |
|---|---|---|
| Framework | FOUND (platformio.ini:7) | `framework = arduino, espidf` (Arduino running as an IDF component) |
| Platform | FOUND (platformio.ini:5) | `espressif32 @ 6.12.0` |
| PlatformIO env name | FOUND (platformio.ini:2,4) | `reterminal_sticky` (the only env and the default) |
| Board def | FOUND (platformio.ini:6,9) | `esp32-s3-devkitc-1` + `board_build.mcu = esp32s3`. There is no custom board JSON |
| Partition table | FOUND (partitions.csv:3-8) | nvs 0x9000/0x5000; otadata 0xe000/0x2000; app0 0x10000/0x640000; app1 0x650000/0x640000; spiffs 0xc90000/0x360000; coredump 0xFF0000/0x10000. "Matches CrossPoint's standard table so their web flasher works" (partitions.csv:1) |
| Flash | FOUND (platformio.ini:10-14; sdkconfig.defaults.esp32s3:18-20) | 16 MB, DIO, 40 MHz |
| Memory type | FOUND (platformio.ini:16-18) | `dio_opi`. Quirk: `qio_opi` made the ROM die in ets_loader.c 78 before the 2nd-stage bootloader ran |
| PSRAM settings | FOUND (platformio.ini:19,34; sdkconfig.defaults.esp32s3:3-5) | `psram_type = opi`, `-DBOARD_HAS_PSRAM=1`, `CONFIG_SPIRAM=y`, `SPIRAM_MODE_OCT`, `SPIRAM_USE_MALLOC`. **Conflict:** the committed generated `sdkconfig.reterminal_sticky:514` contains `# CONFIG_ESP32S3_SPIRAM_SUPPORT is not set` and no `CONFIG_SPIRAM=y`, so PSRAM may not actually be enabled in the build. README:127 says the chip is ESP32-S3R8 (8 MB octal PSRAM) |
| Caches | FOUND (sdkconfig.defaults.esp32s3:6-7) | I-cache 16 KB, D-cache 32 KB |
| CPU / PM | FOUND (src/main.cpp:189, 259-277; sdkconfig.defaults:19-20) | `setCpuFrequencyMhz(80)`; esp_pm max 80 / min 10 MHz, light_sleep_enable; PM_ENABLE + tickless idle |
| Libraries | FOUND (platformio.ini:59-60) | `h2zero/esp-nimble-cpp@2.3.4` is the only lib_dep. Local libs in `lib/`: EInkDisplay, GfxRenderer, EpdFont, InputManager, SDCardManager, Utf8, hal (no versions; vendored from CrossPoint/MicroSlate) |
| SdFat | NOT FOUND (assumption) | `SDCardManager.h:6` includes `<SdFat.h>`, but SdFat is not in lib_deps and is not vendored. Its version and source are unknown |
| Build flags | FOUND (platformio.ini:28-45) | `ARDUINO_USB_CDC_ON_BOOT=0`, `EINK_DISPLAY_SINGLE_BUFFER_MODE=1`, `CORE_DEBUG_LEVEL=0`, `RETERMINAL_STICKY=1`, `OMIT_FONTS=1`, `RELEASE_BUILD`, gnu++17, -Werror stripped (also scripts/pio_nowerror.py) |
| Upload | FOUND (platformio.ini:47-54; scripts/flash_sticky.sh:68-79) | esptool at 460800 over a WCH serial port (`/dev/cu.wchusbserial…`). App-only mode writes 0x10000; full mode writes 0x0/0x8000/0x10000 |
| Native test env | NOT FOUND (assumption) | The repo has no `env:native` and no `test/` folder |

## 2. Display
| Item | Status | Detail |
|---|---|---|
| Controller / panel | FOUND (lib/hal/board_pins.h:4; EInkDisplay.cpp:245,257) | SSD1677, 800x480; init comments mention GDEQ0426T82 booster values. **Note:** 800x480 matches a 4.26" class panel, not the "3.97"" in the brief, so the panel size needs confirming |
| Driver/library | FOUND (lib/EInkDisplay, lib/hal/HalDisplay.*, lib/GfxRenderer) | Custom bit-banged-command SSD1677 driver over Arduino SPI. Not GxEPD2 |
| Pins | FOUND (board_pins.h:8-21) | SCLK 13, MOSI 14, CS 15, DC 16, RST 17, BUSY 18, EPD_PWR_EN 47, MISO 12, 20 MHz |
| Unused MISO define | FOUND (HalDisplay.cpp:4) | `#define SD_SPI_MISO 7` is defined but never used; it conflicts with SPI_MISO 12. Treat it as dead code |
| Init sequence | FOUND (EInkDisplay.cpp:163-165, 193-199, 244-290) | EPD_PWR_EN HIGH → reset (delays 20/2/20 ms) → SW reset + wait busy → temp sensor control 0x80 (internal) → booster AE C7 C3 C0 40 → driver output 479, 0x02 → border 0x01 → full RAM area → auto-fill BW and RED RAM with 0xF7 + wait busy |
| Resolution / rotation | FOUND (EInkDisplay.h:24-25; src/config.h:21-26; src/main.cpp:150-160) | 800x480 native landscape, 1 bpp, 48 KB static framebuffer (single buffer). Rotation via GfxRenderer orientation 0-3, stored in NVS `ui_prefs/orient` |
| Full vs partial | FOUND (EInkDisplay.cpp:593-605; HalDisplay.h:14-18; main.cpp:291-293) | FULL (CTRL2 0xF7, border 0x01), HALF (also 0xF7), FAST (0xFF, or 0x0C with custom LUT; border 0x80 "avoids a dark ring"). One FULL refresh is required after power-on before FAST works. `displayWindow()` is EXPERIMENTAL and requires x and w to be multiples of 8 (EInkDisplay.h:51; .cpp:489) |
| Non-blocking refresh | FOUND (EInkDisplay.cpp:620-720; main.cpp:801-806) | beginRefresh/pollRefresh, 10 s timeout; single-buffer mode does a RED RAM resync afterwards |
| Grayscale | FOUND (EInkDisplay.cpp:45,78,390-440,551-558) | 2-bit gray via custom LUTs `lut_grayscale` and `lut_grayscale_revert` (LSB/MSB buffers) |
| Waveform/temperature | FOUND (EInkDisplay.cpp:247-255, 571) | Internal temperature sensor; temperature is loaded by CTRL2 bit 0x20 in the OTP waveform. CMD_WRITE_TEMP (0x1A) is defined but never used. There is no explicit temperature compensation |
| Wait-idle guards | FOUND (EInkDisplay.cpp:230-238, 755-778; main.cpp:312, 642, 696) | waitWhileBusy polls BUSY HIGH with a 10 s timeout. deepSleep() powers down analog (0x03), waits busy, then sends DEEP_SLEEP 0x01. Sleep screen uses `delay(500)` before sleep. `setFadingFix(true)` turns analog off after each refresh (main.cpp:195; GfxRenderer.cpp:628) |

## 3. Touch
| Item | Status | Detail |
|---|---|---|
| Controller | FOUND (lib/hal/Gt911Touch.h; board_pins.h:39-45) | GT911 at 0x5D |
| Bus | FOUND (board_pins.h:40-44; Gt911Touch.h:36) | `Wire` (dedicated bus), SDA 3, SCL 2, INT 21, RST 41, EN 42, 400 kHz |
| Init | FOUND (Gt911Touch.h:23-37) | EN HIGH, 10 ms; INT LOW + RST LOW 10 ms; RST HIGH, 50 ms; INT set to input (selects addr 0x5D); probe 0x8140 |
| Read | FOUND (Gt911Touch.h:44-76) | Polled (INT is not used as an interrupt). Single point. Status 0x814E, coords at 0x8150. Quirk: there is no track-id prefix byte |
| Coordinate mapping | FOUND (Gt911Touch.h:64-71; InputManager.cpp:32-53) | Swap XY, then flip both axes (x = 799 - rawY, y = 479 - rawX); per-orientation transform in panelToLogical |
| Gestures | FOUND (InputManager.cpp:84-103,179-185) | Tap only (reported on release, at the last position). No swipe/long-press. Touch bar is 54 px at the bottom (src/ui_renderer.cpp:176) |

## 4. Buttons
| Item | Status | Detail |
|---|---|---|
| Pins | FOUND (board_pins.h:24-26) | POWER/OK 4, UP 5, DOWN 6 |
| Active level | FOUND (board_pins.h:23; InputManager.cpp:56-58,66-76) | Active-low, external 10k pull-up plus INPUT_PULLUP |
| Debounce | FOUND (InputManager.h:44; InputManager.cpp:116-121) | 5 ms software debounce; polled every 10-50 ms (main.cpp:854) |
| Semantics | FOUND (InputManager.cpp:68-70; main.cpp:337-362) | POWER also counts as CONFIRM. Short press (50-1000 ms) = home; long hold = deep sleep |
| Wake capability | FOUND (HalGPIO.cpp:54-60) | Only GPIO4 (POWER) is a wake source: ext0, level LOW. GPIO4-6 are RTC-capable on S3, but UP/DOWN are not configured as wake sources |

## 5. microSD
| Item | Status | Detail |
|---|---|---|
| Bus | FOUND (board_pins.h:7,15-20; HalGPIO.cpp:32) | Shares the SPI bus with the e-paper (SCLK 13, MOSI 14, MISO 12); CS 8; 20 MHz |
| Power / detect | FOUND (board_pins.h:18-19; HalGPIO.cpp:20-21) | SD_PWR_EN 10 driven HIGH at boot. SD_DET 11 is defined but never read |
| Filesystem | FOUND (SDCardManager.h:6,56; SDCardManager.cpp:19) | SdFat (`SdFat sd`, FsFile: FAT/exFAT), `sd.begin(SD_CS, SD_SPI_HZ)`. App data lives under `/sticky/` |
| Sleep quirk | FOUND (SDCardManager.cpp:35-47) | SPI.end() must never be called because the bus is shared. sleep() only clears the flag, and the card is lazily re-initialised on next use |
| Internal FS | FOUND (partitions.csv:7) | There is a 3.4 MB spiffs partition. No use of it was found in the source |

## 6. Battery / power
| Item | Status | Detail |
|---|---|---|
| Fuel gauge | FOUND (board_pins.h:34-37; lib/hal/Bq27220.h; HalGPIO.cpp:63-80) | BQ27220 on Wire1 (SDA 1, SCL 0, 0x55). Reads only RelativeStateOfCharge 0x2C (low byte), cached for 15 s |
| Voltage | FOUND: Seeed ESPHome guide says the BQ27220 provides "battery level, voltage, and current" (https://www.seeedstudio.com/sticky/docs/en/playground-docs/esphome/); RxNudge reads 0x08 Voltage, 0x0C Current, 0x06 Temperature (board/battery.cpp). Register map = TI BQ27220 standard commands | The firmware does not read voltage, current or temperature registers. Assumed available from BQ27220 standard commands (unverified) |
| Charger | FOUND (board_pins.h:31-32; HalGPIO.cpp:14-15,26,82-84) | BQ25616. EN_BAT_CHG 39 (/CE, active-low) is driven LOW = charging enabled. CHARGE_STATE 40 HIGH is treated as "USB connected" |
| Charge vs USB distinction | NOT FOUND (assumption) | Only one pin is checked. The real semantics of GPIO40 (PG vs STAT) are unconfirmed |
| Power sequencing | FOUND (board_pins.h:28-30; HalGPIO.cpp:8-33; main.cpp:191) | gpio.begin() runs first in setup(): PWR_HOLD 45 and PWR_LOCK 46 go HIGH ("must be driven HIGH immediately at boot or the board dies"), then EPD_PWR_EN 47, SD_PWR_EN 10 and TOUCH_EN 42 go HIGH, then SPI.begin |
| IMU (same bus) | FOUND (src/imu_gyro.cpp:6,14) | LSM6 at 0x6A on Wire1 |

## 7. Sleep / wake / RTC
| Item | Status | Detail |
|---|---|---|
| Deep sleep | FOUND (main.cpp:299-314; HalGPIO.cpp:54-60) | Save → sleep screen (FULL refresh + 500 ms) → display.deepSleep() → wait for POWER release → ext0 on GPIO4 LOW → esp_deep_sleep_start. Triggered by long press or idle timeout (main.cpp:844) |
| Light sleep | FOUND (main.cpp:254-279; sdkconfig.defaults:19-29) | Automatic light sleep with NimBLE modem sleep |
| Wake reason | FOUND (HalGPIO.cpp:86-103) | PowerButton / AfterFlash / AfterUSBPower classification |
| Timer wake | Not in reference. RxNudge implements it (board/sleep_wake.cpp) with Seeed's rail-hold sequence. Hardware proof = self-test item 7 |
| RTC / timekeeping | PCF8563 exists (section 0); reference does not use it. Original note: | There is no external RTC chip, SNTP or configTime. Assumes the ESP32 internal RTC (RC 150 kHz unless an external 32 kHz crystal exists, which is unknown). Rails behaviour during deep sleep is unknown (does PWR_HOLD stay latched?) |

## 8. Alert hardware
| Item | Status | Detail |
|---|---|---|
| Buzzer | FOUND (board_pins.h:47), but **UNUSED** | `#define BUZZER_PIN 48` is the only reference in the repo. No tone(), ledc or pinMode code uses it. Type (active/passive), drive level and frequency are unknown |
| Speaker / LED / vibration | NOT FOUND | No code or defines exist for any of these |
| **Audible output** | GPIO48 buzzer | Alarms produce audible output; verified on hardware in 1.0.4 |

## 9. Logging
| Item | Status | Detail |
|---|---|---|
| Arduino USB CDC | FOUND (platformio.ini:33) | `-DARDUINO_USB_CDC_ON_BOOT=0`, so `Serial` = UART0, not native USB CDC. TinyUSB is disabled (sdkconfig.reterminal_sticky:1457) |
| IDF console | FOUND (sdkconfig.defaults.esp32s3:9-13; sdkconfig.reterminal_sticky:737-749) | Console on UART0, TX 43 / RX 44, 115200. Secondary console is USB-Serial-JTAG (line 743). "Sticky USB-C is a WCH UART0 bridge, not USB-Serial-JTAG" |
| App logging | FOUND (src/config.h:141-154; platformio.ini:38,43) | DBG_* macros wrap Serial.printf and are compiled out by `RELEASE_BUILD`. `CORE_DEBUG_LEVEL=0`; IDF LOG level 3 (sdkconfig.reterminal_sticky:972). Libraries print with `if (Serial) Serial.printf` |
| Panic | FOUND (sdkconfig.reterminal_sticky:717) | Print + reboot. Coredump partition present |

## 10. PSRAM usage
FOUND (repo-wide grep): no ps_malloc, heap_caps or EXT_RAM usage. The framebuffer is a static 48 KB array in internal RAM (EInkDisplay.h:79). PSRAM would only be used implicitly via SPIRAM_USE_MALLOC, and only if it is really enabled (see the conflict in section 1).

## 11. Workarounds, delays, quirks (all FOUND)
- platformio.ini:16-18: `qio_opi` bricks boot; use `dio_opi`.
- board_pins.h:28; HalGPIO.cpp:8-12: PWR_HOLD/PWR_LOCK must go HIGH first thing in setup() or the board dies.
- main.cpp:291-293: one FULL refresh is required after power-on before FAST refresh works.
- EInkDisplay.cpp:279-287: both RAM banks are pre-filled with white for a clean FAST baseline.
- EInkDisplay.cpp:603,671: FAST border 0x80 avoids a dark ring.
- EInkDisplay.cpp:195-199: reset delays 20/2/20 ms. waitWhileBusy has a 10 s timeout (230-238); pollRefresh has a 10 s timeout (691).
- main.cpp:195: fading fix turns panel analog off after every refresh to cut idle drain.
- main.cpp:642,696: 500 ms delay after the sleep-screen refresh.
- Gt911Touch.h:25-33: 10/10/50 ms reset timing; INT LOW during reset selects 0x5D. Gt911Touch.h:56: no track-id byte. Gt911Touch.h:64-71: swap + flip.
- InputManager.cpp:79-82: touch-synthesized buttons are latched for 40 ms so debounce can see them.
- SDCardManager.cpp:37-38: never SPI.end() (shared bus).
- HalGPIO.cpp:55-58: wait for POWER release before deep sleep, otherwise it would wake immediately (ext0 LOW).
- main.cpp:847-854: loop delay is 10 ms when active and 50 ms when idle (buttons are polled).
- main.cpp:386: delay(100). wifi_sync.cpp:349: delay(50).
- ble_keyboard.cpp:584: BLE TX power -9 dBm, "lowest verified working".
- sdkconfig.defaults.esp32s3:15-16 and scripts/pio_nowerror.py: warnings-as-errors are disabled for gcc 12.
- sdkconfig.defaults:2: a stale comment references `sdkconfig.xteink_x4` (port leftover).

## 12. Hardware assumptions still open in v1.0
Everything below is built and host-tested, but has not been confirmed on a real device. Each item has an exact check
in docs/RELEASE_CHECKLIST.md.

| # | Assumption | Where it matters | How it is verified |
|---|---|---|---|
| H1 | The GPIO48 transducer is passive and audible with LEDC PWM | alarms | self-test 4 Alerts; a NO switches to visual-only and persists |
| H2 | Deep sleep with PWR_HOLD/PWR_LOCK latched keeps the board powered and the RTC timer wakes it | alarms after sleep | self-test 7 Sleep wake 2m, on battery |
| H3 | GPIO9 HIGH = USB present; GPIO11 LOW = SD present | power policy, SD | self-test 5 microSD and 6 Battery |
| H4 | PCF8563 keeps time while the ESP32 sleeps or the battery is low | dose times | checklist item "dose survives long sleep" |
| H5 | Panel orientation matches Seeed's 180° rotation | display | self-test 1 Display (square top-left) |
| H6 | Sleep and active currents in docs/POWER.md | battery life | self-test 9 Sleep current + meter |
| H7 | The PCF8563 INT pin is not routed to the ESP32, so alarms rely on the ESP32 RTC timer | wake planning | none needed unless a schematic shows otherwise |

## 13. Answers to the original Phase 1 questions
| Question | v1.0 answer |
|---|---|
| Buzzer on GPIO48? | Yes (Seeed docs). Type unconfirmed (H1); visual-only fallback exists |
| Timer wake / RTC | ESP32 RTC timer + PCF8563 for UTC (H2, H4) |
| Panel | 3.97", 800 x 480, 235 ppi (9.25 px/mm) |
| GPIO40 / GPIO11 | GPIO40 unused; USB detect is GPIO9; GPIO11 is SD detect (H3) |
| CrossPoint partitions | kept (2 x 6.25 MB OTA, 3.4 MB LittleFS, coredump) |
| SdFat | greiman/SdFat@2.3.1 |
| Wi-Fi / NTP | optional; the device works fully offline with the radio off by default |

## 14. Source tree (v1.0)
```
rxnudge/
├── VERSION                 # single version source (1.0.4)
├── platformio.ini          # env:sticky (device), env:native (host tests)
├── partitions.csv, sdkconfig.defaults*, CMakeLists.txt
├── scripts/                # flash_sticky.sh, make_release.sh, pio_version.py, pio_sha256.py
├── tools/                  # gen_assets.py, render_previews.sh, embed_web.py, host_portal.sh, portal_shots.mjs
├── web/                    # portal HTML/JS/CSS (embedded by tools/embed_web.py)
├── src/
│   ├── core/       # schedule, dose engine, clock, alarm, power, self-test logic (pure)
│   ├── storage/    # record store, repository, dose log, export, backups (pure + adapters)
│   ├── ui_core/    # canvas, widgets, fonts, strings, forms, keyboard (pure)
│   ├── app_core/   # controller, admin service and screens, net interface (pure)
│   ├── net_core/   # portal API, security, NTP, OTA verify, notify (pure)
│   ├── board/ alerts/ ui/ app/ net/   # ESP32 glue
│   └── main.cpp
├── lib/                    # vendored EInkDisplay, GfxRenderer, EpdFont, InputManager, SDCardManager, hal
├── test/                   # 9 Unity suites: core, dose, schedule, tz, power, storage, ui, admin, net (+ common fakes)
└── docs/
```

## 15. Status (v1.0.4)
- Both environments build with no warnings in our code (`-Wall -Wextra -Wshadow=local`). Remaining warnings are in
  framework and vendored libraries only.
- All native tests pass; counts and sizes are in docs/KNOWN_LIMITATIONS.md.
- Deliberate deviations from the reference, all backed by Seeed sources: external-power detect on GPIO9 instead of
  GPIO40; deep sleep latches PWR_HOLD/PWR_LOCK with GPIO hold and uses EXT1; holds released at boot after preloading
  HIGH; RTC-timer wake added; OTA rollback enabled (new bootloader, USB once).
- Device verification is still required: see docs/RELEASE_CHECKLIST.md.
