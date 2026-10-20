#pragma once
// Injectable time source. core/ never reads hardware time directly.
#include <cstdint>

namespace core {

class IClock {
 public:
  virtual ~IClock() = default;
  // Seconds since 1970-01-01T00:00:00Z.
  virtual int64_t nowUtc() const = 0;
};

// Deterministic clock for tests and simulations.
class FakeClock : public IClock {
 public:
  explicit FakeClock(int64_t t = 0) : t_(t) {}
  int64_t nowUtc() const override { return t_; }
  void set(int64_t t) { t_ = t; }
  void advance(int64_t s) { t_ += s; }

 private:
  int64_t t_;
};

// Dev builds: runs `speed` times faster than `source`, anchored at `anchor`
// (time before the anchor is unchanged). speed 1 = pass-through.
class ScaledClock : public IClock {
 public:
  ScaledClock(const IClock& source, int speed, int64_t anchor) : src_(source), speed_(speed < 1 ? 1 : speed), anchor_(anchor) {}
  int64_t nowUtc() const override {
    const int64_t s = src_.nowUtc();
    return s <= anchor_ ? s : anchor_ + (s - anchor_) * speed_;
  }
  // Seconds of source time until this clock reaches `utc` (for sleep/timeouts).
  int64_t sourceSecondsUntil(int64_t utc) const {
    const int64_t d = utc - nowUtc();
    return d <= 0 ? 0 : (d + speed_ - 1) / speed_;
  }
  void rebase(int64_t anchor) { anchor_ = anchor; }
  int speed() const { return speed_; }

 private:
  const IClock& src_;
  int speed_;
  int64_t anchor_;
};

}  // namespace core
