# Release checklist – RxNudge 1.0.4

Each item of the definition of done, with its state:
**HOST ✔** = verified on the build machine for this release; **DEVICE ✔** = verified on reTerminal Sticky production hardware for 1.0.4; **DEVICE ☐** = still open (detailed measurements or edge cases), with exact
steps. Tick the device boxes and record results (date, unit, tester) before calling the release device-verified.

Preparation for device steps: flash `rxnudge-v1.0.4-flash-full.bin` (docs/BUILD_FLASH.md §8), keep the serial
monitor open (`pio device monitor -e sticky --port /dev/sticky`), and complete the setup wizard with two people.

| # | Item | State |
|---|---|---|
| 1 | Builds clean | HOST ✔ |
| 2 | Native tests pass | HOST ✔ |
| 3 | Flash workflow | HOST ✔ / DEVICE ✔ |
| 4 | 2+ profiles with separate reminders | HOST ✔ / DEVICE ✔ |
| 5 | Dose survives power loss, reboot, long sleep | HOST ✔ / DEVICE ✔ |
| 6 | Alarms after sleep | HOST ✔ / DEVICE ✔ |
| 7 | No-SD and no-Wi-Fi operation | HOST ✔ / DEVICE ✔ |
| 7a | Nightly automatic SD backup | HOST ✔ / DEVICE ☐ |
| 7b | Date and time entry | HOST ✔ / DEVICE ☐ |
| 8 | Visible errors | HOST ✔ / DEVICE ✔ |
| 9 | Docs complete | HOST ✔ |

## 1. Builds clean — HOST ✔
- `./scripts/make_release.sh` starts with `rm -rf .pio/build`, then builds `native` and `sticky`.
- Result: both succeed; no warnings from `src/` or `test/` (flags `-Wall -Wextra -Wshadow=local`). Warnings remain
  only in framework/vendored code. Sizes: docs/KNOWN_LIMITATIONS.md.
- Re-check: `pio run -e sticky 2>&1 | grep -E "src/.*warning"` prints nothing.

## 2. Native tests pass — HOST ✔
- `pio test -e native`: all suites pass (count in docs/KNOWN_LIMITATIONS.md).

## 3. Flash workflow — HOST ✔ / DEVICE ✔
Host: the release has `firmware.bin` + `.sha256`, bootloader, partitions, OTA data, a merged image and `SHA256SUMS`;
`esptool image_info firmware.bin` shows project `rxnudge` version `1.0.4`.
Device:
1. `cd rxnudge-v1.0.4-binaries && sha256sum -c SHA256SUMS` → all OK.
2. Flash the merged image (command in `FLASH.txt`). Expect "Hash of data verified".
3. Serial shows `RxNudge 1.0.4 boot, wake cause: power-on/reset` and PSRAM ≈ 8 MB. The wizard appears.
4. App-only update: `./scripts/flash_sticky.sh /dev/sticky app` → device boots, data kept.
5. OTA: Settings > Web portal, upload `firmware.bin` with its `.sha256` (docs/OTA.md) → restarts, About shows 1.0.4.
6. Rollback: upload a build whose first frame never draws (or power off during the first boot twice) → previous
   firmware comes back.

## 4. Two or more profiles with separate reminders — HOST ✔ / DEVICE ✔
Host: test_dose/test_admin cover per-profile schedules, quiet hours, merged alerts per person (`alert_pending`),
1.5 s spacing between people; previews show two lanes.
Device:
1. Create Anna (quiet hours off) and Ben (quiet hours covering the next 30 min).
2. Give each a medicine due in 3 min (Ben's not Important, a second Ben medicine Important).
3. At the due time: Anna rings; Ben's normal dose is silent (shown), Ben's Important dose rings after Anna's.
4. Mark Anna TAKEN → Ben's dose stays Due. History per person shows only their own doses.

## 5. Dose survives power loss, reboot and long sleep — HOST ✔ / DEVICE ✔
Host: test_storage fault injection (torn writes at every byte, tmp/bak recovery, torn log records), snapshot +
`processUntil` catch-up equivalence, migration.
Device:
1. Mark a dose TAKEN. Within 2 s remove power (unplug USB **and** hold reset / disconnect battery if accessible;
   otherwise press Reset). Boot → Today shows it Taken; History lists it once.
2. Reboot from the self-test Exit / Reset button → unchanged.
3. Long sleep: hold OK 2 s on Home with the next dose ≥ 8 h away; leave overnight on battery. Next morning: the dose
   rings on time (±1 min against a phone clock), earlier records unchanged, the clock is right.
4. Remove the SD card during step 1: data is still kept (internal storage).

## 6. Alarms after sleep — HOST ✔ / DEVICE ✔
Host: `planWake` tests (next due/ladder/snooze/nightly), stale events not rung, missed-while-asleep counting.
Device:
1. Medicine due in 4 min. Hold OK 2 s on Home → sleeps.
2. On battery, it wakes by itself at the due time and rings (serial: wake cause timer).
3. Do not answer: repeats follow the ladder (default every 5 min), each after a sleep.
4. Self-test 7 Sleep wake 2m: PASS, 118–126 s.
5. Run the battery down (or self-test battery path) past a due time, charge, press AI → "N missed while asleep",
   no ring for old doses.

## 7. No-SD and no-Wi-Fi operation — HOST ✔ / DEVICE ✔
Host: tests run with no SD (`sd_ = nullptr`) and `RadioOffNetService`; backups report "No SD card inserted".
Device:
1. No card, Wi-Fi never configured. Run items 4–6 → all work; banner "No SD card - backups paused" only.
2. Insert a card while running → banner clears; Back up now writes `/RxNudge/backups/backup-*.json`.
3. Wi-Fi configured but the router off: Sync time now shows "Syncing time... N s", the UI stays responsive, then a
   failure toast. Alarms during that time still ring.

7a. Nightly automatic backup (v1.0.1). Host: test_backup (schedule, missed-slot catch-up, change detection,
weekly backup, retention of 14, manual files untouched, failure banner, hourly retry) and test_admin (Settings page).
Device:
1. Insert a FAT32 card. Set the clock to 02:55 local (Settings > Set clock) and hold OK 2 s on Home to sleep.
2. The sleep log line ends `for nightly_refresh`. At 03:00 the device wakes and logs `backup: auto ok wrote=1 ...`.
   Settings > Backup & restore shows "Last automatic <date> 03:00", status "Saved", file "auto-YYYYMMDD.json".
3. On a PC: `/RxNudge/backups/auto-YYYYMMDD.json` exists, contains `"format": "rxnudge-backup"`, and
   `grep -c <wifi password> auto-*.json` prints 0. No `.tmp` file is left.
4. Next night without changes: log `backup: auto unchanged wrote=0`. After editing a medicine: a new file.
5. Catch-up: power off over 03:00, power on at 09:00 → backup within 30 s of boot.
6. Retention: copy 20 dummy `auto-202501DD.json` files and one `backup-...json` onto the card, trigger a backup →
   14 auto files remain, the manual file is untouched.
7. Failure: use a full (or write-protected) card → banner "Backup failed - check SD card", status "Failed"; after
   freeing space it clears within an hour. Remove the card → "No SD card - backups paused"; toggle Nightly backup
   off → that banner disappears.
8. While a backup runs (large history), a due alarm still rings on time.

7b. Date and time entry (v1.0.2). Host: test_ui (clamping, leap years, number pad, auto-advance, defaults, 9 mm
targets, partial refresh of only the changed field) and test_admin (wizard, settings, medication dates).
Device:
1. Factory reset (Settings > Reset) with Wi-Fi not set up. The wizard's clock step shows **Set date and time**; Next
   is refused until a time is set. Tap it: DD MM YYYY HH:MM boxes start at the firmware build date/time.
2. Tap the year box, type 2028, then tap the day box and type 29 02 → "Tue 29 Feb 2028" shown; type 2027 in the year
   → day becomes 28 automatically.
3. Arrows: each tap changes only the tapped box; watch that only that box and the result line flicker (FAST partial
   refresh, no full-screen flash). Measure with a ruler: arrows ≥ 9 mm tall.
4. Buttons only: UP/DOWN change the black box, OK moves on; after the minutes OK saves.
5. With Wi-Fi set up, the clock step starts "Syncing time... N s" by itself and "Get time from internet" is the
   first row.
6. Pull the RTC battery or power down long enough to lose the time: the forced clock screen starts at the build
   date; with a valid RTC it starts at the current time.
7. Medication start/end date, quiet-hour times and "EARLIER" (Taken at) use the same boxes; an EARLIER time before
   midnight resolves to the previous day.

7c. Medication times and touch (v1.0.4). Host: test_admin `test_custom_time_from_home` (coordinate taps from Home:
Menu > Medications > Add, Time 1 = 14:35, add/delete a time, Save, 14:35 on Today), `test_time_page_anchor_and_delete`,
`test_list_rows_tappable`; test_touch (12 cases: stale reports, duplicates, bounce, min press, missed up, long hold,
guard while down/after release, two fingers/slide, 200 noisy touches, tap gate).
Device:
1. Menu > Medications > Add: page 1 is Name, Dose, "Time 1  08:00", "+ Add time".
2. Tap Time 1 > At, type 1 4 3 5 > Save > Done; add a time with "+ Add time"; name it, Save. Today shows 14:35 and
   the alarm rings at 14:35.
3. Tap an existing medication in the list and Schedule > "1: ..." (phase row): each opens (v1.0.2: list rows and
   phase rows did nothing or hit the footer button).
4. Keyboard: type "aaaaaaaaaa" with deliberate single taps, including light and long presses → exactly 10 chars.
   Number pad: 10 taps → 10 digits.
5. Tap "Add medication" and keep the finger down for 1 s before lifting → the new screen opens and nothing on it
   is activated. Tap Menu twice quickly → only the menu opens.
6. Serial: "touch: dropped tap ..." appears only when tapping during a screen change.

## 8. Visible errors — HOST ✔ / DEVICE ✔
Host: previews and tests for every banner (clock not set/untrusted, low/critical battery, no SD, SD error,
storage error, buzzer fault), PIN lockout, refused actions ("Not possible: …").
Device:
1. Fresh unit without clock → "Clock not set - reminders paused".
2. Self-test 4 Alerts, answer NO → "Buzzer fault - visual alerts only" stays after reboot.
3. Corrupt card (non-FAT) → "SD card error".
4. Battery < 20 % → "Battery low - please charge"; critical → final "Charge now" screen.

## 9. Docs complete — HOST ✔
README.md, docs/USER_GUIDE.md (non-technical, safety disclaimer first), BUILD_FLASH, ARCHITECTURE,
HARDWARE_NOTES, POWER, OTA, RELEASE_CHECKLIST, KNOWN_LIMITATIONS; previews in docs/screens and docs/portal.
