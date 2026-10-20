#pragma once
// Time-zone presets, schedule templates and PIN helpers for the editors and the wizard.
#include <cstdint>
#include <string>

#include "core/model.h"

namespace core {

struct TzPreset {
  const char* name;
  const char* posix;
};
int tzPresetCount();
const TzPreset& tzPreset(int i);
int tzPresetIndex(const std::string& name);  // -1 = not a preset
constexpr int kDefaultTzPreset = 0;          // Europe/Berlin

enum class ScheduleTemplate : uint8_t {
  OnceMorning = 0,
  TwiceDaily,
  ThreeTimes,
  WithMeals,
  Bedtime,
  WeeklyMonday,
  EveryOtherDay,
  Cycle21of28,
  AsNeeded,
  Count
};
const char* scheduleTemplateName(ScheduleTemplate t);
// Replaces the medication's phases with a single phase built from the template (start = m.startDay).
void applyScheduleTemplate(Medication& m, ScheduleTemplate t);

// PIN: 4..8 ASCII digits.
bool pinFormatValid(const std::string& pin);
// Salt from a caller-provided entropy value (hardware RNG on the device, fixed in tests).
std::string makePinSalt(uint64_t entropy);
void setPin(std::string& salt, std::string& hash, const std::string& pin, uint64_t entropy);
bool verifyPin(const std::string& salt, const std::string& hash, const std::string& pin);

// Next free ids.
uint8_t nextProfileId(const Database& db);  // 0 when 8 profiles exist
uint16_t nextMedicationId(const Database& db);

const char* routineName(Routine r);
const char* scheduleTypeName(ScheduleType t);
const char* medFormName(MedForm f);

}  // namespace core
