#pragma once
// Alarm planning (pure): turns dose-engine events into one alert per person,
// chooses the tone pattern and volume, applies quiet hours, and never drops a
// missed alarm (stale events for doses that are still due become a catch-up alert).
// AlarmManager drives the real outputs through the AlertOutput interface.
#include <cstdint>
#include <vector>

#include "core/dose_engine.h"
#include "core/model.h"
#include "core/tz.h"

namespace core {

enum class PatternId : uint8_t { Chime = 0, Beeps, Urgent, Gentle, Count, Inherit = 255 };
const char* patternName(PatternId p);

struct ToneStep {
  uint16_t hz;     // 0 = silence
  uint16_t onMs;
  uint16_t offMs;
};
struct AlertPattern {
  ToneStep steps[6];
  uint8_t count;
  uint8_t repeats;  // the step list is played this many times
};
const AlertPattern& patternFor(PatternId p);
uint32_t patternDurationMs(const AlertPattern& p);

// 10-bit LEDC duty for a volume percentage. A piezo/magnetic buzzer is loudest
// at 50 % duty (512); quieter volumes use narrower pulses. 0 % = off.
uint16_t volumeToDuty(uint8_t percent);

constexpr uint8_t kSystemProfile = 0;  // battery / system alerts (not a person)

struct PersonAlert {
  uint8_t profileId = 0;
  int64_t earliestScheduledUtc = 0;
  uint8_t doseCount = 0;
  uint8_t alarmIndex = 0;  // 0 = first alarm of the ladder, 1.. repeats (max over the group)
  bool audible = true;     // false = silent visual (quiet hours, non-critical)
  bool critical = false;
  bool catchUp = false;    // built from events that happened while asleep / not processed in time
  PatternId pattern = PatternId::Chime;
  uint8_t volume = 70;
};

struct AlertPlan {
  std::vector<PersonAlert> alerts;  // ordered: critical first, then earliest scheduled; one per person
  int missed = 0;                   // BecameMissed events in this batch (recorded in the log, shown as a notice)
  int catchUpDoses = 0;             // stale due events that were still due
};

// nowUtc is used to re-evaluate quiet hours for catch-up alerts.
AlertPlan planAlerts(const std::vector<EngineEvent>& events, const DoseEngine& engine, const Database& db,
                     const PosixTz& tz, int64_t nowUtc);

// Quiet-hours rule shared with the UI: silent unless any dose is critical.
bool alertAudible(const Profile* p, bool critical, int localMinute);

class AlertOutput {
 public:
  virtual ~AlertOutput() = default;
  virtual const char* name() const = 0;
  virtual bool audible() const = 0;    // makes sound
  virtual bool available() const = 0;  // working (e.g. buzzer passed its self-test)
  virtual void play(const PersonAlert& a) = 0;
};

class AlarmManager {
 public:
  static constexpr int kMaxOutputs = 4;
  bool addOutput(AlertOutput* o);
  struct Result {
    bool sounded = false;
    bool shown = false;
  };
  // Audible outputs only when the alert is audible; visual outputs always.
  Result deliver(const PersonAlert& a);
  // No working audible output: the device is in visual-only mode.
  bool visualOnly() const;
  int outputCount() const { return n_; }

 private:
  AlertOutput* out_[kMaxOutputs] = {};
  int n_ = 0;
};

}  // namespace core
