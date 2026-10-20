#pragma once
// Schema migration framework for the JSON database document.
// Each step upgrades a document from version N to N+1 in place.
#include <string>

#include "core/json.h"

namespace storage {

using MigrationFn = bool (*)(core::JsonValue& doc, std::string* err);

struct Migration {
  int fromVersion;
  MigrationFn fn;
  const char* description;
};

// Version of a document: "schemaVersion", or 1 if absent (v1 had no field).
int documentVersion(const core::JsonValue& doc);
// Upgrades to core::kSchemaVersion. Fails for unknown/newer versions. `stepsRun` counts migrations applied.
bool migrateToCurrent(core::JsonValue& doc, std::string* err, int* stepsRun = nullptr);

// v1 -> v2: med "grace" -> "graceMin"; med "times":["HH:MM"] -> phases[0] fixed rule;
// profile "privacy" added (default true); tz moved from root "tz" into settings.
bool migrateV1toV2(core::JsonValue& doc, std::string* err);

}  // namespace storage
