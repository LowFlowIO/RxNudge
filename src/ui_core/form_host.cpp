#include "ui_core/form_host.h"

#include <algorithm>

#include "app_core/format.h"
#include "core/civil.h"
#include "ui_core/icons.h"
#include "ui_core/strings.h"
#include "ui_core/text.h"
#include "ui_core/widgets.h"

namespace ui {
namespace {
constexpr int kHitBackId = 1, kHitPrevId = 2, kHitNextId = 3;
constexpr int kHitEntrySave = 10, kHitEntryCancel = 11;
constexpr int kHitEntryBase = 20;  // 20..69: arrows, boxes, number pad (value_entry.h)
constexpr int kEntryHeaderH = 56, kEntryTop = 60;
// Field ids go up to a few hundred (list items start at 100), so the bases must be far apart:
// with 1000/1100 a row with id 100 (first list item, first phase) became footer button 0.
constexpr int kHitRowBase = FormHost::kHitRowBase, kHitFooterBase = FormHost::kHitFooterBase, kHitPickBase = FormHost::kHitPickBase;
constexpr int kHeaderH = 72, kFooterY = 396;
constexpr uint32_t kArmMs = 5000, kToastMs = 5000;
}  // namespace

std::string formatFieldValue(const FormField& f) {
  if (!f.value.empty()) return f.value;
  switch (f.kind) {
    case FieldKind::Time: return app::fmtHm(((f.num % 1440) + 1440) % 1440);
    case FieldKind::Date: {
      const core::Ymd y = core::civilFromDays(f.num);
      return app::fmtShortDate(f.num) + app::fmt(" %d", y.y);
    }
    case FieldKind::DateTime: {
      const int32_t day = f.num / 1440;
      return app::fmtShortDate(day) + app::fmt(" %d, ", core::civilFromDays(day).y) + app::fmtHm(f.num % 1440);
    }
    case FieldKind::Number: return f.unit.empty() ? app::fmt("%d", f.num) : app::fmt("%d %s", f.num, f.unit.c_str());
    case FieldKind::Choice:
      return f.num >= 0 && f.num < static_cast<int>(f.options.size()) ? f.options[static_cast<size_t>(f.num)] : "-";
    case FieldKind::Toggle: return f.num ? tr(S::ToggleOn) : tr(S::ToggleOff);
    case FieldKind::Text: return f.password ? std::string(f.text.empty() ? "" : "****") : f.text;
    default: return "";
  }
}

void FormHost::open(std::unique_ptr<FormProvider> root, uint32_t nowMs) {
  close();
  lastInput_ = nowMs;
  push(std::move(root));
  if (stack_.empty() && pending_) pendingRoot_ = true;
}

void FormHost::close() {
  stack_.clear();
  pending_.reset();
  mode_ = Mode::Form;
  adminOk_ = false;
  profileOk_ = 0;
  toast_.clear();
  armedId_ = -1;
  focus_ = 0;
}

bool FormHost::unlocked(PinScope s, uint8_t profile) const {
  if (adminOk_) return true;  // the admin PIN opens everything
  return s == PinScope::Profile && profile < 16 && (profileOk_ & (1u << profile));
}

void FormHost::push(std::unique_ptr<FormProvider> p) {
  if (!p) return;
  const PinScope s = p->pinScope();
  if (s != PinScope::None && gate_ && gate_->pinRequired(s, p->pinProfile()) && !unlocked(s, p->pinProfile())) {
    pending_ = std::move(p);
    kb_ = KeyboardModel("", 8, true, true);
    mode_ = Mode::Pin;
    return;
  }
  stack_.push_back(Level{std::move(p), 0});
  mode_ = Mode::Form;
  focus_ = 0;
  armedId_ = -1;
}

void FormHost::popLevel() {
  if (!stack_.empty()) stack_.pop_back();
  mode_ = Mode::Form;
  focus_ = 0;
  armedId_ = -1;
  if (!stack_.empty()) stack_.back().p->onResume();
}

void FormHost::refresh() {
  fields_.clear();
  footer_.clear();
  if (stack_.empty()) return;
  stack_.back().p->build(fields_);
  stack_.back().p->footer(footer_);
}

void FormHost::showToast(const std::string& s, bool err, uint32_t nowMs) {
  toast_ = s;
  toastErr_ = err;
  toastUntil_ = nowMs + kToastMs;
}

void FormHost::apply(FormResult r, uint32_t nowMs) {
  if (!r.message.empty()) showToast(r.message, r.error, nowMs);
  if (r.reopenClockSet) clockReq_ = true;
  switch (r.nav) {
    case FormResult::Nav::Pop: popLevel(); break;
    case FormResult::Nav::Push: push(std::move(r.next)); break;
    case FormResult::Nav::Replace:
      if (!stack_.empty()) stack_.pop_back();
      push(std::move(r.next));
      break;
    case FormResult::Nav::CloseAll: {
      const std::string keep = toast_;
      close();
      toast_ = keep;
      break;
    }
    default: break;
  }
  if (r.openKeyboard && !stack_.empty()) {
    kbReq_ = r.kb;
    kb_ = KeyboardModel(r.kb.initial, r.kb.maxBytes, r.kb.numeric, r.kb.password);
    mode_ = Mode::Keyboard;
  }
}

int FormHost::rowsPerPage() const { return (kFooterY - kHeaderH - 6) / kRowH; }

void FormHost::activateRow(const FormField& f, bool touch, uint32_t nowMs) {
  if (!f.enabled || stack_.empty()) return;
  FormProvider* p = stack_.back().p.get();
  switch (f.kind) {
    case FieldKind::Info: return;
    case FieldKind::Toggle: apply(p->setValue(f.id, f.num ? 0 : 1), nowMs); return;
    case FieldKind::Text:
      kbReq_ = KeyboardRequest();
      kbReq_.id = f.id;
      kbReq_.title = f.label;
      kbReq_.initial = f.password ? "" : f.text;
      kbReq_.maxBytes = f.maxBytes;
      kbReq_.numeric = f.numericKeys;
      kbReq_.password = f.password;
      kb_ = KeyboardModel(kbReq_.initial, f.maxBytes, f.numericKeys, f.password);
      mode_ = Mode::Keyboard;
      return;
    case FieldKind::Number:
    case FieldKind::Time:
    case FieldKind::Date:
    case FieldKind::DateTime:
      openEntry(f);
      return;
    case FieldKind::Choice:
      sub_ = f;
      pickPage_ = std::max(0, f.num) / std::max(1, rowsPerPage());
      mode_ = Mode::Picker;
      return;
    case FieldKind::Action:
      if (f.danger) {
        const int hid = kHitRowBase + f.id;
        if (!(armedId_ == hid && nowMs - armedAt_ <= kArmMs)) {
          armedId_ = hid;
          armedAt_ = nowMs;
          showToast(touch ? tr(S::TapAgain) : tr(S::PressOkAgain), false, nowMs);
          return;
        }
        armedId_ = -1;
      }
      apply(p->action(f.id), nowMs);
      return;
  }
}

bool FormHost::activateField(int fieldId, uint32_t nowMs) {
  refresh();
  for (const auto& f : fields_)
    if (f.id == fieldId) {
      lastInput_ = nowMs;
      activateRow(f, true, nowMs);
      return true;
    }
  return false;
}

bool FormHost::pressFooter(int buttonId, uint32_t nowMs) {
  refresh();
  for (const auto& b : footer_)
    if (b.id == buttonId) {
      apply(stack_.back().p->action(b.id), nowMs);
      return true;
    }
  return false;
}

bool FormHost::submitKeyboard(const std::string& text, uint32_t nowMs) {
  if (mode_ != Mode::Keyboard && mode_ != Mode::Pin) return false;
  kb_.typeText(text);
  for (size_t i = 0; i < kb_.keys().size(); i++)
    if (kb_.keys()[i].kind == KeyKind::Done) {
      InputEvent e;
      e.type = InputType::Tap;
      e.x = static_cast<int16_t>(kb_.keys()[i].r.cx());
      e.y = static_cast<int16_t>(kb_.keys()[i].r.cy());
      return handle(e, nowMs);
    }
  return false;
}

void FormHost::openEntry(const FormField& f) {
  sub_ = f;
  switch (f.kind) {
    case FieldKind::Time: entry_ = ValueEntry::time(f.num, f.step); break;
    case FieldKind::Date: {
      const int lo = f.max > f.min ? core::civilFromDays(f.min).y : 2020;
      const int hi = f.max > f.min ? core::civilFromDays(f.max).y : 2099;
      entry_ = ValueEntry::date(f.num, lo, hi);
      break;
    }
    case FieldKind::DateTime: entry_ = ValueEntry::dateTime(f.num / 1440, f.num % 1440); break;
    default: entry_ = ValueEntry::number(f.num, f.min, f.max, f.step, f.unit); break;
  }
  mode_ = Mode::Entry;
}

bool FormHost::entrySave(int value, uint32_t nowMs) {
  if (mode_ != Mode::Entry) return false;
  FormField f = sub_;
  f.num = value;
  openEntry(f);
  activate(kHitEntrySave, true, nowMs);
  return true;
}

bool FormHost::pickOption(int index, uint32_t nowMs) {
  if (mode_ != Mode::Picker) return false;
  activate(kHitPickBase + index, true, nowMs);
  return true;
}

bool FormHost::back(uint32_t nowMs) {
  activate(kHitBackId, true, nowMs);
  return true;
}

void FormHost::activate(int id, bool touch, uint32_t nowMs) {
  if (mode_ == Mode::Entry) {
    if (id >= kHitEntryBase && id < kHitEntryBase + kEntrySpan) {
      handleEntryHit(entry_, id - kHitEntryBase);
      return;
    }
    if (id == kHitEntrySave) {
      entry_.finishTyping();
      mode_ = Mode::Form;
      int v = entry_.encoded();
      if (sub_.kind == FieldKind::Date && sub_.max > sub_.min) v = std::min(std::max(v, sub_.min), sub_.max);
      apply(stack_.back().p->setValue(sub_.id, v), nowMs);
      return;
    }
    if (id == kHitEntryCancel || id == kHitBackId) mode_ = Mode::Form;
    return;
  }
  if (mode_ == Mode::Picker) {
    const int rpp = rowsPerPage();
    if (id >= kHitPickBase) {
      mode_ = Mode::Form;
      apply(stack_.back().p->setValue(sub_.id, id - kHitPickBase), nowMs);
      return;
    }
    if (id == kHitPrevId) pickPage_ = std::max(0, pickPage_ - 1);
    if (id == kHitNextId) pickPage_ = std::min(pickPage_ + 1, (static_cast<int>(sub_.options.size()) - 1) / rpp);
    if (id == kHitBackId) mode_ = Mode::Form;
    return;
  }
  // Form mode
  if (id == kHitBackId) {
    if (stack_.empty()) return;
    if (!stack_.back().p->canGoBack()) return;
    popLevel();
    return;
  }
  if (stack_.empty()) return;
  Level& lv = stack_.back();
  const int rpp = rowsPerPage();
  const int pages = std::max(1, (static_cast<int>(fields_.size()) + rpp - 1) / rpp);
  if (id == kHitPrevId) {
    lv.page = std::max(0, lv.page - 1);
    focus_ = 0;
    return;
  }
  if (id == kHitNextId) {
    lv.page = std::min(pages - 1, lv.page + 1);
    focus_ = 0;
    return;
  }
  if (id >= kHitFooterBase && id < kHitPickBase) {
    const size_t i = static_cast<size_t>(id - kHitFooterBase);
    if (i < footer_.size()) apply(lv.p->action(footer_[i].id), nowMs);
    return;
  }
  if (id >= kHitRowBase && id < kHitFooterBase) {
    for (const auto& f : fields_)
      if (f.id == id - kHitRowBase) {
        activateRow(f, touch, nowMs);
        return;
      }
  }
}

bool FormHost::handle(const InputEvent& e, uint32_t nowMs) {
  if (!active()) return false;
  lastInput_ = nowMs;
  refresh();
  if (mode_ == Mode::Keyboard || mode_ == Mode::Pin) {
    KbAction a = KbAction::None;
    if (e.type == InputType::Tap) {
      const int k = kb_.hit(e.x, e.y);
      if (k < 0) {
        const Rect* r = hits_.rectOf(kHitBackId);
        if (r && r->contains(e.x, e.y)) a = KbAction::Cancel;
        else return false;
      } else {
        a = kb_.press(static_cast<size_t>(k));
      }
    } else if (e.type == InputType::Up || e.type == InputType::Down) {
      kb_.moveFocus(e.type == InputType::Up ? -1 : 1);
      return true;
    } else if (e.type == InputType::OkClick) {
      a = kb_.pressFocused();
    } else {
      return false;
    }
    if (a == KbAction::Rejected) showToast(tr(S::TextFull), true, nowMs);
    if (a == KbAction::Cancel) {
      if (mode_ == Mode::Pin) {
        pending_.reset();
        if (stack_.empty()) close();
      }
      mode_ = Mode::Form;
      return true;
    }
    if (a == KbAction::Done) {
      const std::string text = kb_.text();
      if (mode_ == Mode::Pin) {
        if (nowMs < pinLockUntil_) {
          showToast(tr(S::PinLocked), true, nowMs);
          kb_ = KeyboardModel("", 8, true, true);
          return true;
        }
        const PinScope s = pending_->pinScope();
        const uint8_t prof = pending_->pinProfile();
        if (gate_ && gate_->checkPin(s, prof, text)) {
          pinFails_ = 0;
          if (gate_->checkPin(PinScope::Admin, 0, text)) adminOk_ = true;
          if (s == PinScope::Profile && prof < 16) profileOk_ |= static_cast<uint16_t>(1u << prof);
          std::unique_ptr<FormProvider> p = std::move(pending_);
          mode_ = Mode::Form;
          push(std::move(p));
        } else {
          pinFails_++;
          if (pinFails_ >= kMaxPinFails) {
            pinLockUntil_ = nowMs + kPinLockMs;
            pinFails_ = 0;
          }
          showToast(tr(S::WrongPin), true, nowMs);
          kb_ = KeyboardModel("", 8, true, true);
        }
        return true;
      }
      mode_ = Mode::Form;
      apply(stack_.back().p->setText(kbReq_.id, text), nowMs);
      return true;
    }
    return true;
  }
  if (mode_ == Mode::Entry && e.type != InputType::Tap) {
    if (e.type == InputType::Up || e.type == InputType::Down) {
      const int dir = e.type == InputType::Up ? 1 : -1;
      if (entry_.onDone()) entry_.setFocus(dir > 0 ? entry_.count() - 1 : 0);  // back to the fields
      else entry_.stepFocused(dir);
      return true;
    }
    if (e.type == InputType::OkClick) {
      if (entry_.onDone()) activate(kHitEntrySave, false, nowMs);
      else entry_.next();
      return true;
    }
    return false;
  }
  switch (e.type) {
    case InputType::Tap: {
      const int id = hits_.hit(e.x, e.y);
      if (id < 0) return false;
      const int idx = hits_.focusIndexOf(id);
      if (idx >= 0) focus_ = idx;
      activate(id, true, nowMs);
      return true;
    }
    case InputType::Up:
    case InputType::Down: {
      const int n = hits_.focusableCount();
      if (n == 0) return false;
      const int d = e.type == InputType::Up ? -1 : 1;
      focus_ = ((focus_ + d) % n + n) % n;
      return true;
    }
    case InputType::OkClick: {
      const int id = hits_.focusableId(focus_);
      if (id < 0) return false;
      activate(id, false, nowMs);
      return true;
    }
    default: return false;
  }
}

bool FormHost::tick(uint32_t nowMs) {
  bool changed = false;
  if (!toast_.empty() && static_cast<int32_t>(nowMs - toastUntil_) >= 0) {
    toast_.clear();
    changed = true;
  }
  if (armedId_ >= 0 && nowMs - armedAt_ > kArmMs) {
    armedId_ = -1;
    changed = true;
  }
  if (active() && mode_ == Mode::Form) {
    FormResult r;
    if (stack_.back().p->poll(r)) {
      apply(std::move(r), nowMs);
      changed = true;
    }
  }
  const bool rootLocked = !stack_.empty() && !stack_.front().p->canGoBack();
  if (active() && !rootLocked && nowMs - lastInput_ > kIdleCloseMs) {
    close();
    changed = true;
  }
  return changed;
}

void FormHost::render(Canvas& c, const Theme& t) {
  hits_.clear();
  c.resetClip();
  c.clear(Color::White);
  refresh();
  if (mode_ == Mode::Keyboard || mode_ == Mode::Pin) {
    std::string title = mode_ == Mode::Pin ? tr(S::EnterPin) : kbReq_.title;
    std::string hint = mode_ == Mode::Pin ? tr(S::PinDigitsHint) : kbReq_.hint;
    drawKeyboard(c, t, kb_, title, hint, true);
    // Cancel in the title bar (letters layout has no Cancel key)
    const Rect cancel(Canvas::kW - 190, 2, 180, 50);
    hits_.add(kHitBackId, cancel, false);
    c.fillRect(Rect(cancel.x - 8, 0, cancel.w + 16, 53), Color::White);
    drawButton(c, t, cancel, tr(S::Cancel), IconId::None, BtnStyle::Normal, false);
  } else if (mode_ == Mode::Entry) {
    renderEntry(c, t);
  } else if (mode_ == Mode::Picker) {
    renderPicker(c, t);
  } else {
    renderForm(c, t);
  }
  if (!toast_.empty()) renderToast(c, t);
}

void FormHost::renderForm(Canvas& c, const Theme& t) {
  if (stack_.empty()) return;
  Level& lv = stack_.back();
  const bool canBack = lv.p->canGoBack();
  // header
  int x = t.margin;
  if (canBack) {
    const Rect b(0, 0, 176, kHeaderH - 4);
    drawButton(c, t, b.inset(6), tr(S::Back), IconId::ChevronLeft, BtnStyle::Normal, false, 0, t.body);
    hits_.add(kHitBackId, b);
    x = b.right() + t.gap;
  }
  c.textIn(*t.title, Rect(x, 0, Canvas::kW - x - t.margin, kHeaderH), lv.p->title(), Align::Left, Color::Black);
  c.fillRect(Rect(0, kHeaderH - 3, Canvas::kW, 3), Color::Black);
  const int rpp = rowsPerPage();
  const int pages = std::max(1, (static_cast<int>(fields_.size()) + rpp - 1) / rpp);
  lv.page = std::min(lv.page, pages - 1);
  const size_t first = static_cast<size_t>(lv.page * rpp);
  int y = kHeaderH + 3;
  for (size_t i = first; i < fields_.size() && i < first + static_cast<size_t>(rpp); i++) {
    const FormField& f = fields_[i];
    const Rect r(0, y, Canvas::kW, kRowH);
    const bool tappable = f.enabled && f.kind != FieldKind::Info;
    const int hid = kHitRowBase + f.id;
    if (tappable) hits_.add(hid, r);
    const bool focused = tappable && hits_.focusIndexOf(hid) == focus_;
    if (f.kind == FieldKind::Action) {
      const Rect br = r.inset(6);
      const bool armed = armedId_ == hid;
      const bool filled = f.danger || armed || f.primary;
      if (filled) c.fillRoundRect(br, t.radius, f.enabled ? Color::Black : Color::White);
      else c.roundRect(br, t.radius, f.enabled ? 3 : 1, Color::Black);
      const Color fg = filled && f.enabled ? Color::White : Color::Black;
      std::string label = armed ? std::string(tr(S::TapAgain)) : f.label;
      c.textIn(*t.bodyBold, br.inset(16), label, Align::Left, fg);
      if (!f.value.empty()) c.textIn(*t.small, br.inset(16), f.value, Align::Right, fg);
    } else {
      const std::string v = formatFieldValue(f);
      const int labelW = f.kind == FieldKind::Info && v.empty() ? Canvas::kW - 2 * t.margin : 380;
      c.textIn(f.enabled ? *t.body : *t.small, Rect(t.margin, y, labelW, kRowH), f.label, Align::Left, Color::Black);
      const int vx = t.margin + 390, vw = Canvas::kW - vx - t.margin - (tappable ? 40 : 0);
      if (f.kind == FieldKind::Toggle) {
        const Rect sw(vx + vw - 96, y + (kRowH - 44) / 2, 96, 44);
        if (f.num) {
          c.fillRoundRect(sw, 22, Color::Black);
          c.fillCircle(sw.right() - 22, sw.cy(), 16, Color::White);
        } else {
          c.roundRect(sw, 22, 3, Color::Black);
          c.fillCircle(sw.x + 22, sw.cy(), 16, Color::Black);
        }
      } else {
        c.textIn(f.enabled ? *t.bodyBold : *t.small, Rect(vx, y, vw, kRowH), v, Align::Right, Color::Black);
      }
      if (tappable && f.kind != FieldKind::Toggle) drawIcon(c, IconId::ChevronRight, Canvas::kW - t.margin - 36, y + (kRowH - kIconSize) / 2);
      c.fillRect(Rect(t.margin, y + kRowH - 1, Canvas::kW - 2 * t.margin, 1), Color::Black);
    }
    if (focused) drawFocusFrame(c, r.inset(2), t.focusBorder);
    y += kRowH;
  }
  // footer
  c.fillRect(Rect(0, kFooterY - 2, Canvas::kW, 2), Color::Black);
  const int bh = Canvas::kH - kFooterY - 6;
  int fx = t.margin;
  if (pages > 1) {
    const Rect prev(fx, kFooterY + 4, 96, bh), next(fx + 104, kFooterY + 4, 96, bh);
    hits_.add(kHitPrevId, prev);
    hits_.add(kHitNextId, next);
    drawButton(c, t, prev, "", IconId::ChevronLeft, lv.page > 0 ? BtnStyle::Normal : BtnStyle::Disabled,
               hits_.focusIndexOf(kHitPrevId) == focus_);
    drawButton(c, t, next, "", IconId::ChevronRight, lv.page + 1 < pages ? BtnStyle::Normal : BtnStyle::Disabled,
               hits_.focusIndexOf(kHitNextId) == focus_);
    c.textIn(*t.small, Rect(fx + 204, kFooterY, 70, Canvas::kH - kFooterY), app::fmt("%d/%d", lv.page + 1, pages), Align::Left,
             Color::Black);
  }
  int rx = Canvas::kW - t.margin;
  for (size_t i = footer_.size(); i-- > 0;) {
    const int w = std::max(170, textWidth(*t.bodyBold, footer_[i].label) + 48);
    rx -= w;
    const Rect b(rx, kFooterY + 4, w, bh);
    const int hid = kHitFooterBase + static_cast<int>(i);
    hits_.add(hid, b);
    drawButton(c, t, b, footer_[i].label, IconId::None, footer_[i].primary ? BtnStyle::Primary : BtnStyle::Normal,
               hits_.focusIndexOf(hid) == focus_);
    rx -= t.gap;
  }
}

void FormHost::renderEntry(Canvas& c, const Theme& t) {
  // Compact header (Cancel + field name) so the arrows, boxes and number pad fit above the footer.
  const Rect cancelTop(0, 0, 176, kEntryHeaderH);
  drawButton(c, t, cancelTop.inset(3), tr(S::Cancel), IconId::ChevronLeft, BtnStyle::Normal, false, 0, t.body);
  hits_.add(kHitBackId, cancelTop, false);
  c.textIn(*t.title, Rect(cancelTop.right() + t.gap, 0, Canvas::kW - cancelTop.right() - 2 * t.margin, kEntryHeaderH),
           sub_.label, Align::Left, Color::Black);
  c.fillRect(Rect(0, kEntryHeaderH - 2, Canvas::kW, 2), Color::Black);
  drawValueEntry(c, t, entry_, kEntryTop, hits_, kHitEntryBase);
  const int bh = Canvas::kH - kFooterY - 6;
  const Rect save(Canvas::kW - t.margin - 300, kFooterY + 4, 300, bh), cancel(t.margin, kFooterY + 4, 220, bh);
  hits_.add(kHitEntrySave, save, false);
  hits_.add(kHitEntryCancel, cancel, false);
  if (!entry_.typing()) c.textIn(*t.small, Rect(cancel.right() + t.gap, kFooterY + 4, save.x - cancel.right() - 2 * t.gap, bh),
                                 tr(S::EntryHint), Align::Center, Color::Black);
  drawButton(c, t, save, tr(S::Save), IconId::Check, BtnStyle::Primary, entry_.onDone());
  drawButton(c, t, cancel, tr(S::Cancel), IconId::None, BtnStyle::Normal, false);
}

void FormHost::renderPicker(Canvas& c, const Theme& t) {
  const Rect b(0, 0, 176, kHeaderH - 4);
  drawButton(c, t, b.inset(6), tr(S::Back), IconId::ChevronLeft, BtnStyle::Normal, false, 0, t.body);
  hits_.add(kHitBackId, b);
  c.textIn(*t.title, Rect(b.right() + t.gap, 0, Canvas::kW - b.right() - 2 * t.margin, kHeaderH), sub_.label, Align::Left,
           Color::Black);
  c.fillRect(Rect(0, kHeaderH - 3, Canvas::kW, 3), Color::Black);
  const int rpp = rowsPerPage();
  const int n = static_cast<int>(sub_.options.size());
  const int pages = std::max(1, (n + rpp - 1) / rpp);
  pickPage_ = std::min(pickPage_, pages - 1);
  int y = kHeaderH + 3;
  for (int i = pickPage_ * rpp; i < n && i < (pickPage_ + 1) * rpp; i++) {
    const Rect r(0, y, Canvas::kW, kRowH);
    hits_.add(kHitPickBase + i, r);
    if (i == sub_.num) drawIcon(c, IconId::Check, t.margin, y + (kRowH - kIconSize) / 2);
    c.textIn(i == sub_.num ? *t.bodyBold : *t.body, Rect(t.margin + 56, y, Canvas::kW - 2 * t.margin - 56, kRowH),
             sub_.options[static_cast<size_t>(i)], Align::Left, Color::Black);
    c.fillRect(Rect(t.margin, y + kRowH - 1, Canvas::kW - 2 * t.margin, 1), Color::Black);
    if (hits_.focusIndexOf(kHitPickBase + i) == focus_) drawFocusFrame(c, r.inset(2), t.focusBorder);
    y += kRowH;
  }
  c.fillRect(Rect(0, kFooterY - 2, Canvas::kW, 2), Color::Black);
  if (pages > 1) {
    const int bh = Canvas::kH - kFooterY - 6;
    const Rect prev(t.margin, kFooterY + 4, 110, bh), next(t.margin + 120, kFooterY + 4, 110, bh);
    hits_.add(kHitPrevId, prev);
    hits_.add(kHitNextId, next);
    drawButton(c, t, prev, "", IconId::ChevronLeft, BtnStyle::Normal, hits_.focusIndexOf(kHitPrevId) == focus_);
    drawButton(c, t, next, "", IconId::ChevronRight, BtnStyle::Normal, hits_.focusIndexOf(kHitNextId) == focus_);
    c.textIn(*t.small, Rect(t.margin + 250, kFooterY, 100, Canvas::kH - kFooterY), app::fmt("%d/%d", pickPage_ + 1, pages),
             Align::Left, Color::Black);
  }
}

void FormHost::renderToast(Canvas& c, const Theme& t) {
  const int h = 70;
  const int y = (mode_ == Mode::Keyboard || mode_ == Mode::Pin) ? kKbTop - h - 4 + 0 : kFooterY - h - 6;
  const Rect r(t.margin, mode_ == Mode::Keyboard || mode_ == Mode::Pin ? 4 : y, Canvas::kW - 2 * t.margin - 200, h - 8);
  c.fillRoundRect(r, t.radius, Color::Black);
  c.textIn(*t.bodyBold, r.inset(16), toast_, Align::Left, Color::White);
}

}  // namespace ui
