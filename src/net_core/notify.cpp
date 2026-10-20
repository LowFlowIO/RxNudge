#include "net_core/notify.h"

#include <map>

#include "app_core/format.h"
#include "core/json.h"

namespace net {

bool isNtfyUrl(const std::string& url) {
  const size_t s = url.find("://");
  if (s == std::string::npos) return false;
  const size_t hs = s + 3;
  const size_t he = url.find_first_of(":/", hs);
  const std::string host = url.substr(hs, he == std::string::npos ? std::string::npos : he - hs);
  return host == "ntfy.sh" || host.rfind("ntfy.", 0) == 0;
}

namespace {
struct PersonGroup {
  std::string label;
  std::vector<std::string> meds;
  int count = 0;
  int64_t first = 0;
};
}  // namespace

OutboundRequest buildMissedNotification(const std::string& url, bool names, const std::vector<MissedDose>& missed,
                                        const core::PosixTz& tz) {
  std::map<uint8_t, PersonGroup> by;
  for (const auto& m : missed) {
    PersonGroup& g = by[m.profileId];
    g.label = names ? m.person : m.initials;
    if (names) g.meds.push_back(m.med);
    if (g.count == 0 || m.scheduledUtc < g.first) g.first = m.scheduledUtc;
    g.count++;
  }
  OutboundRequest r;
  r.url = url;
  if (isNtfyUrl(url)) {
    std::string text;
    for (const auto& kv : by) {
      const PersonGroup& g = kv.second;
      if (!text.empty()) text += "\n";
      text += app::fmt("%s: %d dose%s missed (due %s)", g.label.c_str(), g.count, g.count == 1 ? "" : "s",
                       app::fmtHm(core::utcToLocal(tz, g.first).secOfDay / 60).c_str());
      if (names && !g.meds.empty()) {
        text += " - ";
        for (size_t i = 0; i < g.meds.size(); i++) text += (i ? ", " : "") + g.meds[i];
      }
    }
    r.contentType = "text/plain; charset=utf-8";
    r.body = text;
    r.headers.push_back({"Title", "RxNudge: missed dose"});
    r.headers.push_back({"Priority", "high"});
    r.headers.push_back({"Tags", "pill"});
    return r;
  }
  core::JsonValue root = core::JsonValue::object();
  root.set("event", core::JsonValue::string("missed_dose"));
  root.set("source", core::JsonValue::string("rxnudge"));
  core::JsonValue people = core::JsonValue::array();
  int total = 0;
  for (const auto& kv : by) {
    const PersonGroup& g = kv.second;
    core::JsonValue p = core::JsonValue::object();
    p.set(names ? "name" : "initials", core::JsonValue::string(g.label));
    p.set("missed", core::JsonValue::integer(g.count));
    p.set("firstDue", core::JsonValue::string(app::fmtHm(core::utcToLocal(tz, g.first).secOfDay / 60)));
    if (names) {
      core::JsonValue meds = core::JsonValue::array();
      for (const auto& m : g.meds) meds.push(core::JsonValue::string(m));
      p.set("medications", meds);
    }
    people.push(p);
    total += g.count;
  }
  root.set("total", core::JsonValue::integer(total));
  root.set("people", people);
  r.contentType = "application/json";
  r.body = root.dump();
  return r;
}

OutboundRequest buildTestNotification(const std::string& url) {
  OutboundRequest r;
  r.url = url;
  if (isNtfyUrl(url)) {
    r.contentType = "text/plain; charset=utf-8";
    r.body = "Test from RxNudge: caregiver alerts are working.";
    r.headers.push_back({"Title", "RxNudge test"});
    return r;
  }
  core::JsonValue root = core::JsonValue::object();
  root.set("event", core::JsonValue::string("test"));
  root.set("source", core::JsonValue::string("rxnudge"));
  r.contentType = "application/json";
  r.body = root.dump();
  return r;
}

}  // namespace net
