#pragma once

#include <Arduino.h>
#include <Wire.h>

// Tiny GT911 single-point reader used to synthesize Back / Left / Right
// on the Sticky, which only has three physical keys.
class Gt911Touch {
 public:
  struct Point {
    bool down = false;
    uint16_t x = 0;
    uint16_t y = 0;
  };

  bool begin(TwoWire& wire, uint8_t sda, uint8_t scl, uint8_t rst, uint8_t irq,
             uint8_t en, uint8_t addr = 0x5D) {
    _wire = &wire;
    _addr = addr;
    _rst = rst;
    _irq = irq;

    pinMode(en, OUTPUT);
    digitalWrite(en, HIGH);
    delay(10);

    pinMode(_rst, OUTPUT);
    pinMode(_irq, OUTPUT);
    digitalWrite(_irq, LOW);
    digitalWrite(_rst, LOW);
    delay(10);
    digitalWrite(_rst, HIGH);
    delay(50);
    pinMode(_irq, INPUT);

    _wire->begin(sda, scl, 400000);
    return probe();
  }

  // One raw report. Stale = buffer-status bit clear (nothing new; the finger may still be down)
  // or an I2C error. Every ready report is acknowledged by writing 0 to 0x814E, as the Seeed
  // GT911 component does, so the same report is never read twice.
  struct Report {
    enum Kind : uint8_t { Stale, Up, Down } kind = Stale;
    uint8_t count = 0;
    uint8_t trackId = 0;
    uint16_t x = 0;  // panel 800x480
    uint16_t y = 0;
  };
  Report read() {
    Report r;
    if (!_wire) return r;
    uint8_t status = 0;
    if (!readReg(0x814E, &status, 1)) return r;
    if (!(status & 0x80)) return r;  // no new report
    const uint8_t count = status & 0x0F;
    uint8_t zero = 0;
    if (count > 5) {  // invalid
      writeReg(0x814E, &zero, 1);
      return r;
    }
    if (count == 0) {
      writeReg(0x814E, &zero, 1);
      r.kind = Report::Up;
      return r;
    }
    // 0x814F track id, 0x8150..0x8153 x/y of the first point (the Sticky panel puts x at 0x8150).
    uint8_t raw[5] = {0};
    const bool ok = readReg(0x814F, raw, 5);
    writeReg(0x814E, &zero, 1);
    if (!ok) return r;
    const uint16_t rawX = static_cast<uint16_t>(raw[1] | (raw[2] << 8));
    const uint16_t rawY = static_cast<uint16_t>(raw[3] | (raw[4] << 8));
    // Portrait digitizer on the landscape 800x480 panel: swap XY + flip both (confirmed on bring-up).
    uint16_t x = rawY;
    uint16_t y = rawX;
    if (x > 799) x = 799;
    if (y > 479) y = 479;
    r.kind = Report::Down;
    r.count = count;
    r.trackId = raw[0];
    r.x = static_cast<uint16_t>(799 - x);
    r.y = static_cast<uint16_t>(479 - y);
    return r;
  }

 private:
  bool probe() {
    uint8_t id[4] = {0};
    return readReg(0x8140, id, 4);
  }

  bool readReg(uint16_t reg, uint8_t* buf, size_t len) {
    _wire->beginTransmission(_addr);
    _wire->write(static_cast<uint8_t>(reg >> 8));
    _wire->write(static_cast<uint8_t>(reg & 0xFF));
    if (_wire->endTransmission(false) != 0) return false;
    if (_wire->requestFrom(static_cast<int>(_addr), static_cast<int>(len)) != (int)len) return false;
    for (size_t i = 0; i < len; i++) buf[i] = _wire->read();
    return true;
  }

  bool writeReg(uint16_t reg, const uint8_t* buf, size_t len) {
    _wire->beginTransmission(_addr);
    _wire->write(static_cast<uint8_t>(reg >> 8));
    _wire->write(static_cast<uint8_t>(reg & 0xFF));
    for (size_t i = 0; i < len; i++) _wire->write(buf[i]);
    return _wire->endTransmission() == 0;
  }

  TwoWire* _wire = nullptr;
  uint8_t _addr = 0x5D;
  uint8_t _rst = 0;
  uint8_t _irq = 0;
};
