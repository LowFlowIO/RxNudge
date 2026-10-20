#include "app_core/admin_service.h"

#include <utility>

#include "core/presets.h"
#include "core/tz.h"
#include "storage/export.h"
#include "storage/maintenance.h"

namespace app {

namespace {
struct Guard {
  IDbLock* l;
  explicit Guard(IDbLock* x) : l(x) {
    if (l) l->lock();
  }
  ~Guard() {
    if (l) l->unlock();
  }
};
}  // namespace

AdminService::AdminService(AppModel& model, storage::IRepository* repo, storage::IFileSystem* internal,
                           storage::IFileSystem* sd, INetService& net, IDbLock* lock, AdminHooks hooks,
                           std::string version)
    : m_(model), repo_(repo), internal_(internal), sd_(sd), net_(net), lock_(lock), h_(std::move(hooks)),
      version_(std::move(version)) {}

core::Database AdminService::database() const {
  Guard g(lock_);
  return m_.db();
}

int32_t AdminService::today() const {
  Guard g(lock_);
  return m_.today();
}

core::PosixTz AdminService::timezone() const {
  Guard g(lock_);
  return m_.tz();
}

int64_t AdminService::nowUtc() const { return m_.now(); }

bool AdminService::clockUnset() const {
  Guard g(lock_);
  return m_.status.clockUnset;
}

core::LocalTime AdminService::localNow() const {
  Guard g(lock_);
  return core::utcToLocal(m_.tz(), m_.now());
}

uint64_t AdminService::entropy() {
  // splitmix64 over the hardware entropy (when present) and an internal counter
  uint64_t x = (h_.entropy ? h_.entropy() : 0) ^ (rngState_ += 0x9E3779B97F4A7C15ull) ^
               static_cast<uint64_t>(m_.now());
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

std::string AdminService::commit(core::Database& next, bool tzChanged) {
  const std::string err = core::validateDatabase(next);
  if (!err.empty()) return err;
  {
    Guard g(lock_);
    m_.db() = next;
    if (tzChanged) m_.engine().setTimezone(next.settings.tz);
    if (m_.engineRunning()) m_.engine().regenerateFuture();
  }
  if (repo_ && !repo_->saveDatabase(next)) return "Saved in memory, but writing to flash failed";
  if (h_.settingsChanged) h_.settingsChanged(next.settings);
  return "";
}

core::Medication AdminService::newMedication(uint8_t profileId) const {
  core::Medication m;
  const core::Database db = database();
  m.profileId = profileId;
  m.startDay = today();
  m.ladderIntervalMin = db.settings.defaultLadderIntervalMin;
  m.ladderCount = db.settings.defaultLadderCount;
  core::applyScheduleTemplate(m, core::ScheduleTemplate::OnceMorning);
  return m;
}

std::string AdminService::saveMedication(core::Medication& med) {
  core::Database db = database();
  if (!db.profile(med.profileId)) return "Choose a person";
  core::Medication* cur = med.id ? db.med(med.id) : nullptr;
  {
    core::Medication probe = med;
    if (!cur) probe.id = core::nextMedicationId(db);
    if (probe.id == 0) return "No free medication id";
    const std::string v = core::validateMedication(probe);
    if (!v.empty()) return v;
  }
  if (cur) {
    if (cur->profileId != med.profileId && db.medCount(med.profileId) >= core::kMaxMedsPerProfile)
      return "That person already has 30 medications";
    *cur = med;
  } else {
    if (db.medCount(med.profileId) >= core::kMaxMedsPerProfile) return "That person already has 30 medications";
    med.id = core::nextMedicationId(db);
    if (med.id == 0) return "No free medication id";
    db.meds.push_back(med);
  }
  const std::string e = commit(db, false);
  if (!e.empty() && !cur && e.rfind("Saved in memory", 0) != 0) med.id = 0;
  return e;
}

std::string AdminService::setMedicationActive(uint16_t id, bool active) {
  core::Database db = database();
  core::Medication* m = db.med(id);
  if (!m) return "Medication not found";
  m->active = active;
  return commit(db, false);
}

std::string AdminService::saveProfile(core::Profile& p) {
  core::Database db = database();
  const bool isNew = p.id == 0 || !db.profile(p.id);
  if (isNew) {
    if (db.profiles.size() >= core::kMaxProfiles) return "8 people is the maximum";
    if (p.id == 0) p.id = core::nextProfileId(db);
    if (p.id == 0) return "8 people is the maximum";
  }
  const std::string v = core::validateProfile(p);
  if (!v.empty()) {
    if (isNew) p.id = 0;
    return v;
  }
  if (isNew) {
    db.profiles.push_back(p);
    if (!db.profile(db.settings.activeProfile)) db.settings.activeProfile = p.id;
  } else {
    *db.profile(p.id) = p;
  }
  return commit(db, false);
}

std::string AdminService::deleteProfile(uint8_t id) {
  core::Database db = database();
  if (!db.profile(id)) return "Person not found";
  if (db.medCount(id) > 0) return "Delete this person's medications first";
  if (db.profiles.size() <= 1) return "At least one person is needed";
  for (size_t i = 0; i < db.profiles.size(); ++i)
    if (db.profiles[i].id == id) {
      db.profiles.erase(db.profiles.begin() + static_cast<long>(i));
      break;
    }
  if (db.settings.activeProfile == id) db.settings.activeProfile = db.profiles.front().id;
  return commit(db, false);
}

std::string AdminService::deleteMedication(uint16_t id) {
  core::Database db = database();
  const core::Medication* m = db.med(id);
  if (!m) return "Medication not found";
  if (m->active) return "Archive the medication before deleting it";
  for (size_t i = 0; i < db.meds.size(); ++i)
    if (db.meds[i].id == id) {
      db.meds.erase(db.meds.begin() + static_cast<long>(i));
      break;
    }
  return commit(db, false);
}

std::vector<core::DoseLogEntry> AdminService::recentLog() const {
  Guard g(lock_);
  return m_.recentLog();
}

std::string AdminService::saveSettings(const core::Settings& s) {
  core::Database db = database();
  core::PosixTz tz;
  std::string err;
  if (!core::parsePosixTz(s.tz, tz, &err)) return "Time zone: " + err;
  if (s.refreshFullEvery < 5 || s.refreshFullEvery > 50) return "Full refresh must be 5 to 50";
  const bool tzChanged = s.tz != db.settings.tz;
  db.settings = s;
  return commit(db, tzChanged);
}

std::string AdminService::setTimezone(const std::string& name, const std::string& posix) {
  core::Settings s = database().settings;
  s.tz = posix;
  s.tzName = name;
  return saveSettings(s);
}

std::string AdminService::setClockLocal(int year, int month, int day, int hour, int minute) {
  if (year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 || day > core::daysInMonth(year, month) ||
      hour < 0 || hour > 23 || minute < 0 || minute > 59)
    return "Invalid date or time";
  if (!h_.setClockUtc) return "Clock cannot be set on this build";
  int64_t utc;
  {
    Guard g(lock_);
    utc = core::localToUtc(m_.tz(), core::daysFromCivil(year, month, day), hour * 3600 + minute * 60);
  }
  if (!h_.setClockUtc(utc)) return "Could not write the clock";
  return "";
}

bool AdminService::adminPinSet() const {
  Guard g(lock_);
  return !m_.db().settings.adminPinHash.empty();
}

std::string AdminService::setAdminPin(const std::string& pin) {
  core::Settings s = database().settings;
  if (pin.empty()) {
    s.adminPinSalt.clear();
    s.adminPinHash.clear();
  } else {
    if (!core::pinFormatValid(pin)) return "PIN must be 4 to 8 digits";
    core::setPin(s.adminPinSalt, s.adminPinHash, pin, entropy());
  }
  return saveSettings(s);
}

std::string AdminService::setProfilePin(uint8_t profileId, const std::string& pin) {
  core::Database db = database();
  core::Profile* p = db.profile(profileId);
  if (!p) return "Person not found";
  if (pin.empty()) {
    p->pinSalt.clear();
    p->pinHash.clear();
  } else {
    if (!core::pinFormatValid(pin)) return "PIN must be 4 to 8 digits";
    core::setPin(p->pinSalt, p->pinHash, pin, entropy());
  }
  return commit(db, false);
}

bool AdminService::pinRequired(ui::PinScope scope, uint8_t profileId) {
  Guard g(lock_);
  const core::Database& db = m_.db();
  if (scope == ui::PinScope::Admin) return !db.settings.adminPinHash.empty();
  if (scope == ui::PinScope::Profile) {
    const core::Profile* p = db.profile(profileId);
    return (p && !p->pinHash.empty()) || !db.settings.adminPinHash.empty();
  }
  return false;
}

bool AdminService::checkPin(ui::PinScope scope, uint8_t profileId, const std::string& pin) {
  Guard g(lock_);
  const core::Database& db = m_.db();
  // The admin PIN opens everything; a profile PIN opens that profile only.
  if (!db.settings.adminPinHash.empty() && core::verifyPin(db.settings.adminPinSalt, db.settings.adminPinHash, pin))
    return true;
  if (scope == ui::PinScope::Profile) {
    const core::Profile* p = db.profile(profileId);
    if (p && !p->pinHash.empty()) return core::verifyPin(p->pinSalt, p->pinHash, pin);
    if (p && db.settings.adminPinHash.empty()) return true;
  }
  return scope == ui::PinScope::None;
}

bool AdminService::sdAvailable() const { return sd_ && sd_->mounted(); }

std::vector<std::string> AdminService::backups() const {
  if (!sdAvailable()) return {};
  return storage::listBackups(*sd_);
}

storage::AutoBackupState AdminService::autoBackupState() const {
  return h_.autoBackupState ? h_.autoBackupState() : storage::AutoBackupState{};
}

std::string AdminService::backupNow(std::string* nameOut) {
  if (!sdAvailable()) return "No SD card";
  std::string json;
  core::LocalTime lt;
  {
    Guard g(lock_);
    json = storage::makeBackup(m_.db(), m_.recentLog());
    lt = core::utcToLocal(m_.tz(), m_.now());
  }
  const core::Ymd d = core::civilFromDays(lt.days);
  const std::string name = storage::backupFileName(d.y, d.m, d.d, lt.secOfDay / 3600, (lt.secOfDay / 60) % 60);
  if (!storage::writeBackupFile(*sd_, name, json)) return "Writing the backup failed";
  if (nameOut) *nameOut = name;
  return "";
}

std::string AdminService::backupJson() const {
  Guard g(lock_);
  return storage::makeBackup(m_.db(), m_.recentLog());
}

std::string AdminService::restore(const std::string& name) {
  if (!sdAvailable()) return "No SD card";
  std::string json;
  if (!storage::readBackupFile(*sd_, name, json)) return "Cannot read " + name;
  return restoreJson(json);
}

std::string AdminService::restoreJson(const std::string& json) {
  std::string err;
  core::Database db;
  std::vector<core::DoseLogEntry> log;
  if (!storage::restoreBackup(json, db, log, &err)) return "Backup rejected: " + err;
  db.settings.setupDone = true;  // Wi-Fi credentials are in NVS and never travel in backups
  const std::string v = core::validateDatabase(db);
  if (!v.empty()) return "Backup rejected: " + v;
  {
    Guard g(lock_);
    m_.db() = db;
    m_.engine().setTimezone(db.settings.tz);
    m_.setRecentLog(log);
    if (m_.engineRunning()) m_.startEngine(m_.now());
  }
  std::string res;
  if (repo_ && !repo_->saveDatabase(db)) res = "Restored, but writing to flash failed";
  if (internal_ && !storage::rewriteDoseLog(*internal_, log) && res.empty()) res = "Restored, but the history could not be written";
  if (h_.settingsChanged) h_.settingsChanged(db.settings);
  return res;
}

std::string AdminService::factoryReset() {
  core::Database fresh;
  {
    Guard g(lock_);
    m_.db() = fresh;
    m_.engine().setTimezone(fresh.settings.tz);
    m_.setRecentLog({});
    if (m_.engineRunning()) m_.startEngine(m_.now());
  }
  int left = internal_ ? storage::wipeInternalStore(*internal_) : 0;
  if (repo_ && !repo_->saveDatabase(fresh)) ++left;
  if (h_.settingsChanged) h_.settingsChanged(fresh.settings);
  if (h_.afterFactoryReset) h_.afterFactoryReset();
  return left ? "Some files could not be erased" : "";
}

StorageInfo AdminService::storageInfo() const {
  StorageInfo s;
  {
    Guard g(lock_);
    s.meds = static_cast<int>(m_.db().meds.size());
    s.profiles = static_cast<int>(m_.db().profiles.size());
    s.logEntries = m_.recentLog().size();
  }
  s.internalOk = internal_ ? internal_->mounted() : false;
  s.sdPresent = sdAvailable();
  s.backups = static_cast<int>(backups().size());
  if (repo_) s.health = repo_->health().healthy() ? "OK" : repo_->health().lastError;
  else s.health = "No storage";
  if (s.health.empty()) s.health = "Check storage";
  return s;
}

BatteryInfo AdminService::batteryInfo() const {
  if (h_.battery) return h_.battery();
  BatteryInfo b;
  Guard g(lock_);
  b.pct = m_.status.batteryPct;
  b.known = b.pct >= 0;
  b.usb = m_.status.usbPowered;
  b.charging = m_.status.charging;
  return b;
}

bool AdminService::setupNeeded() const {
  Guard g(lock_);
  return !m_.db().settings.setupDone;
}

std::string AdminService::markSetupDone() {
  core::Settings s = database().settings;
  s.setupDone = true;
  return saveSettings(s);
}

}  // namespace app
