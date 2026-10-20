#pragma once
// In-memory file system for tests and as a RAM fallback. Supports fault injection.
#include <map>

#include "storage/fs.h"

namespace storage {

class MemFs : public IFileSystem {
 public:
  bool mounted() const override { return true; }
  bool exists(const std::string& path) override;
  bool readFile(const std::string& path, std::string& out) override;
  bool writeFile(const std::string& path, const std::string& data) override;
  bool appendFile(const std::string& path, const std::string& data) override;
  bool remove(const std::string& path) override;
  bool rename(const std::string& from, const std::string& to) override;
  bool mkdirs(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;

  // Fault injection
  void flipByte(const std::string& path, size_t offset);  // XOR 0xFF
  void truncate(const std::string& path, size_t len);
  int failRenameAfter = -1;  // >= 0: that many renames succeed, then all fail ("power loss")
  int failWriteAfter = -1;   // >= 0: that many writes succeed, then writes store half the data and fail
  std::map<std::string, std::string> files;
};

}  // namespace storage
