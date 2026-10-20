#include "app/device_app.h"

#include <Arduino.h>
#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>

#include "alerts/alert.h"
#include "alerts/buzzer.h"
#include "alerts/outputs.h"
#include "app/app_storage.h"
#include "app/device_clock.h"
#include "app_core/admin_screens.h"
#include "app_core/admin_service.h"
#include "app_core/app_model.h"
#include "app_core/net_service.h"
#include "app_core/app_port.h"
#include "board/battery.h"
#include "board/board.h"
#include "board/bus_locks.h"
#include "board/log.h"
#include "board/power_stats.h"
#include "core/alarm.h"
#include "core/alert_pending.h"
#include "core/power.h"
#include "storage/auto_backup.h"
#include "app_core/format.h"
#include "ui_core/assets/sleep_logo.h"
#include "ui_core/strings.h"
#include "core/demo_data.h"
#include "net/wifi_net_service.h"
#include "ui/selftest.h"
#include "ui_core/canvas.h"
#include "ui_core/controller.h"
#include "ui_core/input.h"
#include "ui_core/refresh.h"
#include "ui_core/touch_filter.h"

#ifndef MT_FW_VERSION
#error "MT_FW_VERSION comes from the VERSION file via scripts/pio_version.py"
#endif

namespace app {
namespace {

// ------------------------------------------------------------------ shared state
DeviceClock g_clock;
SemaphoreHandle_t g_dbMx = nullptr;     // guards g_model / database / engine
SemaphoreHandle_t g_logMx = nullptr;    // guards g_pendingLog
SemaphoreHandle_t g_storeMx = nullptr;  // serialises flash writes (storage task vs. pre-sleep flush)
AppModel* g_model = nullptr;
std::deque<core::DoseLogEntry> g_pendingLog;
QueueHandle_t g_inputQ = nullptr;
SemaphoreHandle_t g_alertMx = nullptr;  // guards g_pendingAlerts
core::AlertPending g_pendingAlerts;    // one slot per person: never overflows, coalesces
TaskHandle_t g_inputTask = nullptr, g_uiTask = nullptr, g_engineTask = nullptr, g_storageTask = nullptr,
             g_alertTask = nullptr;
std::atomic<bool> g_snapshotReq{false};
std::atomic<bool> g_engineChanged{false};
std::atomic<bool> g_alertBusy{false};
std::atomic<bool> g_shutdownReq{false};
std::atomic<int> g_missedNotice{0};
alerts::BuzzerOutput g_buzzer;
alerts::ScreenFlashOutput g_flash;
core::AlarmManager g_alarms;
core::BatteryMonitor g_battery;
net::WifiNetService* g_net = nullptr;  // null when the Wi-Fi driver did not start
bool g_timerWake = false;  // woke by the RTC timer: go back to sleep quickly if nothing happens

constexpr uint32_t kSnapshotEveryMs = 5UL * 60UL * 1000UL;
constexpr uint32_t kStatusEveryMs = 30UL * 1000UL;

class DbLock {
 public:
  DbLock() { xSemaphoreTakeRecursive(g_dbMx, portMAX_DELAY); }
  ~DbLock() { xSemaphoreGiveRecursive(g_dbMx); }
};


// AppModel -> storage task. Entries are copied (they contain a std::string) under a mutex.
class QueueLogSink : public core::IDoseLogSink {
 public:
  bool append(const core::DoseLogEntry& e) override {
    xSemaphoreTake(g_logMx, portMAX_DELAY);
    g_pendingLog.push_back(e);
    xSemaphoreGive(g_logMx);
    if (g_storageTask) xTaskNotifyGive(g_storageTask);
    return true;
  }
} g_logSink;

void postAlert(const core::PersonAlert& a) {
  xSemaphoreTake(g_alertMx, portMAX_DELAY);
  const bool merged = g_pendingAlerts.push(a);
  xSemaphoreGive(g_alertMx);
  if (merged) MT_LOGF("alerts: coalesced alert for profile %u", a.profileId);
  if (g_alertTask) xTaskNotifyGive(g_alertTask);
}

void startOrRestoreEngine() {
  DbLock l;
  if (g_model->engineRunning()) return;
  core::EngineSnapshot s;
  storage::IRepository* repo = repository();
  if (repo && repo->loadSnapshot(s) == storage::LoadStatus::Ok) {
    g_model->restoreEngine(s);
    MT_LOGF("engine: restored snapshot (%u instances)", static_cast<unsigned>(s.instances.size()));
  } else {
    // No snapshot (first start): begin an hour back so doses inside their window still alarm.
    g_model->startEngine(g_clock.nowUtc() - core::kEarlyTakeWindowSec);
    MT_LOGF("engine: fresh start");
  }
  g_snapshotReq = true;
}

// ------------------------------------------------------------------ device port
class DevicePort : public IAppPort {
 public:
  HomeVM home(int page, int lpp) override {
    DbLock l;
    return g_model->home(page, lpp);
  }
  TodayVM today(uint8_t p) override {
    DbLock l;
    return g_model->today(p);
  }
  DoseDetailVM detail(uint64_t id) override {
    DbLock l;
    return g_model->detail(id);
  }
  AlarmVM alarm() override {
    DbLock l;
    if (!g_model->engineRunning()) return AlarmVM();
    return g_model->alarm();
  }
  PrnVM prn(uint8_t p) override {
    DbLock l;
    return g_model->prn(p);
  }
  HistoryVM history(uint8_t p, HistoryMode m, int32_t a, bool pm) override {
    DbLock l;
    return g_model->history(p, m, a, pm);
  }
  CommandResult execute(const DoseCommand& c) override {
    CommandResult r;
    {
      DbLock l;
      if (!g_model->engineRunning()) {
        r.result = core::ActionResult::InvalidState;
        return r;
      }
      r = g_model->execute(c);
    }
    g_snapshotReq = true;
    if (g_storageTask) xTaskNotifyGive(g_storageTask);
    return r;
  }
  bool lastActionUndoable() override {
    DbLock l;
    return g_model->lastActionUndoable();
  }
  DoseCommand lastAction() override {
    DbLock l;
    return g_model->lastAction();
  }
  StatusFlags status() override {
    DbLock l;
    return g_model->status;
  }
  core::LocalTime localNow() override {
    DbLock l;
    return core::utcToLocal(g_model->tz(), g_clock.nowUtc());
  }
  int64_t nowUtc() override { return g_clock.nowUtc(); }
  bool clockUnset() override {
    DbLock l;
    return g_model->status.clockUnset;
  }
  bool setClockLocal(int y, int mo, int d, int h, int mi) override {
    int64_t utc = 0;
    {
      DbLock l;
      if (!localToUtcChecked(g_model->tz(), y, mo, d, h, mi, utc)) return false;
    }
    return setClockUtc(utc);
  }
  bool setClockUtc(int64_t utc) {
    const bool rtcOk = g_clock.set(utc);
    {
      DbLock l;
      g_model->status.clockUnset = false;
      g_model->status.clockUntrusted = false;
      g_model->status.alarmHwFault = g_model->status.alarmHwFault || !rtcOk;
    }
    startOrRestoreEngine();
    g_engineChanged = true;
    return true;
  }
} g_port;

// ------------------------------------------------------------------ tasks
void inputTask(void*) {
  ui::ButtonDecoder dec;
  ui::InputEvent ev[4];
  HalGPIO& io = board::io();
  for (;;) {
    io.update();
    const uint32_t now = millis();
    const int n = dec.update(now, io.isPressed(HalGPIO::BTN_UP), io.isPressed(HalGPIO::BTN_DOWN),
                             io.isPressed(HalGPIO::BTN_POWER), ev, 4);
    for (int i = 0; i < n; i++) xQueueSend(g_inputQ, &ev[i], 0);
    int x = 0, y = 0;
    if (io.consumeTap(x, y)) {
      ui::InputEvent t;
      t.type = ui::InputType::Tap;
      t.x = static_cast<int16_t>(x);
      t.y = static_cast<int16_t>(y);
      t.atMs = now;
      xQueueSend(g_inputQ, &t, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(MT_TOUCH_POLL_MS));
  }
}

void engineTask(void*) {
  const uint32_t periodMs = std::max(50, 1000 / g_clock.speed());
  uint32_t lastSnapshot = millis();
  for (;;) {
    std::vector<core::EngineEvent> events;
    core::AlertPlan plan;
    {
      DbLock l;
      if (g_model->engineRunning() && !g_model->status.clockUnset) {
        events = g_model->process();
        if (!events.empty())
          plan = core::planAlerts(events, g_model->engine(), g_model->db(), g_model->tz(), g_clock.nowUtc());
      }
    }
    if (!events.empty()) g_engineChanged = true;
    // one alert per person, in queue order (critical first, then earliest)
    for (const core::PersonAlert& a : plan.alerts) {
      if (a.catchUp) board::powerCounters().catchUpAlerts++;
      MT_LOGF("alarm: profile=%u doses=%u step=%u audible=%d critical=%d catchup=%d pattern=%s vol=%u", a.profileId,
              a.doseCount, a.alarmIndex, a.audible, a.critical, a.catchUp, core::patternName(a.pattern), a.volume);
      postAlert(a);
    }
    if (g_net) {
      std::vector<net::MissedDose> missed;
      {
        DbLock l;
        for (const auto& e : events) {
          if (e.kind != core::EngineEventKind::BecameMissed) continue;
          for (const auto& d : g_model->engine().instances()) {
            if (d.eventId != e.eventId) continue;
            net::MissedDose m;
            m.profileId = d.profileId;
            m.scheduledUtc = d.scheduledUtc;
            if (const core::Profile* p = g_model->db().profile(d.profileId)) {
              m.person = p->name;
              m.initials = p->initials;
            }
            if (const core::Medication* md = g_model->db().med(d.medId)) m.med = md->name;
            missed.push_back(m);
            break;
          }
        }
      }
      g_net->notifyMissed(missed);  // no-op unless the caregiver alert is enabled
    }
    if (plan.missed > 0) {
      board::powerCounters().missedWhileAsleep += static_cast<uint32_t>(plan.missed);
      bool stale = false;
      for (const auto& e : events) stale = stale || (e.stale && e.kind == core::EngineEventKind::BecameMissed);
      if (stale) g_missedNotice += plan.missed;  // missed while asleep / not processed: tell the user
    }
    if (millis() - lastSnapshot >= kSnapshotEveryMs) {
      lastSnapshot = millis();
      g_snapshotReq = true;
      xTaskNotifyGive(g_storageTask);
    }
    vTaskDelay(pdMS_TO_TICKS(periodMs));
  }
}

bool popAlert(core::PersonAlert& a) {
  xSemaphoreTake(g_alertMx, portMAX_DELAY);
  const bool ok = g_pendingAlerts.pop(a);
  if (ok) g_alertBusy = true;  // set under the lock so the sleep gate never sees a gap
  xSemaphoreGive(g_alertMx);
  return ok;
}

int pendingAlertCount() {
  xSemaphoreTake(g_alertMx, portMAX_DELAY);
  const int n = g_pendingAlerts.size();
  xSemaphoreGive(g_alertMx);
  return n;
}

void alertTask(void*) {
  core::PersonAlert a;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    while (popAlert(a)) {
      const core::AlarmManager::Result r = g_alarms.deliver(a);
      MT_LOGF("alerts: profile=%u sounded=%d shown=%d", a.profileId, r.sounded, r.shown);
      vTaskDelay(pdMS_TO_TICKS(1500));  // gap between people
    }
    g_alertBusy = false;
  }
}

void drainLogAndSnapshot() {
  xSemaphoreTake(g_storeMx, portMAX_DELAY);
  std::deque<core::DoseLogEntry> batch;
  xSemaphoreTake(g_logMx, portMAX_DELAY);
  batch.swap(g_pendingLog);
  xSemaphoreGive(g_logMx);
  storage::FsDoseLog* log = doseLog();
  for (const auto& e : batch)
    if (!log || !log->append(e)) MT_LOGF("storage: log append failed (event id %llu)", static_cast<unsigned long long>(e.eventId));
  if (g_snapshotReq.exchange(false) && repository()) {
    core::EngineSnapshot s;
    bool have = false;
    {
      DbLock l;
      if (g_model->engineRunning()) {
        s = g_model->engine().snapshot();
        have = true;
      }
    }
    if (have && !repository()->saveSnapshot(s)) MT_LOGF("storage: snapshot save failed");
  }
  xSemaphoreGive(g_storeMx);
}

// ---- nightly automatic backup state (NVS "abk"; read by Settings > Backup and the banner)
SemaphoreHandle_t g_abMx = nullptr;
storage::AutoBackupState g_abState;
std::atomic<bool> g_backupBusy{false};
std::atomic<AdminService*> g_admin{nullptr};

storage::AutoBackupState autoBackupState() {
  if (!g_abMx) return {};
  xSemaphoreTake(g_abMx, portMAX_DELAY);
  storage::AutoBackupState s = g_abState;
  xSemaphoreGive(g_abMx);
  return s;
}

void loadAutoBackupState() {
  storage::AutoBackupState s;
  Preferences p;
  if (p.begin("abk", true)) {
    s.lastBackupUtc = p.getLong64("last", 0);
    s.lastFingerprint = p.getUInt("fp", 0);
    s.lastSlotDay = p.getInt("day", -1);
    s.lastAttemptUtc = p.getLong64("att", 0);
    s.status = static_cast<storage::AutoBackupStatus>(p.getUChar("st", 0));
    s.lastName = p.getString("name", "").c_str();
    p.end();
  }
  xSemaphoreTake(g_abMx, portMAX_DELAY);
  g_abState = s;
  xSemaphoreGive(g_abMx);
}

void saveAutoBackupState(const storage::AutoBackupState& s) {
  Preferences p;
  if (!p.begin("abk", false)) return;
  p.putLong64("last", s.lastBackupUtc);
  p.putUInt("fp", s.lastFingerprint);
  p.putInt("day", s.lastSlotDay);
  p.putLong64("att", s.lastAttemptUtc);
  p.putUChar("st", static_cast<uint8_t>(s.status));
  p.putString("name", s.lastName.c_str());
  p.end();
}

void pollStatus() {
  const bool ntpStale = g_net && g_net->clockStale();  // outside DbLock (reads settings itself)
  const bool radio = g_net && g_net->busy();
  const board::BatteryReading b = board::readBattery();
  const bool sdPresent = board::sdCardDetected();
  const StorageSummary& st = storageSummary();
  storage::FsDoseLog* log = doseLog();
  DbLock l;
  StatusFlags& s = g_model->status;
  s.batteryPct = b.gaugeOk ? b.socPercent : -1;
  s.charging = b.externalPower && b.currentMa > 0;
  const core::BatteryUpdate bu = g_battery.update(b.gaugeOk, b.socPercent, b.externalPower);
  s.usbPowered = b.externalPower;
  s.lowBattery = bu.level >= core::BatteryLevel::Low;
  s.veryLowBattery = bu.level >= core::BatteryLevel::VeryLow;
  if (bu.changed)
    MT_LOGF("battery: level=%s soc=%d mv=%d usb=%d", core::batteryLevelName(bu.level), b.socPercent, b.millivolts,
            b.externalPower);
  if (bu.alertNow) {
    core::PersonAlert a;
    a.profileId = core::kSystemProfile;
    a.pattern = core::PatternId::Urgent;
    a.volume = 80;
    a.critical = true;  // the battery alert ignores quiet hours: reminders are about to stop
    postAlert(a);
  }
  if (bu.shutdownNow) g_shutdownReq = true;
  s.sdPresent = sdPresent;
  s.sdError = sdPresent && !st.sdMounted;
  s.storageError = !st.internalMounted || !st.healthy || (log && log->failedAppends() > 0);
  s.clockUntrusted = g_clock.trust() == core::ClockTrust::Untrusted || ntpStale;
  s.alarmHwFault = s.alarmHwFault || (MT_BUZZER_ENABLED && alerts::buzzerTested() &&
                                      alerts::alertMode() == alerts::AlertMode::VisualOnly);
  s.wifiOn = radio;
  s.autoBackupOn = g_model->db().settings.autoBackup;
  s.backupFailed = storage::autoBackupFailed(autoBackupState(), s.autoBackupOn);
}

void runNightlyBackup();

void storageTask(void*) {
  uint32_t lastStatus = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    drainLogAndSnapshot();
    if (lastStatus == 0 || millis() - lastStatus >= kStatusEveryMs) {
      lastStatus = millis();
      pollStatus();
      runNightlyBackup();  // after pollStatus: SD mount and battery level are fresh
      pollStatus();        // banner reflects the result
      g_engineChanged = true;  // status icons may have changed
    }
  }
}

// ------------------------------------------------------------------ admin (editors, settings, wizard)
class DeviceDbLock : public IDbLock {
 public:
  void lock() override { xSemaphoreTakeRecursive(g_dbMx, portMAX_DELAY); }
  void unlock() override { xSemaphoreGiveRecursive(g_dbMx); }
} g_adminLock;

// Serialises internal-flash writes with the storage task (g_storeMx).
class StoreLock {
 public:
  StoreLock() { xSemaphoreTake(g_storeMx, portMAX_DELAY); }
  ~StoreLock() { xSemaphoreGive(g_storeMx); }
};

class LockedRepo : public storage::IRepository {
 public:
  storage::LoadStatus loadDatabase(core::Database& db) override {
    StoreLock l;
    return repository()->loadDatabase(db);
  }
  bool saveDatabase(const core::Database& db) override {
    StoreLock l;
    return repository()->saveDatabase(db);
  }
  storage::LoadStatus loadSnapshot(core::EngineSnapshot& s) override {
    StoreLock l;
    return repository()->loadSnapshot(s);
  }
  bool saveSnapshot(const core::EngineSnapshot& s) override {
    StoreLock l;
    return repository()->saveSnapshot(s);
  }
  const storage::StorageHealth& health() const override { return repository()->health(); }
} g_lockedRepo;

// Internal flash (g_storeMx) or SD card (SPI bus lock) with per-call locking.
class LockedFs : public storage::IFileSystem {
 public:
  explicit LockedFs(bool sd) : sd_(sd) {}
  bool mounted() const override {
    storage::IFileSystem* f = fs();
    if (sd_ && !f->mounted() && board::sdCardDetected() && millis() - lastTry_ > 5000) {
      lastTry_ = millis();
      board::BusLock spi(board::spiMutex());
      sdRemount();
    }
    return f->mounted();
  }
  bool exists(const std::string& p) override { return run([&] { return fs()->exists(p); }); }
  bool readFile(const std::string& p, std::string& out) override { return run([&] { return fs()->readFile(p, out); }); }
  bool writeFile(const std::string& p, const std::string& d) override { return run([&] { return fs()->writeFile(p, d); }); }
  bool appendFile(const std::string& p, const std::string& d) override { return run([&] { return fs()->appendFile(p, d); }); }
  bool remove(const std::string& p) override { return run([&] { return fs()->remove(p); }); }
  bool rename(const std::string& a, const std::string& b) override { return run([&] { return fs()->rename(a, b); }); }
  bool mkdirs(const std::string& p) override { return run([&] { return fs()->mkdirs(p); }); }
  std::vector<std::string> list(const std::string& dir) override {
    std::vector<std::string> out;
    run([&] {
      out = fs()->list(dir);
      return true;
    });
    return out;
  }

 private:
  storage::IFileSystem* fs() const { return sd_ ? sdFs() : internalFs(); }
  template <typename F>
  bool run(F f) {
    if (sd_) {
      board::BusLock spi(board::spiMutex());
      return f();
    }
    StoreLock l;
    return f();
  }
  bool sd_;
  mutable uint32_t lastTry_ = 0;
};
LockedFs g_lockedInternal(false), g_lockedSd(true);

// Storage task only. Alarms are never blocked: the engine and alert tasks keep running; the
// JSON is built under the DB lock (copy only) and the SD is written through g_lockedSd, which
// takes the SPI lock per file operation.
void runNightlyBackup() {
  AdminService* admin = g_admin.load();
  if (!admin || !g_abMx) return;
  storage::AutoBackupInputs in;
  core::PosixTz tz;
  {
    DbLock l;
    in.enabled = g_model->db().settings.autoBackup;
    tz = g_model->tz();
  }
  in.nowUtc = g_clock.nowUtc();
  in.clockValid = !g_port.clockUnset();
  in.sdMounted = g_lockedSd.mounted();
  in.criticalBattery = g_battery.level() >= core::BatteryLevel::Critical;
  storage::AutoBackupState st = autoBackupState();
  const storage::AutoBackupState before = st;
  g_backupBusy = true;
  const storage::AutoBackupResult r =
      storage::runAutoBackup(tz, in, st, &g_lockedSd, [admin]() { return admin->backupJson(); });
  g_backupBusy = false;
  const bool changed = st.status != before.status || st.lastSlotDay != before.lastSlotDay ||
                       st.lastBackupUtc != before.lastBackupUtc || st.lastAttemptUtc != before.lastAttemptUtc;
  if (!changed) return;
  xSemaphoreTake(g_abMx, portMAX_DELAY);
  g_abState = st;
  xSemaphoreGive(g_abMx);
  saveAutoBackupState(st);
  MT_LOGF("backup: auto %s wrote=%d pruned=%d prune_failed=%d %s", storage::autoBackupStatusName(r.status), r.wrote,
          r.pruned, r.pruneFailed, r.wrote ? st.lastName.c_str() : "");
}

void factoryResetDevice() {
  MT_LOGF("admin: factory reset - erasing NVS and restarting");
  board::BusLock spi(board::spiMutex());  // no display refresh in flight
  nvs_flash_erase();
  delay(200);
  esp_restart();
}

// ------------------------------------------------------------------ UI task
struct UiRuntime {
  std::unique_ptr<ui::UiController> ctl;
  uint8_t* back = nullptr;
  uint8_t* front = nullptr;
  ui::RefreshPolicy policy;
  ui::LatencyStats latency;
  std::vector<ui::Rect> rects;
  uint32_t lastActivity = 0;
  ui::TapGate tapGate;  // drops taps aimed at a screen that has since changed
  int lastMinute = -1;
  bool largeSaved = false;
};
UiRuntime g_ui;

uint8_t* allocFrame() {
  void* p = heap_caps_malloc(ui::Canvas::kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) p = heap_caps_malloc(ui::Canvas::kBytes, MALLOC_CAP_8BIT);
  return static_cast<uint8_t*>(p);
}

HalDisplay::RefreshMode toMode(ui::RefreshKind k) {
  switch (k) {
    case ui::RefreshKind::Full: return HalDisplay::FULL_REFRESH;
    case ui::RefreshKind::Half: return HalDisplay::HALF_REFRESH;
    default: return HalDisplay::FAST_REFRESH;
  }
}

void renderAndRefresh(uint32_t inputAtMs, bool force) {
  ui::Canvas canvas(g_ui.back);
  g_ui.ctl->render(canvas);
  ui::diffFrames(g_ui.front, g_ui.back, g_ui.rects);
  const core::LocalTime lt = g_port.localNow();
  const ui::RefreshKind k =
      g_ui.policy.decide(!g_ui.rects.empty(), g_ui.ctl->screenType(), lt.days, lt.secOfDay / 60, force);
  const uint32_t layout = g_ui.ctl->touchLayoutKey();
  if (k == ui::RefreshKind::None) {
    if (g_ui.tapGate.rendered(layout, millis())) board::io().touchGuard();
    return;
  }
  {
    board::BusLock spi(board::spiMutex());
    memcpy(board::gfx().getFrameBuffer(), g_ui.back, ui::Canvas::kBytes);
    board::gfx().displayBuffer(toMode(k));
  }
  // New touch layout on the panel: the finger that caused it (or any touch made during the
  // refresh) must not click through to the new controls.
  if (g_ui.tapGate.rendered(layout, millis())) board::io().touchGuard();
  memcpy(g_ui.front, g_ui.back, ui::Canvas::kBytes);
  if (inputAtMs) {
    const uint32_t ms = millis() - inputAtMs;
    g_ui.latency.add(ms);
    MT_LOGF("ui: %s %s rects=%u area=%d latency=%u ms (avg %u, p90 %u, n=%u)", ui::screenName(g_ui.ctl->screen()),
            ui::refreshKindName(k), static_cast<unsigned>(g_ui.rects.size()), ui::area(g_ui.rects), ms, g_ui.latency.avgMs(),
            g_ui.latency.p90Ms(), g_ui.latency.count());
  } else {
    MT_LOGF("ui: %s %s rects=%u", ui::screenName(g_ui.ctl->screen()), ui::refreshKindName(k),
            static_cast<unsigned>(g_ui.rects.size()));
  }
}

void saveLargeText(bool on) {
  Preferences p;
  if (p.begin("ui", false)) {
    p.putBool("large", on);
    p.end();
  }
}

bool pendingWrites() {
  xSemaphoreTake(g_logMx, portMAX_DELAY);
  const bool p = !g_pendingLog.empty();
  xSemaphoreGive(g_logMx);
  return p || g_snapshotReq.load() || g_backupBusy.load();
}

void flushForSleep() {
  vTaskSuspend(g_inputTask);
  vTaskSuspend(g_engineTask);
  g_snapshotReq = true;
  drainLogAndSnapshot();
}

[[noreturn]] void goToSleep(const char* why, bool userOff = false) {
  const bool ntp = g_net && g_net->configured();
  if (g_net) g_net->shutdownForSleep();
  if (userOff) {
    // Power-button hold: the RxNudge logo stays on the e-paper while the device
    // is off. The frame lives in flash; it is copied straight into the panel buffer.
    MT_LOGF("ui: drawing sleep logo");
    board::BusLock spi(board::spiMutex());
    memcpy(board::gfx().getFrameBuffer(), ui::kSleepLogoFrame, ui::Canvas::kBytes);
    board::gfx().displayBuffer(HalDisplay::FULL_REFRESH);
    memcpy(g_ui.front, ui::kSleepLogoFrame, ui::Canvas::kBytes);  // keep the diff base in step with the panel
  }
  flushForSleep();
  core::WakeInputs in;
  bool sleepOnUsb = false;
  {
    DbLock l;
    in.nowUtc = g_clock.nowUtc();
    in.engineRunning = g_model->engineRunning();
    if (in.engineRunning) in.engineNextUtc = g_model->engine().nextWakeUtc();
    in.nightlyRefreshMinute = 3 * 60;
    in.ntpEnabled = ntp && g_model->db().settings.autoTimeSync;
    sleepOnUsb = g_model->db().settings.sleepOnUsb;
  }
  const core::WakePlan plan = core::planWake(g_model->tz(), in);
  const uint32_t secs = static_cast<uint32_t>(std::max<int64_t>(g_clock.realSecondsUntil(plan.wakeUtc), core::kMinSleepSec));
  board::setLastSleepTargetUtc(plan.targetUtc);
  board::powerCounters().recordSleep(secs, millis());
  board::powerCountersSave();
  MT_LOGF("ui: deep sleep (%s): wake in %lu s for %s (target %lld)", why, static_cast<unsigned long>(secs),
          core::wakeReasonName(plan.reason), static_cast<long long>(plan.targetUtc));
  board::SleepConfig c;
  c.timerSeconds = secs;
  c.upDown = true;
  c.touch = true;
  c.usbEdge = true;
  (void)sleepOnUsb;
  board::BusLock spi(board::spiMutex());
  board::enterDeepSleepEx(c);
}

// <= 5 %: final screen stays on the e-paper, then deep sleep with button wake only.
[[noreturn]] void batteryShutdown() {
  MT_LOGF("battery: critical, shutting down");
  g_ui.ctl->showBatteryEmpty();
  ui::Canvas canvas(g_ui.back);
  g_ui.ctl->render(canvas);
  {
    board::BusLock spi(board::spiMutex());
    memcpy(board::gfx().getFrameBuffer(), g_ui.back, ui::Canvas::kBytes);
    board::gfx().displayBuffer(HalDisplay::FULL_REFRESH);
  }
  flushForSleep();
  board::powerCounters().shutdowns++;
  board::powerCounters().recordSleep(0, millis());
  board::powerCountersSave();
  board::setLastSleepTargetUtc(0);
  board::SleepConfig c;
  c.timerSeconds = 0;
  c.upDown = false;
  c.usbEdge = false;
  board::BusLock spi(board::spiMutex());
  board::enterDeepSleepEx(c);
}

void uiTask(void*) {
  ui::InputEvent e;
  for (;;) {
    const bool got = xQueueReceive(g_inputQ, &e, pdMS_TO_TICKS(250)) == pdTRUE;
    const uint32_t now = millis();
    bool dirty = false;
    uint32_t inputAt = 0;
    if (got && e.type == ui::InputType::Tap && e.atMs && !g_ui.tapGate.accept(e.atMs)) {
      MT_LOGF("touch: dropped tap %d,%d from before the screen change", e.x, e.y);
      g_ui.lastActivity = now;
    } else if (got) {
      g_ui.lastActivity = now;
      if (g_ui.ctl->handle(e, now)) {
        dirty = true;
        inputAt = e.atMs ? e.atMs : now;
      }
    }
    if (g_engineChanged.exchange(false)) {
      g_ui.ctl->showAlarmIfDue(now);
      dirty = true;
    }
    dirty = g_ui.ctl->tick(now) || dirty;
    const int minute = static_cast<int>((g_clock.nowUtc() / 60) % 1440);
    if (minute != g_ui.lastMinute) {
      g_ui.lastMinute = minute;
      dirty = true;
    }
    if (g_ui.ctl->largeText() != g_ui.largeSaved) {
      g_ui.largeSaved = g_ui.ctl->largeText();
      saveLargeText(g_ui.largeSaved);
    }
    if (dirty) renderAndRefresh(inputAt, false);
    if (const int flashes = g_flash.takeFlashes()) {
      board::BusLock spi(board::spiMutex());
      alerts::visualFlash(flashes);
      g_ui.policy.decide(true, g_ui.ctl->screenType(), 0, 0, true);  // the flash used full refreshes
    }
    if (const int missed = g_missedNotice.exchange(0)) {
      g_ui.ctl->notify(app::fmt(ui::tr(ui::S::MissedWhileAsleep), missed), millis());
      renderAndRefresh(0, false);
    }
    if (g_shutdownReq.exchange(false)) batteryShutdown();
    if (g_ui.ctl->takeSelfTestRequest()) {
      vTaskSuspend(g_inputTask);
      {
        board::BusLock spi(board::spiMutex());
        ui::runSelfTestMenu();
      }
      xQueueReset(g_inputQ);
      vTaskResume(g_inputTask);
      g_ui.policy.reset();
      renderAndRefresh(0, true);
      g_ui.lastActivity = millis();
    }
    if (got) g_timerWake = false;
    const bool user = g_ui.ctl->takeSleepRequest();
    if (user && g_net) g_net->stopInteractive();  // a deliberate sleep closes the portal / hotspot
    core::SleepInputs si;
    si.refreshing = board::display().isRefreshing();
    si.alarmActive = g_port.alarm().active || g_alertBusy.load() || pendingAlertCount() > 0;
    si.usbPowered = board::externalPowerPresent();
    {
      DbLock l;
      si.sleepOnUsb = g_model->db().settings.sleepOnUsb;
    }
    si.pendingWrites = pendingWrites();
    si.clockUnset = g_port.clockUnset();
    si.onHomeScreen = g_ui.ctl->screen() == ui::ScreenId::Home;
    si.idleMs = millis() - g_ui.lastActivity;
    si.idleLimitMs = g_timerWake ? MT_TIMER_WAKE_IDLE_MS : MT_IDLE_SLEEP_MS;
    si.userRequested = user;
    si.networkBusy = g_net && g_net->busy();
    const core::SleepBlocker blk = core::sleepBlocker(si);
    if (blk == core::SleepBlocker::None) goToSleep(user ? "user" : (g_timerWake ? "timer wake done" : "idle"), user);
    if (user) MT_LOGF("ui: sleep refused: %s", core::sleepBlockerName(blk));
  }
}

std::vector<core::DoseLogEntry> loadRecentLog() {
  std::vector<core::DoseLogEntry> out;
  storage::FsDoseLog* log = doseLog();
  if (!log) return out;
  const std::vector<std::string> months = log->months();
  const size_t first = months.size() > 2 ? months.size() - 2 : 0;
  size_t bad = 0;
  for (size_t i = first; i < months.size(); i++) {
    const auto m = log->readMonth(months[i], &bad);
    out.insert(out.end(), m.begin(), m.end());
  }
  MT_LOGF("storage: recent log %u entries (%u bad records skipped)", static_cast<unsigned>(out.size()), static_cast<unsigned>(bad));
  return out;
}

}  // namespace

void deviceAppStart(board::WakeCause cause) {
  board::busLocksInit();
  g_dbMx = xSemaphoreCreateRecursiveMutex();
  g_logMx = xSemaphoreCreateMutex();
  g_storeMx = xSemaphoreCreateMutex();
  g_alertMx = xSemaphoreCreateMutex();
  g_abMx = xSemaphoreCreateMutex();
  loadAutoBackupState();

  const core::ClockTrust trust = g_clock.begin();
  {
    const board::WakePin pin = board::wakePin();
    core::WakeSource src = core::WakeSource::PowerOn;
    switch (pin) {
      case board::WakePin::Timer: src = core::WakeSource::Timer; break;
      case board::WakePin::Ai:
      case board::WakePin::Up:
      case board::WakePin::Down: src = core::WakeSource::Button; break;
      case board::WakePin::Touch: src = core::WakeSource::Touch; break;
      case board::WakePin::Usb: src = core::WakeSource::Usb; break;
      default: src = cause == board::WakeCause::PowerOn ? core::WakeSource::PowerOn : core::WakeSource::Other; break;
    }
    core::PowerCounters& pc = board::powerCounters();
    pc.recordWake(src, g_clock.bootDriftSec());
    g_timerWake = src == core::WakeSource::Timer;
    const int64_t target = board::lastSleepTargetUtc();
    if (g_timerWake && target > 0 && g_clock.nowUtc() < target - 5) pc.earlyWakes++;
    MT_LOGF("power: wake=%s boots=%lu sleeps=%lu drift=%ld s early=%lu", board::wakePinName(pin),
            static_cast<unsigned long>(pc.boots), static_cast<unsigned long>(pc.sleeps), static_cast<long>(pc.lastDriftSec),
            static_cast<unsigned long>(pc.earlyWakes));
  }
  g_alarms.addOutput(&g_buzzer);
  g_alarms.addOutput(&g_flash);
  core::Database& db = database();
  bool demo = false;
#if MT_DEMO_DATA
  if (db.profiles.empty() && trust != core::ClockTrust::Unset) {
    core::PosixTz tz;
    core::parsePosixTz(db.settings.tz.c_str(), tz);
    db = core::makeDemoDatabase(core::utcToLocal(tz, g_clock.nowUtc()).days - 14);
    db.settings.setupDone = true;  // demo builds skip the first-boot wizard
    if (repository()) repository()->saveDatabase(db);
    demo = true;
    MT_LOGF("demo: seeded demo database (2 profiles)");
  }
#endif
  static AppModel model(g_clock, db, &g_logSink);
  g_model = &model;
  std::vector<core::DoseLogEntry> recent = loadRecentLog();
  if (demo && recent.empty()) recent = core::makeDemoLog(db, model.tz(), g_clock.nowUtc() - core::kEarlyTakeWindowSec, 14);
  model.setRecentLog(recent);
  model.status.clockUnset = trust == core::ClockTrust::Unset;
  model.status.clockUntrusted = trust == core::ClockTrust::Untrusted;
  if (trust != core::ClockTrust::Unset) startOrRestoreEngine();
  g_inputQ = xQueueCreate(16, sizeof(ui::InputEvent));
  pollStatus();

  g_ui.back = allocFrame();
  g_ui.front = allocFrame();
  if (!g_ui.back || !g_ui.front) {
    MT_LOGF("ui: FATAL frame buffer allocation failed");
    return;
  }
  memset(g_ui.front, 0xFF, ui::Canvas::kBytes);  // firstFullRefresh() left the panel white
  g_ui.ctl.reset(new ui::UiController(g_port));
  {
    Preferences p;
    if (p.begin("ui", true)) {
      g_ui.largeSaved = p.getBool("large", false);
      p.end();
    }
  }
  g_ui.ctl->setLargeText(g_ui.largeSaved);
  {
    DbLock l;
    g_ui.policy.configure(db.settings.refreshFullEvery, db.settings.nightlyFullRefresh);
  }
  {
    AdminHooks h;
    h.autoBackupState = []() { return autoBackupState(); };
    h.setClockUtc = [](int64_t utc) { return g_port.setClockUtc(utc); };
    h.afterFactoryReset = []() { factoryResetDevice(); };
    h.setLargeText = [](bool on) { g_ui.ctl->setLargeText(on); };
    h.largeText = []() { return g_ui.ctl->largeText(); };
    h.testAlert = [](const core::PersonAlert& a) {
      core::PersonAlert t = a;
      t.audible = true;
      t.critical = true;  // a test the user asked for plays even inside quiet hours
      postAlert(t);
    };
    h.battery = []() {
      BatteryInfo bi;
      board::BatteryReading b;
      {
        board::BusLock i2c(board::i2cMutex());
        b = board::readBattery();
      }
      bi.known = b.gaugeOk;
      bi.pct = b.socPercent;
      bi.mv = b.millivolts > 0 ? b.millivolts : 0;
      bi.ma = b.currentMa;
      bi.usb = b.externalPower;
      bi.charging = b.externalPower && b.currentMa > 0;
      return bi;
    };
    h.entropy = []() { return (static_cast<uint64_t>(esp_random()) << 32) | esp_random(); };
    h.settingsChanged = [](const core::Settings& st) {
      g_ui.policy.configure(st.refreshFullEvery, st.nightlyFullRefresh);
      g_engineChanged = true;
      g_snapshotReq = true;
    };
    net::NetHooks nh;
    nh.nowUtc = []() { return g_clock.nowUtc(); };
    nh.setClockUtc = [](int64_t utc) { return g_port.setClockUtc(utc); };
    nh.tz = []() {
      DbLock l;
      return g_model->tz();
    };
    nh.restart = []() { flushForSleep(); };
    static net::WifiNetService wifi(nh);
    static RadioOffNetService radioOff;
    INetService* netSvc = &radioOff;
    if (wifi.begin()) {
      g_net = &wifi;
      netSvc = &wifi;
    } else {
      MT_LOGF("net: Wi-Fi unavailable, running offline");
    }
    static AdminService admin(model, repository() ? &g_lockedRepo : nullptr, &g_lockedInternal, &g_lockedSd, *netSvc,
                              &g_adminLock, h, MT_FW_VERSION);
    if (g_net) g_net->attachAdmin(&admin);
    g_admin = &admin;
    static AdminFormFactory factory(admin);
    g_ui.ctl->setFormFactory(&factory);
  }
  g_ui.lastActivity = millis();
  // The panel already had its FULL refresh in setup(); the first frame is a policy Full as well.
  g_ui.ctl->tick(millis());
  renderAndRefresh(0, true);
  // OTA: a new image confirms itself only after storage mounted and the first frame was drawn;
  // otherwise the bootloader returns to the previous slot.
  net::otaConfirmBoot(storageSummary().internalMounted);
  if (g_net) {
    net::SyncReason why = net::SyncReason::Wake;
    if (cause == board::WakeCause::PowerOn) {
      why = net::SyncReason::ColdBoot;
    } else if (g_timerWake) {
      const core::LocalTime lt = core::utcToLocal(model.tz(), g_clock.nowUtc());
      const int minute = lt.secOfDay / 60;
      if (std::abs(minute - core::WakeInputs().ntpMinute) <= 3) why = net::SyncReason::DailySlot;
    }
    g_net->autoSync(why);
  }
  MT_LOGF("app: started (wake=%s, clock=%s, psram free %u)", board::wakeCauseName(cause), core::clockTrustName(trust),
          static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

  xTaskCreatePinnedToCore(inputTask, "input", 4096, nullptr, 3, &g_inputTask, 1);
  xTaskCreatePinnedToCore(uiTask, "ui", 12288, nullptr, 2, &g_uiTask, 1);
  xTaskCreatePinnedToCore(engineTask, "engine", 8192, nullptr, 2, &g_engineTask, 1);
  xTaskCreatePinnedToCore(storageTask, "storage", 8192, nullptr, 1, &g_storageTask, 0);
  xTaskCreatePinnedToCore(alertTask, "alerts", 3072, nullptr, 3, &g_alertTask, 0);
}

}  // namespace app
