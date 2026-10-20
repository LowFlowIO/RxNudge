#pragma once
// Medication names must never reach logs unless the debug flag is set.
#include <string>

#include "core/model.h"

#ifndef MT_DEBUG_LOG_MED_NAMES
#define MT_DEBUG_LOG_MED_NAMES 0
#endif

namespace core {

constexpr bool kLogMedNames = MT_DEBUG_LOG_MED_NAMES != 0;

// Label safe for diagnostic logs: "med#12", or "med#12(Name)" only in debug builds.
std::string medLogLabel(const Medication& m, bool debugNames = kLogMedNames);
// Replaces every occurrence of any medication name in `text` with "med#<id>".
std::string redactMedNames(const std::string& text, const Database& db, bool debugNames = kLogMedNames);

}  // namespace core
