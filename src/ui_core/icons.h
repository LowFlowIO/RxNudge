#pragma once
#include "core/model.h"
#include "ui_core/assets/icon_ids.h"
#include "ui_core/canvas.h"

namespace ui {

extern const uint8_t kIconBits[][200];
void drawIcon(Canvas& c, IconId id, int x, int y, Color color = Color::Black);
IconId iconForForm(core::MedForm f);

}  // namespace ui
