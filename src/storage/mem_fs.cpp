#include "storage/mem_fs.h"

namespace storage {

bool MemFs::exists(const std::string& path) { return files.count(path) != 0; }

bool MemFs::readFile(const std::string& path, std::string& out) {
  auto it = files.find(path);
  if (it == files.end()) return false;
  out = it->second;
  return true;
}

bool MemFs::writeFile(const std::string& path, const std::string& data) {
  if (failWriteAfter == 0) {
    files[path] = data.substr(0, data.size() / 2);
    return false;
  }
  if (failWriteAfter > 0) failWriteAfter--;
  files[path] = data;
  return true;
}

bool MemFs::appendFile(const std::string& path, const std::string& data) {
  if (failWriteAfter == 0) {
    files[path] += data.substr(0, data.size() / 2);
    return false;
  }
  if (failWriteAfter > 0) failWriteAfter--;
  files[path] += data;
  return true;
}

bool MemFs::remove(const std::string& path) { return files.erase(path) != 0; }

bool MemFs::rename(const std::string& from, const std::string& to) {
  if (failRenameAfter == 0) return false;
  if (failRenameAfter > 0) failRenameAfter--;
  auto it = files.find(from);
  if (it == files.end() || files.count(to)) return false;
  files[to] = it->second;
  files.erase(from);
  return true;
}

bool MemFs::mkdirs(const std::string&) { return true; }

std::vector<std::string> MemFs::list(const std::string& dir) {
  std::vector<std::string> out;
  const std::string prefix = dir.back() == '/' ? dir : dir + "/";
  for (const auto& kv : files) {
    if (kv.first.compare(0, prefix.size(), prefix) != 0) continue;
    const std::string rest = kv.first.substr(prefix.size());
    if (!rest.empty() && rest.find('/') == std::string::npos) out.push_back(rest);
  }
  return out;
}

void MemFs::flipByte(const std::string& path, size_t offset) {
  auto it = files.find(path);
  if (it != files.end() && offset < it->second.size()) it->second[offset] = static_cast<char>(it->second[offset] ^ 0xFF);
}

void MemFs::truncate(const std::string& path, size_t len) {
  auto it = files.find(path);
  if (it != files.end() && len < it->second.size()) it->second.resize(len);
}

}  // namespace storage
