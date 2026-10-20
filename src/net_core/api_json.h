#pragma once
// JSON mapping and request validation for the web portal API (pure).
// Types and ranges are checked BEFORE values are narrowed into the model, then
// the model validators (core::validate*) and AdminService run as for the
// on-device editors. PIN salts/hashes never leave the device.
#include <string>
#include <vector>

#include "core/dose_log.h"
#include "core/json.h"
#include "core/model.h"
#include "core/tz.h"

namespace net {

constexpr size_t kMaxJsonBody = 16 * 1024;  // profiles / medications
constexpr size_t kMaxRestoreBody = 2 * 1024 * 1024;

bool parseBody(const std::string& body, size_t maxBytes, core::JsonValue& out, std::string* err);

core::JsonValue profileToApi(const core::Profile& p);  // no pinSalt/pinHash; "hasPin"
bool validateProfileBody(const core::JsonValue& j, std::string* err);
// existing == nullptr: new profile (id 0). PINs are kept from `existing`.
bool profileFromApi(const core::JsonValue& j, const core::Profile* existing, core::Profile& out, std::string* err);

core::JsonValue medicationToApi(const core::Medication& m);
bool validateRuleBody(const core::JsonValue& rule, std::string* err);
bool validatePhasesBody(const core::JsonValue& phases, std::string* err);
bool validateMedicationBody(const core::JsonValue& j, std::string* err);
bool medicationFromApi(const core::JsonValue& j, uint16_t id, core::Medication& out, std::string* err);
bool phasesFromApi(const core::JsonValue& phases, std::vector<core::Phase>& out, std::string* err);

// History: newest first, optional person filter.
core::JsonValue historyToApi(const std::vector<core::DoseLogEntry>& log, const core::Database& db, const core::PosixTz& tz,
                             int profileId, int64_t sinceUtc, size_t limit);

core::JsonValue errorJson(const std::string& message);

}  // namespace net
