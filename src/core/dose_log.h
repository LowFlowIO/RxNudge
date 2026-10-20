#pragma once
// Append-only dose log records. Corrections are new entries; nothing is rewritten.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace core {

enum class DoseAction : uint8_t { Taken = 1, TakenLate, Skipped, Snoozed, Missed, Undo, PrnTaken };
enum class ActionSource : uint8_t { Button = 1, Touch, Backfill, System, Import };

const char* doseActionName(DoseAction a);
const char* actionSourceName(ActionSource s);
bool parseDoseAction(const std::string& s, DoseAction& out);
bool parseActionSource(const std::string& s, ActionSource& out);

struct DoseLogEntry {
  uint64_t eventId = 0;
  uint8_t profileId = 0;
  uint16_t medId = 0;
  int64_t scheduledUtc = 0;  // PRN: equals actionUtc
  DoseAction action = DoseAction::Taken;
  int64_t actionUtc = 0;    // when it happened (for backfill: the taken-at time)
  int64_t recordedUtc = 0;  // when it was written
  ActionSource source = ActionSource::System;
  std::string note;  // short system note; never a medication name
};

class IDoseLogSink {
 public:
  virtual ~IDoseLogSink() = default;
  virtual bool append(const DoseLogEntry& e) = 0;
};

// Collects entries in memory (tests, and as a staging buffer on device).
class MemoryDoseLog : public IDoseLogSink {
 public:
  bool append(const DoseLogEntry& e) override {
    entries.push_back(e);
    return true;
  }
  std::vector<DoseLogEntry> entries;
};

struct ResolvedDose {
  uint64_t eventId = 0;
  uint8_t profileId = 0;
  uint16_t medId = 0;
  int64_t scheduledUtc = 0;
  DoseAction action = DoseAction::Missed;  // final effective action
  int64_t actionUtc = 0;
  bool resolved = false;  // false if every action was undone or only snoozes remain
};

// Replays the log (in append order) per event id: Undo pops the last user action,
// Snoozed is not an outcome, later outcomes override earlier ones (corrections).
std::map<uint64_t, ResolvedDose> resolveLog(const std::vector<DoseLogEntry>& entries);

}  // namespace core
