#include "storage/migrations.h"

#include "core/model.h"
#include "core/tz.h"

namespace storage {
using core::JsonValue;

static const Migration kMigrations[] = {
    {1, &migrateV1toV2, "v1->v2 phases, graceMin, privacy, settings.tz"},
};

int documentVersion(const JsonValue& doc) {
  return doc.has("schemaVersion") ? static_cast<int>(doc["schemaVersion"].asInt()) : 1;
}

bool migrateToCurrent(JsonValue& doc, std::string* err, int* stepsRun) {
  if (stepsRun) *stepsRun = 0;
  if (!doc.isObject()) {
    if (err) *err = "document is not an object";
    return false;
  }
  int v = documentVersion(doc);
  if (v > core::kSchemaVersion || v < 1) {
    if (err) *err = "unsupported schema version " + std::to_string(v);
    return false;
  }
  while (v < core::kSchemaVersion) {
    const Migration* step = nullptr;
    for (const auto& m : kMigrations)
      if (m.fromVersion == v) step = &m;
    if (!step) {
      if (err) *err = "no migration from version " + std::to_string(v);
      return false;
    }
    JsonValue work = doc;  // all-or-nothing per step
    if (!step->fn(work, err)) return false;
    work.set("schemaVersion", JsonValue::integer(v + 1));
    doc = work;
    v++;
    if (stepsRun) (*stepsRun)++;
  }
  return true;
}

bool migrateV1toV2(JsonValue& doc, std::string* err) {
  JsonValue settings = doc["settings"].isObject() ? doc["settings"] : JsonValue::object();
  if (!settings.has("tz")) settings.set("tz", JsonValue::string(doc.has("tz") ? doc["tz"].asString() : core::kDefaultTz));
  if (!settings.has("activeProfile")) settings.set("activeProfile", JsonValue::integer(1));
  doc.remove("tz");
  doc.set("settings", settings);

  JsonValue profiles = JsonValue::array();
  const JsonValue& ps = doc["profiles"];
  for (size_t i = 0; i < ps.size(); i++) {
    JsonValue p = ps.at(i);
    if (!p.isObject()) {
      if (err) *err = "v1 profile is not an object";
      return false;
    }
    if (!p.has("privacy")) p.set("privacy", JsonValue::boolean(true));
    profiles.push(p);
  }
  doc.set("profiles", profiles);

  JsonValue meds = JsonValue::array();
  const JsonValue& ms = doc["meds"];
  for (size_t i = 0; i < ms.size(); i++) {
    JsonValue m = ms.at(i);
    if (!m.isObject()) {
      if (err) *err = "v1 medication is not an object";
      return false;
    }
    if (m.has("grace")) {
      m.set("graceMin", JsonValue::integer(m["grace"].asInt(60)));
      m.remove("grace");
    }
    if (!m.has("phases")) {
      JsonValue rule = JsonValue::object();
      rule.set("type", JsonValue::string("fixed"));
      JsonValue times = JsonValue::array();
      const JsonValue& ts = m["times"];
      for (size_t k = 0; k < ts.size(); k++) {
        JsonValue t = JsonValue::object();
        t.set("at", ts.at(k));
        times.push(t);
      }
      rule.set("times", times);
      JsonValue phase = JsonValue::object();
      phase.set("from", JsonValue::string(""));
      phase.set("to", JsonValue::string(""));
      phase.set("dose", JsonValue::string(""));
      phase.set("rule", rule);
      JsonValue phases = JsonValue::array();
      phases.push(phase);
      m.set("phases", phases);
    }
    m.remove("times");
    meds.push(m);
  }
  doc.set("meds", meds);
  return true;
}

}  // namespace storage
