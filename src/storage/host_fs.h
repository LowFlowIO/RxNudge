#pragma once
// Host (Linux/macOS) file system rooted at a directory. Not built for the device.
#ifndef ARDUINO
#include "storage/fs.h"

namespace storage {

class HostFs : public IFileSystem {
 public:
  explicit HostFs(std::string rootDir) : root_(std::move(rootDir)) {}
  bool mounted() const override { return true; }
  bool exists(const std::string& path) override;
  bool readFile(const std::string& path, std::string& out) override;
  bool writeFile(const std::string& path, const std::string& data) override;
  bool appendFile(const std::string& path, const std::string& data) override;
  bool remove(const std::string& path) override;
  bool rename(const std::string& from, const std::string& to) override;
  bool mkdirs(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;

 private:
  std::string real(const std::string& p) const { return root_ + p; }
  std::string root_;
};

}  // namespace storage
#endif
