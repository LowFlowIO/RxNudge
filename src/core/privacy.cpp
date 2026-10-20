#include "core/privacy.h"

namespace core {

std::string medLogLabel(const Medication& m, bool debugNames) {
  std::string s = "med#" + std::to_string(m.id);
  if (debugNames) s += "(" + m.name + ")";
  return s;
}

std::string redactMedNames(const std::string& text, const Database& db, bool debugNames) {
  if (debugNames) return text;
  std::string out = text;
  for (const auto& m : db.meds) {
    if (m.name.empty()) continue;
    const std::string label = "med#" + std::to_string(m.id);
    size_t pos = 0;
    while ((pos = out.find(m.name, pos)) != std::string::npos) {
      out.replace(pos, m.name.size(), label);
      pos += label.size();
    }
  }
  return out;
}

}  // namespace core
