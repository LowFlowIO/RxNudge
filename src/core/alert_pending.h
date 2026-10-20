#pragma once
// Pending-alert set between the engine (producer) and the alert task (consumer).
// One slot per person (profiles 1..8) plus the system slot 0, so it can never
// overflow: a new alert for a person who already has one pending is merged into it
// (coalesced) instead of being dropped. Not thread-safe by itself; the firmware
// guards it with a mutex.
#include <cstdint>

#include "core/alarm.h"
#include "core/model.h"

namespace core {

class AlertPending {
 public:
  static constexpr int kSlots = static_cast<int>(kMaxProfiles) + 1;
  // Returns true if merged into an existing entry (coalesced), false if a new slot was used.
  bool push(const PersonAlert& a);
  // Next alert by priority (critical first, then earliest scheduled, then profile id).
  bool pop(PersonAlert& out);
  int size() const;
  bool empty() const { return size() == 0; }
  uint32_t coalesced() const { return coalesced_; }
  void clear();

 private:
  PersonAlert slot_[kSlots];
  bool used_[kSlots] = {};
  uint32_t coalesced_ = 0;
};

// Merge rule used by push(): the stronger of both wins for every field.
PersonAlert mergeAlerts(const PersonAlert& a, const PersonAlert& b);

}  // namespace core
