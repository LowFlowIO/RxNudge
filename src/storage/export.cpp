#include "storage/export.h"

#include "core/codec.h"
#include "core/json.h"
#include "storage/migrations.h"

namespace storage {
using core::JsonValue;

std::string csvEscape(const std::string& s) {
  std::string v = s;
  // Neutralise spreadsheet formula injection.
  if (!v.empty() && (v[0] == '=' || v[0] == '+' || v[0] == '-' || v[0] == '@')) v = "'" + v;
  if (v.find_first_of(",\"\n\r") == std::string::npos) return v;
  std::string out = "\"";
  for (char c : v) {
    if (c == '"') out += '"';
    out += c;
  }
  return out + "\"";
}

static std::string medName(const core::Database& db, uint16_t id) {
  const core::Medication* m = db.med(id);
  return m ? m->name : "";
}

std::string exportCsv(const std::vector<core::DoseLogEntry>& log, const core::Database& db, const ExportOptions& o) {
  std::string out = "event_id,profile,med_id,";
  if (o.includeNames) out += "med_name,";
  out += "scheduled_utc,action,action_utc,recorded_utc,source,note\r\n";
  for (const auto& e : log) {
    if (o.profileId && e.profileId != o.profileId) continue;
    out += core::formatEventId(e.eventId) + "," + std::to_string(e.profileId) + "," + std::to_string(e.medId) + ",";
    if (o.includeNames) out += csvEscape(medName(db, e.medId)) + ",";
    out += core::formatUtcIso(e.scheduledUtc) + "," + core::doseActionName(e.action) + "," + core::formatUtcIso(e.actionUtc) +
           "," + core::formatUtcIso(e.recordedUtc) + "," + core::actionSourceName(e.source) + "," + csvEscape(e.note) + "\r\n";
  }
  return out;
}

std::string exportJson(const std::vector<core::DoseLogEntry>& log, const core::Database& db, const ExportOptions& o) {
  JsonValue arr = JsonValue::array();
  for (const auto& e : log) {
    if (o.profileId && e.profileId != o.profileId) continue;
    JsonValue j = core::encodeLogEntry(e);
    j.set("scheduledIso", JsonValue::string(core::formatUtcIso(e.scheduledUtc)));
    j.set("actionIso", JsonValue::string(core::formatUtcIso(e.actionUtc)));
    if (o.includeNames) j.set("medName", JsonValue::string(medName(db, e.medId)));
    arr.push(j);
  }
  JsonValue root = JsonValue::object();
  root.set("format", JsonValue::string("rxnudge-log"));
  root.set("entries", arr);
  return root.dump(true);
}

std::string makeBackup(const core::Database& db, const std::vector<core::DoseLogEntry>& log) {
  JsonValue root = JsonValue::object();
  root.set("format", JsonValue::string("rxnudge-backup"));
  root.set("schemaVersion", JsonValue::integer(core::kSchemaVersion));
  core::Database safe = db;
  // Wi-Fi credentials are kept in NVS only, so they can never reach removable media.
  root.set("database", core::encodeDatabase(safe));
  JsonValue arr = JsonValue::array();
  for (const auto& e : log) arr.push(core::encodeLogEntry(e));
  root.set("log", arr);
  return root.dump(true);
}

bool restoreBackup(const std::string& json, core::Database& db, std::vector<core::DoseLogEntry>& log, std::string* err,
                   int* migrationsRun) {
  JsonValue root;
  if (!core::parseJson(json, root, err)) return false;
  if (root["format"].asString() != "rxnudge-backup") {
    if (err) *err = "not a RxNudge backup";
    return false;
  }
  JsonValue dbDoc = root["database"];
  if (!dbDoc.has("schemaVersion") && root.has("schemaVersion")) dbDoc.set("schemaVersion", root["schemaVersion"]);
  if (!migrateToCurrent(dbDoc, err, migrationsRun)) return false;
  core::Database newDb;
  if (!core::decodeDatabase(dbDoc, newDb, err)) return false;
  std::vector<core::DoseLogEntry> newLog;
  const JsonValue& arr = root["log"];
  for (size_t i = 0; i < arr.size(); i++) {
    core::DoseLogEntry e;
    if (!core::decodeLogEntry(arr.at(i), e)) {
      if (err) *err = "bad log entry #" + std::to_string(i);
      return false;
    }
    newLog.push_back(e);
  }
  db = newDb;
  log = newLog;
  return true;
}

}  // namespace storage
