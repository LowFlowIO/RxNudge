# Known limitations – RxNudge 1.0.4

## Measured numbers
Host = the Linux build machine (2026-10-19 00:29 CEST), clean build via `scripts/make_release.sh`.
Nothing in the "to be measured on device" rows has been measured; those cells are deliberately empty.

| Quantity | Value | Where / how |
|---|---|---|
| Native tests | **160 / 160 pass** (10 suites: admin 17, backup 6, core 10, dose 20, net 19, power 17, schedule 15, storage 15, tz 8, ui 33) | host, `pio test -e native` |
| Firmware image `firmware.bin` | 2,118,304 B | host build |
| Flash used (app) | 2,117,769 B of 6,553,600 B (32.3 %) per OTA slot | host build |
| Static RAM | 89,396 B of 327,680 B (27.3 %) | host build |
| Merged flash image | 2,183,840 B | host, `esptool merge_bin` |
| firmware.bin SHA-256 | `ad022ec60f2cf3ebc5b96bcbcbb696781d87426131563b725ffaff8fa534ced0` | host |
| App descriptor | project `rxnudge`, version `1.0.4`, ESP-IDF 4.4.7 | host, `esptool image_info` |
| Full UI render, host CPU | home 0.36, today 0.47, history 0.47 ms/frame (varies ±25 % run to run) | host, `test_render_timing` (render into the 800x480 canvas only, no panel I/O) |
| Rendered previews | 49 device screens regenerated (+ 8 portal shots from Phase 6) | host, `tools/render_previews.sh` |
| Render time on ESP32-S3 | *to be measured on device* | serial latency log (`ui` task) |
| Tap-to-screen latency (FAST refresh) | *to be measured on device* | serial latency log, stopwatch |
| FULL refresh time | *to be measured on device* | serial latency log |
| Deep-sleep current | *to be measured on device* | self-test 9 Sleep current + µA meter |
| Awake idle current, Wi-Fi job current | *to be measured on device* | USB meter |
| Battery life | *to be measured on device* (the ~30 mAh/day in POWER.md is an ASSUMPTION-based estimate) | full charge → shutdown with a fixed schedule |
| RTC drift per day | *to be measured on device* | self-test 8 Counters (drift) vs NTP |
| Nightly backup duration (SD write + prune) | *to be measured on device* | serial `backup: auto` log timestamps |
| Timer-wake accuracy | *to be measured on device* | self-test 7 (pass window 118–126 s) |

## Limitations
1. **Core functions verified on hardware (1.0.4).** Flash, boot, setup wizard, home screen, alarms, touch/buttons, sleep/wake and optional Wi-Fi/portal were tested on reTerminal Sticky production hardware. Remaining open items (exact current draw, panel latency, RTC drift, etc.) are listed in the measured-numbers table and docs/RELEASE_CHECKLIST.md.
2. **Not a medical device;** no regulatory validation. Reminders can fail (flat battery, broken buzzer, wrong clock).
3. **Battery:** with an empty battery there are no reminders. Doses that fall due then are recorded as missed when
   the device wakes; they are never rung late.
4. **Sound:** a single piezo/magnetic buzzer, no vibration, no LED under firmware control. If the self-test buzzer
   check is answered NO, alerts are visual only.
5. **Touch targets:** keyboard keys are 80 x 84 px = 8.65 x 9.1 mm. 9 mm width is impossible with 10 QWERTY keys
   on an 800 px (86.5 mm) panel. Form rows are 78 px (8.4 mm) to fit 4 rows per page; self-test rows are 9.1 mm.
   Touch is single-tap only (on release): no swipe, long-press or auto-repeat. After a screen change a finger must
   be lifted and wait ~0.3 s before the next tap counts; taps during an e-paper refresh that changes the screen are
   dropped. Two taps less than 70 ms apart count as one. The GT911 timings were tuned on host simulations; confirm
   with RELEASE_CHECKLIST 7c on the device.
6. **Language:** English only. All text is in `src/ui_core/strings.cpp` ready for translation, but no other table
   ships; the font covers Latin-1 and common accents, not other scripts.
7. **Nightly backups** need a microSD card in the device. A backup is skipped while the battery is critical and is
   named by the local date it ran (one file per day; a second run that day replaces it). Only 14 automatic backups
   are kept.
8. **OTA authenticity:** SHA-256 protects against corrupted files, not against a malicious image uploaded by
   someone with the on-device portal code. Signed images are documented as an option (docs/OTA.md), not enabled.
   The bootloader can only be changed over USB.
9. **Web portal:** HTTP on the LAN only (no TLS), one-time code, 10 min idle timeout. Do not expose it to the
   internet.
10. **Clock:** without Wi-Fi the clock depends on the PCF8563; after 7 days without sync (only when Wi-Fi is
    configured) a "check the clock" banner appears. Time zones come from a built-in list.
11. **Profiles:** max 8 people. Dose logs store medication ids, not names.
12. **Flash:** the image uses 16 MB of the 32 MB flash (CrossPoint-compatible table).
13. **Clock entry default** without a valid RTC is the firmware build date/time, so a firmware built long ago starts
    far from today; the result line ("Mon 19 Oct 2026, 08:30") must be checked before saving. Number-pad keys are
    121 x 84 px (13 x 9.1 mm); form rows stay 78 px.
14. **Caregiver alerts** (ntfy/webhook) need Wi-Fi at the moment of the missed dose and are best effort.
