#pragma once
// Runs a stack of FormProviders: form pages, keyboard, value entry, choice picker and
// PIN entry. Owned by UiController (ScreenId::Form).
#include <memory>
#include <string>
#include <vector>

#include "ui_core/canvas.h"
#include "ui_core/form.h"
#include "ui_core/input.h"
#include "ui_core/keyboard.h"
#include "ui_core/theme.h"
#include "ui_core/value_entry.h"

namespace ui {

class FormHost {
 public:
  enum class Mode : uint8_t { Form, Keyboard, Entry, Picker, Pin };
  static constexpr uint32_t kIdleCloseMs = 3UL * 60UL * 1000UL;  // closes (and relocks) when idle
  static constexpr int kRowH = 78;
  static constexpr int kMaxPinFails = 5;
  static constexpr uint32_t kPinLockMs = 60000;
  // Hit ids: row = kHitRowBase + field id (field ids must stay below kMaxFieldId), footer, picker option.
  static constexpr int kMaxFieldId = 9999;
  static constexpr int kHitRowBase = 10000, kHitFooterBase = 20000, kHitPickBase = 30000;

  explicit FormHost(IPinGate* gate = nullptr) : gate_(gate) {}
  void setGate(IPinGate* g) { gate_ = g; }
  void open(std::unique_ptr<FormProvider> root, uint32_t nowMs);
  void close();
  bool active() const { return !stack_.empty() || pending_ != nullptr; }
  bool handle(const InputEvent& e, uint32_t nowMs);
  bool tick(uint32_t nowMs);
  void render(Canvas& c, const Theme& t);
  const HitMap& hits() const { return hits_; }
  Mode mode() const { return mode_; }
  int screenType() const { return 100 + static_cast<int>(mode_); }
  bool takeClockSetRequest() {
    const bool r = clockReq_;
    clockReq_ = false;
    return r;
  }
  // Test/inspection helpers
  FormProvider* top() const { return stack_.empty() ? nullptr : stack_.back().p.get(); }
  size_t depth() const { return stack_.size(); }
  const std::string& toast() const { return toast_; }
  bool toastIsError() const { return toastErr_; }
  KeyboardModel& keyboard() { return kb_; }
  const std::vector<FormField>& fields() {
    refresh();
    return fields_;
  }
  int focusIndex() const { return focus_; }
  // Activates the row with this field id (as a tap would). Returns false if it is not on screen.
  bool activateField(int fieldId, uint32_t nowMs);
  bool pressFooter(int buttonId, uint32_t nowMs);
  bool submitKeyboard(const std::string& text, uint32_t nowMs);  // types + OK
  // Value entry (Number/Time/Date/DateTime): sets the value (encoded as in FormField::num) and saves.
  bool entrySave(int value, uint32_t nowMs);
  ValueEntry& entry() { return entry_; }
  bool pickOption(int index, uint32_t nowMs);
  bool back(uint32_t nowMs);

 private:
  struct Level {
    std::unique_ptr<FormProvider> p;
    int page = 0;
  };
  void refresh();
  void push(std::unique_ptr<FormProvider> p);
  void popLevel();
  void apply(FormResult r, uint32_t nowMs);
  void activate(int hitId, bool touch, uint32_t nowMs);
  void activateRow(const FormField& f, bool touch, uint32_t nowMs);
  void showToast(const std::string& s, bool err, uint32_t nowMs);
  int rowsPerPage() const;
  void renderForm(Canvas& c, const Theme& t);
  void renderEntry(Canvas& c, const Theme& t);
  void openEntry(const FormField& f);
  void renderPicker(Canvas& c, const Theme& t);
  void renderToast(Canvas& c, const Theme& t);
  bool unlocked(PinScope s, uint8_t profile) const;

  IPinGate* gate_;
  std::vector<Level> stack_;
  std::vector<FormField> fields_;
  std::vector<FooterButton> footer_;
  Mode mode_ = Mode::Form;
  HitMap hits_;
  int focus_ = 0;
  // keyboard
  KeyboardModel kb_;
  KeyboardRequest kbReq_;
  // value entry / picker
  FormField sub_;
  ValueEntry entry_;
  int pickPage_ = 0;
  // pin
  std::unique_ptr<FormProvider> pending_;
  bool pendingRoot_ = false;
  int pinFails_ = 0;
  uint32_t pinLockUntil_ = 0;
  bool adminOk_ = false;
  uint16_t profileOk_ = 0;
  // misc
  std::string toast_;
  bool toastErr_ = false;
  uint32_t toastUntil_ = 0;
  int armedId_ = -1;
  uint32_t armedAt_ = 0;
  uint32_t lastInput_ = 0;
  bool clockReq_ = false;
};

}  // namespace ui
