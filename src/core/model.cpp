#include "core/model.h"

#include <cstdio>

namespace core {

const Profile* Database::profile(uint8_t id) const {
  for (const auto& p : profiles)
    if (p.id == id) return &p;
  return nullptr;
}
Profile* Database::profile(uint8_t id) {
  for (auto& p : profiles)
    if (p.id == id) return &p;
  return nullptr;
}
const Medication* Database::med(uint16_t id) const {
  for (const auto& m : meds)
    if (m.id == id) return &m;
  return nullptr;
}
Medication* Database::med(uint16_t id) {
  for (auto& m : meds)
    if (m.id == id) return &m;
  return nullptr;
}
size_t Database::medCount(uint8_t profileId) const {
  size_t n = 0;
  for (const auto& m : meds) n += m.profileId == profileId;
  return n;
}

int32_t effectiveEndDay(const Medication& m) {
  if (m.endDay != kNoDate) return m.endDay;
  if (m.courseDays > 0 && m.startDay != kNoDate) return m.startDay + m.courseDays - 1;
  return kNoDate;
}

const Phase* phaseOn(const Medication& m, int32_t day) {
  if (m.startDay == kNoDate || day < m.startDay) return nullptr;
  const int32_t end = effectiveEndDay(m);
  if (end != kNoDate && day > end) return nullptr;
  for (const auto& ph : m.phases) {
    const int32_t from = ph.fromDay == kNoDate ? m.startDay : ph.fromDay;
    if (day < from) continue;
    if (ph.toDay != kNoDate && day > ph.toDay) continue;
    return &ph;
  }
  return nullptr;
}

bool utf8Valid(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int n = 0;
    if (c < 0x80) n = 0;
    else if ((c & 0xE0) == 0xC0 && c >= 0xC2) n = 1;
    else if ((c & 0xF0) == 0xE0) n = 2;
    else if ((c & 0xF8) == 0xF0 && c <= 0xF4) n = 3;
    else return false;
    if (n > 0 && i + n >= s.size()) return false;
    for (int k = 1; k <= n; k++)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += n + 1;
  }
  return true;
}

std::string utf8Truncate(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t cut = maxBytes;
  while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
  return s.substr(0, cut);
}

std::string validateProfile(const Profile& p) {
  if (p.id < 1 || p.id > kMaxProfiles) return "profile id out of range";
  if (p.name.empty() || p.name.size() > kMaxNameBytes || !utf8Valid(p.name)) return "profile name invalid";
  if (p.initials.empty() || p.initials.size() > 8 || !utf8Valid(p.initials)) return "initials invalid";
  if (p.quietStartMin < 0 || p.quietStartMin >= 1440 || p.quietEndMin < 0 || p.quietEndMin >= 1440)
    return "quiet hours invalid";
  if (p.alertPattern >= 4) return "alert pattern invalid";
  if (p.alertVolume > 100) return "alert volume invalid";
  for (int16_t r : p.routineMin)
    if (r < 0 || r >= 1440) return "routine time invalid";
  return "";
}

static std::string validateRule(const ScheduleRule& r) {
  if (r.type == ScheduleType::Prn) {
    if (r.prnMaxPer24h == 0) return "PRN max per 24h must be > 0";
    return "";
  }
  if (r.times.empty() || r.times.size() > kMaxTimesPerSchedule) return "schedule needs 1..12 times";
  for (const auto& t : r.times) {
    if (t.routine == Routine::None && (t.minute < 0 || t.minute >= 1440)) return "dose time invalid";
    if (t.routine != Routine::None && (t.offsetMin < -720 || t.offsetMin > 720)) return "routine offset invalid";
  }
  if (r.type == ScheduleType::Routine) {
    for (const auto& t : r.times)
      if (t.routine == Routine::None) return "routine schedule needs routine anchors";
  }
  if (r.type == ScheduleType::Weekdays && (r.weekdayMask & 0x7F) == 0) return "no weekday selected";
  if (r.type == ScheduleType::EveryN && r.intervalN == 0) return "interval must be > 0";
  if (r.type == ScheduleType::SpecificDates && (r.dates.empty() || r.dates.size() > kMaxSpecificDates))
    return "specific dates need 1..12 dates";
  if (r.type == ScheduleType::Cycle && (r.cycleOn == 0)) return "cycle needs on-days";
  return "";
}

std::string validateMedication(const Medication& m) {
  if (m.id == 0) return "medication id missing";
  if (m.profileId < 1 || m.profileId > kMaxProfiles) return "medication profile invalid";
  if (m.name.empty() || m.name.size() > kMaxNameBytes || !utf8Valid(m.name)) return "medication name invalid";
  if (m.doseText.size() > kMaxNameBytes || !utf8Valid(m.doseText)) return "dose text invalid";
  if (m.instructions.size() > kMaxTextBytes || m.notes.size() > kMaxTextBytes) return "text too long";
  if (!utf8Valid(m.instructions) || !utf8Valid(m.notes)) return "text not UTF-8";
  if (m.startDay == kNoDate) return "start date missing";
  if (m.endDay != kNoDate && m.endDay < m.startDay) return "end before start";
  if (m.graceMin == 0 || m.graceMin > 24 * 60) return "grace window invalid";
  if (m.onTimeMin > m.graceMin) return "on-time window exceeds grace";
  if (m.ladderIntervalMin == 0 || m.ladderCount == 0) return "alarm ladder invalid";
  if (m.alertPattern != 255 && m.alertPattern >= 4) return "alert pattern invalid";
  if (m.alertVolume > 100) return "alert volume invalid";
  if (m.phases.empty() || m.phases.size() > kMaxPhases) return "medication needs 1..4 phases";
  int32_t prevTo = kNoDate;
  for (size_t i = 0; i < m.phases.size(); i++) {
    const Phase& ph = m.phases[i];
    const int32_t from = ph.fromDay == kNoDate ? m.startDay : ph.fromDay;
    if (ph.toDay != kNoDate && ph.toDay < from) return "phase ends before it starts";
    if (i > 0) {
      if (prevTo == kNoDate || from <= prevTo) return "phases overlap or previous phase is open";
    }
    prevTo = ph.toDay;
    const std::string e = validateRule(ph.rule);
    if (!e.empty()) return e;
  }
  return "";
}

std::string validateDatabase(const Database& db) {
  if (db.profiles.size() > kMaxProfiles) return "too many profiles";
  for (size_t i = 0; i < db.profiles.size(); i++) {
    std::string e = validateProfile(db.profiles[i]);
    if (!e.empty()) return e;
    for (size_t j = i + 1; j < db.profiles.size(); j++)
      if (db.profiles[j].id == db.profiles[i].id) return "duplicate profile id";
  }
  for (size_t i = 0; i < db.meds.size(); i++) {
    std::string e = validateMedication(db.meds[i]);
    if (!e.empty()) return e;
    if (!db.profile(db.meds[i].profileId)) return "medication references missing profile";
    for (size_t j = i + 1; j < db.meds.size(); j++)
      if (db.meds[j].id == db.meds[i].id) return "duplicate medication id";
  }
  for (const auto& p : db.profiles)
    if (db.medCount(p.id) > kMaxMedsPerProfile) return "too many medications in profile";
  return "";
}

std::string hashPin(const std::string& pin, const std::string& saltHex) {
  uint64_t h = 0xcbf29ce484222325ULL;
  auto mix = [&h](const std::string& s) {
    for (unsigned char c : s) {
      h ^= c;
      h *= 0x100000001b3ULL;
    }
  };
  for (int i = 0; i < 4096; i++) {
    mix(saltHex);
    mix(pin);
  }
  char buf[17];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
  return buf;
}

bool checkPin(const Profile& p, const std::string& pin) {
  if (p.pinHash.empty()) return true;
  return hashPin(pin, p.pinSalt) == p.pinHash;
}

bool inQuietHours(const Profile& p, int minuteOfDay) {
  if (p.quietStartMin == p.quietEndMin) return false;
  if (p.quietStartMin < p.quietEndMin) return minuteOfDay >= p.quietStartMin && minuteOfDay < p.quietEndMin;
  return minuteOfDay >= p.quietStartMin || minuteOfDay < p.quietEndMin;
}

}  // namespace core
