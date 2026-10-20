#pragma once
// File-system abstraction used by all storage code. Paths are absolute, '/'-separated.
#include <cstdint>
#include <string>
#include <vector>

namespace storage {

class IFileSystem {
 public:
  virtual ~IFileSystem() = default;
  virtual bool mounted() const = 0;
  virtual bool exists(const std::string& path) = 0;
  virtual bool readFile(const std::string& path, std::string& out) = 0;
  // Create/truncate, write everything, flush to media, close. Not atomic by itself.
  virtual bool writeFile(const std::string& path, const std::string& data) = 0;
  // Append, flush, close.
  virtual bool appendFile(const std::string& path, const std::string& data) = 0;
  virtual bool remove(const std::string& path) = 0;
  // Contract: fails if `to` exists (callers remove it first) -- matches SdFat; LittleFS adapters enforce it.
  virtual bool rename(const std::string& from, const std::string& to) = 0;
  virtual bool mkdirs(const std::string& path) = 0;
  // File names (not paths) directly inside `dir`.
  virtual std::vector<std::string> list(const std::string& dir) = 0;
};

std::string parentDir(const std::string& path);
std::string joinPath(const std::string& a, const std::string& b);

}  // namespace storage
