#include "core/alert_pending.h"

#include <algorithm>

namespace core {

PersonAlert mergeAlerts(const PersonAlert& a, const PersonAlert& b) {
  PersonAlert m = a;
  m.earliestScheduledUtc = std::min(a.earliestScheduledUtc, b.earliestScheduledUtc);
  m.doseCount = std::max(a.doseCount, b.doseCount);
  m.alarmIndex = std::max(a.alarmIndex, b.alarmIndex);
  m.audible = a.audible || b.audible;
  m.critical = a.critical || b.critical;
  m.catchUp = a.catchUp && b.catchUp;  // a fresh alert makes the merged one fresh
  m.volume = std::max(a.volume, b.volume);
  if (b.critical && !a.critical) m.pattern = b.pattern;
  return m;
}

bool AlertPending::push(const PersonAlert& a) {
  const int i = a.profileId < kSlots ? a.profileId : 0;  // out-of-range ids share the system slot
  if (used_[i]) {
    slot_[i] = mergeAlerts(slot_[i], a);
    coalesced_++;
    return true;
  }
  slot_[i] = a;
  used_[i] = true;
  return false;
}

bool AlertPending::pop(PersonAlert& out) {
  int best = -1;
  for (int i = 0; i < kSlots; i++) {
    if (!used_[i]) continue;
    if (best < 0) {
      best = i;
      continue;
    }
    const PersonAlert& x = slot_[i];
    const PersonAlert& y = slot_[best];
    if (x.critical != y.critical ? x.critical : x.earliestScheduledUtc < y.earliestScheduledUtc) best = i;
  }
  if (best < 0) return false;
  out = slot_[best];
  used_[best] = false;
  return true;
}

int AlertPending::size() const {
  int n = 0;
  for (bool u : used_) n += u;
  return n;
}

void AlertPending::clear() {
  for (bool& u : used_) u = false;
}

}  // namespace core
