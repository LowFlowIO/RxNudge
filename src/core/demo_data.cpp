#include "core/demo_data.h"

#include "core/schedule.h"

namespace core {

static DoseTime at(int h, int m = 0) {
  DoseTime t;
  t.minute = static_cast<int16_t>(h * 60 + m);
  return t;
}

static Medication med(uint16_t id, uint8_t profile, const char* name, const char* dose, MedForm form, int32_t start,
                      ScheduleRule rule) {
  Medication m;
  m.id = id;
  m.profileId = profile;
  m.name = name;
  m.doseText = dose;
  m.form = form;
  m.startDay = start;
  Phase p;
  p.rule = rule;
  m.phases.push_back(p);
  return m;
}

static ScheduleRule fixed(std::initializer_list<DoseTime> t) {
  ScheduleRule r;
  r.type = ScheduleType::FixedTimes;
  r.times = t;
  return r;
}

Database makeDemoDatabase(int32_t today) {
  Database db;
  Profile anna;
  anna.id = 1;
  anna.name = "Anna";
  anna.initials = "AN";
  anna.privacy = false;
  Profile ben;
  ben.id = 2;
  ben.name = "Ben";
  ben.initials = "BK";
  ben.privacy = true;
  ben.routineMin[1] = 7 * 60;  // breakfast 07:00
  db.profiles = {anna, ben};

  const int32_t start = today - 60;
  Medication metformin = med(1, 1, "Metformin", "500 mg", MedForm::Pill, start, fixed({at(8), at(20)}));
  metformin.instructions = "Take with food";
  metformin.supply = 41;
  Medication ramipril = med(2, 1, "Ramipril", "5 mg", MedForm::Capsule, start, fixed({at(8)}));
  ramipril.critical = true;
  ramipril.supply = 6;
  ScheduleRule weekly = fixed({at(9)});
  weekly.type = ScheduleType::Weekdays;
  weekly.weekdayMask = 1;  // Sundays
  Medication vitd = med(3, 1, "Vitamin D", "20 drops", MedForm::Drops, start, weekly);
  ScheduleRule prn;
  prn.type = ScheduleType::Prn;
  prn.prnMinIntervalMin = 360;
  prn.prnMaxPer24h = 3;
  Medication ibu = med(4, 1, "Ibuprofen", "400 mg", MedForm::Pill, start, prn);
  ibu.instructions = "For pain. Not on an empty stomach";
  ibu.supply = 18;

  ScheduleRule breakfast;
  breakfast.type = ScheduleType::Routine;
  DoseTime bt;
  bt.routine = Routine::Breakfast;
  bt.offsetMin = -30;
  breakfast.times = {bt};
  Medication levo = med(5, 2, "Levothyroxine", "50 \xC2\xB5g", MedForm::Pill, start, breakfast);
  levo.instructions = "30 min before breakfast";
  levo.supply = 64;
  Medication insulin = med(6, 2, "Insulin glargine", "10 units", MedForm::Injection, start, fixed({at(21, 30)}));
  insulin.critical = true;
  ScheduleRule inh;
  inh.type = ScheduleType::Prn;
  inh.prnMinIntervalMin = 240;
  inh.prnMaxPer24h = 8;
  Medication salbu = med(7, 2, "Salbutamol", "2 puffs", MedForm::Inhaler, start, inh);
  Medication amox = med(8, 2, "Amoxicillin", "500 mg", MedForm::Capsule, today - 2, fixed({at(8), at(14), at(20)}));
  amox.courseDays = 7;
  amox.supply = 13;
  db.meds = {metformin, ramipril, vitd, ibu, levo, insulin, salbu, amox};
  return db;
}

std::vector<DoseLogEntry> makeDemoLog(const Database& db, const PosixTz& tz, int64_t nowUtc, int days) {
  std::vector<DoseLogEntry> out;
  std::vector<Occurrence> occ;
  const int64_t from = nowUtc - static_cast<int64_t>(days) * 86400;
  // only doses whose grace window is over (the engine owns the rest)
  expandOccurrences(db, tz, from, nowUtc - 3 * 3600, occ);
  uint32_t seed = 12345;
  auto rnd = [&]() {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 16) % 100;
  };
  for (const Occurrence& o : occ) {
    const uint32_t r = rnd();
    DoseLogEntry e;
    e.eventId = o.eventId;
    e.profileId = o.profileId;
    e.medId = o.medId;
    e.scheduledUtc = o.scheduledUtc;
    e.source = ActionSource::Button;
    if (r < 80) {
      e.action = DoseAction::Taken;
      e.actionUtc = o.scheduledUtc + static_cast<int64_t>(rnd() % 12) * 60;
    } else if (r < 89) {
      e.action = DoseAction::TakenLate;
      e.actionUtc = o.scheduledUtc + (20 + static_cast<int64_t>(rnd() % 35)) * 60;
    } else if (r < 94) {
      e.action = DoseAction::Skipped;
      e.actionUtc = o.scheduledUtc + 300;
    } else {
      e.action = DoseAction::Missed;
      e.actionUtc = o.scheduledUtc + 3600;
      e.source = ActionSource::System;
    }
    e.recordedUtc = e.actionUtc;
    out.push_back(e);
  }
  // a few PRN takes
  for (const Medication& m : db.meds) {
    if (m.phases.empty() || m.phases[0].rule.type != ScheduleType::Prn) continue;
    for (int d = days; d > 0; d -= 3 + static_cast<int>(rnd() % 4)) {
      DoseLogEntry e;
      e.profileId = m.profileId;
      e.medId = m.id;
      e.actionUtc = e.scheduledUtc = e.recordedUtc = nowUtc - static_cast<int64_t>(d) * 86400 + 4 * 3600;
      e.eventId = makeEventId(m.profileId, m.id, e.actionUtc, true);
      e.action = DoseAction::PrnTaken;
      e.source = ActionSource::Button;
      out.push_back(e);
    }
  }
  return out;
}

}  // namespace core
