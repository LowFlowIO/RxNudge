#pragma once
// Generic form model used by every editor, settings page and wizard step.
// Providers (app_core) describe fields and react to edits; FormHost (ui_core)
// renders them and runs the sub-editors (keyboard, stepper, picker, PIN).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace ui {

enum class FieldKind : uint8_t {
  Info,    // read-only text
  Text,    // keyboard
  Number,    // value entry (value_entry.h): min..max, arrows step by `step`, number pad
  Time,      // value entry HH : MM; num = minute of day; `step` = minute arrow step
  Date,      // value entry DD MM YYYY; num = local day number; min/max = day range
  DateTime,  // value entry DD MM YYYY HH : MM; num = day * 1440 + minute
  Choice,  // picker list
  Toggle,  // flips on tap
  Action   // calls action(); danger = two taps
};

struct FormField {
  int id = 0;
  FieldKind kind = FieldKind::Info;
  std::string label;
  std::string value;  // display text (providers fill it; the host formats Time/Date/Number when empty)
  int num = 0;        // Number/Time/Date/Choice index/Toggle (0/1)
  int min = 0, max = 0, step = 1;
  std::vector<std::string> options;  // Choice
  std::string text;                  // Text initial value
  size_t maxBytes = 48;
  bool numericKeys = false, password = false;
  bool enabled = true;
  bool danger = false;
  bool primary = false;  // Action drawn filled (the recommended choice); still a single tap
  std::string unit;  // shown after Number values ("min", "days")
};

struct FooterButton {
  int id;
  std::string label;
  bool primary;
};

enum class PinScope : uint8_t { None, Admin, Profile };

class FormProvider;

struct KeyboardRequest {
  int id = 0;
  std::string title, initial, hint;
  size_t maxBytes = 48;
  bool numeric = false, password = false;
};

struct FormResult {
  enum class Nav : uint8_t { Stay, Pop, Push, Replace, CloseAll };
  Nav nav = Nav::Stay;
  std::unique_ptr<FormProvider> next;  // Push / Replace
  std::string message;                 // toast
  bool error = false;
  bool openKeyboard = false;  // provider-driven text entry (e.g. PIN twice)
  KeyboardRequest kb;
  bool reopenClockSet = false;  // ask the host's owner to show the clock screen

  static FormResult stay(const std::string& msg = "", bool err = false) {
    FormResult r;
    r.message = msg;
    r.error = err;
    return r;
  }
  static FormResult pop(const std::string& msg = "") {
    FormResult r;
    r.nav = Nav::Pop;
    r.message = msg;
    return r;
  }
  static FormResult push(std::unique_ptr<FormProvider> p) {
    FormResult r;
    r.nav = Nav::Push;
    r.next = std::move(p);
    return r;
  }
  static FormResult replace(std::unique_ptr<FormProvider> p, const std::string& msg = "") {
    FormResult r;
    r.nav = Nav::Replace;
    r.next = std::move(p);
    r.message = msg;
    return r;
  }
  static FormResult closeAll(const std::string& msg = "") {
    FormResult r;
    r.nav = Nav::CloseAll;
    r.message = msg;
    return r;
  }
  static FormResult keyboard(const KeyboardRequest& k) {
    FormResult r;
    r.openKeyboard = true;
    r.kb = k;
    return r;
  }
};

class FormProvider {
 public:
  virtual ~FormProvider() = default;
  virtual std::string title() const = 0;
  virtual void build(std::vector<FormField>& out) = 0;
  virtual FormResult setValue(int id, int value) {
    (void)id;
    (void)value;
    return FormResult::stay();
  }
  virtual FormResult setText(int id, const std::string& text) {
    (void)id;
    (void)text;
    return FormResult::stay();
  }
  virtual FormResult action(int id) {
    (void)id;
    return FormResult::stay();
  }
  virtual void footer(std::vector<FooterButton>& out) { (void)out; }
  // Back from this page. Returning false keeps the page (e.g. wizard first step).
  virtual bool canGoBack() const { return true; }
  virtual PinScope pinScope() const { return PinScope::None; }
  virtual uint8_t pinProfile() const { return 0; }
  // Called when the page becomes visible again after a sub-page closed.
  virtual void onResume() {}
  // Called on every UI tick: return true to apply `out` (a toast, a navigation or
  // just a redraw), e.g. when a background job progressed or finished.
  virtual bool poll(FormResult& out) {
    (void)out;
    return false;
  }
};

// Formats a Time/Date/Number field value when the provider left `value` empty.
std::string formatFieldValue(const FormField& f);

// PIN checks for gated pages (implemented by the app layer).
class IPinGate {
 public:
  virtual ~IPinGate() = default;
  virtual bool pinRequired(PinScope scope, uint8_t profileId) = 0;
  virtual bool checkPin(PinScope scope, uint8_t profileId, const std::string& pin) = 0;
};

}  // namespace ui

namespace ui {
// Supplies the menu and the setup wizard to the UI controller (implemented by app_core).
class IFormFactory {
 public:
  virtual ~IFormFactory() = default;
  virtual std::unique_ptr<FormProvider> menu() = 0;
  virtual std::unique_ptr<FormProvider> wizard() = 0;
  virtual bool setupNeeded() = 0;
  virtual IPinGate* gate() = 0;
};
}  // namespace ui
