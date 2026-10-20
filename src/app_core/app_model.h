#pragma once
// Application model: owns the dose engine for one Database, tees the dose log
// into a RAM window for history/today views, builds view models and executes
// user commands. Pure C++ (host-testable). On the device the engine task is
// the only caller of execute()/process(); the UI only reads view models while
// holding the database mutex.
#include <cstdint>
#include <vector>

#include "app_core/view_models.h"
#include "core/clock.h"
#include "core/dose_engine.h"
#include "core/dose_log.h"
#include "core/model.h"

namespace app {

constexpr int kMaxGroup = 16;  // doses confirmed together (one person, one time)

enum class CommandType : uint8_t { Take, Skip, Snooze, Undo, TakePrn };

struct DoseCommand {
  CommandType type = CommandType::Take;
  uint8_t count = 0;
  uint64_t ids[kMaxGroup] = {};
  int minutes = 0;                // snooze
  int64_t takenAt = core::kNow;   // backfill time (Take)
  uint16_t medId = 0;             // TakePrn
  core::ActionSource source = core::ActionSource::Touch;
  bool add(uint64_t id) {
    if (count >= kMaxGroup) return false;
    ids[count++] = id;
    return true;
  }
};

struct CommandResult {
  core::ActionResult result = core::ActionResult::Ok;
  int applied = 0;
  uint64_t prnEventId = 0;
};

class AppModel : public core::IDoseLogSink {
 public:
  static constexpr size_t kRecentLogCap = 6000;

  AppModel(const core::IClock& clock, core::Database& db, core::IDoseLogSink* persist);

  // IDoseLogSink: keeps a RAM copy and forwards to persistent storage.
  bool append(const core::DoseLogEntry& e) override;

  core::DoseEngine& engine() { return engine_; }
  const core::DoseEngine& engine() const { return engine_; }
  const core::Database& db() const { return db_; }
  core::Database& db() { return db_; }
  const core::PosixTz& tz() const { return engine_.timezone(); }
  int64_t now() const { return clock_.nowUtc(); }
  int32_t today() const;

  void setRecentLog(const std::vector<core::DoseLogEntry>& entries);  // e.g. loaded from storage at boot
  const std::vector<core::DoseLogEntry>& recentLog() const { return recent_; }

  // Engine lifecycle: schedules only run once the clock is usable.
  void startEngine(int64_t nowUtc);
  void restoreEngine(const core::EngineSnapshot& s);
  bool engineRunning() const { return running_; }
  std::vector<core::EngineEvent> process();

  StatusFlags status;

  HomeVM home(int page, int lanesPerPage) const;
  TodayVM today(uint8_t profileId) const;
  DoseDetailVM detail(uint64_t eventId) const;
  AlarmVM alarm() const;
  PrnVM prn(uint8_t profileId) const;
  HistoryVM history(uint8_t profileId, HistoryMode mode, int32_t anchorDay, bool perMed) const;

  // Group actions must all belong to one person; mixed groups are refused before anything changes.
  CommandResult execute(const DoseCommand& c);
  // The last group action (for the 60 s undo toast).
  const DoseCommand& lastAction() const { return last_; }
  bool lastActionUndoable() const;

 private:
  std::string lastTakenText(uint16_t medId) const;
  int64_t lastTakenUtc(uint16_t medId) const;

  const core::IClock& clock_;
  core::Database& db_;
  core::IDoseLogSink* persist_;
  core::DoseEngine engine_;
  std::vector<core::DoseLogEntry> recent_;
  bool running_ = false;
  DoseCommand last_;
  int64_t lastAt_ = 0;
};

}  // namespace app
