#pragma once
// Immediate-mode widgets drawn into a Canvas. Every interactive widget is
// registered in the HitMap by the caller with the rect it was drawn in.
#include <string>

#include "app_core/view_models.h"
#include "ui_core/canvas.h"
#include "ui_core/icons.h"
#include "ui_core/input.h"
#include "ui_core/theme.h"

namespace ui {

enum class BtnStyle : uint8_t { Normal, Primary, Disabled };

// fillPermille > 0 draws hold-to-confirm progress from the left.
// Font/icon choice so a label fits a button: drop the icon first, then use smaller fonts.
struct ButtonFit {
  const Font* font;
  bool icon;
};
ButtonFit fitButton(const Theme& t, const Font* preferred, int width, const std::string& label, bool hasIcon);
void drawButton(Canvas& c, const Theme& t, const Rect& r, const std::string& label, IconId icon, BtnStyle st, bool focused,
                int fillPermille = 0, const Font* font = nullptr);
// Rounded black badge with white text, right-aligned at rightX and vertically centred on cy. Returns its rect.
Rect drawBadge(Canvas& c, const Font& f, int rightX, int cy, const std::string& text);
void drawProgress(Canvas& c, const Rect& r, int num, int den);
// Sub-screen header with a Back button (hit id `backId`, -1 = none) and an optional right text. Returns its height.
int drawHeader(Canvas& c, const Theme& t, HitMap& hm, int backId, const std::string& title, const std::string& right);
// Status banner under the header (highest-priority problem). Returns the height used (0 = none).
int drawBanner(Canvas& c, const Theme& t, int y, const app::StatusFlags& s);
const char* bannerText(app::BannerKind k);
// Status icons (battery, SD, Wi-Fi, clock) right-aligned at rightX; returns the left edge.
int drawStatusIcons(Canvas& c, const Theme& t, int rightX, int y, const app::StatusFlags& s);
IconId stateIcon(app::RowState s);
const char* stateText(app::RowState s);
// Dashed outline for focus on list rows.
void drawFocusFrame(Canvas& c, const Rect& r, int thickness);

}  // namespace ui
