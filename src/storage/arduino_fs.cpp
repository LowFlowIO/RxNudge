#ifdef ARDUINO
#include "storage/arduino_fs.h"

#include <LittleFS.h>
#include <SDCardManager.h>

namespace storage {

// ---------------- LittleFS ----------------
bool LittleFsAdapter::begin() {
  mounted_ = LittleFS.begin(true, "/littlefs", 10, "spiffs");
  return mounted_;
}

bool LittleFsAdapter::exists(const std::string& path) { return mounted_ && LittleFS.exists(path.c_str()); }

bool LittleFsAdapter::readFile(const std::string& path, std::string& out) {
  if (!mounted_ || !LittleFS.exists(path.c_str())) return false;
  File f = LittleFS.open(path.c_str(), "r");
  if (!f || f.isDirectory()) return false;
  out.clear();
  out.reserve(f.size());
  char buf[256];
  while (f.available()) {
    const size_t n = f.read(reinterpret_cast<uint8_t*>(buf), sizeof(buf));
    if (n == 0) break;
    out.append(buf, n);
  }
  const bool ok = out.size() == f.size();
  f.close();
  return ok;
}

bool LittleFsAdapter::write(const std::string& path, const std::string& data, const char* mode) {
  if (!mounted_) return false;
  File f = LittleFS.open(path.c_str(), mode, true);
  if (!f) return false;
  const bool ok = f.write(reinterpret_cast<const uint8_t*>(data.data()), data.size()) == data.size();
  f.flush();
  f.close();  // LittleFS commits metadata on close
  return ok;
}

bool LittleFsAdapter::writeFile(const std::string& path, const std::string& data) { return write(path, data, "w"); }
bool LittleFsAdapter::appendFile(const std::string& path, const std::string& data) { return write(path, data, "a"); }
bool LittleFsAdapter::remove(const std::string& path) { return mounted_ && LittleFS.remove(path.c_str()); }

bool LittleFsAdapter::rename(const std::string& from, const std::string& to) {
  if (!mounted_ || LittleFS.exists(to.c_str())) return false;  // enforce the IFileSystem contract
  return LittleFS.rename(from.c_str(), to.c_str());
}

bool LittleFsAdapter::mkdirs(const std::string& path) {
  if (!mounted_) return false;
  size_t pos = 1;
  while (pos <= path.size()) {
    size_t next = path.find('/', pos);
    if (next == std::string::npos) next = path.size();
    const std::string part = path.substr(0, next);
    if (!LittleFS.exists(part.c_str()) && !LittleFS.mkdir(part.c_str())) return false;
    pos = next + 1;
  }
  return true;
}

std::vector<std::string> LittleFsAdapter::list(const std::string& dir) {
  std::vector<std::string> out;
  if (!mounted_) return out;
  File d = LittleFS.open(dir.c_str());
  if (!d || !d.isDirectory()) return out;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    if (!f.isDirectory()) {
      std::string n = f.name();
      const size_t s = n.find_last_of('/');
      out.push_back(s == std::string::npos ? n : n.substr(s + 1));
    }
    f.close();
  }
  d.close();
  return out;
}

// ---------------- SD ----------------
bool SdAdapter::begin() {
  mounted_ = SdMan.begin();
  return mounted_;
}

bool SdAdapter::exists(const std::string& path) { return mounted_ && SdMan.exists(path.c_str()); }

bool SdAdapter::readFile(const std::string& path, std::string& out) {
  if (!mounted_) return false;
  FsFile f = SdMan.open(path.c_str(), O_RDONLY);
  if (!f) return false;
  const uint64_t size = f.fileSize();
  out.clear();
  out.reserve(static_cast<size_t>(size));
  char buf[512];
  for (;;) {
    const int n = f.read(buf, sizeof(buf));
    if (n <= 0) break;
    out.append(buf, static_cast<size_t>(n));
  }
  f.close();
  return out.size() == size;
}

bool SdAdapter::write(const std::string& path, const std::string& data, bool append) {
  if (!mounted_) return false;
  const oflag_t flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
  FsFile f = SdMan.open(path.c_str(), flags);
  if (!f) return false;
  const bool ok = f.write(data.data(), data.size()) == data.size();
  const bool synced = f.sync();
  f.close();
  return ok && synced;
}

bool SdAdapter::writeFile(const std::string& path, const std::string& data) { return write(path, data, false); }
bool SdAdapter::appendFile(const std::string& path, const std::string& data) { return write(path, data, true); }
bool SdAdapter::remove(const std::string& path) { return mounted_ && SdMan.remove(path.c_str()); }
bool SdAdapter::rename(const std::string& from, const std::string& to) {
  if (!mounted_ || SdMan.exists(to.c_str())) return false;
  return SdMan.rename(from.c_str(), to.c_str());
}
bool SdAdapter::mkdirs(const std::string& path) { return mounted_ && (SdMan.exists(path.c_str()) || SdMan.mkdir(path.c_str(), true)); }

std::vector<std::string> SdAdapter::list(const std::string& dir) {
  std::vector<std::string> out;
  if (!mounted_) return out;
  FsFile d = SdMan.open(dir.c_str(), O_RDONLY);
  if (!d || !d.isDir()) return out;
  FsFile f;
  char name[96];
  while (f.openNext(&d, O_RDONLY)) {
    if (!f.isDir() && f.getName(name, sizeof(name)) > 0) out.push_back(name);
    f.close();
  }
  d.close();
  return out;
}

}  // namespace storage
#endif
