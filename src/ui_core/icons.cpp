#include "ui_core/icons.h"

namespace ui {

void drawIcon(Canvas& c, IconId id, int x, int y, Color color) {
  if (id == IconId::None || id >= IconId::Count) return;
  c.bitmap(x, y, kIconBits[static_cast<int>(id)], kIconSize, kIconSize, color);
}

IconId iconForForm(core::MedForm f) {
  switch (f) {
    case core::MedForm::Capsule: return IconId::Capsule;
    case core::MedForm::Liquid: return IconId::Liquid;
    case core::MedForm::Injection: return IconId::Injection;
    case core::MedForm::Inhaler: return IconId::Inhaler;
    case core::MedForm::Drops: return IconId::Drops;
    case core::MedForm::Patch: return IconId::Patch;
    default: return IconId::Pill;
  }
}

}  // namespace ui
