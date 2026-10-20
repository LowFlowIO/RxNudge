#include "ui_core/theme.h"

#include "ui_core/assets/fonts.h"

namespace ui {

const Theme& normalTheme() {
  static const Theme t = [] {
    Theme x;
    x.large = false;
    x.small = &atk14r;
    x.body = &atk18r;
    x.bodyBold = &atk18b;
    x.title = &atk24b;
    x.huge = &atk40b;
    return x;
  }();
  return t;
}

const Theme& largeTheme() {
  static const Theme t = [] {
    Theme x;
    x.large = true;
    x.small = &atk18r;
    x.body = &atk24r;
    x.bodyBold = &atk24b;
    x.title = &atk24b;
    x.huge = &atk40b;
    x.headerH = 96;
    x.homeHeaderH = 72;
    return x;
  }();
  return t;
}

}  // namespace ui
