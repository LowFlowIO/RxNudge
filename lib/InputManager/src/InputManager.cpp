#include "InputManager.h"

#include <Wire.h>
#include "board_pins.h"
#include "Gt911Touch.h"
#include "ui_core/touch_filter.h"

const char* InputManager::BUTTON_NAMES[] = {"Back", "Confirm", "Left", "Right", "Up", "Down", "Power"};


InputManager::InputManager()
    : currentState(0),
      lastState(0),
      pressedEvents(0),
      releasedEvents(0),
      lastDebounceTime(0),
      buttonPressStart(0),
      buttonPressFinish(0) {}

static Gt911Touch touch;
static bool touchReady = false;
static ui::TouchFilter touchFilter;     // one tap per touch, on release (ui_core/touch_filter.h)
static volatile bool touchGuardReq = false;
static uint8_t touchUiOrient = 0;
static bool pendingTap = false;
static int pendingTapX = 0;
static int pendingTapY = 0;

static void panelToLogical(uint16_t px, uint16_t py, int& lx, int& ly) {
  const int W = 800;
  const int H = 480;
  switch (touchUiOrient) {
    case 0:
      lx = H - 1 - (int)py;
      ly = (int)px;
      break;
    case 1:
      lx = W - 1 - (int)px;
      ly = H - 1 - (int)py;
      break;
    case 2:
      lx = (int)py;
      ly = W - 1 - (int)px;
      break;
    default:
      lx = (int)px;
      ly = (int)py;
      break;
  }
}

void InputManager::begin() {
  pinMode(BTN_PIN_POWER, INPUT_PULLUP);
  pinMode(BTN_PIN_UP, INPUT_PULLUP);
  pinMode(BTN_PIN_DOWN, INPUT_PULLUP);
  touchReady = touch.begin(Wire, TOUCH_SDA, TOUCH_SCL, TOUCH_RST, TOUCH_INT, TOUCH_EN, TOUCH_I2C_ADDR);
}


uint8_t InputManager::getState() {
  uint8_t state = 0;

  if (digitalRead(BTN_PIN_POWER) == LOW) {
    state |= (1 << BTN_POWER);
    // Shared OK/PWR key: while held it also counts as Confirm so menus work
    // with only three buttons. Short-press-as-home is handled in main.cpp.
    state |= (1 << BTN_CONFIRM);
  }
  if (digitalRead(BTN_PIN_UP) == LOW) {
    state |= (1 << BTN_UP);
  }
  if (digitalRead(BTN_PIN_DOWN) == LOW) {
    state |= (1 << BTN_DOWN);
  }

  if (touchReady) {
    const unsigned long now = millis();
    if (touchGuardReq) {
      touchGuardReq = false;
      touchFilter.guard(now);
    }
    const Gt911Touch::Report raw = touch.read();
    ui::TouchReport r;
    r.kind = static_cast<ui::TouchReport::Kind>(raw.kind);
    r.count = raw.count;
    r.trackId = raw.trackId;
    if (raw.kind == Gt911Touch::Report::Down) {
      int lx, ly;
      panelToLogical(raw.x, raw.y, lx, ly);
      r.x = static_cast<int16_t>(lx);
      r.y = static_cast<int16_t>(ly);
    }
    const ui::TouchTap t = touchFilter.feed(r, now);
    if (t.tap) {
      pendingTap = true;
      pendingTapX = t.x;
      pendingTapY = t.y;
    }
  }

  return state;
}

void InputManager::update() {
  const unsigned long currentTime = millis();
  const uint8_t state = getState();

  pressedEvents = 0;
  releasedEvents = 0;

  if (state != lastState) {
    lastDebounceTime = currentTime;
    lastState = state;
  }

  if ((currentTime - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (state != currentState) {
      pressedEvents = state & ~currentState;
      releasedEvents = currentState & ~state;

      if (pressedEvents > 0 && currentState == 0) {
        buttonPressStart = currentTime;
      }

      if (releasedEvents > 0 && state == 0) {
        buttonPressFinish = currentTime;
      }

      currentState = state;
    }
  }
}

bool InputManager::isPressed(const uint8_t buttonIndex) const {
  return currentState & (1 << buttonIndex);
}

bool InputManager::wasPressed(const uint8_t buttonIndex) const {
  return pressedEvents & (1 << buttonIndex);
}

bool InputManager::wasAnyPressed() const {
  return pressedEvents > 0;
}

bool InputManager::wasReleased(const uint8_t buttonIndex) const {
  return releasedEvents & (1 << buttonIndex);
}

bool InputManager::wasAnyReleased() const {
  return releasedEvents > 0;
}

unsigned long InputManager::getHeldTime() const {
  if (currentState > 0) {
    return millis() - buttonPressStart;
  }
  return buttonPressFinish - buttonPressStart;
}

const char* InputManager::getButtonName(const uint8_t buttonIndex) {
  if (buttonIndex <= BTN_POWER) {
    return BUTTON_NAMES[buttonIndex];
  }
  return "Unknown";
}

bool InputManager::isPowerButtonPressed() const {
  return isPressed(BTN_POWER);
}

void InputManager::setUiOrientation(uint8_t orientation) { touchUiOrient = orientation; }

void InputManager::touchGuard() { touchGuardReq = true; }

bool InputManager::consumeTap(int& x, int& y) {
  if (!pendingTap) return false;
  pendingTap = false;
  x = pendingTapX;
  y = pendingTapY;
  return true;
}
