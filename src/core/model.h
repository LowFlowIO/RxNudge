#pragma once
// RxNudge data model. Pure C++, UTF-8 strings, no hardware types.
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/civil.h"

namespace core {

constexpr int kSchemaVersion = 2;
constexpr size_t kMaxProfiles = 8;
constexpr size_t kMaxMedsPerProfile = 30;
constexpr size_t kMaxTimesPerSchedule = 12;
constexpr size_t kMaxSpecificDates = 12;
constexpr size_t kMaxPhases = 4;
constexpr size_t kMaxNameBytes = 48;
constexpr size_t kMaxTextBytes = 160;

enum class Routine : uint8_t { None = 0, Wake, Breakfast, Lunch, Dinner, Bed };
constexpr size_t kRoutineCount = 5;  // Wake..Bed

struct Profile {
  uint8_t id = 0;  // 1..8
  std::string name;
  std::string initials;
  std::string pinSalt;  // hex
  std::string pinHash;  // hex, empty = no PIN
  int16_t quietStartMin = 22 * 60;  // quiet hours [start, end) local minute-of-day; start == end = none
  int16_t quietEndMin = 7 * 60;
  std::array<int16_t, kRoutineCount> routineMin{{7 * 60, 7 * 60 + 30, 12 * 60 + 30, 18 * 60 + 30, 22 * 60}};
  bool privacy = true;  // hide medication names on shared/locked screens
  uint8_t alertPattern = 0;   // core::PatternId (alarm.h)
  uint8_t alertVolume = 70;   // percent, mapped to buzzer PWM duty
};

enum class MedForm : uint8_t { Pill = 0, Capsule, Liquid, Injection, Inhaler, Drops, Patch, Cream, Other };

enum class ScheduleType : uint8_t {
  FixedTimes = 0,  // every day at fixed times
  Routine,         // every day, times anchored to profile routines
  Weekdays,        // selected weekdays
  EveryN,          // every N days or N weeks from phase start
  SpecificDates,   // listed dates only
  Cycle,           // X days on / Y days off from phase start
  Prn              // as needed
};

struct DoseTime {
  int16_t minute = 8 * 60;          // local minute-of-day (used when routine == None)
  Routine routine = Routine::None;  // anchor
  int16_t offsetMin = 0;            // offset from the routine anchor
};

struct ScheduleRule {
  ScheduleType type = ScheduleType::FixedTimes;
  std::vector<DoseTime> times;  // <= 12 (not used for PRN)
  uint8_t weekdayMask = 0x7F;   // bit0 = Sunday .. bit6 = Saturday (Weekdays)
  uint16_t intervalN = 1;       // EveryN
  bool intervalWeeks = false;   // EveryN unit
  std::vector<int32_t> dates;   // SpecificDates, local day numbers, <= 12
  uint16_t cycleOn = 21;        // Cycle
  uint16_t cycleOff = 7;
  uint16_t prnMinIntervalMin = 240;  // PRN
  uint8_t prnMaxPer24h = 4;
};

// A dosing phase; multi-phase medications (tapering, titration) use several.
struct Phase {
  int32_t fromDay = kNoDate;  // inclusive; kNoDate = medication start
  int32_t toDay = kNoDate;    // inclusive; kNoDate = open ended
  std::string doseText;       // overrides Medication::doseText when not empty
  ScheduleRule rule;
};

struct Medication {
  uint16_t id = 0;  // unique within the database
  uint8_t profileId = 0;
  std::string name;  // UTF-8
  std::string doseText;
  MedForm form = MedForm::Pill;
  std::string instructions;
  std::string notes;
  int32_t startDay = kNoDate;  // required
  int32_t endDay = kNoDate;    // inclusive, kNoDate = none
  uint16_t courseDays = 0;     // > 0: end = start + courseDays - 1 (if endDay unset)
  uint16_t graceMin = 60;      // after this a dose becomes Missed
  uint16_t onTimeMin = 15;     // taken within this after schedule = on time
  uint8_t ladderIntervalMin = 5;
  uint8_t ladderCount = 6;
  bool critical = false;
  uint8_t alertPattern = 255;  // 255 = use the profile pattern
  uint8_t alertVolume = 0;     // 0 = use the profile volume
  int32_t supply = -1;  // units on hand, -1 = not tracked
  int32_t lowStockThreshold = 7;
  int32_t refillDay = kNoDate;
  bool active = true;
  std::vector<Phase> phases;  // >= 1, <= 4, non-overlapping, ordered
};

struct Settings {
  std::string tz = "CET-1CEST,M3.5.0,M10.5.0/3";
  uint8_t activeProfile = 1;
  bool sleepOnUsb = false;          // deep sleep is normally blocked while USB powered
  uint8_t defaultLadderIntervalMin = 5;  // used for new medications (editor) and the battery alert repeat
  uint8_t defaultLadderCount = 6;
  bool setupDone = false;           // first-boot wizard completed
  std::string language = "en";      // strings table id (English only for now)
  std::string tzName = "Europe/Berlin";  // preset name or "Custom"
  std::string adminPinSalt;         // hex; empty hash = no settings/editor lock
  std::string adminPinHash;
  uint8_t refreshFullEvery = 20;    // full refresh after this many fast refreshes (5..50)
  bool nightlyFullRefresh = true;
  bool autoBackup = true;           // nightly SD backup (storage/auto_backup.h)
  bool wifiEnabled = false;         // user intent; Wi-Fi credentials live in NVS (never in the database)
  bool autoTimeSync = true;         // NTP on boot / daily wake / after 24 h when Wi-Fi is configured
  // Caregiver missed-dose push (opt-in). The URL is an ntfy topic URL or a generic webhook.
  bool notifyEnabled = false;
  std::string notifyUrl;            // http(s)://..., <= 160 bytes
  bool notifyNames = false;         // include person and medication names (off: initials + counts only)
};

struct Database {
  int schemaVersion = kSchemaVersion;
  Settings settings;
  std::vector<Profile> profiles;
  std::vector<Medication> meds;

  const Profile* profile(uint8_t id) const;
  Profile* profile(uint8_t id);
  const Medication* med(uint16_t id) const;
  Medication* med(uint16_t id);
  size_t medCount(uint8_t profileId) const;
};

// Effective last day of a medication (endDay, or derived from courseDays), kNoDate if open.
int32_t effectiveEndDay(const Medication& m);
// Phase active on a local day, or nullptr.
const Phase* phaseOn(const Medication& m, int32_t day);

// Validation; returns empty string when valid, else a human-readable reason.
std::string validateProfile(const Profile& p);
std::string validateMedication(const Medication& m);
std::string validateDatabase(const Database& db);

// UTF-8 safe truncation to at most maxBytes.
std::string utf8Truncate(const std::string& s, size_t maxBytes);
bool utf8Valid(const std::string& s);

// PIN hashing (salted, iterated FNV-1a 64). Not cryptographically strong;
// it only keeps casual readers of a backup from seeing the PIN.
std::string hashPin(const std::string& pin, const std::string& saltHex);
bool checkPin(const Profile& p, const std::string& pin);

// Quiet hours: true if minute-of-day falls inside [quietStart, quietEnd) (wraps midnight).
bool inQuietHours(const Profile& p, int minuteOfDay);

}  // namespace core
