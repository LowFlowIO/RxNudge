#include "ui_core/controller.h"

#include <algorithm>
#include <cstdio>

#include "app_core/format.h"
#include "core/civil.h"
#include "ui_core/icons.h"
#include "ui_core/strings.h"
#include "ui_core/widgets.h"

namespace ui {
using app::fmt;

const char* screenName(ScreenId s) {
  static const char* const n[] = {"home", "today", "detail", "alarm", "prn", "history", "clock_set", "backfill", "battery_empty", "form"};
  return n[static_cast<int>(s)];
}

int homeLanesPerPage(bool banner, bool large) { return std::max(2, 4 - (banner ? 1 : 0) - (large ? 1 : 0)); }
int listRowsPerPage(int top, int rowH) { return std::max(1, (Canvas::kH - kMinTouchPx - 1 - top) / rowH); }

static const char* friendly(core::ActionResult r) {
  switch (r) {
    case core::ActionResult::TooEarly: return "too early";
    case core::ActionResult::TakenAtInFuture: return "time is in the future";
    case core::ActionResult::WouldDoubleDose: return "too close to the next dose";
    case core::ActionResult::SnoozeLimit: return "snoozed 3 times already";
    case core::ActionResult::SnoozeBeyondGrace: return "too late to snooze";
    case core::ActionResult::UndoExpired: return "undo time is over";
    case core::ActionResult::PrnTooSoon: return "too soon after the last dose";
    case core::ActionResult::PrnDailyCap: return "daily limit reached";
    case core::ActionResult::InvalidState: return "already recorded";
    case core::ActionResult::NotFound: return "dose not found";
    default: return core::actionResultName(r);
  }
}

// ------------------------------------------------------------------ navigation
void UiController::push(ScreenId s) {
  stack_.push_back(Nav{scr_, profile_, eventId_, listPage_});
  scr_ = s;
  focus_ = 0;
  pendingFocus_ = -1;
  armed_ = false;
  hold_ = 0;
  holdHint_ = false;
  snoozeMode_ = false;
}

void UiController::back() {
  armed_ = false;
  hold_ = 0;
  holdHint_ = false;
  snoozeMode_ = false;
  focus_ = 0;
  if (stack_.empty()) {
    scr_ = ScreenId::Home;
    return;
  }
  const Nav n = stack_.back();
  stack_.pop_back();
  scr_ = n.s;
  profile_ = n.profile;
  eventId_ = n.eventId;
  listPage_ = n.page;
}

void UiController::openForm(std::unique_ptr<FormProvider> p, uint32_t nowMs) {
  form_.open(std::move(p), nowMs);
  if (!form_.active()) return;
  if (scr_ != ScreenId::Form) push(ScreenId::Form);
}

void UiController::openMenu(uint32_t nowMs) {
  if (factory_) openForm(factory_->menu(), nowMs);
}

void UiController::reset(ScreenId s) {
  stack_.clear();
  scr_ = s;
  focus_ = 0;
  armed_ = false;
  hold_ = 0;
  snoozeMode_ = false;
}

void UiController::openToday(uint8_t p) {
  push(ScreenId::Today);
  profile_ = p;
  listPage_ = 0;
}

void UiController::openDetail(uint64_t id) {
  push(ScreenId::Detail);
  eventId_ = id;
}

void UiController::openPrn(uint8_t p) {
  push(ScreenId::Prn);
  profile_ = p;
  listPage_ = 0;
}

void UiController::openHistory(uint8_t p, app::HistoryMode m) {
  push(ScreenId::History);
  profile_ = p;
  histMode_ = m;
  histAnchor_ = port_.localNow().days;
  perMed_ = false;
}

void UiController::showAlarmIfDue(uint32_t nowMs) {
  (void)nowMs;
  const app::AlarmVM vm = port_.alarm();
  if (!vm.active) {
    alarmIds_.clear();
    if (scr_ == ScreenId::Alarm) back();
    return;
  }
  if (scr_ == ScreenId::ClockSet || scr_ == ScreenId::Backfill || scr_ == ScreenId::BatteryEmpty) return;
  if (wizardActive()) return;  // no medications exist before setup completes
  std::vector<uint64_t> ids;
  for (const auto& m : vm.meds) ids.push_back(m.eventId);
  const bool changed = ids != alarmIds_ || vm.profileId != alarmProfile_;
  alarmIds_ = ids;
  alarmProfile_ = vm.profileId;
  if (scr_ != ScreenId::Alarm) {
    push(ScreenId::Alarm);
    focusId(kHitTaken);
  } else if (changed) {
    snoozeMode_ = false;
    armed_ = false;
    focusId(kHitTaken);
  }
}

void UiController::notify(const std::string& text, uint32_t nowMs) { showToast(text, false, nowMs, kErrorToastMs * 2); }

void UiController::showBatteryEmpty() {
  reset(ScreenId::BatteryEmpty);
  toast_.clear();
  toastUndo_ = false;
}

void UiController::showToast(const std::string& text, bool undo, uint32_t nowMs, uint32_t ms) {
  toast_ = text;
  toastUndo_ = undo;
  toastUntil_ = nowMs + ms;
}

void UiController::startBackfill(const std::vector<uint64_t>& ids, int64_t scheduledUtc) {
  bfIds_ = ids;
  const int64_t now = port_.nowUtc();
  bfMin_ = scheduledUtc - core::kEarlyTakeWindowSec;
  bfMax_ = now;
  bfAt_ = std::min(std::max(scheduledUtc, bfMin_), bfMax_);
  bfAt_ -= bfAt_ % 60;
  bf_ = ValueEntry::time(localSec(bfAt_) / 60);
  push(ScreenId::Backfill);
  focusId(kHitSave);
}

// ------------------------------------------------------------------ input
bool UiController::isDeliberate(int id) const {
  if (id == kHitTaken) return true;
  if (id >= kHitLog0 && id < kHitLog0 + 20) return true;
  return id == kHitSave && scr_ == ScreenId::Backfill;
}

bool UiController::handle(const InputEvent& e, uint32_t now) {
  if (scr_ == ScreenId::BatteryEmpty) return false;
  if (e.type == InputType::Combo) {
    selfTestReq_ = true;
    return false;
  }
  if (scr_ == ScreenId::Form) {
    const bool r = form_.handle(e, now);
    if (form_.takeClockSetRequest()) {
      form_.close();
      wizard_ = false;
      reset(ScreenId::ClockSet);
      return true;
    }
    if (!form_.active()) {
      wizard_ = false;
      reset(ScreenId::Home);
      homePage_ = 0;
      return true;
    }
    return r;
  }
  if (scr_ == ScreenId::ClockSet && handleEntryKeys(cs_, e, now)) return true;
  if (scr_ == ScreenId::Backfill && handleEntryKeys(bf_, e, now)) {
    syncBackfillFromEntry();
    return true;
  }
  const int focused = hits_.focusableId(focus_);
  switch (e.type) {
    case InputType::Up:
    case InputType::Down: {
      const int dir = e.type == InputType::Up ? -1 : 1;
      const int n = hits_.focusableCount();
      if (n == 0) return false;
      focus_ = ((focus_ + dir) % n + n) % n;
      hold_ = 0;
      holdHint_ = false;
      armed_ = false;
      return true;
    }
    case InputType::OkClick:
      if (focused < 0) return false;
      if (isDeliberate(focused)) {
        holdHint_ = true;
        return true;
      }
      activate(focused, false, now);
      return true;
    case InputType::OkHold:
      if (focused >= 0 && isDeliberate(focused)) {
        hold_ = e.permille;
        return true;
      }
      return false;
    case InputType::OkHoldDone:
      if (focused >= 0 && isDeliberate(focused)) {
        hold_ = 0;
        confirm(focused, now);
        return true;
      }
      return false;
    case InputType::OkRelease:
      if (hold_) {
        hold_ = 0;
        return true;
      }
      return false;
    case InputType::OkLong:
      if (scr_ == ScreenId::Home) sleepReq_ = true;
      return false;
    case InputType::Tap: {
      const int id = hits_.hit(e.x, e.y);
      if (id < 0) return false;
      const int idx = hits_.focusIndexOf(id);
      if (idx >= 0) focus_ = idx;
      activate(id, true, now);
      return true;
    }
    default:
      return false;
  }
}

void UiController::activate(int id, bool touch, uint32_t now) {
  if (isDeliberate(id)) {
    if (!touch) {
      holdHint_ = true;
      return;
    }
    if (armed_ && armedId_ == id && now - armedAt_ <= kArmTimeoutMs) {
      confirm(id, now);
    } else {
      armed_ = true;
      armedId_ = id;
      armedAt_ = now;
    }
    return;
  }
  armed_ = false;
  if (id == kHitBack || id == kHitCancel) {
    back();
    return;
  }
  if (id >= kHitLane0 && id < kHitLane0 + 20) {
    const size_t i = static_cast<size_t>(id - kHitLane0);
    if (i < laneProfiles_.size()) openToday(laneProfiles_[i]);
    return;
  }
  if (id >= kHitRow0 && id < kHitRow0 + 30) {
    const size_t i = static_cast<size_t>(id - kHitRow0);
    if (i < rowIds_.size() && !core::eventIdIsPrn(rowIds_[i])) openDetail(rowIds_[i]);
    return;
  }
  if (id >= kHitEntry0 && id < kHitEntry0 + kEntrySpan) {
    if (scr_ == ScreenId::ClockSet) handleEntryHit(cs_, id - kHitEntry0);
    if (scr_ == ScreenId::Backfill) {
      handleEntryHit(bf_, id - kHitEntry0);
      syncBackfillFromEntry();
    }
    return;
  }
  app::DoseCommand cmd;
  cmd.source = touch ? core::ActionSource::Touch : core::ActionSource::Button;
  switch (id) {
    case kHitPrev:
    case kHitNext: {
      const int d = id == kHitPrev ? -1 : 1;
      if (scr_ == ScreenId::Home) homePage_ = std::max(0, homePage_ + d);
      else if (scr_ == ScreenId::History) {
        if (histMode_ == app::HistoryMode::Day) histAnchor_ += d;
        else if (histMode_ == app::HistoryMode::Week) histAnchor_ += 7 * d;
        else {
          const core::Ymd y = core::civilFromDays(histAnchor_);
          int mo = y.m + d, yr = y.y;
          if (mo < 1) {
            mo = 12;
            yr--;
          } else if (mo > 12) {
            mo = 1;
            yr++;
          }
          histAnchor_ = core::daysFromCivil(yr, mo, 1);
        }
        histAnchor_ = std::min(histAnchor_, port_.localNow().days);
      } else {
        listPage_ = std::min(std::max(0, listPage_ + d), std::max(0, listPages_ - 1));
        focus_ = 0;
      }
      return;
    }
    case kHitMenu: openMenu(now); return;
    case kHitLargeText:
      large_ = !large_;
      homePage_ = 0;
      return;
    case kHitPrn: openPrn(profile_); return;
    case kHitHistory: openHistory(profile_, app::HistoryMode::Week); return;
    case kHitSnooze:
      snoozeMode_ = true;
      focusId(kHitSnooze5);
      return;
    case kHitSnoozeBack:
      snoozeMode_ = false;
      focusId(kHitSnooze);
      return;
    case kHitSnooze5:
    case kHitSnooze10:
    case kHitSnooze15:
      cmd.type = app::CommandType::Snooze;
      cmd.minutes = id == kHitSnooze5 ? 5 : (id == kHitSnooze10 ? 10 : 15);
      for (uint64_t x : alarmIds_) cmd.add(x);
      snoozeMode_ = false;
      runCommand(cmd, now);
      return;
    case kHitSkip:
      cmd.type = app::CommandType::Skip;
      if (scr_ == ScreenId::Alarm)
        for (uint64_t x : alarmIds_) cmd.add(x);
      else cmd.add(eventId_);
      runCommand(cmd, now);
      return;
    case kHitEarlier:
      if (scr_ == ScreenId::Alarm) startBackfill(alarmIds_, port_.alarm().earliestScheduledUtc);
      else {
        const app::DoseDetailVM d = port_.detail(eventId_);
        startBackfill({eventId_}, d.scheduledUtc);
      }
      return;
    case kHitUndo:
      cmd.type = app::CommandType::Undo;
      cmd.add(eventId_);
      runCommand(cmd, now);
      return;
    case kHitToastUndo: {
      cmd = port_.lastAction();
      cmd.type = app::CommandType::Undo;
      cmd.source = touch ? core::ActionSource::Touch : core::ActionSource::Button;
      runCommand(cmd, now);
      return;
    }
    case kHitMode:
      histMode_ = histMode_ == app::HistoryMode::Day ? app::HistoryMode::Week
                  : histMode_ == app::HistoryMode::Week ? app::HistoryMode::Month
                                                        : app::HistoryMode::Day;
      return;
    case kHitPerMed: perMed_ = !perMed_; return;
    case kHitSave: {  // clock set (backfill SAVE is deliberate and handled in confirm)
      cs_.finishTyping();
      const core::Ymd d = core::civilFromDays(cs_.day());
      if (port_.setClockLocal(d.y, d.m, d.d, cs_.minute() / 60, cs_.minute() % 60)) {
        reset(ScreenId::Home);
        csInit_ = false;
      } else {
        showToast(fmt(tr(S::ToastFailed), "invalid date"), false, now, kErrorToastMs);
      }
      return;
    }
    default:
      return;
  }
}

void UiController::confirm(int id, uint32_t now) {
  armed_ = false;
  hold_ = 0;
  holdHint_ = false;
  app::DoseCommand cmd;
  cmd.source = core::ActionSource::Button;
  if (id == kHitTaken) {
    cmd.type = app::CommandType::Take;
    if (scr_ == ScreenId::Alarm)
      for (uint64_t x : alarmIds_) cmd.add(x);
    else cmd.add(eventId_);
    runCommand(cmd, now);
  } else if (id >= kHitLog0 && id < kHitLog0 + 20) {
    const size_t i = static_cast<size_t>(id - kHitLog0);
    if (i >= prnMeds_.size()) return;
    cmd.type = app::CommandType::TakePrn;
    cmd.medId = prnMeds_[i];
    runCommand(cmd, now);
  } else if (id == kHitSave && scr_ == ScreenId::Backfill) {
    cmd.type = app::CommandType::Take;
    cmd.takenAt = bfAt_;
    cmd.source = core::ActionSource::Backfill;
    for (uint64_t x : bfIds_) cmd.add(x);
    back();  // leave the backfill screen first so the alarm/detail re-evaluates
    runCommand(cmd, now);
  }
}

void UiController::runCommand(app::DoseCommand& cmd, uint32_t now) {
  const app::CommandResult r = port_.execute(cmd);
  if (r.applied == 0) {
    showToast(fmt(tr(S::ToastFailed), friendly(r.result)), false, now, kErrorToastMs);
    return;
  }
  std::string text;
  switch (cmd.type) {
    case app::CommandType::Take:
    case app::CommandType::TakePrn:
      text = fmt(tr(S::ToastTaken), r.applied == 1 ? "1 dose" : fmt("%d doses", r.applied).c_str());
      break;
    case app::CommandType::Skip: text = tr(S::ToastSkipped); break;
    case app::CommandType::Snooze: text = fmt(tr(S::ToastSnoozed), cmd.minutes); break;
    case app::CommandType::Undo: text = tr(S::ToastUndone); break;
  }
  const bool undo = cmd.type != app::CommandType::Undo && port_.lastActionUndoable();
  showToast(text, undo, now, undo ? kToastMs : kErrorToastMs);
  if (scr_ == ScreenId::Alarm || cmd.type == app::CommandType::Undo) showAlarmIfDue(now);
  pendingFocus_ = undo ? kHitToastUndo : pendingFocus_;
}

bool UiController::tick(uint32_t now) {
  bool changed = false;
  if (scr_ == ScreenId::BatteryEmpty) return false;
  // First boot: the setup wizard comes before everything else (it also sets the clock).
  if (factory_ && !wizardActive() && scr_ != ScreenId::Alarm && factory_->setupNeeded()) {
    form_.open(factory_->wizard(), now);
    wizard_ = true;
    reset(ScreenId::Form);
    changed = true;
  }
  if (scr_ == ScreenId::Form) {
    changed = form_.tick(now) || changed;
    if (form_.takeClockSetRequest()) {
      form_.close();
      wizard_ = false;
      reset(ScreenId::ClockSet);
      return true;
    }
    if (!form_.active()) {
      wizard_ = false;
      reset(ScreenId::Home);
      homePage_ = 0;
      changed = true;
    }
  }
  if (port_.clockUnset() && scr_ != ScreenId::ClockSet && !wizardActive()) {
    form_.close();
    reset(ScreenId::ClockSet);
    changed = true;
  }
  if (armed_ && now - armedAt_ > kArmTimeoutMs) {
    armed_ = false;
    changed = true;
  }
  if (!toast_.empty() && static_cast<int32_t>(now - toastUntil_) >= 0) {
    toast_.clear();
    toastUndo_ = false;
    changed = true;
  }
  if (toastUndo_ && !port_.lastActionUndoable()) {
    toast_.clear();
    toastUndo_ = false;
    changed = true;
  }
  if (now - lastAlarmCheck_ >= 1000 || lastAlarmCheck_ == 0) {
    lastAlarmCheck_ = now ? now : 1;
    const ScreenId before = scr_;
    const std::vector<uint64_t> ids = alarmIds_;
    if (!port_.clockUnset()) showAlarmIfDue(now);
    changed = changed || before != scr_ || ids != alarmIds_;
  }
  return changed;
}

// ------------------------------------------------------------------ rendering
void UiController::render(Canvas& c) {
  hits_.clear();
  c.resetClip();
  c.clear(Color::White);
  switch (scr_) {
    case ScreenId::Home: renderHome(c); break;
    case ScreenId::Today: renderToday(c); break;
    case ScreenId::Detail: renderDetail(c); break;
    case ScreenId::Alarm: renderAlarm(c); break;
    case ScreenId::Prn: renderPrn(c); break;
    case ScreenId::History: renderHistory(c); break;
    case ScreenId::ClockSet: renderClockSet(c); break;
    case ScreenId::Backfill: renderBackfill(c); break;
    case ScreenId::BatteryEmpty: renderBatteryEmpty(c); break;
    case ScreenId::Form:
      form_.render(c, th());
      hits_ = form_.hits();
      return;
  }
  if (!toast_.empty() && scr_ != ScreenId::ClockSet && scr_ != ScreenId::BatteryEmpty) renderToast(c);
  if (pendingFocus_ >= 0) {
    const int idx = hits_.focusIndexOf(pendingFocus_);
    if (idx >= 0) {
      focus_ = idx;
      pendingFocus_ = -1;
      // focus changed after drawing: draw again so the highlight is right (cheap, no refresh in between)
      render(c);
      return;
    }
    pendingFocus_ = -1;
  }
  const int n = hits_.focusableCount();
  if (n > 0 && focus_ >= n) focus_ = n - 1;
}

void UiController::renderHome(Canvas& c) {
  const Theme& t = th();
  const app::StatusFlags st = port_.status();
  int bannerMore = 0;
  const bool banner = app::topBanner(st, &bannerMore) != app::BannerKind::None;
  const int lpp = homeLanesPerPage(banner, large_);
  const app::HomeVM vm = port_.home(homePage_, lpp);
  homePage_ = vm.page;
  // header: clock, date, status icons
  const int hh = t.homeHeaderH;
  const int iconsLeft = drawStatusIcons(c, t, Canvas::kW - t.margin, (hh - kIconSize) / 2, st);
  c.textIn(*t.title, Rect(t.margin, 0, 140, hh), vm.clock, Align::Left, Color::Black);
  const int dx = t.margin + textWidth(*t.title, vm.clock) + 20;
  c.textIn(*t.body, Rect(dx, 0, iconsLeft - dx - t.gap, hh), vm.date, Align::Left, Color::Black);
  c.fillRect(Rect(0, hh - 3, Canvas::kW, 3), Color::Black);
  int y = hh + drawBanner(c, t, hh, st);
  const int fy = footerY();
  laneProfiles_.clear();
  const int laneH = (fy - y) / lpp;
  const int focusedId = hits_.focusableId(focus_);
  (void)focusedId;
  for (size_t i = 0; i < vm.lanes.size(); i++) {
    const app::LaneVM& l = vm.lanes[i];
    const Rect r(0, y + static_cast<int>(i) * laneH, Canvas::kW, laneH);
    laneProfiles_.push_back(l.profileId);
    const int id = kHitLane0 + static_cast<int>(i);
    hits_.add(id, r);
    const bool focused = hits_.focusIndexOf(id) == focus_;
    const int line1 = r.y + 6, line2 = r.y + laneH / 2;
    const int lh1 = laneH / 2 - 4, lh2 = laneH / 2 - 6;
    c.textIn(*t.title, Rect(t.margin, r.y, 200, laneH), l.title, Align::Left, Color::Black);
    // middle column: next dose
    const int rx = large_ ? 470 : 560, rw = Canvas::kW - t.margin - rx;
    const int mx = large_ ? 170 : 230, mw = rx - mx - t.gap;
    std::string next;
    if (l.nextTime.empty()) next = l.total > 0 && l.taken == l.total ? tr(S::AllDone) : tr(S::NoMoreToday);
    else next = fmt(tr(S::NextDose), l.nextTime.c_str());
    c.textIn(*t.bodyBold, Rect(mx, line1, mw, lh1), next, Align::Left, Color::Black);
    if (!l.nextLabel.empty()) c.textIn(*t.small, Rect(mx, line2, mw, lh2), l.nextLabel, Align::Left, Color::Black);
    // right column: progress and overdue badge
    c.textIn(*t.body, Rect(rx, line1, rw, lh1), fmt(tr(S::OfTaken), l.taken, l.total), Align::Right, Color::Black);
    if (l.overdue > 0) {
      drawBadge(c, *t.small, rx + rw, line2 + lh2 / 2, fmt(tr(S::Overdue), l.overdue));
    } else {
      drawProgress(c, Rect(rx + 40, line2 + lh2 / 2 - 9, rw - 40, 18), l.taken, l.total);
    }
    if (i + 1 < vm.lanes.size()) c.fillRect(Rect(t.margin, r.bottom() - 1, Canvas::kW - 2 * t.margin, 2), Color::Black);
    if (focused) drawFocusFrame(c, r.inset(2), t.focusBorder);
  }
  if (vm.lanes.empty()) c.textIn(*t.body, Rect(0, y, Canvas::kW, fy - y), tr(S::NoData), Align::Center, Color::Black);
  // footer: pager + large-text toggle
  c.fillRect(Rect(0, fy - 2, Canvas::kW, 2), Color::Black);
  const int bh = Canvas::kH - fy - 6;
  if (vm.pageCount > 1) {
    const Rect prev(t.margin, fy + 4, 110, bh), next(t.margin + 110 + 150, fy + 4, 110, bh);
    hits_.add(kHitPrev, prev);
    hits_.add(kHitNext, next);
    drawButton(c, t, prev, "", IconId::ChevronLeft, vm.page > 0 ? BtnStyle::Normal : BtnStyle::Disabled,
               hits_.focusIndexOf(kHitPrev) == focus_);
    drawButton(c, t, next, "", IconId::ChevronRight, vm.page + 1 < vm.pageCount ? BtnStyle::Normal : BtnStyle::Disabled,
               hits_.focusIndexOf(kHitNext) == focus_);
    c.textIn(*t.bodyBold, Rect(prev.right(), fy, 150, Canvas::kH - fy), fmt(tr(S::Page), vm.page + 1, vm.pageCount),
             Align::Center, Color::Black);
  }
  const Rect aa(Canvas::kW - t.margin - 130, fy + 4, 130, bh);
  if (factory_) {
    const Rect menu(aa.x - 16 - 150, fy + 4, 150, bh);
    hits_.add(kHitMenu, Rect(menu.x, fy, menu.w, Canvas::kH - fy));  // full footer height (>= 83 px)
    drawButton(c, t, menu, tr(S::Menu), IconId::None, BtnStyle::Normal, hits_.focusIndexOf(kHitMenu) == focus_);
  }
  hits_.add(kHitLargeText, aa);
  drawButton(c, t, aa, "", IconId::TextSize, large_ ? BtnStyle::Primary : BtnStyle::Normal, hits_.focusIndexOf(kHitLargeText) == focus_);
}

void UiController::renderToday(Canvas& c) {
  const Theme& t = th();
  const app::TodayVM vm = port_.today(profile_);
  int y = drawHeader(c, t, hits_, kHitBack, vm.person, fmt(tr(S::OfTaken), vm.taken, vm.total));
  y += drawBanner(c, t, y, port_.status());
  const int rowH = t.large ? 120 : 104;
  const int rpp = listRowsPerPage(y, rowH);
  listPages_ = std::max(1, static_cast<int>((vm.rows.size() + rpp - 1) / rpp));
  listPage_ = std::min(listPage_, listPages_ - 1);
  rowIds_.clear();
  const size_t first = static_cast<size_t>(listPage_ * rpp);
  for (size_t i = first; i < vm.rows.size() && i < first + static_cast<size_t>(rpp); i++) {
    const app::TimelineRow& r = vm.rows[i];
    const int idx = static_cast<int>(rowIds_.size());
    const Rect rr(0, y + idx * rowH, Canvas::kW, rowH);
    rowIds_.push_back(r.eventId);
    const int id = kHitRow0 + idx;
    hits_.add(id, rr, !r.prn);
    const int timeW = textWidth(*t.title, "00:00") + 6;
    c.textIn(*t.title, Rect(t.margin, rr.y, timeW, rowH / 2 + 10), r.time, Align::Left, Color::Black);
    drawIcon(c, stateIcon(r.state), t.margin + 4, rr.y + rowH / 2 + 4);
    drawIcon(c, iconForForm(r.form), t.margin + timeW + 8, rr.y + 12);
    const int tx = t.margin + timeW + 8 + kIconSize + 10, tw = 560 - tx;
    c.textIn(*t.bodyBold, Rect(tx, rr.y + 2, tw, rowH / 2), r.med + "  " + r.dose, Align::Left, Color::Black);
    std::string sub = r.lastTaken.empty() ? tr(S::NeverTaken) : r.lastTaken;
    if (r.prn) sub = std::string(tr(S::PrnTitle)) + "  " + sub;
    else if (r.critical) sub = std::string(tr(S::Critical)) + "  " + sub;
    c.textIn(*t.small, Rect(tx, rr.y + rowH / 2, tw, rowH / 2 - 4), sub, Align::Left, Color::Black);
    const Rect sr(560, rr.y, Canvas::kW - t.margin - 560, rowH);
    if (r.state == app::RowState::Due || r.state == app::RowState::Missed)
      drawBadge(c, *t.bodyBold, sr.right(), sr.cy(), stateText(r.state));
    else c.textIn(*t.body, sr, stateText(r.state), Align::Right, Color::Black);
    c.fillRect(Rect(t.margin, rr.bottom() - 1, Canvas::kW - 2 * t.margin, 1), Color::Black);
    if (hits_.focusIndexOf(id) == focus_) drawFocusFrame(c, rr.inset(2), t.focusBorder);
  }
  if (vm.rows.empty()) c.textIn(*t.body, Rect(0, y, Canvas::kW, rowH), tr(S::NoMoreToday), Align::Center, Color::Black);
  // footer
  const int fy = footerY(), bh = Canvas::kH - fy - 6;
  c.fillRect(Rect(0, fy - 2, Canvas::kW, 2), Color::Black);
  int x = t.margin;
  if (listPages_ > 1) {
    const Rect prev(x, fy + 4, 110, bh), next(x + 120, fy + 4, 110, bh);
    hits_.add(kHitPrev, prev);
    hits_.add(kHitNext, next);
    drawButton(c, t, prev, "", IconId::ChevronLeft, listPage_ > 0 ? BtnStyle::Normal : BtnStyle::Disabled, hits_.focusIndexOf(kHitPrev) == focus_);
    drawButton(c, t, next, "", IconId::ChevronRight, listPage_ + 1 < listPages_ ? BtnStyle::Normal : BtnStyle::Disabled,
               hits_.focusIndexOf(kHitNext) == focus_);
    x += 240;
  }
  const Rect hist(Canvas::kW - t.margin - 230, fy + 4, 230, bh);
  if (vm.hasPrn) {
    const Rect prn(hist.x - 290, fy + 4, 280, bh);
    hits_.add(kHitPrn, prn);
    drawButton(c, t, prn, tr(S::PrnTitle), IconId::Pill, BtnStyle::Normal, hits_.focusIndexOf(kHitPrn) == focus_);
  }
  hits_.add(kHitHistory, hist);
  drawButton(c, t, hist, tr(S::History), IconId::History, BtnStyle::Normal, hits_.focusIndexOf(kHitHistory) == focus_);
}

void UiController::renderDetail(Canvas& c) {
  const Theme& t = th();
  const app::DoseDetailVM vm = port_.detail(eventId_);
  int y = drawHeader(c, t, hits_, kHitBack, vm.found ? vm.med : "-", vm.person);
  if (!vm.found) return;
  drawIcon(c, iconForForm(vm.form), t.margin, y + 14);
  c.textIn(*t.title, Rect(t.margin + 56, y + 4, 500, 60), vm.dose, Align::Left, Color::Black);
  if (vm.critical) drawBadge(c, *t.small, Canvas::kW - t.margin, y + 34, tr(S::Critical));
  y += 64;
  const int lh = t.body->lineHeight + 2;
  c.textIn(*t.body, Rect(t.margin, y, 760, lh), vm.scheduled + "   " + stateText(vm.state), Align::Left, Color::Black);
  y += lh;
  if (!vm.instructions.empty()) {
    std::vector<std::string> lines;
    wrapText(*t.body, vm.instructions, Canvas::kW - 2 * t.margin, 2, lines);
    for (const auto& l : lines) {
      c.textIn(*t.bodyBold, Rect(t.margin, y, 760, lh), l, Align::Left, Color::Black);
      y += lh;
    }
  }
  c.textIn(*t.body, Rect(t.margin, y, 760, lh), vm.lastTaken.empty() ? tr(S::NeverTaken) : vm.lastTaken, Align::Left, Color::Black);
  y += lh;
  if (!vm.supply.empty()) {
    std::string s = vm.supply;
    if (!vm.refillBy.empty()) s += "   " + vm.refillBy;
    if (vm.lowStock) s += "   (" + std::string(tr(S::LowStock)) + ")";
    c.textIn(*t.small, Rect(t.margin, y, 760, t.small->lineHeight + 2), s, Align::Left, Color::Black);
  }
  const int fy = footerY(), bh = Canvas::kH - fy - 6;
  c.fillRect(Rect(0, fy - 2, Canvas::kW, 2), Color::Black);
  int x = t.margin;
  auto btn = [&](int id, const char* label, IconId icon, int w, BtnStyle st) {
    const Rect r(x, fy + 4, w, bh);
    hits_.add(id, r);
    const bool f = hits_.focusIndexOf(id) == focus_;
    std::string text = label;
    if (id == kHitTaken && armed_) text = tr(S::ConfirmTaken);
    else if (id == kHitTaken && holdHint_ && f) text = tr(S::HoldToConfirm);
    drawButton(c, t, r, text, icon, st, f, id == kHitTaken && f ? hold_ : 0);
    x += w + t.gap;
  };
  if (vm.canTake) btn(kHitTaken, tr(S::Taken), IconId::Check, 260, BtnStyle::Primary);
  if (vm.canSkip) btn(kHitSkip, tr(S::Skip), IconId::Skip, 160, BtnStyle::Normal);
  if (vm.canBackfill) btn(kHitEarlier, tr(S::TakenEarlier), IconId::Clock, 200, BtnStyle::Normal);
  if (vm.canUndo) btn(kHitUndo, tr(S::Undo), IconId::Back, 160, BtnStyle::Normal);
}

void UiController::renderAlarm(Canvas& c) {
  const Theme& t = th();
  const app::AlarmVM vm = port_.alarm();
  if (!vm.active) return;
  alarmIds_.clear();
  for (const auto& m : vm.meds) alarmIds_.push_back(m.eventId);
  const int bandH = 128;
  c.fillRect(Rect(0, 0, Canvas::kW, bandH), Color::Black);
  const int timeW = 220;
  c.textIn(*t.huge, Rect(24, 0, Canvas::kW - timeW - 48, bandH - 10), vm.person, Align::Left, Color::White);
  c.textIn(*t.title, Rect(Canvas::kW - timeW - 16, 4, timeW, 64), vm.time, Align::Right, Color::White);
  if (vm.othersWaiting > 0)
    c.textIn(*t.small, Rect(Canvas::kW - 300 - 16, 64, 300, 52), fmt(tr(S::Waiting), vm.othersWaiting, vm.othersNames.c_str()),
             Align::Right, Color::White);
  else if (vm.critical)
    c.textIn(*t.bodyBold, Rect(Canvas::kW - 300 - 16, 64, 300, 52), tr(S::Critical), Align::Right, Color::White);
  else if (vm.silent)
    c.textIn(*t.bodyBold, Rect(Canvas::kW - 440 - 16, 64, 440, 52), tr(S::QuietSilent), Align::Right, Color::White);
  // medicines for this person only
  const int btnH = std::max(t.touch, 88);
  const int by = Canvas::kH - btnH - 6;
  const int rowH = t.large ? 92 : 76;
  const int maxRows = std::max(1, (by - bandH - 8) / rowH);
  int y = bandH + 6;
  for (size_t i = 0; i < vm.meds.size(); i++) {
    if (static_cast<int>(i) == maxRows - 1 && vm.meds.size() > static_cast<size_t>(maxRows)) {
      c.textIn(*t.bodyBold, Rect(80, y, 600, rowH), fmt(tr(S::MoreMeds), static_cast<int>(vm.meds.size() - i)), Align::Left,
               Color::Black);
      break;
    }
    const app::AlarmMed& m = vm.meds[i];
    drawIcon(c, iconForForm(m.form), 24, y + 8);
    c.textIn(*t.title, Rect(80, y, Canvas::kW - 104, rowH * 55 / 100), m.med + "  " + m.dose, Align::Left, Color::Black);
    if (!m.instructions.empty())
      c.textIn(*t.small, Rect(80, y + rowH * 55 / 100, Canvas::kW - 104, rowH * 45 / 100 - 2), m.instructions, Align::Left, Color::Black);
    y += rowH;
  }
  // actions
  int x = t.margin;
  auto btn = [&](int id, const std::string& label, IconId icon, int w, BtnStyle st) {
    const Rect r(x, by, w, btnH);
    hits_.add(id, r, st != BtnStyle::Disabled);
    const bool f = hits_.focusIndexOf(id) == focus_;
    std::string text = label;
    if (id == kHitTaken && armed_) text = tr(S::ConfirmTaken);
    else if (id == kHitTaken && holdHint_ && f) text = tr(S::HoldToConfirm);
    drawButton(c, t, r, text, icon, st, f, id == kHitTaken && f ? hold_ : 0, id == kHitTaken ? t.title : nullptr);
    x += w + t.gap;
  };
  if (snoozeMode_) {
    btn(kHitSnooze5, fmt(tr(S::SnoozeMinutes), 5), IconId::None, 180, BtnStyle::Normal);
    btn(kHitSnooze10, fmt(tr(S::SnoozeMinutes), 10), IconId::None, 180, BtnStyle::Normal);
    btn(kHitSnooze15, fmt(tr(S::SnoozeMinutes), 15), IconId::None, 180, BtnStyle::Normal);
    btn(kHitSnoozeBack, tr(S::Back), IconId::ChevronLeft, 198, BtnStyle::Normal);
  } else {
    btn(kHitTaken, tr(S::Taken), IconId::Check, 244, BtnStyle::Primary);
    btn(kHitSnooze, tr(S::Snooze), IconId::None, 185, vm.canSnooze ? BtnStyle::Normal : BtnStyle::Disabled);
    btn(kHitSkip, tr(S::Skip), IconId::None, 120, BtnStyle::Normal);
    btn(kHitEarlier, tr(S::TakenEarlier), IconId::None, 185, BtnStyle::Normal);
  }
}

void UiController::renderPrn(Canvas& c) {
  const Theme& t = th();
  const app::PrnVM vm = port_.prn(profile_);
  int y = drawHeader(c, t, hits_, kHitBack, std::string(tr(S::PrnTitle)) + " - " + vm.person, "");
  const int rowH = t.large ? 120 : 104;
  const int rpp = listRowsPerPage(y, rowH);
  listPages_ = std::max(1, static_cast<int>((vm.rows.size() + rpp - 1) / rpp));
  listPage_ = std::min(listPage_, listPages_ - 1);
  prnMeds_.clear();
  prnAllowed_.clear();
  prnStatus_.clear();
  if (vm.rows.empty()) c.textIn(*t.body, Rect(0, y, Canvas::kW, rowH), tr(S::PrnNone), Align::Center, Color::Black);
  const size_t first = static_cast<size_t>(listPage_ * rpp);
  for (size_t i = first; i < vm.rows.size() && i < first + static_cast<size_t>(rpp); i++) {
    const app::PrnRow& r = vm.rows[i];
    const int idx = static_cast<int>(prnMeds_.size());
    prnMeds_.push_back(r.medId);
    prnAllowed_.push_back(r.allowed);
    prnStatus_.push_back(r.status);
    const Rect rr(0, y + idx * rowH, Canvas::kW, rowH);
    drawIcon(c, iconForForm(r.form), t.margin, rr.y + 14);
    const int tx = t.margin + 56, tw = 520 - tx;
    c.textIn(*t.bodyBold, Rect(tx, rr.y + 2, tw, rowH / 2), r.med + "  " + r.dose, Align::Left, Color::Black);
    c.textIn(r.allowed ? *t.small : *t.bodyBold, Rect(tx, rr.y + rowH / 2, tw, rowH / 2 - 4), r.status, Align::Left, Color::Black);
    const Rect b(Canvas::kW - t.margin - 250, rr.y + (rowH - t.touch) / 2, 250, t.touch);
    const int id = kHitLog0 + idx;
    hits_.add(id, b, r.allowed);
    const bool f = hits_.focusIndexOf(id) == focus_;
    std::string label = tr(S::LogDose);
    if (armed_ && armedId_ == id) label = tr(S::ConfirmTaken);
    else if (holdHint_ && f) label = tr(S::HoldToConfirm);
    drawButton(c, t, b, label, IconId::None, r.allowed ? BtnStyle::Primary : BtnStyle::Disabled, f, f ? hold_ : 0);
    c.fillRect(Rect(t.margin, rr.bottom() - 1, Canvas::kW - 2 * t.margin, 1), Color::Black);
  }
  if (listPages_ > 1) {
    const int fy = footerY(), bh = Canvas::kH - fy - 6;
    const Rect prev(t.margin, fy + 4, 110, bh), next(t.margin + 120, fy + 4, 110, bh);
    hits_.add(kHitPrev, prev);
    hits_.add(kHitNext, next);
    drawButton(c, t, prev, "", IconId::ChevronLeft, BtnStyle::Normal, hits_.focusIndexOf(kHitPrev) == focus_);
    drawButton(c, t, next, "", IconId::ChevronRight, BtnStyle::Normal, hits_.focusIndexOf(kHitNext) == focus_);
  }
}

static void drawCellGlyph(Canvas& c, const Theme& t, const Rect& r, const app::DayCell& cell, bool showNumber) {
  if (cell.state == app::CellState::Blank) return;
  char num[4];
  snprintf(num, sizeof(num), "%d", cell.dayOfMonth);
  const Rect in = r.inset(3);
  switch (cell.state) {
    case app::CellState::Perfect:
      c.fillRoundRect(in, 6, Color::Black);
      if (showNumber) c.textIn(*t.small, in, num, Align::Center, Color::White);
      break;
    case app::CellState::Partial:
      c.roundRect(in, 6, 4, Color::Black);
      c.fillRect(Rect(in.x, in.bottom() - in.h / 3, in.w, in.h / 3), Color::Black);
      if (showNumber) c.textIn(*t.small, Rect(in.x, in.y, in.w, in.h * 2 / 3), num, Align::Center, Color::Black);
      break;
    case app::CellState::Missed:
      c.roundRect(in, 6, 2, Color::Black);
      c.line(in.x + 6, in.y + 6, in.right() - 7, in.bottom() - 7, 3, Color::Black);
      c.line(in.right() - 7, in.y + 6, in.x + 6, in.bottom() - 7, 3, Color::Black);
      if (showNumber) {
        const int w = textWidth(*t.small, num) + 8;
        c.fillRect(Rect(in.cx() - w / 2, in.cy() - t.small->ascent / 2 - 2, w, t.small->ascent), Color::White);
        c.textIn(*t.small, in, num, Align::Center, Color::Black);
      }
      break;
    default:  // NoData / Future
      if (showNumber) c.textIn(*t.small, in, num, Align::Center, Color::Black);
      if (cell.state == app::CellState::Future)
        for (int x = in.x + 4; x < in.right() - 4; x += 6) c.pixel(x, in.bottom() - 4, Color::Black);
      break;
  }
  if (cell.today) c.rect(r, 3, Color::Black);
}

void UiController::renderHistory(Canvas& c) {
  const Theme& t = th();
  if (histAnchor_ == 0) histAnchor_ = port_.localNow().days;
  const app::HistoryVM vm = port_.history(profile_, histMode_, histAnchor_, perMed_);
  int y = drawHeader(c, t, hits_, kHitBack, std::string(tr(S::History)) + " - " + vm.person, "");
  const int sh = t.bodyBold->lineHeight + 8;
  c.textIn(*t.bodyBold, Rect(t.margin, y, 460, sh), vm.periodTitle, Align::Left, Color::Black);
  c.textIn(*t.bodyBold, Rect(480, y, Canvas::kW - 480 - t.margin, sh), fmt(tr(S::Adherence), vm.percent), Align::Right, Color::Black);
  y += sh;
  const int fy = footerY();
  const int bodyH = fy - 4 - y;
  const int lh = t.small->lineHeight + 2;
  if (perMed_) {
    int yy = y;
    if (vm.perMed.empty()) c.textIn(*t.body, Rect(0, y, Canvas::kW, 60), tr(S::NoData), Align::Center, Color::Black);
    for (const auto& m : vm.perMed) {
      if (yy + t.body->lineHeight > fy) break;
      c.textIn(*t.body, Rect(t.margin, yy, 420, t.body->lineHeight), m.med, Align::Left, Color::Black);
      drawProgress(c, Rect(450, yy + 10, 200, t.body->lineHeight - 20), m.percent, 100);
      c.textIn(*t.bodyBold, Rect(660, yy, Canvas::kW - 660 - t.margin, t.body->lineHeight), fmt("%d%%", m.percent), Align::Right,
               Color::Black);
      yy += t.body->lineHeight + 4;
    }
  } else if (vm.mode == app::HistoryMode::Month) {
    const int gx = t.margin, gw = 500, cw = gw / 7;
    const int ch = (bodyH - lh) / 6;
    for (int i = 0; i < 7; i++)
      c.textIn(*t.small, Rect(gx + i * cw, y, cw, lh), app::weekdayShort((i + 1) % 7), Align::Center, Color::Black);
    for (size_t i = 0; i < vm.cells.size(); i++) {
      const Rect r(gx + static_cast<int>(i % 7) * cw, y + lh + static_cast<int>(i / 7) * ch, cw, ch);
      drawCellGlyph(c, t, r, vm.cells[i], true);
    }
    int sy = y + 8;
    const int sx = gx + gw + 24, sw = Canvas::kW - sx - t.margin;
    c.textIn(*t.small, Rect(sx, sy, sw, lh), fmt(tr(S::Streak), vm.streak), Align::Left, Color::Black);
    sy += lh;
    c.textIn(*t.small, Rect(sx, sy, sw, lh), fmt(tr(S::Longest), vm.longest), Align::Left, Color::Black);
    sy += lh + 6;
    const std::string counts[] = {fmt("%s %d", tr(S::StateTaken), vm.onTime), fmt("%s %d", tr(S::StateTakenLate), vm.late),
                                  fmt("%s %d", tr(S::StateSkipped), vm.skipped), fmt("%s %d", tr(S::StateMissed), vm.missed)};
    for (const auto& s : counts) {
      c.textIn(*t.small, Rect(sx, sy, sw, lh), s, Align::Left, Color::Black);
      sy += lh;
    }
  } else if (vm.mode == app::HistoryMode::Week) {
    const int cw = (Canvas::kW - 2 * t.margin) / 7;
    const int cellH = bodyH - 2 * lh - 8;
    for (size_t i = 0; i < vm.cells.size(); i++) {
      const app::DayCell& cell = vm.cells[i];
      const int x = t.margin + static_cast<int>(i) * cw;
      c.textIn(*t.small, Rect(x, y, cw, lh), fmt("%s %d", app::weekdayShort(core::weekdayFromDays(cell.day)), cell.dayOfMonth),
               Align::Center, Color::Black);
      const Rect box(x + 6, y + lh + 4, cw - 12, cellH - lh * 2);
      drawCellGlyph(c, t, box, cell, false);
      if (cell.state != app::CellState::Future)
        c.textIn(*t.bodyBold, Rect(x, box.bottom() + 2, cw, lh + 8), fmt("%d/%d", cell.taken, cell.total), Align::Center,
                 Color::Black);
    }
    const int sy = y + cellH + 4;
    c.textIn(*t.small, Rect(t.margin, sy, 760, lh),
             fmt(tr(S::Streak), vm.streak) + "    " + fmt(tr(S::Longest), vm.longest), Align::Left, Color::Black);
    c.textIn(*t.small, Rect(t.margin, sy + lh, 760, lh), fmt(tr(S::Counts), vm.onTime, vm.late, vm.skipped, vm.missed),
             Align::Left, Color::Black);
  } else {
    int yy = y;
    if (vm.rows.empty()) c.textIn(*t.body, Rect(0, y, Canvas::kW, 60), tr(S::NoData), Align::Center, Color::Black);
    for (const auto& r : vm.rows) {
      if (yy + t.body->lineHeight > fy - lh) break;
      c.textIn(*t.bodyBold, Rect(t.margin, yy, 110, t.body->lineHeight), r.time, Align::Left, Color::Black);
      drawIcon(c, stateIcon(r.state), 130, yy + (t.body->lineHeight - kIconSize) / 2);
      c.textIn(*t.body, Rect(184, yy, 400, t.body->lineHeight), r.med, Align::Left, Color::Black);
      c.textIn(*t.body, Rect(590, yy, Canvas::kW - 590 - t.margin, t.body->lineHeight), stateText(r.state), Align::Right, Color::Black);
      yy += t.body->lineHeight;
    }
    c.textIn(*t.small, Rect(t.margin, fy - lh - 4, 760, lh), fmt(tr(S::Counts), vm.onTime, vm.late, vm.skipped, vm.missed),
             Align::Left, Color::Black);
  }
  // footer
  const int bh = Canvas::kH - fy - 6;
  c.fillRect(Rect(0, fy - 2, Canvas::kW, 2), Color::Black);
  const Rect prev(t.margin, fy + 4, 110, bh), next(t.margin + 120, fy + 4, 110, bh);
  const bool canNext = vm.toDay < port_.localNow().days;
  hits_.add(kHitPrev, prev);
  hits_.add(kHitNext, next, canNext);
  drawButton(c, t, prev, "", IconId::ChevronLeft, BtnStyle::Normal, hits_.focusIndexOf(kHitPrev) == focus_);
  drawButton(c, t, next, "", IconId::ChevronRight, canNext ? BtnStyle::Normal : BtnStyle::Disabled, hits_.focusIndexOf(kHitNext) == focus_);
  const char* modeLabel = vm.mode == app::HistoryMode::Day ? tr(S::Day) : vm.mode == app::HistoryMode::Week ? tr(S::Week) : tr(S::Month);
  const Rect mode(next.right() + 24, fy + 4, 230, bh);
  hits_.add(kHitMode, mode);
  drawButton(c, t, mode, modeLabel, IconId::History, BtnStyle::Normal, hits_.focusIndexOf(kHitMode) == focus_);
  const Rect pm(Canvas::kW - t.margin - 200, fy + 4, 200, bh);
  hits_.add(kHitPerMed, pm);
  drawButton(c, t, pm, perMed_ ? tr(S::AllMedications) : tr(S::ByMedication), IconId::None, perMed_ ? BtnStyle::Primary : BtnStyle::Normal,
             hits_.focusIndexOf(kHitPerMed) == focus_);
}

// Buttons on the clock and backfill screens: UP/DOWN change the focused field, OK moves to the
// next field; past the last field the Save button has the focus.
bool UiController::handleEntryKeys(ValueEntry& e, const InputEvent& ev, uint32_t now) {
  (void)now;
  if (ev.type == InputType::Up || ev.type == InputType::Down) {
    const int dir = ev.type == InputType::Up ? 1 : -1;
    if (e.onDone()) e.setFocus(dir > 0 ? e.count() - 1 : 0);
    else e.stepFocused(dir);
    return true;
  }
  if (ev.type == InputType::OkClick && !e.onDone()) {
    if (e.next()) focusId(kHitSave);
    return true;
  }
  return false;  // OK on Save (and holds) go through the normal path
}

// The backfill time is a wall-clock HH:MM: pick the instant in [bfMin_, bfMax_] nearest to the
// previous choice (yesterday, today or tomorrow's date), so a dose before midnight works too.
// The UTC offset in effect now (the backfill window is at most a day, so one offset is enough).
int64_t UiController::localOffsetSec() {
  const core::LocalTime lt = port_.localNow();
  return static_cast<int64_t>(lt.days) * 86400 + lt.secOfDay - port_.nowUtc();
}
int32_t UiController::localDay(int64_t utc) {
  const int64_t l = utc + localOffsetSec();
  return static_cast<int32_t>(l >= 0 ? l / 86400 : (l - 86399) / 86400);
}
int UiController::localSec(int64_t utc) {
  const int64_t l = utc + localOffsetSec();
  return static_cast<int>(((l % 86400) + 86400) % 86400);
}

void UiController::syncBackfillFromEntry() {
  bfMax_ = port_.nowUtc();
  const int64_t off = localOffsetSec();
  const int32_t base = localDay(bfAt_);
  int64_t best = bfAt_;
  int64_t bestDist = -1;
  for (int d = -1; d <= 1; ++d) {
    const int64_t cand = static_cast<int64_t>(base + d) * 86400 + bf_.minute() * 60 - off;
    const int64_t clamped = std::min(std::max(cand, bfMin_), bfMax_);
    const int64_t dist = (clamped != cand ? 1000000000LL : 0) + (cand > bfAt_ ? cand - bfAt_ : bfAt_ - cand);
    if (bestDist < 0 || dist < bestDist) {
      bestDist = dist;
      best = clamped;
    }
  }
  bfAt_ = best - best % 60;
  // Show what was actually accepted (clamped to the allowed window).
  const int m = localSec(bfAt_) / 60;
  if (!bf_.typing() && bf_.minute() != m) {
    const int f = bf_.focus();
    bf_ = ValueEntry::time(m);
    bf_.setFocus(f);
  }
}

void UiController::renderClockSet(Canvas& c) {
  const Theme& t = th();
  if (!csInit_) {
    const DateTimeValue v = defaultClockValue(!port_.clockUnset(), port_.localNow(), firmwareBuildDate(), firmwareBuildTime());
    cs_ = ValueEntry::dateTime(v.day, v.minute);
    csInit_ = true;
  }
  const int hh = 56;
  c.textIn(*t.title, Rect(t.margin, 0, Canvas::kW - 2 * t.margin, hh), tr(S::ClockSetTitle), Align::Left, Color::Black);
  c.fillRect(Rect(0, hh - 2, Canvas::kW, 2), Color::Black);
  drawValueEntry(c, t, cs_, hh + 4, hits_, kHitEntry0);
  const int fy = footerY(), bh = Canvas::kH - fy - 6;
  const Rect save(Canvas::kW - t.margin - 260, fy + 4, 260, bh);
  hits_.add(kHitSave, save);
  drawButton(c, t, save, tr(S::Save), IconId::Check, BtnStyle::Primary, cs_.onDone());
  if (!cs_.typing())
    c.textIn(*t.small, Rect(t.margin, fy + 4, save.x - 2 * t.margin, bh), tr(S::ClockSetHint), Align::Left, Color::Black);
}

void UiController::renderBackfill(Canvas& c) {
  const Theme& t = th();
  const int hh = 56;
  c.textIn(*t.title, Rect(t.margin, 0, Canvas::kW - 2 * t.margin, hh), tr(S::WhenTaken), Align::Left, Color::Black);
  c.fillRect(Rect(0, hh - 2, Canvas::kW, 2), Color::Black);
  const int top = hh + 4;
  drawValueEntry(c, t, bf_, top, hits_, kHitEntry0);
  // Replace the plain "HH:MM" summary with the date and how long ago it was.
  const Rect sum = layoutValueEntry(bf_, top).summary;
  c.fillRect(sum, Color::White);
  const int ago = static_cast<int>((port_.nowUtc() - bfAt_) / 60);
  c.textIn(*t.bodyBold, sum, app::fmtShortDate(localDay(bfAt_)) + ", " + app::fmtHm(localSec(bfAt_) / 60) + "  -  " + fmt(tr(S::MinutesAgo), ago),
           Align::Center, Color::Black);
  const int fy = footerY(), bh = Canvas::kH - fy - 6;
  const Rect save(t.margin, fy + 4, 360, bh), cancel(Canvas::kW - t.margin - 220, fy + 4, 220, bh);
  hits_.add(kHitSave, save);
  hits_.add(kHitCancel, cancel);
  const bool f = bf_.onDone() || hits_.focusIndexOf(kHitSave) == focus_;
  std::string label = tr(S::Save);
  if (armed_) label = tr(S::ConfirmTaken);
  else if (holdHint_ && f) label = tr(S::HoldToConfirm);
  drawButton(c, t, save, label, IconId::Check, BtnStyle::Primary, bf_.onDone(), bf_.onDone() ? hold_ : 0);
  drawButton(c, t, cancel, tr(S::Cancel), IconId::None, BtnStyle::Normal, false);
}

void UiController::renderBatteryEmpty(Canvas& c) {
  const Theme& t = th();
  c.fillRect(Rect(0, 0, Canvas::kW, 150), Color::Black);
  c.fillRoundRect(Rect(32, 47, 56, 56), 10, Color::White);
  drawIcon(c, IconId::BatteryLow, 40, 55);
  c.textIn(*t.huge, Rect(110, 0, Canvas::kW - 140, 150), tr(S::BatteryEmptyTitle), Align::Left, Color::White);
  std::vector<std::string> lines;
  wrapText(*t.title, tr(S::BatteryEmptyBody), Canvas::kW - 2 * t.margin - 16, 3, lines);
  int y = 180;
  for (const auto& l : lines) {
    c.textIn(*t.title, Rect(t.margin + 8, y, Canvas::kW - 2 * t.margin, t.title->lineHeight), l, Align::Left, Color::Black);
    y += t.title->lineHeight;
  }
  c.textIn(*t.body, Rect(t.margin + 8, 400, Canvas::kW - 2 * t.margin, 60), tr(S::BatteryEmptyHint), Align::Left, Color::Black);
}

void UiController::renderToast(Canvas& c) {
  const Theme& t = th();
  const int h = t.touch + 8;
  const int y = (scr_ == ScreenId::Alarm ? Canvas::kH - std::max(t.touch, 88) - 6 : footerY()) - h - 6;
  const Rect r(t.margin, y, Canvas::kW - 2 * t.margin, h);
  c.fillRoundRect(r, t.radius, Color::Black);
  int rightW = 0;
  if (toastUndo_) {
    const Rect u(r.right() - 200 - 4, r.y + 4, 200, t.touch);
    hits_.add(kHitToastUndo, u);
    const bool f = hits_.focusIndexOf(kHitToastUndo) == focus_;
    c.fillRoundRect(u, t.radius, f ? Color::Black : Color::White);
    c.roundRect(u, t.radius, 3, Color::White);
    c.textIn(*t.bodyBold, u, tr(S::Undo), Align::Center, f ? Color::White : Color::Black);
    rightW = 210;
  }
  c.textIn(*t.bodyBold, Rect(r.x + 20, r.y, r.w - 40 - rightW, r.h), toast_, Align::Left, Color::White);
}

}  // namespace ui
