#include "storage/fs.h"

namespace storage {

std::string parentDir(const std::string& path) {
  const size_t p = path.find_last_of('/');
  if (p == std::string::npos || p == 0) return "/";
  return path.substr(0, p);
}

std::string joinPath(const std::string& a, const std::string& b) {
  if (a.empty() || a == "/") return "/" + (b.size() && b[0] == '/' ? b.substr(1) : b);
  if (a.back() == '/') return a + (b.size() && b[0] == '/' ? b.substr(1) : b);
  return a + (b.size() && b[0] == '/' ? b : "/" + b);
}

}  // namespace storage
