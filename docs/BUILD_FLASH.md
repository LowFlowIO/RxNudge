# RxNudge: build, flash, monitor (Debian 12/13)

## 1. One-time setup
```bash
sudo apt-get update
sudo apt-get install -y python3 python3-pip python3-venv git build-essential
python3 -m pip install --user --break-system-packages platformio   # or: pipx install platformio
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
pio --version
```
`build-essential` (g++) is only needed for `pio test -e native`.

### Serial permissions and udev
The Sticky's USB-C port goes through a WCH USB-UART bridge (VID 0x1a86; the reference's port name
`wchusbserial…` points to a CH34x/CH910x part). Linux shows it as `/dev/ttyACM0` (cdc_acm) or `/dev/ttyUSB0` (ch341).
```bash
sudo usermod -aG dialout "$USER"        # log out and back in afterwards
sudo tee /etc/udev/rules.d/99-reterminal-sticky.rules >/dev/null <<'RULES'
# WCH USB-UART bridges (CH340/CH343/CH9102) used by reTerminal Sticky
SUBSYSTEM=="tty", ATTRS{idVendor}=="1a86", MODE="0660", GROUP="dialout", SYMLINK+="sticky"
RULES
# PlatformIO's own rule set (covers many other boards too)
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules >/dev/null
sudo udevadm control --reload-rules && sudo udevadm trigger
# ModemManager can grab new ACM ports; stop it if flashing fails at "Connecting..."
sudo systemctl disable --now ModemManager 2>/dev/null || true
```
Plug in with a **data** USB-C cable (charge-only cables enumerate nothing; Seeed FAQ). Then check:
`ls -l /dev/sticky /dev/ttyACM* /dev/ttyUSB*` and `lsusb | grep -i 1a86`.

## 2. Build and test
```bash
cd rxnudge
pio run -e sticky            # firmware (.pio/build/sticky/firmware.bin)
pio test -e native           # host unit tests (Unity)
```

## 3. Flash
Back up first: flashing replaces the current firmware.
```bash
# first install: bootloader + partition table + app (also resets OTA data)
pio run -e sticky -t upload --upload-port /dev/sticky
# or with the bundled script:
./scripts/flash_sticky.sh /dev/sticky full     # bootloader 0x0, partitions 0x8000, otadata 0xe000, app 0x10000
./scripts/flash_sticky.sh /dev/sticky app      # later updates: app only at 0x10000
```
esptool resets the chip into the bootloader through the bridge's DTR/RTS lines (`--before default_reset`, the
same as the reference). If "Connecting..." times out, stop ModemManager, re-plug the cable, and press Reset (the pin hole)
just as esptool starts connecting. Seeed does not document a manual boot-mode button combination, so do not guess one.

Optional backup of the existing firmware before the first flash:
```bash
python3 -m esptool --chip esp32s3 --port /dev/sticky --baud 460800 read_flash 0 0x1000000 sticky-backup-16MB.bin
```

## 4. Monitor
```bash
pio device monitor -e sticky --port /dev/sticky      # 115200, esp32_exception_decoder; Ctrl+C to quit
```
Logs come from Arduino `Serial` on UART0 (GPIO43/44) through the WCH bridge (`ARDUINO_USB_CDC_ON_BOOT=0`).
Expected first lines: `RxNudge 1.0.4 boot, wake cause: power-on/reset`, then
`board: up, PSRAM 8388608 bytes, ...` (about 8 MB; a value of 0 means PSRAM is not enabled).

## 5. Hidden self-test
On the home screen, hold **UP + DOWN for 3 s**. The menu is a 2 x 5 grid (rows 84 px = 9.1 mm):
1 Display, 2 Touch, 3 Buttons, 4 Alerts, 5 microSD / 6 Battery, 7 Sleep wake 2m, 8 Counters, 9 Sleep current, 10 Exit.
Use UP/DOWN to move, a short AI press to run, or tap a row.
Hold AI for 2 s to abort the touch/display tests.

## 6. Hardware acceptance criteria (self-test)
| # | Test | Pass criteria |
|---|---|---|
| A0 | Boot | Device stays powered on battery alone (PWR_HOLD works). Serial shows the boot banner and PSRAM ≈ 8 MB. Home screen shows a full refresh with no ghosting |
| A1 | Display pattern | Solid black, solid white, 8-px checkerboard, 4 dithered bands with a frame touching all 4 edges, and a filled square at the **top-left**. Then 6 FAST refreshes with the bar moving left to right and no dark ring at the border. Operator answers YES. If the image is upside down, file an orientation fix (Seeed's driver rotates 180°) |
| A2 | Touch | All 5 targets (4 corners + centre) register within 90 s. Reported coordinates match the touched corner (≈0,0 top-left; ≈799,479 bottom-right) |
| A3 | Buttons | UP, DOWN and AI each register exactly once, with no ghost presses |
| A4 | Alert outputs | 3 beeps at 2400 Hz plus tones at 1000/2000/4000 Hz are audible; the screen inverts twice. Alert mode reads "AUDIBLE + VISUAL". If the operator answers NO for the buzzer, the mode must switch to "VISUAL ONLY" and stay that way after a reboot |
| A5 | microSD | With a FAT32 or exFAT card: GPIO11 reports "card present". Mount, 16 KiB write, read-back, CRC match and delete all succeed. Without a card: detect shows "no card" and the result is FAIL with no hang. After the SD test, display refreshes still work (shared SPI) |
| A6 | Battery | BQ27220 responds. Voltage is 3000-4500 mV. SOC is plausible against the charge LED. Plugging USB-C changes GPIO9 to PRESENT within 3 s, and current turns positive while charging. Record GPIO40 behaviour for documentation |
| A7 | Deep-sleep wake | The device sleeps (screen keeps the "Sleeping" frame) and wakes **by itself**. Result screen: cause "RTC timer", measured 118-126 s, PASS. Stopwatch time is also about 2 min ±10 s. Repeat on battery (USB unplugged) |
| A8 | Button wake | On the home screen, hold AI for 2 s to sleep; a short AI press wakes it. Home shows "Woke by: AI/power button" |
| A9 | Sleep current (optional) | With USB meter / battery shunt: deep-sleep current is in the low hundreds of µA or less. Record the value |

## 7. Version
The version lives in one place: the `VERSION` file (currently `1.0.4`). `CMakeLists.txt` reads it into
`PROJECT_VER` (the app descriptor the OTA check and `esptool image_info` show) and `scripts/pio_version.py`
passes it as `MT_FW_VERSION` (boot banner, About screen, portal). The build fails if either is missing.

## 8. Release binaries
`./scripts/make_release.sh` does a clean build of both environments, runs every test, regenerates the
previews and writes `../rxnudge-v<ver>-binaries/` (+ `.tar.gz`) and `../rxnudge-v<ver>.tar.gz`
(sources without `.pio`). The binaries folder holds:

| File | Offset | Use |
|---|---|---|
| `bootloader.bin` | 0x0 | USB only (rollback-enabled bootloader) |
| `partitions.bin` | 0x8000 | partition table |
| `ota_data_initial.bin` | 0xe000 | selects app0 |
| `firmware.bin` (+ `.sha256`) | 0x10000 | app; also the file for portal OTA |
| `rxnudge-v<ver>-flash-full.bin` | 0x0 | the four above merged (`esptool merge_bin`), for a one-command install |
| `SHA256SUMS`, `FLASH.txt` | | checksums and the commands |

Install the merged image without the source tree:
```bash
python3 -m esptool --chip esp32s3 --port /dev/sticky --baud 460800 write_flash \
  --flash_mode dio --flash_freq 40m --flash_size 16MB 0x0 rxnudge-v1.0.4-flash-full.bin
```

## 9. Bootloader update and OTA
- v1.0 has OTA rollback turned on, which needs the matching bootloader. When coming from a pre-Phase-6 build, Flash once over USB (bootloader, partition table and app) using the first-install command above.
- After that, firmware can be updated from the web portal. See `docs/OTA.md`.
- Each build writes `.pio/build/sticky/firmware.bin.sha256`, which the portal upload needs.
- Portal UI preview on a PC:
  1. `tools/host_portal.sh && .pio/host_portal 8080` prints the one-time code.
  2. `node --experimental-websocket tools/portal_shots.mjs <code> docs/portal` regenerates the screenshots with headless Chrome.
- Web UI sources are in `web/`. After editing them, run `python3 tools/embed_web.py` (it regenerates `src/net_core/web_assets.cpp`).

## 10. Troubleshooting
- **`esptool` fails with `No module named intelhex`** (seen when `pio` runs inside another virtualenv, e.g.
  `~/.platformio-venv`): PlatformIO then starts esptool with that venv's Python, which lacks esptool's dependencies.
  Either install intelhex into that interpreter, or use PlatformIO's own environment:
  ```bash
  ~/.platformio-venv/bin/python -m pip install intelhex     # the python that runs your pio
  # or
  ~/.platformio/penv/bin/pio run -e sticky -t upload --upload-port /dev/sticky
  ```
  (`head -1 $(which pio)` shows which Python your `pio` uses.)
- **"Connecting..." times out:** see section 3 (ModemManager, re-plug, Reset at connect).
