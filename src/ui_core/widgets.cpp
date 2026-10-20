#include "ui_core/widgets.h"

#include <cstdio>

#include "ui_core/strings.h"

namespace ui {

ButtonFit fitButton(const Theme& t, const Font* preferred, int width, const std::string& label, bool hasIcon) {
  ButtonFit b{preferred, hasIcon};
  const int pad = 16, iconW = kIconSize + 8;
  if (label.empty()) return b;
  if (textWidth(*b.font, label) + (b.icon ? iconW : 0) <= width - pad) return b;
  b.icon = false;  // 1) drop the icon
  if (textWidth(*b.font, label) <= width - pad) return b;
  for (const Font* f : {t.body, t.small}) {  // 2) step down to smaller fonts
    if (!f || textWidth(*f, label) > width - pad) continue;
    b.font = f;
    b.icon = hasIcon && textWidth(*f, label) + iconW <= width - pad;
    return b;
  }
  b.font = t.small ? t.small : preferred;  // 3) truncated with an ellipsis (last resort)
  return b;
}

void drawButton(Canvas& c, const Theme& t, const Rect& r, const std::string& label, IconId icon, BtnStyle st, bool focused,
                int fillPermille, const Font* font) {
  const ButtonFit fit = fitButton(t, font ? font : t.bodyBold, r.w, label, icon != IconId::None);
  const Font& f = *fit.font;
  if (!fit.icon) icon = IconId::None;
  const bool filled = st == BtnStyle::Primary || (focused && st == BtnStyle::Normal);
  if (filled) c.fillRoundRect(r, t.radius, Color::Black);
  else {
    c.fillRoundRect(r, t.radius, Color::White);
    c.roundRect(r, t.radius, st == BtnStyle::Disabled ? 1 : t.border, Color::Black);
  }
  if (focused && st == BtnStyle::Primary) c.roundRect(r.inset(5), t.radius - 4, 4, Color::White);
  if (focused && st == BtnStyle::Disabled) c.roundRect(r, t.radius, t.focusBorder, Color::Black);
  const Color fg = filled ? Color::White : Color::Black;
  const int iconW = icon == IconId::None ? 0 : kIconSize + 8;
  const int tw = std::min(textWidth(f, label), r.w - iconW - 16);
  const int x0 = r.x + (r.w - (iconW + tw)) / 2;
  if (iconW) drawIcon(c, icon, x0, r.cy() - kIconSize / 2, fg);
  c.textIn(f, Rect(x0 + iconW, r.y, r.w - (x0 - r.x) - iconW - 8, r.h), label, Align::Left, fg);
  if (st == BtnStyle::Disabled) {  // light cross-hatch so disabled is visible without colour
    for (int x = r.x + 8; x < r.right() - 8; x += 8) c.pixel(x, r.bottom() - 10, Color::Black);
  }
  if (fillPermille > 0) {
    // progress: inverted band growing from the left
    const int w = (r.w - 12) * std::min(fillPermille, 1000) / 1000;
    c.invert(Rect(r.x + 6, r.y + 6, w, r.h - 12));
  }
}

Rect drawBadge(Canvas& c, const Font& f, int rightX, int cy, const std::string& text) {
  const int w = textWidth(f, text) + 24, h = f.ascent + 12;
  const Rect r(rightX - w, cy - h / 2, w, h);
  c.fillRoundRect(r, h / 2, Color::Black);
  c.textIn(f, Rect(r.x + 12, r.y, r.w - 24, r.h), text, Align::Center, Color::White);
  return r;
}

void drawProgress(Canvas& c, const Rect& r, int num, int den) {
  c.rect(r, 2, Color::Black);
  if (den > 0 && num > 0) c.fillRect(Rect(r.x + 4, r.y + 4, (r.w - 8) * std::min(num, den) / den, r.h - 8), Color::Black);
}

int drawHeader(Canvas& c, const Theme& t, HitMap& hm, int backId, const std::string& title, const std::string& right) {
  const int h = t.headerH;
  int x = t.margin;
  if (backId >= 0) {
    const Rect b(0, 0, 176, h - 4);
    drawButton(c, t, b.inset(6), tr(S::Back), IconId::ChevronLeft, BtnStyle::Normal, false, 0, t.body);
    hm.add(backId, b);
    x = b.right() + t.gap;
  }
  int rightW = 0;
  if (!right.empty()) {
    rightW = textWidth(*t.body, right) + t.margin;
    c.textIn(*t.body, Rect(Canvas::kW - rightW, 0, rightW - t.margin, h), right, Align::Right, Color::Black);
  }
  c.textIn(*t.title, Rect(x, 0, Canvas::kW - x - rightW - t.gap, h), title, Align::Left, Color::Black);
  c.fillRect(Rect(0, h - 3, Canvas::kW, 3), Color::Black);
  return h;
}

const char* bannerText(app::BannerKind k) {
  switch (k) {
    case app::BannerKind::ClockUnset: return tr(S::BannerClockUnset);
    case app::BannerKind::VeryLowBattery: return tr(S::BannerVeryLowBattery);
    case app::BannerKind::StorageError: return tr(S::BannerStorage);
    case app::BannerKind::AlarmHw: return tr(S::BannerAlarmHw);
    case app::BannerKind::ClockUntrusted: return tr(S::BannerClockUntrusted);
    case app::BannerKind::LowBattery: return tr(S::BannerLowBattery);
    case app::BannerKind::SdError: return tr(S::BannerSdError);
    case app::BannerKind::BackupFailed: return tr(S::BannerBackupFailed);
    case app::BannerKind::SdMissing: return tr(S::BannerSdMissing);
    default: return "";
  }
}

int drawBanner(Canvas& c, const Theme& t, int y, const app::StatusFlags& s) {
  int more = 0;
  const app::BannerKind k = app::topBanner(s, &more);
  if (k == app::BannerKind::None) return 0;
  const int h = t.bodyBold->lineHeight + 8;
  c.fillRect(Rect(0, y, Canvas::kW, h), Color::Black);
  drawIcon(c, IconId::Warning, t.margin, y + (h - kIconSize) / 2, Color::White);
  std::string text = bannerText(k);
  int rightW = 0;
  if (more > 0) {
    char buf[16];
    snprintf(buf, sizeof(buf), tr(S::BannerMore), more);
    rightW = textWidth(*t.bodyBold, buf) + t.margin;
    c.textIn(*t.bodyBold, Rect(Canvas::kW - rightW, y, rightW - t.margin, h), buf, Align::Right, Color::White);
  }
  const int x = t.margin + kIconSize + t.gap;
  c.textIn(*t.bodyBold, Rect(x, y, Canvas::kW - x - rightW - t.gap, h), text, Align::Left, Color::White);
  return h;
}

int drawStatusIcons(Canvas& c, const Theme& t, int rightX, int y, const app::StatusFlags& s) {
  int x = rightX;
  if (s.batteryPct >= 0) {
    char buf[16];
    if (s.usbPowered && !s.charging) snprintf(buf, sizeof(buf), "USB %d%%", s.batteryPct);
    else snprintf(buf, sizeof(buf), "%d%%", s.batteryPct);
    const int w = textWidth(*t.small, buf);
    x -= w;
    c.text(*t.small, x, y + kIconSize / 2 + t.small->ascent * 36 / 100, buf, Color::Black);
    x -= kIconSize + 4;
    drawIcon(c, (s.charging || s.usbPowered) ? IconId::Charging : (s.lowBattery ? IconId::BatteryLow : IconId::Battery), x, y);
    x -= 12;
  }
  if (s.sdPresent || s.sdError) {
    x -= kIconSize;
    drawIcon(c, s.sdError ? IconId::SdError : IconId::Sd, x, y);
    x -= 8;
  }
  x -= kIconSize;
  drawIcon(c, s.wifiOn ? IconId::Wifi : IconId::WifiOff, x, y);
  x -= 8;
  if (s.clockUnset || s.clockUntrusted) {
    x -= kIconSize;
    drawIcon(c, IconId::Clock, x, y);
    c.line(x, y + kIconSize, x + kIconSize, y, 4, Color::Black);
    x -= 8;
  }
  return x;
}

IconId stateIcon(app::RowState s) {
  switch (s) {
    case app::RowState::Taken: return IconId::Check;
    case app::RowState::TakenLate: return IconId::CheckLate;
    case app::RowState::Skipped: return IconId::Skip;
    case app::RowState::Missed: return IconId::Cross;
    case app::RowState::Due: return IconId::Bell;
    case app::RowState::Snoozed: return IconId::Snooze;
    case app::RowState::Pending: return IconId::Pending;
    default: return IconId::None;
  }
}

const char* stateText(app::RowState s) {
  switch (s) {
    case app::RowState::Taken: return tr(S::StateTaken);
    case app::RowState::TakenLate: return tr(S::StateTakenLate);
    case app::RowState::Skipped: return tr(S::StateSkipped);
    case app::RowState::Missed: return tr(S::StateMissed);
    case app::RowState::Due: return tr(S::StateDue);
    case app::RowState::Snoozed: return tr(S::StateSnoozed);
    case app::RowState::Pending: return tr(S::StatePending);
    default: return "-";
  }
}

void drawFocusFrame(Canvas& c, const Rect& r, int t) { c.rect(r, t, Color::Black); }

}  // namespace ui
