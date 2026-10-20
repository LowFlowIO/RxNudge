#pragma once
// Screen manager: owns navigation, focus, deliberate-confirm state, toasts and
// renders the current screen. Pure C++: the device feeds it input events and
// ticks from the UI task, the host tests and the preview renderer drive it directly.
#include <string>
#include <vector>

#include "app_core/app_port.h"
#include "ui_core/canvas.h"
#include "ui_core/form.h"
#include "ui_core/form_host.h"
#include "ui_core/value_entry.h"
#include "ui_core/input.h"
#include "ui_core/theme.h"

namespace ui {

enum class ScreenId : uint8_t { Home, Today, Detail, Alarm, Prn, History, ClockSet, Backfill, BatteryEmpty, Form };
const char* screenName(ScreenId s);

// Hit / focus ids
enum HitId : int {
  kHitBack = 1,
  kHitLane0 = 10,
  kHitPrev = 30,
  kHitNext,
  kHitLargeText,
  kHitRow0 = 40,
  kHitPrn = 70,
  kHitHistory,
  kHitTaken,
  kHitSnooze,
  kHitSkip,
  kHitEarlier,
  kHitUndo,
  kHitSnooze5 = 80,
  kHitSnooze10,
  kHitSnooze15,
  kHitSnoozeBack,
  kHitLog0 = 90,
  kHitMode = 110,
  kHitPerMed,
  kHitSave = 120,
  kHitCancel,
  kHitToastUndo = 150,
  kHitMenu = 160,
  kHitEntry0 = 200,  // 200..249: value entry targets (ui_core/value_entry.h)
};

// Layout rules (pure, tested)
int homeLanesPerPage(bool bannerShown, bool largeText);
int listRowsPerPage(int top, int rowH);

class UiController {
 public:
  static constexpr uint32_t kArmTimeoutMs = 5000;   // second tap must follow within 5 s
  static constexpr uint32_t kToastMs = 60000;       // matches the engine's 60 s undo window
  static constexpr uint32_t kErrorToastMs = 6000;

  explicit UiController(app::IAppPort& port) : port_(port) {}

  bool handle(const InputEvent& e, uint32_t nowMs);  // true = redraw needed
  bool tick(uint32_t nowMs);                         // timeouts, alarm activation, forced clock setting
  void render(Canvas& c);

  ScreenId screen() const { return scr_; }
  int screenType() const { return scr_ == ScreenId::Form ? form_.screenType() : static_cast<int>(scr_); }
  // Menu, editors, settings and the first-boot wizard (Phase 5).
  void setFormFactory(IFormFactory* f) {
    factory_ = f;
    form_.setGate(f ? f->gate() : nullptr);
  }
  void openMenu(uint32_t nowMs);
  void openForm(std::unique_ptr<FormProvider> p, uint32_t nowMs);
  FormHost& form() { return form_; }
  // Identifies the touch layout of the last render (screen + targets); a change arms the touch guard.
  uint32_t touchLayoutKey() const {
    return (scr_ == ScreenId::Form ? form_.hits().layoutKey() : hits_.layoutKey()) ^ (static_cast<uint32_t>(scr_) * 0x9E3779B9u);
  }
  bool wizardActive() const { return wizard_ && scr_ == ScreenId::Form; }
  void setLargeText(bool on) { large_ = on; }
  bool largeText() const { return large_; }
  bool takeSleepRequest() { bool r = sleepReq_; sleepReq_ = false; return r; }
  bool takeSelfTestRequest() { bool r = selfTestReq_; selfTestReq_ = false; return r; }
  bool alarmShowing() const { return scr_ == ScreenId::Alarm; }
  const HitMap& hits() const { return hits_; }
  int focusedId() const { return hits_.focusableId(focus_); }
  const std::string& toastText() const { return toast_; }
  bool armed() const { return armed_; }
  int focusIndex() const { return focus_; }
  bool toastVisible() const { return !toast_.empty(); }
  int holdPermille() const { return hold_; }

  // Navigation entry points (also used by tests and previews)
  void openToday(uint8_t profileId);
  void openDetail(uint64_t eventId);
  void openPrn(uint8_t profileId);
  void openHistory(uint8_t profileId, app::HistoryMode mode);
  void showAlarmIfDue(uint32_t nowMs);
  // Shows an informational toast (e.g. doses missed while asleep).
  void notify(const std::string& text, uint32_t nowMs);
  // Final low-battery screen: stays on the e-paper after shutdown; no hit targets.
  void showBatteryEmpty();

 private:
  struct Nav {
    ScreenId s;
    uint8_t profile;
    uint64_t eventId;
    int page;
  };
  const Theme& th() const { return themeFor(large_); }
  void push(ScreenId s);
  void back();
  void reset(ScreenId s);
  void activate(int id, bool touch, uint32_t nowMs);
  void confirm(int id, uint32_t nowMs);
  bool isDeliberate(int id) const;
  void runCommand(app::DoseCommand& cmd, uint32_t nowMs);
  void showToast(const std::string& text, bool undo, uint32_t nowMs, uint32_t ms);
  void startBackfill(const std::vector<uint64_t>& ids, int64_t scheduledUtc);
  void focusId(int id) { pendingFocus_ = id; }

  void renderHome(Canvas& c);
  void renderToday(Canvas& c);
  void renderDetail(Canvas& c);
  void renderAlarm(Canvas& c);
  void renderPrn(Canvas& c);
  void renderHistory(Canvas& c);
  void renderClockSet(Canvas& c);
  void renderBackfill(Canvas& c);
  void renderBatteryEmpty(Canvas& c);
  void renderToast(Canvas& c);
  int footerY() const { return Canvas::kH - th().touch - 1; }

  app::IAppPort& port_;
  ScreenId scr_ = ScreenId::Home;
  std::vector<Nav> stack_;
  uint8_t profile_ = 0;
  uint64_t eventId_ = 0;
  int homePage_ = 0, listPage_ = 0, listPages_ = 1;
  app::HistoryMode histMode_ = app::HistoryMode::Week;
  int32_t histAnchor_ = 0;
  bool perMed_ = false;
  bool snoozeMode_ = false;
  bool armed_ = false;
  int armedId_ = -1;
  uint32_t armedAt_ = 0;
  int hold_ = 0;
  bool holdHint_ = false;
  std::string toast_;
  bool toastUndo_ = false;
  uint32_t toastUntil_ = 0;
  // the alarm group currently shown (one person)
  std::vector<uint64_t> alarmIds_;
  uint8_t alarmProfile_ = 0;
  // backfill
  std::vector<uint64_t> bfIds_;
  int64_t bfAt_ = 0, bfMin_ = 0, bfMax_ = 0;
  // clock set fields: year, month, day, hour, minute
  ValueEntry cs_;   // clock screen: DD MM YYYY HH : MM
  bool csInit_ = false;
  ValueEntry bf_;   // backfill: HH : MM
  void syncBackfillFromEntry();
  int64_t localOffsetSec();
  int32_t localDay(int64_t utc);
  int localSec(int64_t utc);
  bool handleEntryKeys(ValueEntry& e, const InputEvent& ev, uint32_t now);
  // per-screen data captured at render time for actions
  std::vector<uint8_t> laneProfiles_;
  std::vector<uint64_t> rowIds_;
  std::vector<uint16_t> prnMeds_;
  std::vector<bool> prnAllowed_;
  std::vector<std::string> prnStatus_;
  HitMap hits_;
  int focus_ = 0;
  int pendingFocus_ = -1;
  bool large_ = false;
  bool sleepReq_ = false, selfTestReq_ = false;
  uint32_t lastAlarmCheck_ = 0;
  IFormFactory* factory_ = nullptr;
  FormHost form_;
  bool wizard_ = false;
};

}  // namespace ui
