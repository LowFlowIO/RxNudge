#pragma once
// What the UI needs from the application. The host implementation calls the
// AppModel directly; the device implementation adds locking and posts
// commands to the engine task.
#include "app_core/app_model.h"
#include "core/tz.h"

namespace app {

class IAppPort {
 public:
  virtual ~IAppPort() = default;
  virtual HomeVM home(int page, int lanesPerPage) = 0;
  virtual TodayVM today(uint8_t profileId) = 0;
  virtual DoseDetailVM detail(uint64_t eventId) = 0;
  virtual AlarmVM alarm() = 0;
  virtual PrnVM prn(uint8_t profileId) = 0;
  virtual HistoryVM history(uint8_t profileId, HistoryMode mode, int32_t anchorDay, bool perMed) = 0;
  virtual CommandResult execute(const DoseCommand& c) = 0;
  virtual bool lastActionUndoable() = 0;
  virtual DoseCommand lastAction() = 0;
  virtual StatusFlags status() = 0;
  virtual core::LocalTime localNow() = 0;
  virtual int64_t nowUtc() = 0;
  virtual bool clockUnset() = 0;
  // Local wall-clock date/time entered by the user. Returns false if invalid.
  virtual bool setClockLocal(int year, int month, int day, int hour, int minute) = 0;
};

// Direct port over an AppModel (host tests, previews; the device wraps it with locks).
class ModelPort : public IAppPort {
 public:
  using ClockSetter = bool (*)(void* ctx, int64_t utc);
  ModelPort(AppModel& m, ClockSetter setter = nullptr, void* ctx = nullptr) : m_(m), setter_(setter), ctx_(ctx) {}
  HomeVM home(int page, int lpp) override { return m_.home(page, lpp); }
  TodayVM today(uint8_t p) override { return m_.today(p); }
  DoseDetailVM detail(uint64_t id) override { return m_.detail(id); }
  AlarmVM alarm() override { return m_.alarm(); }
  PrnVM prn(uint8_t p) override { return m_.prn(p); }
  HistoryVM history(uint8_t p, HistoryMode mo, int32_t a, bool pm) override { return m_.history(p, mo, a, pm); }
  CommandResult execute(const DoseCommand& c) override { return m_.execute(c); }
  bool lastActionUndoable() override { return m_.lastActionUndoable(); }
  DoseCommand lastAction() override { return m_.lastAction(); }
  StatusFlags status() override { return m_.status; }
  core::LocalTime localNow() override { return core::utcToLocal(m_.tz(), m_.now()); }
  int64_t nowUtc() override { return m_.now(); }
  bool clockUnset() override { return m_.status.clockUnset; }
  bool setClockLocal(int y, int mo, int d, int h, int mi) override;

 private:
  AppModel& m_;
  ClockSetter setter_;
  void* ctx_;
};

// Validates a local date/time and converts it with the model's time zone (gap -> first valid minute).
bool localToUtcChecked(const core::PosixTz& tz, int y, int mo, int d, int h, int mi, int64_t& utc);

}  // namespace app
