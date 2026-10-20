#include "core/dose_log.h"

#include <iterator>

namespace core {

static const char* const kActionNames[] = {"", "taken", "taken_late", "skipped", "snoozed", "missed", "undo", "prn_taken"};
static const char* const kSourceNames[] = {"", "button", "touch", "backfill", "system", "import"};

const char* doseActionName(DoseAction a) {
  const unsigned i = static_cast<unsigned>(a);
  return i < 8 ? kActionNames[i] : "?";
}
const char* actionSourceName(ActionSource s) {
  const unsigned i = static_cast<unsigned>(s);
  return i < 6 ? kSourceNames[i] : "?";
}
bool parseDoseAction(const std::string& s, DoseAction& out) {
  for (unsigned i = 1; i < 8; i++)
    if (s == kActionNames[i]) {
      out = static_cast<DoseAction>(i);
      return true;
    }
  return false;
}
bool parseActionSource(const std::string& s, ActionSource& out) {
  for (unsigned i = 1; i < 6; i++)
    if (s == kSourceNames[i]) {
      out = static_cast<ActionSource>(i);
      return true;
    }
  return false;
}

std::map<uint64_t, ResolvedDose> resolveLog(const std::vector<DoseLogEntry>& entries) {
  std::map<uint64_t, std::vector<const DoseLogEntry*>> stacks;
  for (const auto& e : entries) {
    auto& st = stacks[e.eventId];
    if (e.action == DoseAction::Undo) {
      // pop the most recent user action (Missed is system-generated and not undoable)
      for (auto it = st.rbegin(); it != st.rend(); ++it) {
        if ((*it)->action != DoseAction::Missed) {
          st.erase(std::next(it).base());
          break;
        }
      }
    } else {
      st.push_back(&e);
    }
  }
  std::map<uint64_t, ResolvedDose> out;
  for (const auto& kv : stacks) {
    ResolvedDose r;
    r.eventId = kv.first;
    for (const DoseLogEntry* e : kv.second) {
      r.profileId = e->profileId;
      r.medId = e->medId;
      r.scheduledUtc = e->scheduledUtc;
      if (e->action == DoseAction::Snoozed) continue;
      r.action = e->action;
      r.actionUtc = e->actionUtc;
      r.resolved = true;
    }
    if (!kv.second.empty()) {
      r.profileId = kv.second.back()->profileId;
      r.medId = kv.second.back()->medId;
      r.scheduledUtc = kv.second.back()->scheduledUtc;
    }
    if (kv.second.empty()) continue;
    out[kv.first] = r;
  }
  return out;
}

}  // namespace core
