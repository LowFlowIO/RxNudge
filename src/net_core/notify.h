#pragma once
// Caregiver missed-dose push (opt-in). Builds the HTTP request for an ntfy
// topic URL (plain text + headers) or a generic webhook (JSON). Medication and
// person names are only included when the user enabled it.
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "core/tz.h"

namespace net {

struct MissedDose {
  uint8_t profileId = 0;
  std::string person, initials, med;
  int64_t scheduledUtc = 0;
};

struct OutboundRequest {
  std::string url;
  std::string contentType;
  std::string body;
  std::vector<std::pair<std::string, std::string>> headers;
};

bool isNtfyUrl(const std::string& url);
OutboundRequest buildMissedNotification(const std::string& url, bool includeNames, const std::vector<MissedDose>& missed,
                                        const core::PosixTz& tz);
OutboundRequest buildTestNotification(const std::string& url);

}  // namespace net
