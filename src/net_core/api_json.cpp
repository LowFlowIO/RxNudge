#include "net_core/api_json.h"

#include <cmath>

#include "app_core/format.h"
#include "core/codec.h"
#include "core/presets.h"

namespace net {

using core::JsonValue;

namespace {
bool fail(std::string* err, const std::string& m) {
  if (err) *err = m;
  return false;
}
bool utf8Ok(const std::string& s) {
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    size_t n;
    if (c < 0x80) n = 0;
    else if ((c >> 5) == 6) n = 1;
    else if ((c >> 4) == 14) n = 2;
    else if ((c >> 3) == 30) n = 3;
    else return false;
    if (c < 0x20 && c != '\t') return false;  // no control characters
    if (i + n >= s.size() && n > 0) return false;
    for (size_t k = 1; k <= n; k++)
      if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
    i += n + 1;
  }
  return true;
}
// optional unless `required`
bool str(const JsonValue& j, const char* key, size_t maxBytes, bool required, std::string* err, size_t minBytes = 0) {
  if (!j.has(key)) return required ? fail(err, std::string(key) + " is required") : true;
  const JsonValue& v = j[key];
  if (!v.isString()) return fail(err, std::string(key) + " must be a string");
  const std::string& s = v.asString();
  if (s.size() < minBytes) return fail(err, std::string(key) + " must not be empty");
  if (s.size() > maxBytes) return fail(err, std::string(key) + app::fmt(" is longer than %u bytes", static_cast<unsigned>(maxBytes)));
  if (!utf8Ok(s)) return fail(err, std::string(key) + " is not valid text");
  return true;
}
bool integer(const JsonValue& j, const char* key, int64_t mn, int64_t mx, bool required, std::string* err) {
  if (!j.has(key)) return required ? fail(err, std::string(key) + " is required") : true;
  const JsonValue& v = j[key];
  if (v.type() == JsonValue::Type::Double) {
    const double d = v.asDouble();
    if (std::floor(d) != d) return fail(err, std::string(key) + " must be a whole number");
  } else if (v.type() != JsonValue::Type::Int) {
    return fail(err, std::string(key) + " must be a number");
  }
  const double d = v.asDouble();
  if (d < static_cast<double>(mn) || d > static_cast<double>(mx))
    return fail(err, std::string(key) + app::fmt(" must be %lld..%lld", static_cast<long long>(mn), static_cast<long long>(mx)));
  return true;
}
bool boolean(const JsonValue& j, const char* key, std::string* err) {
  if (!j.has(key)) return true;
  if (j[key].type() != JsonValue::Type::Bool) return fail(err, std::string(key) + " must be true or false");
  return true;
}
bool hm(const JsonValue& j, const char* key, bool required, std::string* err) {
  if (!j.has(key)) return required ? fail(err, std::string(key) + " is required") : true;
  int m;
  if (!j[key].isString() || !core::parseHm(j[key].asString(), m)) return fail(err, std::string(key) + " must be HH:MM");
  return true;
}
bool date(const JsonValue& j, const char* key, std::string* err) {
  if (!j.has(key)) return true;
  int32_t d;
  if (!j[key].isString() || !core::parseDate(j[key].asString(), d)) return fail(err, std::string(key) + " must be YYYY-MM-DD or empty");
  return true;
}
}  // namespace

bool parseBody(const std::string& body, size_t maxBytes, JsonValue& out, std::string* err) {
  if (body.size() > maxBytes) return fail(err, "request body too large");
  std::string e;
  if (!core::parseJson(body, out, &e)) return fail(err, "invalid JSON: " + e);
  if (!out.isObject()) return fail(err, "JSON object expected");
  return true;
}

JsonValue errorJson(const std::string& m) {
  JsonValue j = JsonValue::object();
  j.set("error", JsonValue::string(m));
  return j;
}

JsonValue profileToApi(const core::Profile& p) {
  JsonValue j = core::encodeProfile(p);
  j.remove("pinSalt");
  j.remove("pinHash");
  j.set("hasPin", JsonValue::boolean(!p.pinHash.empty()));
  return j;
}

bool validateProfileBody(const JsonValue& j, std::string* err) {
  if (!j.isObject()) return fail(err, "profile must be an object");
  if (!str(j, "name", core::kMaxNameBytes, true, err, 1)) return false;
  if (!str(j, "initials", 8, false, err)) return false;
  if (!hm(j, "quietStart", false, err) || !hm(j, "quietEnd", false, err)) return false;
  if (!boolean(j, "privacy", err)) return false;
  if (!integer(j, "alertPattern", 0, 3, false, err) || !integer(j, "alertVolume", 0, 100, false, err)) return false;
  if (j.has("routines")) {
    const JsonValue& r = j["routines"];
    if (!r.isObject()) return fail(err, "routines must be an object");
    for (const auto& kv : r.members()) {
      bool known = false;
      for (int i = 1; i <= static_cast<int>(core::kRoutineCount); i++) {
        std::string n = core::routineName(static_cast<core::Routine>(i));
        for (char& c : n) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        known = known || n == kv.first;
      }
      if (!known) return fail(err, "unknown routine '" + kv.first + "'");
      if (!hm(r, kv.first.c_str(), true, err)) return false;
    }
  }
  if (j.has("pinSalt") || j.has("pinHash")) return fail(err, "PINs can only be changed on the device");
  return true;
}

bool profileFromApi(const JsonValue& j, const core::Profile* existing, core::Profile& out, std::string* err) {
  if (!validateProfileBody(j, err)) return false;
  JsonValue merged = existing ? core::encodeProfile(*existing) : JsonValue::object();
  for (const auto& kv : j.members())
    if (kv.first != "id" && kv.first != "hasPin") merged.set(kv.first, kv.second);
  merged.set("id", JsonValue::integer(existing ? existing->id : 0));
  if (!existing) {
    merged.remove("pinSalt");
    merged.remove("pinHash");
  }
  if (!core::decodeProfile(merged, out, err)) return false;
  if (out.initials.empty()) {
    for (size_t i = 0; i < out.name.size() && out.initials.size() < 2; ++i) {
      const unsigned char c = static_cast<unsigned char>(out.name[i]);
      if (c < 0x80 && (i == 0 || out.name[i - 1] == ' ') && c != ' ') out.initials.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
    }
    if (out.initials.empty()) out.initials = "?";
  }
  return true;
}

JsonValue medicationToApi(const core::Medication& m) { return core::encodeMedication(m); }

bool validateRuleBody(const JsonValue& r, std::string* err) {
  if (!r.isObject()) return fail(err, "rule must be an object");
  if (!str(r, "type", 16, true, err, 1)) return false;
  if (r.has("times")) {
    const JsonValue& t = r["times"];
    if (!t.isArray()) return fail(err, "times must be a list");
    if (t.size() > core::kMaxTimesPerSchedule) return fail(err, "at most 12 times");
    for (size_t i = 0; i < t.size(); i++) {
      const JsonValue& x = t.at(i);
      if (!x.isObject()) return fail(err, "each time must be an object");
      if (x.has("routine")) {
        if (!str(x, "routine", 16, true, err, 1) || !integer(x, "offset", -720, 720, false, err)) return false;
      } else if (!hm(x, "at", true, err)) {
        return false;
      }
    }
  }
  if (!integer(r, "weekdays", 1, 127, false, err) || !integer(r, "every", 1, 365, false, err)) return false;
  if (!str(r, "unit", 8, false, err)) return false;
  if (r.has("unit") && r["unit"].asString() != "days" && r["unit"].asString() != "weeks") return fail(err, "unit must be days or weeks");
  if (r.has("dates")) {
    const JsonValue& d = r["dates"];
    if (!d.isArray() || d.size() > core::kMaxSpecificDates) return fail(err, "dates must be a list of at most 12");
    for (size_t i = 0; i < d.size(); i++) {
      int32_t day;
      if (!d.at(i).isString() || !core::parseDate(d.at(i).asString(), day) || day == core::kNoDate) return fail(err, "dates must be YYYY-MM-DD");
    }
  }
  if (!integer(r, "on", 1, 365, false, err) || !integer(r, "off", 1, 365, false, err)) return false;
  if (!integer(r, "minIntervalMin", 1, 1440, false, err) || !integer(r, "maxPer24h", 1, 24, false, err)) return false;
  return true;
}

bool validatePhasesBody(const JsonValue& p, std::string* err) {
  if (!p.isArray()) return fail(err, "phases must be a list");
  if (p.size() < 1 || p.size() > core::kMaxPhases) return fail(err, "1 to 4 phases");
  for (size_t i = 0; i < p.size(); i++) {
    const JsonValue& ph = p.at(i);
    if (!ph.isObject()) return fail(err, "each phase must be an object");
    if (!date(ph, "from", err) || !date(ph, "to", err) || !str(ph, "dose", core::kMaxNameBytes, false, err)) return false;
    if (!ph.has("rule")) return fail(err, "phase rule is required");
    if (!validateRuleBody(ph["rule"], err)) return false;
  }
  return true;
}

bool validateMedicationBody(const JsonValue& j, std::string* err) {
  if (!j.isObject()) return fail(err, "medication must be an object");
  if (!integer(j, "profile", 1, static_cast<int64_t>(core::kMaxProfiles), true, err)) return false;
  if (!str(j, "name", core::kMaxNameBytes, true, err, 1)) return false;
  if (!str(j, "dose", core::kMaxNameBytes, false, err) || !str(j, "form", 16, false, err)) return false;
  if (!str(j, "instructions", core::kMaxTextBytes, false, err) || !str(j, "notes", core::kMaxTextBytes, false, err)) return false;
  if (!j.has("start")) return fail(err, "start is required");
  if (!date(j, "start", err) || !date(j, "end", err) || !date(j, "refill", err)) return false;
  if (!integer(j, "courseDays", 0, 3650, false, err)) return false;
  if (!integer(j, "graceMin", 1, 1440, false, err) || !integer(j, "onTimeMin", 0, 1440, false, err)) return false;
  if (j.has("ladder")) {
    const JsonValue& l = j["ladder"];
    if (!l.isObject()) return fail(err, "ladder must be an object");
    if (!integer(l, "intervalMin", 1, 60, false, err) || !integer(l, "repeats", 1, 20, false, err)) return false;
  }
  if (!boolean(j, "critical", err) || !boolean(j, "active", err)) return false;
  if (j.has("alertPattern") && j["alertPattern"].asInt(-1) != 255 && !integer(j, "alertPattern", 0, 3, false, err)) return false;
  if (!integer(j, "alertVolume", 0, 100, false, err)) return false;
  if (!integer(j, "supply", -1, 99999, false, err) || !integer(j, "lowStock", 0, 9999, false, err)) return false;
  if (!j.has("phases")) return fail(err, "phases are required");
  return validatePhasesBody(j["phases"], err);
}

bool medicationFromApi(const JsonValue& j, uint16_t id, core::Medication& out, std::string* err) {
  if (!validateMedicationBody(j, err)) return false;
  JsonValue c = j;
  c.set("id", JsonValue::integer(id));
  if (!core::decodeMedication(c, out, err)) return false;
  // names that the decoder would silently map to a default are rejected instead
  const JsonValue back = core::encodeMedication(out);
  if (j.has("form") && back["form"].asString() != j["form"].asString()) return fail(err, "unknown form '" + j["form"].asString() + "'");
  return true;
}

bool phasesFromApi(const JsonValue& phases, std::vector<core::Phase>& out, std::string* err) {
  if (!validatePhasesBody(phases, err)) return false;
  JsonValue m = JsonValue::object();
  m.set("start", JsonValue::string("2026-01-01"));
  m.set("phases", phases);
  core::Medication tmp;
  if (!core::decodeMedication(m, tmp, err)) return false;
  out = tmp.phases;
  return true;
}

JsonValue historyToApi(const std::vector<core::DoseLogEntry>& log, const core::Database& db, const core::PosixTz& tz,
                       int profileId, int64_t sinceUtc, size_t limit) {
  JsonValue arr = JsonValue::array();
  for (auto it = log.rbegin(); it != log.rend() && arr.size() < limit; ++it) {
    const core::DoseLogEntry& e = *it;
    if (profileId && e.profileId != profileId) continue;
    if (e.actionUtc < sinceUtc) continue;
    if (e.action == core::DoseAction::Snoozed) continue;
    const core::Medication* m = db.med(e.medId);
    const core::Profile* p = db.profile(e.profileId);
    JsonValue o = JsonValue::object();
    const core::LocalTime sl = core::utcToLocal(tz, e.scheduledUtc), al = core::utcToLocal(tz, e.actionUtc);
    o.set("date", JsonValue::string(core::formatDate(sl.days)));
    o.set("scheduled", JsonValue::string(core::formatHm(sl.secOfDay / 60)));
    o.set("at", JsonValue::string(core::formatDate(al.days) + " " + core::formatHm(al.secOfDay / 60)));
    o.set("person", JsonValue::string(p ? p->name : "?"));
    o.set("med", JsonValue::string(m ? m->name : "(deleted)"));
    o.set("action", JsonValue::string(core::doseActionName(e.action)));
    o.set("source", JsonValue::string(core::actionSourceName(e.source)));
    arr.push(o);
  }
  return arr;
}

}  // namespace net
