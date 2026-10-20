#pragma once
// JSON encoding of the model (schema version kSchemaVersion). Pure.
#include <string>

#include "core/dose_engine.h"
#include "core/dose_log.h"
#include "core/json.h"
#include "core/model.h"

namespace core {

std::string formatDate(int32_t day);                // "YYYY-MM-DD", "" for kNoDate
bool parseDate(const std::string& s, int32_t& day);  // "" -> kNoDate
std::string formatHm(int minute);                   // "HH:MM"
bool parseHm(const std::string& s, int& minute);
std::string formatEventId(uint64_t id);             // 16 hex digits
bool parseEventId(const std::string& s, uint64_t& id);
std::string formatUtcIso(int64_t utc);              // "YYYY-MM-DDTHH:MM:SSZ"

JsonValue encodeProfile(const Profile& p);
bool decodeProfile(const JsonValue& j, Profile& p, std::string* err);
JsonValue encodeMedication(const Medication& m);
bool decodeMedication(const JsonValue& j, Medication& m, std::string* err);
JsonValue encodeDatabase(const Database& db);
// Expects an already-migrated document (schemaVersion == kSchemaVersion).
bool decodeDatabase(const JsonValue& j, Database& db, std::string* err);

JsonValue encodeLogEntry(const DoseLogEntry& e);
bool decodeLogEntry(const JsonValue& j, DoseLogEntry& e);

JsonValue encodeSnapshot(const EngineSnapshot& s);
bool decodeSnapshot(const JsonValue& j, EngineSnapshot& s);

}  // namespace core
