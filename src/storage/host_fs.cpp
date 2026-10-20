#ifndef ARDUINO
#include "storage/host_fs.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace storage {
namespace fsys = std::filesystem;

bool HostFs::exists(const std::string& path) {
  std::error_code ec;
  return fsys::exists(real(path), ec);
}

bool HostFs::readFile(const std::string& path, std::string& out) {
  std::ifstream f(real(path), std::ios::binary);
  if (!f) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

static bool writeMode(const std::string& p, const std::string& data, const char* mode) {
  FILE* f = std::fopen(p.c_str(), mode);
  if (!f) return false;
  const bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0 &&
                  fsync(fileno(f)) == 0;
  return std::fclose(f) == 0 && ok;
}

bool HostFs::writeFile(const std::string& path, const std::string& data) { return writeMode(real(path), data, "wb"); }
bool HostFs::appendFile(const std::string& path, const std::string& data) { return writeMode(real(path), data, "ab"); }

bool HostFs::remove(const std::string& path) {
  std::error_code ec;
  return fsys::remove(real(path), ec);
}

bool HostFs::rename(const std::string& from, const std::string& to) {
  if (exists(to)) return false;
  return std::rename(real(from).c_str(), real(to).c_str()) == 0;
}

bool HostFs::mkdirs(const std::string& path) {
  std::error_code ec;
  fsys::create_directories(real(path), ec);
  return fsys::is_directory(real(path), ec);
}

std::vector<std::string> HostFs::list(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  for (const auto& e : fsys::directory_iterator(real(dir), ec))
    if (e.is_regular_file()) out.push_back(e.path().filename().string());
  return out;
}

}  // namespace storage
#endif
