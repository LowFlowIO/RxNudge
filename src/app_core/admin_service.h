#pragma once
// Every change made from the editors, settings and the setup wizard goes
// through AdminService: it validates on a copy, swaps the copy into the live
// database under the database lock, regenerates future doses and saves.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "app_core/app_model.h"
#include "app_core/net_service.h"
#include "core/alarm.h"
#include "storage/auto_backup.h"
#include "storage/fs.h"
#include "storage/repository.h"
#include "ui_core/form.h"

namespace app {

class IDbLock {
 public:
  virtual ~IDbLock() = default;
  virtual void lock() = 0;
  virtual void unlock() = 0;
};

struct BatteryInfo {
  bool known = false;
  int pct = -1, mv = 0, ma = 0;
  bool usb = false, charging = false;
  std::string estimate;  // e.g. "about 9 days"
};

struct StorageInfo {
  bool internalOk = true;
  bool sdPresent = false;
  int backups = 0;
  int meds = 0, profiles = 0;
  size_t logEntries = 0;
  std::string health;  // "OK" or the last error
};

struct AdminHooks {
  std::function<bool(int64_t utc)> setClockUtc;          // RTC + system clock
  std::function<void()> afterFactoryReset;               // device: erase NVS and reboot
  std::function<void(bool)> setLargeText;
  std::function<bool()> largeText;
  std::function<void(const core::PersonAlert&)> testAlert;
  std::function<BatteryInfo()> battery;
  std::function<uint64_t()> entropy;                     // hardware RNG on the device
  std::function<void(const core::Settings&)> settingsChanged;  // refresh policy etc.
  std::function<storage::AutoBackupState()> autoBackupState;  // device: NVS copy kept by the storage task
};

class AdminService : public ui::IPinGate {
 public:
  AdminService(AppModel& model, storage::IRepository* repo, storage::IFileSystem* internal, storage::IFileSystem* sd,
               INetService& net, IDbLock* lock, AdminHooks hooks, std::string version);

  core::Database database() const;  // copy, taken under the lock
  AppModel& model() { return m_; }
  INetService& net() { return net_; }
  AdminHooks& hooks() { return h_; }
  const std::string& version() const { return version_; }
  int32_t today() const;
  core::LocalTime localNow() const;
  bool clockUnset() const;  // the RTC value is not valid (forced clock screen / wizard)
  core::PosixTz timezone() const;
  int64_t nowUtc() const;

  // Edits. Return "" on success, otherwise the reason (nothing changed).
  std::string saveMedication(core::Medication& m);  // assigns an id when m.id == 0
  std::string setMedicationActive(uint16_t id, bool active);
  std::string saveProfile(core::Profile& p);        // assigns an id when p.id == 0 (max 8)
  std::string deleteProfile(uint8_t id);            // only a person without medications; never the last one
  std::string deleteMedication(uint16_t id);        // archive first; history entries stay
  std::vector<core::DoseLogEntry> recentLog() const;
  std::string saveSettings(const core::Settings& s);
  std::string setTimezone(const std::string& name, const std::string& posix);
  std::string setClockLocal(int year, int month, int day, int hour, int minute);
  core::Medication newMedication(uint8_t profileId) const;  // defaults from settings

  // PINs ("" clears).
  bool adminPinSet() const;
  std::string setAdminPin(const std::string& pin);
  std::string setProfilePin(uint8_t profileId, const std::string& pin);
  uint64_t entropy();

  // ui::IPinGate
  bool pinRequired(ui::PinScope scope, uint8_t profileId) override;
  bool checkPin(ui::PinScope scope, uint8_t profileId, const std::string& pin) override;

  // SD backup / restore.
  bool sdAvailable() const;
  std::vector<std::string> backups() const;
  storage::AutoBackupState autoBackupState() const;  // from the device hook; default (Never) on the host
  std::string backupNow(std::string* nameOut = nullptr);
  std::string restore(const std::string& name);
  std::string backupJson() const;                    // same content as an SD backup
  std::string restoreJson(const std::string& json);  // validates fully before anything changes

  std::string factoryReset();
  StorageInfo storageInfo() const;
  BatteryInfo batteryInfo() const;

  bool setupNeeded() const;
  std::string markSetupDone();

 private:
  std::string commit(core::Database& next, bool tzChanged);

  AppModel& m_;
  storage::IRepository* repo_;
  storage::IFileSystem* internal_;
  storage::IFileSystem* sd_;
  INetService& net_;
  IDbLock* lock_;
  AdminHooks h_;
  std::string version_;
  uint64_t rngState_ = 0x9E3779B97F4A7C15ull;
};

}  // namespace app
