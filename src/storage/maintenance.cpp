#include "storage/maintenance.h"

#include <algorithm>
#include <cstdio>

#include "storage/auto_backup.h"
#include "storage/log_store.h"

namespace storage {

std::string backupDir() { return "/RxNudge/backups"; }

std::string backupFileName(int year, int month, int day, int hour, int minute) {
  char buf[40];
  std::snprintf(buf, sizeof(buf), "backup-%04d%02d%02d-%02d%02d.json", year, month, day, hour, minute);
  return buf;
}

bool writeBackupFile(IFileSystem& sd, const std::string& name, const std::string& json) {
  if (!sd.mounted() || name.empty() || name.find('/') != std::string::npos) return false;
  const std::string dir = backupDir();
  if (!sd.mkdirs(dir)) return false;
  const std::string path = dir + "/" + name, tmp = path + ".tmp";
  sd.remove(tmp);
  if (!sd.writeFile(tmp, json)) return false;
  std::string check;
  if (!sd.readFile(tmp, check) || check != json) return false;  // verify before replacing
  if (sd.exists(path)) sd.remove(path);
  return sd.rename(tmp, path);
}

std::vector<std::string> listBackups(IFileSystem& sd) {
  std::vector<std::string> out;
  if (!sd.mounted()) return out;
  for (const std::string& n : sd.list(backupDir())) {
    const bool json = n.size() > 5 && n.compare(n.size() - 5, 5, ".json") == 0;
    if (json && (n.rfind("backup-", 0) == 0 || isAutoBackupName(n))) out.push_back(n);
  }
  // Newest first by the date after the prefix (manual "backup-" and nightly "auto-" files mixed).
  auto key = [](const std::string& n) { return n.substr(n.find('-') + 1); };
  std::sort(out.begin(), out.end(), [&](const std::string& a, const std::string& b) {
    const std::string ka = key(a), kb = key(b);
    return ka != kb ? ka > kb : a > b;
  });
  return out;
}

bool readBackupFile(IFileSystem& sd, const std::string& name, std::string& json) {
  if (!sd.mounted() || name.find('/') != std::string::npos) return false;
  return sd.readFile(backupDir() + "/" + name, json);
}

int wipeInternalStore(IFileSystem& fs, const std::string& root) {
  int failed = 0;
  const std::string logs = root + "/logs";
  for (const std::string& n : fs.list(logs))
    if (!fs.remove(logs + "/" + n)) failed++;
  for (const std::string& n : fs.list(root)) {
    if (n == "logs") continue;
    if (!fs.remove(root + "/" + n)) failed++;
  }
  return failed;
}

bool rewriteDoseLog(IFileSystem& fs, const std::vector<core::DoseLogEntry>& entries, const std::string& root) {
  const std::string logs = root + "/logs";
  for (const std::string& n : fs.list(logs)) fs.remove(logs + "/" + n);
  fs.mkdirs(logs);
  FsDoseLog log(fs, root);
  bool ok = true;
  for (const auto& e : entries) ok = log.append(e) && ok;
  return ok;
}

}  // namespace storage
