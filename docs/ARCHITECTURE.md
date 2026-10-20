# RxNudge architecture (v1.0.4)

This document grew phase by phase; each section describes the state after that phase, and later sections
supersede earlier ones where they differ. The v1.0 summary is at the end ("Phase 7").

## Layers

```
src/
  board/    hardware (Phase 1, ported from LowFlowIO/sticky-micronotes; pins and init order unchanged)
  alerts/   buzzer + visual alert (Phase 1)
  ui/       e-paper screens (home now shows storage health, profile and med counts)
  app/      firmware glue: SystemClock (IClock), storage bring-up (app_storage)
  core/     PURE C++17: no Arduino/IDF includes, no global time. Built and tested on the host.
  storage/  repositories over an IFileSystem; device adapters only in arduino_fs.* (#ifdef ARDUINO)
components/esp_littlefs  vendored joltwallet/esp_littlefs v1.14.8 (IDF 4.4), needed by Arduino LittleFS
```

`env:native` builds `src/core/` and `src/storage/` (minus `sd_selftest.cpp` and `arduino_fs.cpp`) and runs the Unity
suites in `test/`. `env:sticky` builds everything.

### core/
| File | Role |
|---|---|
| clock.h | `IClock::nowUtc()`. Every time read goes through it (`FakeClock` for tests, `app::SystemClock` on the device). |
| civil.* | Proleptic Gregorian day numbers, weekday, leap years, floor division. |
| tz.* | Our own POSIX TZ parser and converter (`Mm.w.d`, `Jn`, `n`, `<+hh>` names, `/time` incl. negative/>24 h). There is no libc `TZ` dependency. |
| model.* | Profiles (≤8), medications (≤30 per profile), phases (≤4), schedule rules (≤12 times), validation, salted PIN hash, quiet hours. |
| schedule.* | Expands rules into `Occurrence`s with deterministic event ids. |
| dose_log.* | Log entry type, sink interface, `resolveLog` (final outcome per event id, corrections applied). |
| dose_engine.* | Dose state machine, alarm ladder, catch-up, PRN, supply and undo. |
| stats.* | Adherence, streaks, ISO-week summary, supply forecast and refill-by date. |
| json.*, codec.* | Small JSON DOM plus encoding of the model, log entries and the engine snapshot. |
| privacy.* | `medLogLabel`/`redactMedNames`. Names appear in logs only with `-DMT_DEBUG_LOG_MED_NAMES=1`. |

### Time model
* Schedules hold **local wall-clock minutes**. The engine converts them to **UTC instants** with the
  configured POSIX TZ (default `CET-1CEST,M3.5.0,M10.5.0/3`). Every stored instant (log, snapshot) is UTC seconds.
* **Nonexistent local times** (spring-forward gap) fire at the first valid instant after the gap. In Berlin, 02:30
  becomes 03:00 CEST. If that collides with another dose of the same medication, both share one event id and fire
  once.
* **Ambiguous local times** (fall back) fire once, at the first occurrence (CEST).
* Event id = `prn:1 | profile:7 | med:16 | utcMinute:40`. Ids are deterministic, so expanding any window again never
  duplicates an event. PRN takes set the top bit.

### Schedule types
FixedTimes, Routine (profile anchor plus offset, clamped to the same day), Weekdays (mask), EveryN days or weeks,
SpecificDates, Cycle X on / Y off, and PRN (min interval, max per rolling 24 h, next-allowed time).
EveryN and Cycle are anchored at the phase start, or the medication start if the phase has none. Multi-phase
medications (titration or taper) are ordered, non-overlapping phases, each with its own rule and dose text. The
course end is `endDay`, or `start + courseDays - 1`.

### Dose state machine
```
Pending --t>=scheduled--> Due --snooze 5/10/15 (max 3, must end before grace)--> Snoozed --end--> Due (new ladder)
Pending|Due|Snoozed --take--> Taken (<= onTimeMin after schedule) | TakenLate
Pending|Due|Snoozed --skip--> Skipped
Due|Snoozed --t>=scheduled+grace (default 60 min)--> Missed  (logged, source=system)
Missed --take(takenAt)--> Taken/TakenLate  (backfill correction; refused if within 60 min of the next dose)
any user action --undo within 60 s--> previous state (supply and last-taken restored, Undo entry logged)
```
* The alarm ladder is the initial alarm plus `ladderCount` repeats (default 6) every `ladderIntervalMin` (default 5).
  Alarms are muted in the profile's quiet hours unless the medication is critical.
* Taking a dose up to 60 min early is allowed. The engine never proposes a catch-up or double dose: a taken dose
  cannot be taken again, a missed dose can only be **backfilled** with the time it was really taken, and a backfill
  near the next dose is refused (`WouldDoubleDose`).
* **Catch-up** (`processUntil(now)` after a long sleep) repeatedly applies the globally earliest pending transition,
  with ties broken by scheduled time then id. Every intermediate event is processed in order, exactly once. Events
  more than 60 s old are flagged `stale` and must not ring. Running it in steps or in one call gives the same log
  (tested).
* Supply is decremented on Taken/PRN and restored on undo. Low stock (≤ threshold) and the run-out and refill-by days
  come from the schedule over the next 28 days.

## Storage
* `IFileSystem` has these implementations:
  * `MemFs`: tests, with fault injection.
  * `HostFs`: host only.
  * `LittleFsAdapter`: the internal "spiffs" partition at 0xC90000, 3.4 MB, formatted on first boot.
  * `SdAdapter`: SdFat through `SdMan`.
* `RecordStore`: the record layout is `MTR1 | schema | flags | len | CRC32 | payload`.
  * **Write:** write `p.tmp` and flush, read it back and verify, then rotate a *valid* `p` to `p.bak`, then rename
    `p.tmp` to `p`.
  * **Read:** use `p` if valid. Otherwise use a valid `p.tmp` when `p` is missing (power loss between the renames).
    Otherwise use `p.bak` (last known good). In that case the corrupt main is kept as `p.bad`, main is repaired, and
    the status is `RecoveredFromBackup`.
  * If no copy is valid the status is `Corrupt`. Nothing is loaded, nothing is overwritten, and an error is shown.
* `FsRepository` (`IRepository`): `/RxNudge/db.rec` (database JSON) and `/RxNudge/engine.rec` (engine snapshot).
  `StorageHealth` carries status and last error. The home screen shows it (prefixed with `!` when unhealthy).
* **Migrations:** `migrateToCurrent` applies all-or-nothing steps from the document version (missing = v1). The real
  step v1 to v2 does the following:
  * `grace` becomes `graceMin`;
  * `times[]` becomes `phases[0]` with a fixed rule;
  * profile `privacy` defaults to true;
  * root `tz` moves to `settings.tz`.
  
  The pre-migration record is kept as `db.rec.v1`. Documents newer than the firmware are refused.
* **Dose log:** append-only, `/RxNudge/logs/YYYY-MM.log` (UTC month of recording). Each record is
  `\n<json>|<crc32>`. A torn record from a power cut is skipped and counted, and later records stay readable.
  * Fields: event id, profile, med id, scheduled UTC, action, action UTC, recorded UTC, source, note.
  * Corrections (undo, backfill) are new entries.
  * **No medication names** are stored.
* **Export:** CSV (ISO UTC, formula-injection safe) and JSON, filtered by profile. Names are included only if the user
  explicitly asks (`includeNames`).
* **Backup/restore:** one JSON file holding the database and the log. Restore migrates old backups and changes nothing
  on failure.

## Tasks and mutex ownership plan (firmware, Phase 3+)

| Task (core) | Priority | Owns | Talks to others via |
|---|---|---|---|
| `ui` (loopTask, core 1) | 1 | display, GfxRenderer, input, SPI bus while refreshing | posts `UserAction` to the engine queue; reads `EngineView` copies |
| `engine` (core 1) | 2 | `DoseEngine`, `Database` (RAM copy) | the only mutator of the database and engine. It wakes on a queue message or on `nextWakeUtc()` |
| `storage` (core 0) | 1 | `IRepository`, `FsDoseLog`, LittleFS, SD | receives write jobs (log entry, db snapshot) through a queue. It writes in order and reports `StorageHealth` |
| `alert` (core 0) | 3 | buzzer LEDC, LED | receives `EngineEvent`s that are not stale; obeys `audible` |

Mutexes (FreeRTOS, priority inheritance):
1. **`g_spiMutex`**: the display and SD share SPI. It is held by the UI for one refresh, or by storage for one file
   operation. Never hold it while waiting on a queue.
2. **`g_i2cMutex`**: GT911, BQ27220 and PCF8563 share the bus. It is held per transaction.
3. **`g_dbMutex`**: protects `Database` and `DoseEngine`. Only the engine task takes it for writes. The UI takes it
   briefly to copy an `EngineView`. Lock order when nesting is unavoidable: `db → spi → i2c`. Storage never takes
   `g_dbMutex`: it receives already-serialised payloads, so a slow SD card cannot block alarms.

Ordering guarantees:
* The engine appends the log entry to the storage queue *before* acknowledging an action to the UI.
* The storage task writes the log before the snapshot. After a crash, the snapshot is never ahead of the log; the
  engine rebuilds from the snapshot, and `resolveLog` and the deterministic ids reconcile.
* The snapshot is saved after each user action and before deep sleep. On boot the device does restore, then
  `processUntil(now)` (catch-up), then shows the result.

The task split above was designed in Phase 2 and implemented in Phase 3 (see the table below).

## Phase 3: runtime, clock and UI

### Tasks (src/app/device_app.cpp)
| Task | Core | Prio | Job |
|---|---|---|---|
| input | 1 | 3 | 10 ms: HalGPIO::update, ButtonDecoder (click / 1 s hold-confirm / 2 s long / repeat / UP+DOWN combo), taps -> input queue |
| ui | 1 | 2 | input queue (250 ms timeout) -> UiController; render to PSRAM back buffer; diff vs front; RefreshPolicy; copy into the GfxRenderer frame buffer under the spi lock; latency log; flash, self-test, deep sleep |
| engine | 1 | 2 | every 1 s (divided by MT_DEV_CLOCK_SPEED): AppModel::process(); non-stale due/repeat/snooze-end events -> alert queue |
| storage | 0 | 1 | drains the dose-log queue to LittleFS, saves engine snapshots (after commands and every 5 min), polls battery / SD / health every 30 s |
| alerts | 0 | 3 | buzzer pattern (critical: 5 beeps), then asks the UI task for one visual flash |

Locks: db (recursive, AppModel + engine + database) -> spi (display + SD) -> i2c (Wire1: BQ27220 + PCF8563). Always taken in that order. Dose-log entries are copied into a mutex-protected deque (not a FreeRTOS queue: they hold a std::string).

### Clock
PCF8563 at 0x51 on Wire1 (SDA GPIO1 / SCL GPIO0), confirmed in the Seeed peripheral demo (I2C_NUM_1, PCF8563_I2C_ADDR 0x51). The RTC holds UTC. `core::evaluateClock` gives Trusted (RTC ok, VL clear), Untrusted (RTC unreadable or VL set but system time plausible, e.g. after deep sleep) or Unset. When Unset the UI forces the Set date and time screen and the engine is not started; saving writes the RTC and system time and starts the engine (snapshot restore, or a fresh start one hour back so doses within their window still alarm). `-DMT_DEV_CLOCK_SPEED=N` runs the device clock N times faster (ScaledClock); deep-sleep timers are converted back to real seconds.

### Display refresh
Pure rendering into our own 800x480 1-bit canvas (same pixels on host and device). `diffFrames` finds byte-aligned dirty rects (x and width multiples of 8, the SSD1677 window guard). HalDisplay has no windowed update (only the experimental EInkDisplay::displayWindow), so changed frames use FAST_REFRESH, which is the panel's differential update: unchanged pixels do not flash. Dirty rects decide skip / partial and are logged with the latency. RefreshPolicy: Full on the first frame and when forced, Half on screen-type change, Full after 20 partials and once a night after 03:00. The power-on FULL refresh and the screen-off-forces-HALF guard from the reference stay in HalDisplay.

### Input
Touch: InputManager reports taps on release only, so deliberate actions (TAKEN, LOG DOSE, Taken-at SAVE) need two taps within 5 s (the button turns into "Tap again to confirm"). Buttons: UP/DOWN move focus (or change the focused clock field), OK activates; for deliberate actions OK must be held 1 s, with the button filling up in quarters. OK long (2 s) on Home = sleep. UP+DOWN 3 s = hidden self-test (input task suspended, spi lock held).

### Fonts and icons
Atkinson Hyperlegible (SIL Open Font License 1.1, Braille Institute), rasterised to 1-bit by tools/gen_assets.py at 14/18/24/40 pt (150 dpi basis). Body text 18 pt; large-text mode raises body to 24 pt and is saved in NVS ("ui"/"large"). Icons are 40x40 bitmaps drawn by the same script (docs/screens/icons.png).

### Build flags
`-DMT_DEMO_DATA=1` seeds the 2-profile demo database (only when the database is empty and the clock is set) and an in-memory demo history. `-DMT_DEV_CLOCK_SPEED=N` (default 1). `-DMT_IDLE_SLEEP_MS` (default 5 min, battery only, Home screen, nothing due).

### Previews
`tools/render_previews.sh` runs the native UI suite with MT_SCREENS_DIR set and converts the frames to docs/screens/*.png.

## Phase 4: alarms, power and battery
- `core/alarm.*`: tone patterns (chime, beeps, urgent, gentle), volume to 10-bit LEDC duty, per-profile pattern/volume with a per-medication override (`alertPattern` 255 / `alertVolume` 0 = inherit; JSON keys are optional, so schema v2 data still loads), `planAlerts` (one alert per person, critical first then earliest, quiet hours, catch-up), `AlertOutput` + `AlarmManager`.
- The ladder itself is still in `DoseEngine` (per-medication `ladderIntervalMin`/`ladderCount`, defaults 5 min x 6 repeats, `graceMin` -> Missed, snooze 5/10/15, max 3). `Settings` gains `sleepOnUsb` and default ladder values.
- `alerts/outputs.*`: `BuzzerOutput` (GPIO48 PWM, pattern + duty, unavailable when the buzzer self-test failed, which gives the visual-only fallback) and `ScreenFlashOutput` (inversion, done by the UI task, which owns the spi lock).
- `core/power.*`: `planWake`, `sleepBlocker`, `BatteryMonitor`, `PowerCounters`. `board/power_stats.*` keeps the counters (RTC memory + NVS). `board/sleep_wake.*`: `enterDeepSleepEx` with buttons, USB edge (EXT0 GPIO9) and optional touch wake, plus `wakePin()`.
- Engine task -> `planAlerts` -> alert queue (POD `PersonAlert`) -> alert task -> `AlarmManager` (1.5 s between people). Storage task feeds `BatteryMonitor`; critical -> UI task shows the final screen and sleeps with AI-button wake only.
- Details and the battery estimate: docs/POWER.md.

## Phase 5: management UI, setup wizard, settings

- **Alert queue fix:** `core/alert_pending` keeps one slot per profile plus a system slot. A new alert for a person who already has a pending alert is merged into that slot (dose counts summed, critical/audible OR-ed, earliest time kept). Nothing is ever dropped. The FreeRTOS queue is gone; `postAlert`/`popAlert` run under `g_alertMx`.
- **Form framework (`ui_core/form.h`, `form_host`):** pages are `FormProvider`s, rendered by `FormHost`. A page has rows (Info/Text/Number/Time/Date/Choice/Toggle/Action), 4 rows of 78 px per page and a footer. Sub-editors are the keyboard, stepper, picker and PIN entry. Danger actions need two taps within 5 s. PIN gating happens when a page is pushed: 5 wrong entries lock entry for 60 s; the admin PIN opens everything; profile PINs open only their own person. Forms close and relock after 3 min idle (the wizard never closes).
- **Value entry:** since v1.0.2 the stepper is replaced by `ui_core/value_entry` (see the v1.0.2 section).
- **Keyboard (`ui_core/keyboard`):** US QWERTY with an accent layer (á … ÿ, ä ö ü ß), a one-shot Shift, a symbols layer and a numeric layer. Input is UTF-8 with byte limits; backspace removes whole code points. The text field and key area are fixed rectangles below the title bar. A keypress changes only those pixels and keeps the same screen type, so the refresh policy uses a FAST partial refresh (tested by frame diff).
- **AdminService (`app_core/admin_service`):** every edit is validated on a copy, swapped in under the database lock, followed by future-dose regeneration and a save. It covers medications, profiles (8 max), settings, time zone, manual clock, admin and profile PINs (salted hash, hardware RNG on the device), SD backup/restore (Wi-Fi password never exported and kept on restore), factory reset (wipe the internal store, empty DB; on the device also erase NVS and reboot) and storage/battery info.
- **Screens (`app_core/admin_screens`):** Menu, Medications list/editor (all fields, templates, multi-phase list, a phase editor for all 7 schedule types including PRN and cycles), People list/editor, Settings (time/TZ, sound, quiet hours, display, lock, battery, storage, backup/restore, Wi-Fi, software update, About, factory reset typed "RESET" plus a two-tap confirm) and the 8-step setup wizard.
- **Network hooks (`app_core/net_service.h`):** the Wi-Fi, time-sync and OTA screens call `INetService`. Since Phase 6 the device uses `net::WifiNetService`; `RadioOffNetService` remains the fallback when the Wi-Fi driver does not start.
- **Controller:** `ScreenId::Form` delegates to FormHost. Home has a Menu button. On first boot (or after a factory reset) the wizard opens before the clock screen.
- **Device:** AdminService gets locked adapters. The repo and internal flash use `g_storeMx`; the SD card uses the SPI bus lock and re-mounts when a card is inserted after boot. The DB lock is recursive. Settings > Display feeds `RefreshPolicy::configure` (full refresh every N, nightly on/off).

## Phase 6: Wi-Fi, time sync, web portal, OTA

Split the same way as earlier phases. Pure logic lives in `src/net_core/` and is host-tested (`test/test_net`, 19 tests). Thin ESP32 glue lives in `src/net/`.

| Module | Role |
|---|---|
| `net_core/sha256` | Streaming SHA-256 (FIPS 180-4 vectors in tests), hex parse, constant-time compare |
| `net_core/portal_security` | One-time code, sessions, CSRF, rate limit, LAN-only and Host allow-list checks |
| `net_core/api_json` | Strict validation of request bodies, plus JSON mapping that reuses `core/codec` (PIN hashes never leave the device) |
| `net_core/portal_api` | All routes, run over transport-independent `HttpRequest`/`HttpResponse` |
| `net_core/time_sync` | NTP packet build/parse (era-safe), sync policy, back-off, trust state |
| `net_core/ota_verify` | `OtaSession` checks the size, the ESP32-S3 header, the app descriptor (project `rxnudge`) and SHA-256 before commit |
| `net_core/notify` | Caregiver missed-dose message for ntfy or a webhook (JSON) |
| `net_core/wifi_creds` | SSID/password rules |
| `net_core/web_assets` | Gzipped `web/` files, generated by `tools/embed_web.py` |
| `net/wifi_net_service` | Device `INetService`: NVS credentials, worker task, hotspot portal, LAN portal (esp_http_server), NTP over UDP, esp_http_client with the CA bundle, `esp_ota_*` |

### Radio policy
- The radio is off by default. It turns on only for a job: NTP, a notification, the hotspot, the portal, or a connection test.
- After each job, `releaseRadio()` turns it off again unless the hotspot or the portal still needs it.
- `SleepBlocker::NetworkBusy` keeps the device awake while any job is queued or running, or while the hotspot or portal is open.
- `goToSleep()` calls `shutdownForSleep()`, so deep sleep always starts with the radio off.
- A deliberate sleep (long OK on Home) closes the portal and the hotspot first.
- Credentials are stored only in NVS namespace `net` (`ssid`, `pass`):
  - They never go into the database, backups or exports.
  - They are never logged.
  - `WiFi.persistent(false)` stops the Arduino layer from keeping a second copy.
  - The driver's INFO logs, which mention SSIDs, are lowered to WARN.

### Wi-Fi setup
- **Phone hotspot** (Settings → Wi-Fi, or wizard step 4):
  1. The device scans for networks.
  2. It opens AP `StickyMed-XXXX` with a random 8-character WPA2 password, shown on screen only.
  3. A DNS server answers every name with 192.168.4.1, so the phone's captive-portal check opens `setup.html`.
  4. The user picks a network from the list (or types a hidden SSID) and enters the password.
  5. The device tests the connection while the AP stays up. On success it turns on "Use Wi-Fi", syncs the time, and closes the hotspot 8 s later. On failure it restores the previous credentials, and the phone page shows the reason.
- The hotspot closes after 15 min without activity.
- **Manual entry** on the device: SSID, then password, on the on-screen keyboard.

### Time sync
- **When it runs** (`syncDue`):
  - At cold boot.
  - In the daily 04:00 wake slot (`WakeInputs.ntpEnabled`).
  - After any other wake if the last sync is more than 24 h old.
  - On demand ("Sync time now", or the wizard's "Get time from internet").
- **Back-off:** failures wait 15 min, doubling up to 6 h.
- **Servers:** `pool.ntp.org`, `time.cloudflare.com`, `time.google.com`, tried in that order. Each reply is checked: mode 4, LI ≠ 3, stratum 1–15, and a year between 2024 and 2099. Half the round-trip time is added.
- **Writing the time:** the result goes through `setClockUtc`, which writes the PCF8563 and the system time and restarts the engine.
- **Persistence:** the state (`tsLast`, `tsAtt`, `tsFail`, `tsCorr`) is kept in NVS.
- **Clock trust:** `StatusFlags.clockUntrusted` is also set after 7 days without a successful sync, but only when Wi-Fi is configured. This drives the existing "check the clock" indicator.

### Web portal (`http://rx-nudge.local`)
- **Starting and stopping:** started from Settings → Web portal only; there is no button-hold gesture because no free hold event exists. It stops after 10 min without requests, when the user stops it, or before sleep.
- **Access control:**
  - **No default credentials.** A 6-digit one-time code is shown on the device and replaced after each successful sign-in.
  - **Lock-out:** 5 wrong codes from one address lock it out for 5 min. 15 wrong codes in total replace the code.
  - **Sessions:** 128-bit token in an `HttpOnly; SameSite=Strict` cookie. Sessions expire after 15 min idle or 2 h total, with at most 3 at a time.
  - **CSRF:** every non-GET request needs the per-session `X-CSRF-Token`, and its `Origin` (if sent) must match the Host.
  - **Network checks:** only private, loopback or link-local IPv4 source addresses are accepted. The Host header must be `rx-nudge.local` or the device IP on port 80, which blocks DNS rebinding. Rate limit: 120 requests per minute per address.
  - **Response headers:** `Cache-Control: no-store`, CSP `default-src 'self'` (no inline script), `X-Frame-Options: DENY`, `nosniff`, `no-referrer`.
  - **No TLS:** stated on the sign-in page, on the device and in `/api/info`.
- **Pages:**
  - Medications: CRUD with multi-phase schedules (all 7 types), archive, and delete after archiving.
  - People: CRUD. PINs can be set on the device only.
  - History: filter by person and 7–90 days.
  - Data: CSV/JSON export (names are opt-in), backup download, and restore upload (fully validated before anything changes).
  - More: caregiver alert settings and the firmware update.
- **Size:** about 8.6 KB of gzipped assets in flash (index 0.9 KB, app.js 6.3 KB, setup 1.4 KB).
- **Edits:** the portal calls the same `AdminService` as the on-device editors, so it uses the same validation, lock, regeneration and save.

### OTA
- **Upload:** `POST /api/ota` sends the raw `firmware.bin` with header `X-Firmware-SHA256`. The value comes from `firmware.bin.sha256`, which the post-build script `scripts/pio_sha256.py` writes. The browser cannot compute it itself because WebCrypto needs HTTPS.
- **Writing and checks:** bytes stream in 4 KB chunks into the inactive slot (`esp_ota_begin/write`) while they are hashed. The header is checked as soon as 288 bytes have arrived: magic `0xE9`, chip ID 9 (ESP32-S3), app-descriptor magic, and project name `rxnudge`.
- **Finish:** the upload must have exactly the announced size and a matching SHA-256 (constant-time compare). `esp_ota_end` then checks the image structure, and `esp_ota_set_boot_partition` selects the new slot. The device restarts 1.5 s later, after flushing storage.
- **Rollback:** `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` is set (bootloader rebuilt). A new image boots as *pending verify*. `otaConfirmBoot()` runs at the end of `deviceAppStart()`, before any task can sleep. It marks the image valid once storage has mounted and the first frame is drawn; otherwise it calls `esp_ota_mark_app_invalid_rollback_and_reboot()`. A crash or reset before that point also makes the bootloader return to the previous slot.
- **Signing:** see `docs/OTA.md`.

### Caregiver alerts (opt-in)
- Off by default. The URL is set under Settings → Caregiver alert, or under More in the portal, and must be http(s).
- Missed doses from one engine pass go out as one message:
  - **ntfy.sh topic URL:** a text body with `Title`, `Priority: high` and `Tags: pill` headers.
  - **Any other URL:** a JSON `{"event":"missed_dose",...}` POST.
- Person and medication names are included only if "Include names" is on; otherwise the message carries only the person's initials and the scheduled time.
- HTTPS uses the IDF certificate bundle. The URL is never logged.

### Not done (future work)
- **Home Assistant MQTT discovery:** not done. It would add an MQTT client, keep the radio on or wake it often, and need broker settings. That conflicts with the on-demand radio policy and battery life. A cheaper later route: an HA webhook automation on the existing caregiver webhook.

### Partitions and size
- The app is 2.09 MB (up from 1.68 MB). WiFi, lwIP, mbedTLS with the CA bundle, esp_http_client/server and mDNS are now linked.
- Each OTA slot is 6.25 MB (`app0`/`app1` at 0x640000), leaving 4.1 MB of headroom. **No partition change is needed.**
- Changing `sdkconfig` rebuilt the bootloader (21 KB). **A device on the Phase 1–5 bootloader must be flashed over USB once** to get rollback; the bootloader is never updated over the air.

## Phase 7: v1.0 polish
- **Version:** `VERSION` is the only source. `CMakeLists.txt` sets `PROJECT_VER` from it; `scripts/pio_version.py`
  (PlatformIO pre-script) defines `MT_FW_VERSION`. `device_app.cpp` fails the build if the define is missing.
- **Strings:** all user-visible text is in `ui_core/strings.cpp`. `enum class S` covers the daily screens and the form
  host, keyboard, PIN and clock editors; `enum class A` covers every management/settings/portal-facing label in
  `admin_screens.cpp`. `ui::tr()` looks both up. Only separators, number formats, data values ("Custom", "RESET"
  confirmation word) and URL prefixes stay as literals.
- **Button text fit:** `ui::fitButton()` picks how a label is drawn in a fixed button: preferred font with icon, then
  without the icon, then body, then small font. `drawButton` always uses it, so no label is cut off (test
  `test_button_labels_fit` checks every action label in normal and large text; preview `today_large.png`).
- **Non-blocking network actions:** `INetService::startJob(NetJob)` + `jobState()` replace the blocking
  `syncTime()`/`testConnection()`. The Wi-Fi worker task runs the job; `jobState` returns running/elapsed/result with a
  sequence number. `FormHost::tick()` calls `FormProvider::poll()`; `admin_screens`' `JobWatch` shows
  "Syncing time... N s" / "Connecting... N s" in the row, disables the button while running and shows the result as a
  toast. The UI and alarms keep running meanwhile.
- **Touch target sizes (235 ppi = 9.25 px/mm):**

| Element | Size | mm |
|---|---|---|
| Form rows | 78 px | 8.4 |
| Keyboard keys | 80 x 84 px | 8.65 x 9.1 |
| Self-test menu rows | 2 columns x 5 rows of 84 px | 9.1 tall |

  Keyboard keys cannot be 9 mm wide: a QWERTY row has 10 keys and the panel is 800 px (86.5 mm) wide, so the maximum
  is 80 px = 8.65 mm with zero gaps. Keys are 9.1 mm tall, and there is no gap between keys, so a touch slightly off
  centre still lands on the nearest key.
- **Clean-up:** unused `radio::wifiActive` and the empty `ClockFields` string were removed. Our code builds without
  warnings on host and device.

## v1.0.1: nightly automatic SD backups
- `storage/auto_backup.*` (pure, `test/test_backup`):
  - `dueSlotDay`: the slot of local day D is D 03:00 (`kAutoBackupSlotMinute`, the existing nightly refresh wake that
    `planWake` always schedules). Before 03:00 the due slot is yesterday's, so a slot missed while asleep or off is
    caught up at the next wake, boot or 30 s status poll — once, not once per missed night.
  - `decideAutoBackup`: None (toggle off, clock unset, slot handled, failure < 1 h ago) / WaitBattery (critical) /
    WaitCard / Run. Waiting does not consume the slot.
  - `runAutoBackup`: builds the JSON only when due (same `makeBackup` as manual backups; no Wi-Fi data, which lives in
    NVS). CRC32 fingerprint: written when changed or ≥ 7 days since the last success, else `Unchanged`. Written with
    `writeBackupFile` (tmp, read-back verify, rename) as `auto-YYYYMMDD.json` (local date). A failed write removes the
    `.tmp`, sets `Failed` and retries hourly. After a write, `autoBackupsToPrune` deletes auto files beyond the newest
    14; only names matching `auto-NNNNNNNN.json` are ever deleted.
- Device (`app/device_app.cpp`): the storage task runs `runNightlyBackup()` after each status poll. The JSON copy is
  taken under the DB lock; SD access goes through `g_lockedSd` (SPI lock per file operation), so the engine and alert
  tasks never wait on the card. `g_backupBusy` blocks deep sleep while it runs. State (`last`, `fp`, `day`, `att`,
  `st`, `name`) is in NVS namespace `abk`.
- UI: `Settings.autoBackup` (database, default true, missing key = true). Settings > Backup & restore shows the toggle,
  "Last automatic" and the status. Banners: `BackupFailed` ("Backup failed - check SD card", below SdError);
  `SdMissing` only when automatic backups are on. `listBackups` lists manual and auto files newest first by date.

## v1.0.2: date, time and number entry
- `ui_core/value_entry.*` (pure, tested in test_ui) replaces the -7/-1/+1/+7 stepper everywhere: form Number, Time,
  Date and the new DateTime field (`num = day * 1440 + minute`), the forced clock screen and the backfill screen.
- Fields: DD MM YYYY and/or HH : MM, or one number. Each is a box (116 px, year 224 px, 100 px tall, 40 pt digits)
  with 84 px (9.1 mm) arrow buttons above and below. Buttons: UP/DOWN step the focused field, OK moves on; after the
  last field Save has the focus. Tapping a box switches the arrows for a 2 x 6 number pad (keys 121 x 84 px; a
  "+/-" key for signed numbers). Typing auto-advances after 2 digits (4 for the year) or as soon as no further digit
  can fit (month 2-9, hour 3-9, day 4-9).
- The day the user chose is kept (`wantedDay_`) and clamped to the month length whenever month or year change, so
  31 Jan -> Feb 2028 shows 29 and March brings back 31. Minutes/hours/day/month wrap; the year clamps to its range.
- Defaults (`defaultClockValue`): the RTC when valid (not `clockUnset` and year >= 2025), otherwise the firmware
  build time (`__DATE__`/`__TIME__` of value_entry.cpp; a clean build refreshes it).
- Wizard clock step: with Wi-Fi configured the NTP job starts automatically on entering the step and "Get time from
  internet" is the first, filled row; manual entry is "Or set by hand". Next is refused while the clock is unset (the
  wizard never sets the clock from a default). Settings > Time has one "Set date and time" row that sets the clock on
  Save.
- Backfill: HH : MM; the instant is resolved to yesterday/today/tomorrow's date nearest the previous choice inside
  [scheduled - 60 min, now] and clamped there.
- Refresh: the entry keeps the same screen type, so the refresh policy diffs the frames and updates only the changed
  box (and the result line) with a FAST partial refresh (test `test_entry_targets_and_partial_refresh`).

## v1.0.4: medication times and touch input

**Custom times (bug).** Root cause: `FormHost` built hit ids as `1000 + field id` for rows and `1100 + index` for
footer buttons. Field ids >= 100 (list items `ListItem0 = 100`, phase rows `PlPhase0 = 100`, backup items) therefore
collided with the footer: tapping "1: from start" in Schedule hit footer button 0 ("Done") and popped back, so the
phase editor with the HH:MM rows could never be reached by touch (tapping the first medication in the list opened
"Add medication" instead). Host tests used `activateField()` (by id, no hit test), so they passed. Even when reached,
the time row sat on page 2 of the phase editor, three levels down. Fixes: hit bases 10000/20000/30000
(`FormHost::kHitRowBase` etc.); the medication editor shows a **Times** list right under Dose (`Time n`, tap =
`TimeEditPage`: anchor = fixed clock time or a routine + offset, HH:MM entry, delete) and **+ Add time** (a Time
field that opens the HH:MM entry directly). The phase editor uses the same list; "Times per day" is gone. Adding a
fixed time to a "daily by routine" rule turns it into "daily at times" (anchors remain per time; the validator and
the scheduler already resolved anchors per `DoseTime`). Tests drive the UI with coordinate taps (`Tapper`).

**Touch.** v1.0.2 (and sticky-micronotes, whose `Gt911Touch::poll()` we had copied) returned "not down" whenever the
GT911 buffer-status bit (0x814E bit 7) was clear. That bit only means "no new report"; polling every 10 ms between
the chip's reports produced down/up/down/up, i.e. several taps per touch. Seeed's `gt911.cpp` returns 0 points for
a clear bit, treats count 0 as release, reads 8 bytes per point from 0x8150 and writes 0 to 0x814E after every read
(its demo polls every 200 ms and has no release logic). Now:
- `Gt911Touch::read()` → Stale (bit clear / I2C error), Up (ready, count 0), Down (count, track id from 0x814F,
  x/y); 0x814E is cleared after every ready report; count > 5 is discarded.
- `ui_core/touch_filter` `TouchFilter` (pure, host tested): one tap per touch at release, at the down position;
  press < `kTouchMinPressMs` (30) ignored; a down within `kTouchMinUpMs` (70) of the last up is the same touch
  (bounce); Stale for `kTouchStaleReleaseMs` (250) while down = missed up report; two fingers or a move >
  `kTouchSlopPx` (60) is not a tap; `guard()` ignores the current touch and any touch starting less than
  `kTouchGuardMs` (280) after the finger is up.
- After each render, `UiController::touchLayoutKey()` (hash of the hit targets) is compared by `TapGate`; on a change
  the input task's filter is guarded and queued taps detected before the new frame reached the panel are dropped.
  Typing on the keyboard or number pad keeps the layout, so it is not slowed down.
- Taps are the only touch events; there is no touch auto-repeat (UP/DOWN button repeat is unchanged).
