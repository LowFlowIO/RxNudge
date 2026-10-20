#include "storage/repository.h"

#include "core/codec.h"
#include "storage/migrations.h"

namespace storage {
using core::JsonValue;

LoadStatus FsRepository::loadDatabase(core::Database& db) {
  uint16_t schema = 0;
  std::string payload;
  LoadStatus st = store_.read(dbPath(), schema, payload);
  health_.dbStatus = st;
  if (!loadOk(st)) {
    if (st == LoadStatus::Corrupt) health_.lastError = "database corrupt; no valid copy";
    return st;
  }
  if (st == LoadStatus::RecoveredFromBackup) health_.lastError = "database restored from last known good copy";
  JsonValue doc;
  std::string err;
  if (!core::parseJson(payload, doc, &err)) {
    health_.dbStatus = LoadStatus::Corrupt;
    health_.lastError = "database unreadable: " + err;
    return LoadStatus::Corrupt;
  }
  const int fromVersion = doc.has("schemaVersion") ? documentVersion(doc) : (schema >= 1 ? schema : 1);
  int steps = 0;
  if (!doc.has("schemaVersion")) doc.set("schemaVersion", JsonValue::integer(fromVersion));
  if (!migrateToCurrent(doc, &err, &steps)) {
    health_.dbStatus = LoadStatus::Corrupt;
    health_.lastError = "migration failed: " + err;
    return LoadStatus::Corrupt;
  }
  core::Database out;
  if (!core::decodeDatabase(doc, out, &err)) {
    health_.dbStatus = LoadStatus::Corrupt;
    health_.lastError = "database invalid: " + err;
    return LoadStatus::Corrupt;
  }
  health_.migrationsRun = steps;
  if (steps > 0) {
    // Keep the pre-migration record, then persist the upgraded document.
    const std::string pre = dbPath() + ".v" + std::to_string(fromVersion);
    if (fs_.exists(pre)) fs_.remove(pre);
    fs_.writeFile(pre, encodeRecord(schema, payload));
    if (!saveDatabase(out)) health_.lastError = "could not save migrated database";
  }
  db = out;
  return st;
}

bool FsRepository::saveDatabase(const core::Database& db) {
  const std::string v = core::validateDatabase(db);
  if (!v.empty()) {
    health_.lastError = "refused to save invalid database: " + v;
    return false;
  }
  const bool ok = store_.write(dbPath(), static_cast<uint16_t>(core::kSchemaVersion), core::encodeDatabase(db).dump());
  health_.lastWriteFailed = !ok;
  if (!ok) health_.lastError = "database write failed";
  return ok;
}

LoadStatus FsRepository::loadSnapshot(core::EngineSnapshot& s) {
  uint16_t schema = 0;
  std::string payload;
  LoadStatus st = store_.read(snapshotPath(), schema, payload);
  JsonValue doc;
  if (loadOk(st) && !(core::parseJson(payload, doc) && core::decodeSnapshot(doc, s))) st = LoadStatus::Corrupt;
  // A lost snapshot is not fatal: the engine is rebuilt from the dose log; only report it.
  health_.snapshotStatus = st;
  return st;
}

bool FsRepository::saveSnapshot(const core::EngineSnapshot& s) {
  const bool ok = store_.write(snapshotPath(), static_cast<uint16_t>(core::kSchemaVersion), core::encodeSnapshot(s).dump());
  health_.lastWriteFailed = !ok;
  if (!ok) health_.lastError = "engine state write failed";
  return ok;
}

}  // namespace storage
