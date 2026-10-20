#include "core/codec.h"

#include <cstdio>
#include <cstdlib>

#include "core/civil.h"

namespace core {

std::string formatDate(int32_t day) {
  if (day == kNoDate) return "";
  const Ymd d = civilFromDays(day);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", d.y, d.m, d.d);
  return buf;
}

bool parseDate(const std::string& s, int32_t& day) {
  if (s.empty()) {
    day = kNoDate;
    return true;
  }
  int y, m, d;
  char extra;
  if (std::sscanf(s.c_str(), "%4d-%2d-%2d%c", &y, &m, &d, &extra) != 3) return false;
  if (m < 1 || m > 12 || d < 1 || d > daysInMonth(y, m)) return false;
  day = daysFromCivil(y, m, d);
  return true;
}

std::string formatHm(int minute) {
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d", minute / 60, minute % 60);
  return buf;
}

bool parseHm(const std::string& s, int& minute) {
  int h, m;
  char extra;
  if (std::sscanf(s.c_str(), "%2d:%2d%c", &h, &m, &extra) != 2) return false;
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  minute = h * 60 + m;
  return true;
}

std::string formatEventId(uint64_t id) {
  char buf[20];
  std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(id));
  return buf;
}

bool parseEventId(const std::string& s, uint64_t& id) {
  if (s.size() != 16) return false;
  char* end = nullptr;
  id = std::strtoull(s.c_str(), &end, 16);
  return end && *end == '\0';
}

std::string formatUtcIso(int64_t utc) {
  const int32_t days = static_cast<int32_t>(floorDiv(utc, 86400));
  const int64_t sod = floorMod(utc, 86400);
  const Ymd d = civilFromDays(days);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ", d.y, d.m, d.d, static_cast<int>(sod / 3600),
                static_cast<int>((sod / 60) % 60), static_cast<int>(sod % 60));
  return buf;
}

static JsonValue S(const std::string& s) { return JsonValue::string(s); }
static JsonValue I(int64_t v) { return JsonValue::integer(v); }
static JsonValue B(bool v) { return JsonValue::boolean(v); }

static const char* const kRoutineNames[] = {"", "wake", "breakfast", "lunch", "dinner", "bed"};
static const char* const kFormNames[] = {"pill", "capsule", "liquid", "injection", "inhaler", "drops", "patch", "cream", "other"};
static const char* const kTypeNames[] = {"fixed", "routine", "weekdays", "every_n", "dates", "cycle", "prn"};

template <size_t N>
static int lookup(const char* const (&names)[N], const std::string& s) {
  for (size_t i = 0; i < N; i++)
    if (s == names[i]) return static_cast<int>(i);
  return -1;
}

JsonValue encodeProfile(const Profile& p) {
  JsonValue j = JsonValue::object();
  j.set("id", I(p.id));
  j.set("name", S(p.name));
  j.set("initials", S(p.initials));
  j.set("pinSalt", S(p.pinSalt));
  j.set("pinHash", S(p.pinHash));
  j.set("quietStart", S(formatHm(p.quietStartMin)));
  j.set("quietEnd", S(formatHm(p.quietEndMin)));
  JsonValue r = JsonValue::object();
  for (size_t i = 0; i < kRoutineCount; i++) r.set(kRoutineNames[i + 1], S(formatHm(p.routineMin[i])));
  j.set("routines", r);
  j.set("privacy", B(p.privacy));
  j.set("alertPattern", I(p.alertPattern));
  j.set("alertVolume", I(p.alertVolume));
  return j;
}

bool decodeProfile(const JsonValue& j, Profile& p, std::string* err) {
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  if (!j.isObject()) return fail("profile is not an object");
  p = Profile();
  p.id = static_cast<uint8_t>(j["id"].asInt());
  p.name = j["name"].asString();
  p.initials = j["initials"].asString();
  p.pinSalt = j["pinSalt"].asString();
  p.pinHash = j["pinHash"].asString();
  int m;
  if (j.has("quietStart")) {
    if (!parseHm(j["quietStart"].asString(), m)) return fail("bad quietStart");
    p.quietStartMin = static_cast<int16_t>(m);
  }
  if (j.has("quietEnd")) {
    if (!parseHm(j["quietEnd"].asString(), m)) return fail("bad quietEnd");
    p.quietEndMin = static_cast<int16_t>(m);
  }
  p.alertPattern = static_cast<uint8_t>(j["alertPattern"].asInt(0));
  p.alertVolume = static_cast<uint8_t>(j["alertVolume"].asInt(70));
  const JsonValue& r = j["routines"];
  for (size_t i = 0; i < kRoutineCount; i++) {
    if (!r.has(kRoutineNames[i + 1])) continue;
    if (!parseHm(r[kRoutineNames[i + 1]].asString(), m)) return fail("bad routine time");
    p.routineMin[i] = static_cast<int16_t>(m);
  }
  p.privacy = j["privacy"].asBool(true);
  return true;
}

static JsonValue encodeRule(const ScheduleRule& r) {
  JsonValue j = JsonValue::object();
  j.set("type", S(kTypeNames[static_cast<int>(r.type)]));
  JsonValue times = JsonValue::array();
  for (const auto& t : r.times) {
    JsonValue tj = JsonValue::object();
    if (t.routine == Routine::None) {
      tj.set("at", S(formatHm(t.minute)));
    } else {
      tj.set("routine", S(kRoutineNames[static_cast<int>(t.routine)]));
      tj.set("offset", I(t.offsetMin));
    }
    times.push(tj);
  }
  j.set("times", times);
  if (r.type == ScheduleType::Weekdays) j.set("weekdays", I(r.weekdayMask));
  if (r.type == ScheduleType::EveryN) {
    j.set("every", I(r.intervalN));
    j.set("unit", S(r.intervalWeeks ? "weeks" : "days"));
  }
  if (r.type == ScheduleType::SpecificDates) {
    JsonValue d = JsonValue::array();
    for (int32_t day : r.dates) d.push(S(formatDate(day)));
    j.set("dates", d);
  }
  if (r.type == ScheduleType::Cycle) {
    j.set("on", I(r.cycleOn));
    j.set("off", I(r.cycleOff));
  }
  if (r.type == ScheduleType::Prn) {
    j.set("minIntervalMin", I(r.prnMinIntervalMin));
    j.set("maxPer24h", I(r.prnMaxPer24h));
  }
  return j;
}

static bool decodeRule(const JsonValue& j, ScheduleRule& r, std::string* err) {
  auto fail = [&](const char* m) {
    if (err) *err = m;
    return false;
  };
  r = ScheduleRule();
  const int t = lookup(kTypeNames, j["type"].asString());
  if (t < 0) return fail("unknown schedule type");
  r.type = static_cast<ScheduleType>(t);
  const JsonValue& times = j["times"];
  for (size_t i = 0; i < times.size(); i++) {
    const JsonValue& tj = times.at(i);
    DoseTime dt;
    if (tj.has("routine")) {
      const int ri = lookup(kRoutineNames, tj["routine"].asString());
      if (ri <= 0) return fail("unknown routine");
      dt.routine = static_cast<Routine>(ri);
      dt.offsetMin = static_cast<int16_t>(tj["offset"].asInt());
    } else {
      int m;
      if (!parseHm(tj["at"].asString(), m)) return fail("bad dose time");
      dt.minute = static_cast<int16_t>(m);
    }
    r.times.push_back(dt);
  }
  if (j.has("weekdays")) r.weekdayMask = static_cast<uint8_t>(j["weekdays"].asInt());
  if (j.has("every")) r.intervalN = static_cast<uint16_t>(j["every"].asInt());
  r.intervalWeeks = j["unit"].asString() == "weeks";
  const JsonValue& dates = j["dates"];
  for (size_t i = 0; i < dates.size(); i++) {
    int32_t d;
    if (!parseDate(dates.at(i).asString(), d) || d == kNoDate) return fail("bad specific date");
    r.dates.push_back(d);
  }
  if (j.has("on")) r.cycleOn = static_cast<uint16_t>(j["on"].asInt());
  if (j.has("off")) r.cycleOff = static_cast<uint16_t>(j["off"].asInt());
  if (j.has("minIntervalMin")) r.prnMinIntervalMin = static_cast<uint16_t>(j["minIntervalMin"].asInt());
  if (j.has("maxPer24h")) r.prnMaxPer24h = static_cast<uint8_t>(j["maxPer24h"].asInt());
  return true;
}

JsonValue encodeMedication(const Medication& m) {
  JsonValue j = JsonValue::object();
  j.set("id", I(m.id));
  j.set("profile", I(m.profileId));
  j.set("name", S(m.name));
  j.set("dose", S(m.doseText));
  j.set("form", S(kFormNames[static_cast<int>(m.form)]));
  j.set("instructions", S(m.instructions));
  j.set("notes", S(m.notes));
  j.set("start", S(formatDate(m.startDay)));
  j.set("end", S(formatDate(m.endDay)));
  j.set("courseDays", I(m.courseDays));
  j.set("graceMin", I(m.graceMin));
  j.set("onTimeMin", I(m.onTimeMin));
  JsonValue ladder = JsonValue::object();
  ladder.set("intervalMin", I(m.ladderIntervalMin));
  ladder.set("repeats", I(m.ladderCount));
  j.set("ladder", ladder);
  j.set("critical", B(m.critical));
  j.set("alertPattern", I(m.alertPattern));
  j.set("alertVolume", I(m.alertVolume));
  j.set("supply", I(m.supply));
  j.set("lowStock", I(m.lowStockThreshold));
  j.set("refill", S(formatDate(m.refillDay)));
  j.set("active", B(m.active));
  JsonValue phases = JsonValue::array();
  for (const auto& ph : m.phases) {
    JsonValue pj = JsonValue::object();
    pj.set("from", S(formatDate(ph.fromDay)));
    pj.set("to", S(formatDate(ph.toDay)));
    pj.set("dose", S(ph.doseText));
    pj.set("rule", encodeRule(ph.rule));
    phases.push(pj);
  }
  j.set("phases", phases);
  return j;
}

bool decodeMedication(const JsonValue& j, Medication& m, std::string* err) {
  auto fail = [&](const char* msg) {
    if (err) *err = msg;
    return false;
  };
  if (!j.isObject()) return fail("medication is not an object");
  m = Medication();
  m.id = static_cast<uint16_t>(j["id"].asInt());
  m.profileId = static_cast<uint8_t>(j["profile"].asInt());
  m.name = j["name"].asString();
  m.doseText = j["dose"].asString();
  const int f = lookup(kFormNames, j["form"].asString());
  m.form = f < 0 ? MedForm::Other : static_cast<MedForm>(f);
  m.instructions = j["instructions"].asString();
  m.notes = j["notes"].asString();
  if (!parseDate(j["start"].asString(), m.startDay)) return fail("bad start date");
  if (!parseDate(j["end"].asString(), m.endDay)) return fail("bad end date");
  m.courseDays = static_cast<uint16_t>(j["courseDays"].asInt());
  m.graceMin = static_cast<uint16_t>(j["graceMin"].asInt(60));
  m.onTimeMin = static_cast<uint16_t>(j["onTimeMin"].asInt(15));
  m.ladderIntervalMin = static_cast<uint8_t>(j["ladder"]["intervalMin"].asInt(5));
  m.ladderCount = static_cast<uint8_t>(j["ladder"]["repeats"].asInt(6));
  m.critical = j["critical"].asBool(false);
  m.alertPattern = static_cast<uint8_t>(j["alertPattern"].asInt(255));
  m.alertVolume = static_cast<uint8_t>(j["alertVolume"].asInt(0));
  m.supply = static_cast<int32_t>(j["supply"].asInt(-1));
  m.lowStockThreshold = static_cast<int32_t>(j["lowStock"].asInt(7));
  if (!parseDate(j["refill"].asString(), m.refillDay)) return fail("bad refill date");
  m.active = j["active"].asBool(true);
  const JsonValue& phases = j["phases"];
  for (size_t i = 0; i < phases.size(); i++) {
    const JsonValue& pj = phases.at(i);
    Phase ph;
    if (!parseDate(pj["from"].asString(), ph.fromDay)) return fail("bad phase from");
    if (!parseDate(pj["to"].asString(), ph.toDay)) return fail("bad phase to");
    ph.doseText = pj["dose"].asString();
    if (!decodeRule(pj["rule"], ph.rule, err)) return false;
    m.phases.push_back(ph);
  }
  return true;
}

JsonValue encodeDatabase(const Database& db) {
  JsonValue j = JsonValue::object();
  j.set("schemaVersion", I(kSchemaVersion));
  JsonValue s = JsonValue::object();
  s.set("tz", S(db.settings.tz));
  s.set("activeProfile", I(db.settings.activeProfile));
  s.set("sleepOnUsb", B(db.settings.sleepOnUsb));
  s.set("ladderIntervalMin", I(db.settings.defaultLadderIntervalMin));
  s.set("ladderRepeats", I(db.settings.defaultLadderCount));
  s.set("setupDone", B(db.settings.setupDone));
  s.set("language", S(db.settings.language));
  s.set("tzName", S(db.settings.tzName));
  s.set("adminPinSalt", S(db.settings.adminPinSalt));
  s.set("adminPinHash", S(db.settings.adminPinHash));
  s.set("refreshFullEvery", I(db.settings.refreshFullEvery));
  s.set("nightlyFullRefresh", B(db.settings.nightlyFullRefresh));
  s.set("autoBackup", B(db.settings.autoBackup));
  s.set("wifiEnabled", B(db.settings.wifiEnabled));
  s.set("autoTimeSync", B(db.settings.autoTimeSync));
  s.set("notifyEnabled", B(db.settings.notifyEnabled));
  s.set("notifyUrl", S(db.settings.notifyUrl));
  s.set("notifyNames", B(db.settings.notifyNames));
  j.set("settings", s);
  JsonValue ps = JsonValue::array();
  for (const auto& p : db.profiles) ps.push(encodeProfile(p));
  j.set("profiles", ps);
  JsonValue ms = JsonValue::array();
  for (const auto& m : db.meds) ms.push(encodeMedication(m));
  j.set("meds", ms);
  return j;
}

bool decodeDatabase(const JsonValue& j, Database& db, std::string* err) {
  if (!j.isObject()) {
    if (err) *err = "database is not an object";
    return false;
  }
  if (j["schemaVersion"].asInt() != kSchemaVersion) {
    if (err) *err = "schema version mismatch (migrate first)";
    return false;
  }
  Database out;
  out.settings.tz = j["settings"]["tz"].asString();
  if (out.settings.tz.empty()) out.settings.tz = kDefaultTz;
  out.settings.activeProfile = static_cast<uint8_t>(j["settings"]["activeProfile"].asInt(1));
  out.settings.sleepOnUsb = j["settings"]["sleepOnUsb"].asBool(false);
  out.settings.defaultLadderIntervalMin = static_cast<uint8_t>(j["settings"]["ladderIntervalMin"].asInt(5));
  out.settings.defaultLadderCount = static_cast<uint8_t>(j["settings"]["ladderRepeats"].asInt(6));
  const JsonValue& st = j["settings"];
  // Databases written before Phase 5 have profiles but no setupDone flag: treat them as set up.
  out.settings.setupDone = st.has("setupDone") ? st["setupDone"].asBool(false) : j["profiles"].size() > 0;
  out.settings.language = st.has("language") ? st["language"].asString() : "en";
  out.settings.tzName = st.has("tzName") ? st["tzName"].asString() : "Europe/Berlin";
  out.settings.adminPinSalt = st["adminPinSalt"].asString();
  out.settings.adminPinHash = st["adminPinHash"].asString();
  out.settings.refreshFullEvery = static_cast<uint8_t>(st["refreshFullEvery"].asInt(20));
  out.settings.nightlyFullRefresh = st["nightlyFullRefresh"].asBool(true);
  out.settings.autoBackup = st["autoBackup"].asBool(true);
  out.settings.wifiEnabled = st["wifiEnabled"].asBool(false);
  out.settings.autoTimeSync = st["autoTimeSync"].asBool(true);
  out.settings.notifyEnabled = st["notifyEnabled"].asBool(false);
  out.settings.notifyUrl = st["notifyUrl"].asString();
  out.settings.notifyNames = st["notifyNames"].asBool(false);
  const JsonValue& ps = j["profiles"];
  for (size_t i = 0; i < ps.size(); i++) {
    Profile p;
    if (!decodeProfile(ps.at(i), p, err)) return false;
    out.profiles.push_back(p);
  }
  const JsonValue& ms = j["meds"];
  for (size_t i = 0; i < ms.size(); i++) {
    Medication m;
    if (!decodeMedication(ms.at(i), m, err)) return false;
    out.meds.push_back(m);
  }
  const std::string v = validateDatabase(out);
  if (!v.empty()) {
    if (err) *err = v;
    return false;
  }
  db = out;
  return true;
}

JsonValue encodeLogEntry(const DoseLogEntry& e) {
  JsonValue j = JsonValue::object();
  j.set("eventId", S(formatEventId(e.eventId)));
  j.set("profile", I(e.profileId));
  j.set("med", I(e.medId));
  j.set("scheduledUtc", I(e.scheduledUtc));
  j.set("action", S(doseActionName(e.action)));
  j.set("actionUtc", I(e.actionUtc));
  j.set("recordedUtc", I(e.recordedUtc));
  j.set("source", S(actionSourceName(e.source)));
  j.set("note", S(e.note));
  return j;
}

bool decodeLogEntry(const JsonValue& j, DoseLogEntry& e) {
  if (!j.isObject()) return false;
  if (!parseEventId(j["eventId"].asString(), e.eventId)) return false;
  e.profileId = static_cast<uint8_t>(j["profile"].asInt());
  e.medId = static_cast<uint16_t>(j["med"].asInt());
  e.scheduledUtc = j["scheduledUtc"].asInt();
  if (!parseDoseAction(j["action"].asString(), e.action)) return false;
  e.actionUtc = j["actionUtc"].asInt();
  e.recordedUtc = j["recordedUtc"].asInt();
  if (!parseActionSource(j["source"].asString(), e.source)) return false;
  e.note = j["note"].asString();
  return true;
}

JsonValue encodeSnapshot(const EngineSnapshot& s) {
  JsonValue j = JsonValue::object();
  j.set("generatedUntil", I(s.generatedUntilUtc));
  j.set("processedUntil", I(s.processedUntilUtc));
  JsonValue arr = JsonValue::array();
  for (const auto& d : s.instances) {
    JsonValue x = JsonValue::object();
    x.set("id", S(formatEventId(d.eventId)));
    x.set("p", I(d.profileId));
    x.set("m", I(d.medId));
    x.set("s", I(d.scheduledUtc));
    x.set("day", I(d.localDay));
    x.set("min", I(d.localMinute));
    x.set("st", I(static_cast<int>(d.state)));
    x.set("sn", I(d.snoozeCount));
    x.set("su", I(d.snoozeUntil));
    x.set("af", I(d.alarmsFired));
    x.set("ls", I(d.ladderStartUtc));
    x.set("a", I(d.actionUtc));
    x.set("lr", I(d.lastActionRecorded));
    x.set("u", B(d.undoable));
    x.set("ps", I(static_cast<int>(d.prevState)));
    x.set("psn", I(d.prevSnoozeCount));
    x.set("psu", I(d.prevSnoozeUntil));
    x.set("paf", I(d.prevAlarmsFired));
    x.set("pls", I(d.prevLadderStart));
    x.set("plt", I(d.prevLastTaken));
    x.set("sd", B(d.supplyDecremented));
    arr.push(x);
  }
  j.set("instances", arr);
  JsonValue lt = JsonValue::array();
  for (const auto& kv : s.lastTaken) {
    JsonValue x = JsonValue::object();
    x.set("m", I(kv.first));
    x.set("t", I(kv.second));
    lt.push(x);
  }
  j.set("lastTaken", lt);
  return j;
}

bool decodeSnapshot(const JsonValue& j, EngineSnapshot& s) {
  if (!j.isObject()) return false;
  s = EngineSnapshot();
  s.generatedUntilUtc = j["generatedUntil"].asInt();
  s.processedUntilUtc = j["processedUntil"].asInt();
  const JsonValue& arr = j["instances"];
  for (size_t i = 0; i < arr.size(); i++) {
    const JsonValue& x = arr.at(i);
    DoseInstance d;
    if (!parseEventId(x["id"].asString(), d.eventId)) return false;
    d.profileId = static_cast<uint8_t>(x["p"].asInt());
    d.medId = static_cast<uint16_t>(x["m"].asInt());
    d.scheduledUtc = x["s"].asInt();
    d.localDay = static_cast<int32_t>(x["day"].asInt());
    d.localMinute = static_cast<int16_t>(x["min"].asInt());
    const int64_t st = x["st"].asInt();
    if (st < 0 || st > 6) return false;
    d.state = static_cast<DoseState>(st);
    d.snoozeCount = static_cast<uint8_t>(x["sn"].asInt());
    d.snoozeUntil = x["su"].asInt();
    d.alarmsFired = static_cast<uint8_t>(x["af"].asInt());
    d.ladderStartUtc = x["ls"].asInt();
    d.actionUtc = x["a"].asInt();
    d.lastActionRecorded = x["lr"].asInt();
    d.undoable = x["u"].asBool();
    d.prevState = static_cast<DoseState>(x["ps"].asInt() & 7);
    d.prevSnoozeCount = static_cast<uint8_t>(x["psn"].asInt());
    d.prevSnoozeUntil = x["psu"].asInt();
    d.prevAlarmsFired = static_cast<uint8_t>(x["paf"].asInt());
    d.prevLadderStart = x["pls"].asInt();
    d.prevLastTaken = x["plt"].asInt();
    d.supplyDecremented = x["sd"].asBool();
    s.instances.push_back(d);
  }
  const JsonValue& lt = j["lastTaken"];
  for (size_t i = 0; i < lt.size(); i++)
    s.lastTaken.emplace_back(static_cast<uint16_t>(lt.at(i)["m"].asInt()), lt.at(i)["t"].asInt());
  return true;
}

}  // namespace core
