#include "core/presets.h"

#include <cstdio>

namespace core {
namespace {
const TzPreset kTz[] = {
    {"Europe/Berlin", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Vienna", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Zurich", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Amsterdam", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Paris", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Madrid", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Rome", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/Warsaw", "CET-1CEST,M3.5.0,M10.5.0/3"},
    {"Europe/London", "GMT0BST,M3.5.0/1,M10.5.0"},
    {"Europe/Lisbon", "WET0WEST,M3.5.0/1,M10.5.0"},
    {"Europe/Helsinki", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Athens", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
    {"Europe/Istanbul", "<+03>-3"},
    {"UTC", "UTC0"},
    {"America/New_York", "EST5EDT,M3.2.0,M11.1.0"},
    {"America/Chicago", "CST6CDT,M3.2.0,M11.1.0"},
    {"America/Denver", "MST7MDT,M3.2.0,M11.1.0"},
    {"America/Los_Angeles", "PST8PDT,M3.2.0,M11.1.0"},
    {"Asia/Tokyo", "JST-9"},
    {"Australia/Sydney", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
};
}  // namespace

int tzPresetCount() { return static_cast<int>(sizeof(kTz) / sizeof(kTz[0])); }
const TzPreset& tzPreset(int i) { return kTz[(i >= 0 && i < tzPresetCount()) ? i : 0]; }
int tzPresetIndex(const std::string& name) {
  for (int i = 0; i < tzPresetCount(); i++)
    if (name == kTz[i].name) return i;
  return -1;
}

const char* scheduleTemplateName(ScheduleTemplate t) {
  static const char* const n[] = {"Once daily (08:00)", "Twice daily (08:00, 20:00)", "3 times (08, 14, 20)",
                                  "With meals",         "At bedtime",                 "Weekly (Monday 08:00)",
                                  "Every other day",    "21 days on, 7 off",          "As needed"};
  const int i = static_cast<int>(t);
  return i < static_cast<int>(ScheduleTemplate::Count) ? n[i] : "?";
}

static DoseTime clockAt(int h, int m = 0) {
  DoseTime t;
  t.minute = static_cast<int16_t>(h * 60 + m);
  return t;
}
static DoseTime anchored(Routine r) {
  DoseTime t;
  t.routine = r;
  return t;
}

void applyScheduleTemplate(Medication& m, ScheduleTemplate t) {
  Phase ph;
  ScheduleRule& r = ph.rule;
  r.type = ScheduleType::FixedTimes;
  switch (t) {
    case ScheduleTemplate::OnceMorning: r.times = {clockAt(8)}; break;
    case ScheduleTemplate::TwiceDaily: r.times = {clockAt(8), clockAt(20)}; break;
    case ScheduleTemplate::ThreeTimes: r.times = {clockAt(8), clockAt(14), clockAt(20)}; break;
    case ScheduleTemplate::WithMeals:
      r.type = ScheduleType::Routine;
      r.times = {anchored(Routine::Breakfast), anchored(Routine::Lunch), anchored(Routine::Dinner)};
      break;
    case ScheduleTemplate::Bedtime:
      r.type = ScheduleType::Routine;
      r.times = {anchored(Routine::Bed)};
      break;
    case ScheduleTemplate::WeeklyMonday:
      r.type = ScheduleType::Weekdays;
      r.weekdayMask = 1 << 1;
      r.times = {clockAt(8)};
      break;
    case ScheduleTemplate::EveryOtherDay:
      r.type = ScheduleType::EveryN;
      r.intervalN = 2;
      r.times = {clockAt(8)};
      break;
    case ScheduleTemplate::Cycle21of28:
      r.type = ScheduleType::Cycle;
      r.cycleOn = 21;
      r.cycleOff = 7;
      r.times = {clockAt(8)};
      break;
    case ScheduleTemplate::AsNeeded:
      r.type = ScheduleType::Prn;
      r.prnMinIntervalMin = 240;
      r.prnMaxPer24h = 4;
      break;
    default: return;
  }
  m.phases.clear();
  m.phases.push_back(ph);
}

bool pinFormatValid(const std::string& pin) {
  if (pin.size() < 4 || pin.size() > 8) return false;
  for (char c : pin)
    if (c < '0' || c > '9') return false;
  return true;
}

std::string makePinSalt(uint64_t entropy) {
  // splitmix64 so similar entropy values give unrelated salts
  uint64_t z = entropy + 0x9E3779B97F4A7C15ull;
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  z ^= z >> 31;
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(z));
  return buf;
}

void setPin(std::string& salt, std::string& hash, const std::string& pin, uint64_t entropy) {
  if (pin.empty()) {
    salt.clear();
    hash.clear();
    return;
  }
  salt = makePinSalt(entropy);
  hash = hashPin(pin, salt);
}

bool verifyPin(const std::string& salt, const std::string& hash, const std::string& pin) {
  if (hash.empty()) return true;
  return hashPin(pin, salt) == hash;
}

uint8_t nextProfileId(const Database& db) {
  for (uint8_t id = 1; id <= kMaxProfiles; id++)
    if (!db.profile(id)) return id;
  return 0;
}

uint16_t nextMedicationId(const Database& db) {
  uint16_t mx = 0;
  for (const auto& m : db.meds) mx = m.id > mx ? m.id : mx;
  return static_cast<uint16_t>(mx + 1);
}

const char* routineName(Routine r) {
  static const char* const n[] = {"Clock time", "Wake", "Breakfast", "Lunch", "Dinner", "Bed"};
  return n[static_cast<int>(r)];
}
const char* scheduleTypeName(ScheduleType t) {
  static const char* const n[] = {"Daily at times", "Daily by routine", "Weekdays", "Every N days/weeks",
                                  "Specific dates", "Cycle (on/off)",   "As needed (PRN)"};
  return n[static_cast<int>(t)];
}
const char* medFormName(MedForm f) {
  static const char* const n[] = {"Pill", "Capsule", "Liquid", "Injection", "Inhaler", "Drops", "Patch", "Cream", "Other"};
  return n[static_cast<int>(f)];
}

}  // namespace core
