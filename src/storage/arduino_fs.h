#pragma once
// Device file-system adapters (Arduino builds only).
#ifdef ARDUINO
#include "storage/fs.h"

namespace storage {

// Internal flash: LittleFS on the "spiffs" data partition (0xC90000, 3.4 MB).
class LittleFsAdapter : public IFileSystem {
 public:
  bool begin();  // formats the partition if it has never been formatted
  bool mounted() const override { return mounted_; }
  bool exists(const std::string& path) override;
  bool readFile(const std::string& path, std::string& out) override;
  bool writeFile(const std::string& path, const std::string& data) override;
  bool appendFile(const std::string& path, const std::string& data) override;
  bool remove(const std::string& path) override;
  bool rename(const std::string& from, const std::string& to) override;
  bool mkdirs(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;

 private:
  bool write(const std::string& path, const std::string& data, const char* mode);
  bool mounted_ = false;
};

// microSD through the vendored SDCardManager (SdFat, shared SPI bus).
class SdAdapter : public IFileSystem {
 public:
  bool begin();
  bool mounted() const override { return mounted_; }
  bool exists(const std::string& path) override;
  bool readFile(const std::string& path, std::string& out) override;
  bool writeFile(const std::string& path, const std::string& data) override;
  bool appendFile(const std::string& path, const std::string& data) override;
  bool remove(const std::string& path) override;
  bool rename(const std::string& from, const std::string& to) override;
  bool mkdirs(const std::string& path) override;
  std::vector<std::string> list(const std::string& dir) override;

 private:
  bool write(const std::string& path, const std::string& data, bool append);
  bool mounted_ = false;
};

}  // namespace storage
#endif
