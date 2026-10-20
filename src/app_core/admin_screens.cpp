#include "app_core/admin_screens.h"

#include <algorithm>
#include <functional>

#include "app_core/format.h"
#include "core/presets.h"
#include "core/tz.h"
#include "ui_core/strings.h"
#include "ui_core/value_entry.h"

namespace app {

using ui::FieldKind;
using ui::FooterButton;
using ui::FormField;
using ui::FormProvider;
using ui::FormResult;
using ui::KeyboardRequest;
using ui::PinScope;

namespace {

// Settings > Backup: when the last automatic backup was written ("Sun 18 Oct 03:00").
std::string autoBackupWhen(const AdminService& s, const storage::AutoBackupState& ab) {
  if (ab.lastBackupUtc <= 0) return ui::tr(ui::A::AbNever);
  const core::LocalTime lt = core::utcToLocal(s.timezone(), ab.lastBackupUtc);
  return fmtShortDate(lt.days) + " " + fmtHm(lt.secOfDay / 60);
}

const char* autoBackupStatusText(storage::AutoBackupStatus st) {
  switch (st) {
    case storage::AutoBackupStatus::Ok: return ui::tr(ui::A::AbOk);
    case storage::AutoBackupStatus::Unchanged: return ui::tr(ui::A::AbUnchanged);
    case storage::AutoBackupStatus::NoCard: return ui::tr(ui::A::AbNoCard);
    case storage::AutoBackupStatus::Failed: return ui::tr(ui::A::AbFailed);
    case storage::AutoBackupStatus::SkippedBattery: return ui::tr(ui::A::AbBattery);
    case storage::AutoBackupStatus::Never: break;
  }
  return ui::tr(ui::A::AbWaiting);
}

// ---------- field builders ----------
// Tracks one background network job started from a page (never blocks the UI).
class JobWatch {
 public:
  std::string start(INetService& n, NetJob kind) {
    seq_ = n.jobState().seq;
    const std::string e = n.startJob(kind);
    if (e.empty()) {
      kind_ = kind;
      lastElapsed_ = -1;
    }
    return e;
  }
  bool busy() const { return kind_ != NetJob::None; }
  bool busy(NetJob k) const { return kind_ == k; }
  int elapsed() const { return lastElapsed_ < 0 ? 0 : lastElapsed_; }
  // 0 = nothing new, 1 = progress (redraw), 2 = finished (st holds the result)
  int poll(INetService& n, NetJobState& st) {
    if (kind_ == NetJob::None) return 0;
    st = n.jobState();
    if (!st.running && st.seq != seq_) {
      kind_ = NetJob::None;
      return 2;
    }
    if (st.elapsedSec != lastElapsed_) {
      lastElapsed_ = st.elapsedSec;
      return 1;
    }
    return 0;
  }

 private:
  NetJob kind_ = NetJob::None;
  uint32_t seq_ = 0;
  int lastElapsed_ = -1;
};

FormField info(int id, const std::string& label, const std::string& value = "") {
  FormField f;
  f.id = id;
  f.kind = FieldKind::Info;
  f.label = label;
  f.value = value;
  return f;
}
FormField text(int id, const std::string& label, const std::string& t, size_t maxBytes = core::kMaxNameBytes) {
  FormField f;
  f.id = id;
  f.kind = FieldKind::Text;
  f.label = label;
  f.text = t;
  f.maxBytes = maxBytes;
  return f;
}
FormField number(int id, const std::string& label, int v, int mn, int mx, int step, const std::string& unit = "") {
  FormField f;
  f.id = id;
  f.kind = FieldKind::Number;
  f.label = label;
  f.num = v;
  f.min = mn;
  f.max = mx;
  f.step = step;
  f.unit = unit;
  return f;
}
FormField timeF(int id, const std::string& label, int minute) {
  FormField f;
  f.id = id;
  f.kind = FieldKind::Time;
  f.label = label;
  f.num = minute;
  f.min = 0;
  f.max = 1439;
  f.step = 5;  // minute arrows; any minute can be typed
  return f;
}
FormField dateF(int id, const std::string& label, int32_t day) {
  FormField f;
  f.id = id;
  f.kind = FieldKind::Date;
  f.label = label;
  f.num = day;
  f.min = core::daysFromCivil(2020, 1, 1);
  f.max = core::daysFromCivil(2099, 12, 31);
  f.step = 1;
  return f;
}
FormField dateTimeF(int id, const std::string& label, int32_t day, int minute) {
  FormField f;
  f.id = id;
  f.kind = FieldKind::DateTime;
  f.label = label;
  f.num = day * 1440 + minute;
  return f;
}
// Start value for clock entry: the RTC when valid, otherwise the firmware build date/time.
ui::DateTimeValue clockDefault(const AdminService& s) {
  return ui::defaultClockValue(!s.clockUnset(), s.localNow(), ui::firmwareBuildDate(), ui::firmwareBuildTime());
}
std::string setClockFromEntry(AdminService& s, int encoded) {
  const int32_t day = encoded / 1440;
  const int minute = encoded % 1440;
  const core::Ymd d = core::civilFromDays(day);
  return s.setClockLocal(d.y, d.m, d.d, minute / 60, minute % 60);
}
FormField choice(int id, const std::string& label, int idx, std::vector<std::string> opts) {
  FormField f;
  f.id = id;
  f.kind = FieldKind::Choice;
  f.label = label;
  f.num = idx;
  f.options = std::move(opts);
  return f;
}
FormField toggle(int id, const std::string& label, bool on) {
  FormField f;
  f.id = id;
  f.kind = FieldKind::Toggle;
  f.label = label;
  f.num = on ? 1 : 0;
  return f;
}
FormField act(int id, const std::string& label, const std::string& value = "", bool danger = false) {
  FormField f;
  f.id = id;
  f.kind = FieldKind::Action;
  f.label = label;
  f.value = value;
  f.danger = danger;
  return f;
}

}  // namespace

std::string validateNotifyUrl(const std::string& u) {
  if (u.empty()) return "";
  if (u.size() > 160) return ui::tr(ui::A::URLTooLongMax160);
  const bool https = u.rfind("https://", 0) == 0, http = u.rfind("http://", 0) == 0;
  if (!https && !http) return ui::tr(ui::A::URLMustStartWithHttp);
  const size_t hostStart = https ? 8 : 7;
  if (u.size() <= hostStart || u[hostStart] == '/' || u[hostStart] == ':') return ui::tr(ui::A::URLNeedsAHost);
  for (char c : u)
    if (static_cast<unsigned char>(c) <= 0x20 || static_cast<unsigned char>(c) >= 0x7F) return ui::tr(ui::A::URLMustNotContainSpaces);
  return "";
}

namespace {

std::string cap(std::string s) {
  if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
  return s;
}
FormResult result(const std::string& err, const std::string& okMsg) {
  return err.empty() ? FormResult::stay(okMsg) : FormResult::stay(cap(err), true);
}

std::vector<std::string> patternOptions(bool withInherit) {
  std::vector<std::string> o;
  if (withInherit) o.push_back(ui::tr(ui::A::PersonDefault));
  for (int i = 0; i < static_cast<int>(core::PatternId::Count); ++i)
    o.push_back(core::patternName(static_cast<core::PatternId>(i)));
  return o;
}

std::vector<std::string> tzOptions() {
  std::vector<std::string> o;
  for (int i = 0; i < core::tzPresetCount(); ++i) o.push_back(core::tzPreset(i).name);
  o.push_back(ui::tr(ui::A::CustomPOSIXRule));
  return o;
}

std::string initialsFrom(const std::string& name) {
  std::string out;
  bool start = true;
  for (size_t i = 0; i < name.size() && out.size() < 2; ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c == ' ') {
      start = true;
      continue;
    }
    if (start && c < 0x80) out.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c));
    start = false;
  }
  return out.empty() ? "?" : out;
}

std::string dateRange(int32_t from, int32_t to) {
  const std::string a = from == core::kNoDate ? ui::tr(ui::A::Start) : fmtShortDate(from);
  const std::string b = to == core::kNoDate ? ui::tr(ui::A::Open) : fmtShortDate(to);
  return a + " - " + b;
}

// Two-step PIN entry via the keyboard. Returns true when `id` belonged to the flow.
struct PinFlow {
  int idA, idB;
  std::string first;
  FormResult start() const {
    KeyboardRequest k;
    k.id = idA;
    k.title = ui::tr(ui::A::NewPIN48Digits);
    k.maxBytes = 8;
    k.numeric = true;
    k.password = true;
    return FormResult::keyboard(k);
  }
  bool handles(int id) const { return id == idA || id == idB; }
  FormResult step(int id, const std::string& t, const std::function<std::string(const std::string&)>& apply) {
    if (id == idA) {
      if (!core::pinFormatValid(t)) return FormResult::stay(ui::tr(ui::A::PINMustBe4To), true);
      first = t;
      KeyboardRequest k;
      k.id = idB;
      k.title = ui::tr(ui::A::RepeatThePIN);
      k.maxBytes = 8;
      k.numeric = true;
      k.password = true;
      return FormResult::keyboard(k);
    }
    const std::string a = first;
    first.clear();
    if (t != a) return FormResult::stay(ui::tr(ui::A::PINsDidNotMatchNothing), true);
    return result(apply(t), ui::tr(ui::A::PINSet));
  }
};

// ---------- Menu ----------
class MenuPage : public FormProvider {
 public:
  explicit MenuPage(AdminService& s) : s_(s) {}
  std::string title() const override { return ui::tr(ui::A::Menu); }
  PinScope pinScope() const override { return PinScope::Admin; }
  void build(std::vector<FormField>& out) override {
    const core::Database db = s_.database();
    int active = 0;
    for (const auto& m : db.meds) active += m.active ? 1 : 0;
    out.push_back(act(1, ui::tr(ui::A::Medications), fmt(ui::tr(ui::A::NActive), active)));
    out.push_back(act(2, ui::tr(ui::A::People), fmt(ui::tr(ui::A::NOfN), static_cast<int>(db.profiles.size()), static_cast<int>(core::kMaxProfiles))));
    out.push_back(act(3, ui::tr(ui::A::Settings)));
  }
  FormResult action(int id) override {
    if (id == 1) return FormResult::push(makeMedListPage(s_));
    if (id == 2) return FormResult::push(makeProfileListPage(s_));
    if (id == 3) return FormResult::push(makeSettingsPage(s_));
    return FormResult::stay();
  }

 private:
  AdminService& s_;
};

// ---------- Medication list ----------
class MedListPage : public FormProvider {
 public:
  explicit MedListPage(AdminService& s) : s_(s) {}
  std::string title() const override { return ui::tr(ui::A::Medications); }
  void build(std::vector<FormField>& out) override {
    const core::Database db = s_.database();
    out.push_back(toggle(fid::ListShowArchived, ui::tr(ui::A::ShowArchived), showArchived_));
    ids_.clear();
    for (const auto& p : db.profiles)
      for (const auto& m : db.meds) {
        if (m.profileId != p.id || (!m.active && !showArchived_)) continue;
        std::string v = p.initials;
        if (!m.active) v += ui::tr(ui::A::Archived);
        out.push_back(act(fid::ListItem0 + static_cast<int>(ids_.size()), m.name, v));
        ids_.push_back(m.id);
      }
    if (ids_.empty()) out.push_back(info(2, db.profiles.empty() ? ui::tr(ui::A::AddAPersonFirst) : ui::tr(ui::A::NoMedicationsYet)));
  }
  FormResult setValue(int id, int v) override {
    if (id == fid::ListShowArchived) showArchived_ = v != 0;
    return FormResult::stay();
  }
  FormResult action(int id) override {
    if (id == fid::FtAdd) {
      const core::Database db = s_.database();
      if (db.profiles.empty()) return FormResult::stay(ui::tr(ui::A::AddAPersonFirst), true);
      return FormResult::push(makeMedEditPage(s_, s_.newMedication(db.profiles.front().id), true));
    }
    const int i = id - fid::ListItem0;
    if (i >= 0 && i < static_cast<int>(ids_.size())) {
      const core::Database db = s_.database();
      const core::Medication* m = db.med(ids_[static_cast<size_t>(i)]);
      if (m) return FormResult::push(makeMedEditPage(s_, *m, false));
    }
    return FormResult::stay();
  }
  void footer(std::vector<FooterButton>& out) override { out.push_back({fid::FtAdd, ui::tr(ui::A::AddMedication), true}); }

 private:
  AdminService& s_;
  bool showArchived_ = false;
  std::vector<uint16_t> ids_;
};

// ---------- Dose times (shared by the medication editor and the phase editor) ----------
// "08:00" for a fixed time, "Breakfast" / "Breakfast +15 min" for a routine anchor.
std::string doseTimeText(const core::DoseTime& t) {
  if (t.routine == core::Routine::None) return fmtHm(t.minute);
  const char* n = core::routineName(t.routine);
  return t.offsetMin == 0 ? std::string(n) : fmt(ui::tr(ui::A::RoutineOffsetN), n, static_cast<int>(t.offsetMin));
}

// Appends one row per time ("Time 1  08:00", tap = edit/delete) and "+ Add time" (opens HH:MM directly).
void addTimeRows(std::vector<FormField>& out, const core::ScheduleRule& r, int firstId, int addId) {
  for (size_t i = 0; i < r.times.size(); ++i)
    out.push_back(act(firstId + static_cast<int>(i), fmt(ui::tr(ui::A::TimeN), static_cast<int>(i) + 1), doseTimeText(r.times[i])));
  if (r.times.size() < core::kMaxTimesPerSchedule) {
    const int next = r.times.empty() ? 8 * 60 : std::min(23 * 60 + 55, r.times.back().minute + 4 * 60);
    FormField add = timeF(addId, ui::tr(ui::A::AddTime), r.times.empty() || r.times.back().routine == core::Routine::None ? next : 12 * 60);
    add.value = " ";  // a plain "+ Add time" row; the HH:MM entry opens on tap
    out.push_back(add);
  }
}

// A new fixed time from "+ Add time". A daily routine schedule becomes "daily at times" (anchors stay per time).
FormResult addDoseTime(core::ScheduleRule& r, int minute) {
  if (r.times.size() >= core::kMaxTimesPerSchedule) return FormResult::stay(ui::tr(ui::A::AtMost12Times), true);
  core::DoseTime t;
  t.minute = static_cast<int16_t>(minute);
  r.times.push_back(t);
  if (r.type == core::ScheduleType::Routine) r.type = core::ScheduleType::FixedTimes;
  return FormResult::stay(std::string(ui::tr(ui::A::TimeAdded)) + " " + fmtHm(minute));
}

// One dose time: a fixed HH:MM or a routine anchor with an offset; can be deleted (not the last one).
class TimeEditPage : public FormProvider {
 public:
  TimeEditPage(core::ScheduleRule& r, size_t idx) : r_(r), idx_(idx) {}
  std::string title() const override { return fmt(ui::tr(ui::A::TimeN), static_cast<int>(idx_) + 1); }
  void build(std::vector<FormField>& out) override {
    if (idx_ >= r_.times.size()) return;
    const core::DoseTime& t = r_.times[idx_];
    std::vector<std::string> anchors{ui::tr(ui::A::FixedClockTime)};
    for (int a = 1; a <= static_cast<int>(core::kRoutineCount); ++a) anchors.push_back(core::routineName(static_cast<core::Routine>(a)));
    out.push_back(choice(fid::TeAnchor, ui::tr(ui::A::TimeAnchor), static_cast<int>(t.routine), anchors));
    if (t.routine == core::Routine::None) out.push_back(timeF(fid::TeTime, ui::tr(ui::A::At), t.minute));
    else out.push_back(number(fid::TeOffset, ui::tr(ui::A::Offset), t.offsetMin, -180, 180, 15, ui::tr(ui::A::Min)));
    FormField del = act(fid::TeDelete, ui::tr(ui::A::DeleteThisTime), "", true);
    del.enabled = r_.times.size() > 1;
    out.push_back(del);
  }
  FormResult setValue(int id, int v) override {
    if (idx_ >= r_.times.size()) return FormResult::pop();
    core::DoseTime& t = r_.times[idx_];
    if (id == fid::TeAnchor) {
      t.routine = static_cast<core::Routine>(std::max(0, std::min(v, static_cast<int>(core::kRoutineCount))));
      t.offsetMin = 0;
      if (t.routine == core::Routine::None && r_.type == core::ScheduleType::Routine) r_.type = core::ScheduleType::FixedTimes;
    } else if (id == fid::TeTime) {
      t.minute = static_cast<int16_t>(v);
    } else if (id == fid::TeOffset) {
      t.offsetMin = static_cast<int16_t>(v);
    }
    return FormResult::stay();
  }
  FormResult action(int id) override {
    if (id == fid::TeDelete) {
      if (r_.times.size() <= 1) return FormResult::stay(ui::tr(ui::A::KeepAtLeastOneTime), true);
      r_.times.erase(r_.times.begin() + static_cast<long>(idx_));
      return FormResult::pop(ui::tr(ui::A::TimeDeleted));
    }
    return id == fid::FtDone ? FormResult::pop() : FormResult::stay();
  }
  void footer(std::vector<FooterButton>& out) override { out.push_back({fid::FtDone, ui::tr(ui::A::Done), true}); }

 private:
  core::ScheduleRule& r_;
  size_t idx_;
};

// ---------- Phase editor ----------
class PhaseEditPage : public FormProvider {
 public:
  PhaseEditPage(core::Medication& m, size_t idx, int32_t today) : m_(m), idx_(idx), today_(today) {}
  std::string title() const override { return fmt(ui::tr(ui::A::PhaseN), static_cast<int>(idx_) + 1); }
  core::Phase& ph() { return m_.phases[idx_]; }
  void build(std::vector<FormField>& out) override {
    core::Phase& p = ph();
    core::ScheduleRule& r = p.rule;
    out.push_back(toggle(fid::PhHasFrom, ui::tr(ui::A::OwnStartDate), p.fromDay != core::kNoDate));
    if (p.fromDay != core::kNoDate) out.push_back(dateF(fid::PhFrom, ui::tr(ui::A::Starts), p.fromDay));
    out.push_back(toggle(fid::PhHasTo, ui::tr(ui::A::Ends), p.toDay != core::kNoDate));
    if (p.toDay != core::kNoDate) out.push_back(dateF(fid::PhTo, ui::tr(ui::A::LastDay), p.toDay));
    FormField d = text(fid::PhDose, ui::tr(ui::A::DoseInThisPhase), p.doseText);
    d.value = p.doseText.empty() ? ui::tr(ui::A::SameAsMedication) : p.doseText;
    out.push_back(d);
    std::vector<std::string> types;
    for (int t = 0; t <= static_cast<int>(core::ScheduleType::Prn); ++t)
      types.push_back(core::scheduleTypeName(static_cast<core::ScheduleType>(t)));
    out.push_back(choice(fid::PhType, ui::tr(ui::A::Schedule), static_cast<int>(r.type), types));
    if (r.type == core::ScheduleType::Prn) {
      out.push_back(number(fid::PhPrnInterval, ui::tr(ui::A::MinimumGap), r.prnMinIntervalMin, 30, 1440, 30, ui::tr(ui::A::Min)));
      out.push_back(number(fid::PhPrnMax, ui::tr(ui::A::MaxPer24H), r.prnMaxPer24h, 1, 12, 1, ui::tr(ui::A::Doses)));
      return;
    }
    switch (r.type) {
      case core::ScheduleType::Weekdays: {
        static const char* kDays[7] = {ui::tr(ui::A::Monday), ui::tr(ui::A::Tuesday), ui::tr(ui::A::Wednesday), ui::tr(ui::A::Thursday), ui::tr(ui::A::Friday), ui::tr(ui::A::Saturday), ui::tr(ui::A::Sunday)};
        for (int i = 0; i < 7; ++i) {
          const int bit = (i + 1) % 7;  // Monday first; bit0 = Sunday
          out.push_back(toggle(fid::PhWeekday0 + i, kDays[i], (r.weekdayMask >> bit) & 1));
        }
        break;
      }
      case core::ScheduleType::EveryN:
        out.push_back(number(fid::PhEveryN, ui::tr(ui::A::Every), r.intervalN, 1, 60, 1));
        out.push_back(choice(fid::PhEveryUnit, ui::tr(ui::A::Unit), r.intervalWeeks ? 1 : 0, {ui::tr(ui::A::Days), ui::tr(ui::A::Weeks)}));
        break;
      case core::ScheduleType::SpecificDates:
        out.push_back(number(fid::PhDateCount, ui::tr(ui::A::NumberOfDates), static_cast<int>(r.dates.size()), 1,
                             static_cast<int>(core::kMaxSpecificDates), 1));
        for (size_t i = 0; i < r.dates.size(); ++i)
          out.push_back(dateF(fid::PhDate0 + static_cast<int>(i), fmt(ui::tr(ui::A::DateN), static_cast<int>(i) + 1), r.dates[i]));
        break;
      case core::ScheduleType::Cycle:
        out.push_back(number(fid::PhCycleOn, ui::tr(ui::A::DaysOn), r.cycleOn, 1, 90, 1, ui::tr(ui::A::Days)));
        out.push_back(number(fid::PhCycleOff, ui::tr(ui::A::DaysOff), r.cycleOff, 1, 90, 1, ui::tr(ui::A::Days)));
        break;
      default: break;
    }
    addTimeRows(out, r, fid::PhTime0, fid::PhAddTime);
  }
  void setType(core::ScheduleType t) {
    core::ScheduleRule& r = ph().rule;
    const core::ScheduleType old = r.type;
    r.type = t;
    if (t == core::ScheduleType::Prn) {
      r.times.clear();
      return;
    }
    if (r.times.empty()) r.times.push_back(core::DoseTime());
    if (t == core::ScheduleType::Routine) {
      for (auto& x : r.times)
        if (x.routine == core::Routine::None) {
          x.routine = core::Routine::Breakfast;
          x.offsetMin = 0;
        }
    } else if (old == core::ScheduleType::Routine) {
      for (auto& x : r.times) x.routine = core::Routine::None;
    }
    if (t == core::ScheduleType::SpecificDates && r.dates.empty()) r.dates.push_back(today_);
    if (t == core::ScheduleType::Weekdays && r.weekdayMask == 0) r.weekdayMask = 0x7F;
  }
  FormResult setValue(int id, int v) override {
    core::Phase& p = ph();
    core::ScheduleRule& r = p.rule;
    const int32_t base = m_.startDay == core::kNoDate ? today_ : m_.startDay;
    if (id == fid::PhHasFrom) p.fromDay = v ? (p.toDay != core::kNoDate ? std::min(base, p.toDay) : base) : core::kNoDate;
    else if (id == fid::PhFrom) p.fromDay = v;
    else if (id == fid::PhHasTo) p.toDay = v ? (p.fromDay != core::kNoDate ? p.fromDay : base) + 13 : core::kNoDate;
    else if (id == fid::PhTo) p.toDay = v;
    else if (id == fid::PhType) setType(static_cast<core::ScheduleType>(v));
    else if (id == fid::PhPrnInterval) r.prnMinIntervalMin = static_cast<uint16_t>(v);
    else if (id == fid::PhPrnMax) r.prnMaxPer24h = static_cast<uint8_t>(v);
    else if (id >= fid::PhWeekday0 && id < fid::PhWeekday0 + 7) {
      const int bit = (id - fid::PhWeekday0 + 1) % 7;
      const uint8_t mask = static_cast<uint8_t>(v ? (r.weekdayMask | (1u << bit)) : (r.weekdayMask & ~(1u << bit)));
      if (mask == 0) return FormResult::stay(ui::tr(ui::A::KeepAtLeastOneDay), true);
      r.weekdayMask = mask;
    } else if (id == fid::PhEveryN) r.intervalN = static_cast<uint16_t>(v);
    else if (id == fid::PhEveryUnit) r.intervalWeeks = v == 1;
    else if (id == fid::PhDateCount) {
      while (static_cast<int>(r.dates.size()) < v) r.dates.push_back(r.dates.empty() ? today_ : r.dates.back() + 1);
      r.dates.resize(static_cast<size_t>(v));
    } else if (id >= fid::PhDate0 && id < fid::PhDate0 + static_cast<int>(r.dates.size())) {
      r.dates[static_cast<size_t>(id - fid::PhDate0)] = v;
      std::sort(r.dates.begin(), r.dates.end());
    } else if (id == fid::PhCycleOn) r.cycleOn = static_cast<uint16_t>(v);
    else if (id == fid::PhCycleOff) r.cycleOff = static_cast<uint16_t>(v);
    else if (id == fid::PhAddTime) return addDoseTime(r, v);
    return FormResult::stay();
  }
  FormResult setText(int id, const std::string& t) override {
    if (id == fid::PhDose) ph().doseText = t;
    return FormResult::stay();
  }
  FormResult action(int id) override {
    core::ScheduleRule& r = ph().rule;
    const int i = id - fid::PhTime0;
    if (i >= 0 && i < static_cast<int>(r.times.size()))
      return FormResult::push(std::unique_ptr<FormProvider>(new TimeEditPage(r, static_cast<size_t>(i))));
    return id == fid::FtDone ? FormResult::pop() : FormResult::stay();
  }
  void footer(std::vector<FooterButton>& out) override { out.push_back({fid::FtDone, ui::tr(ui::A::Done), true}); }

 private:
  core::Medication& m_;
  size_t idx_;
  int32_t today_;
};

class PhaseListPage : public FormProvider {
 public:
  PhaseListPage(core::Medication& m, int32_t today) : m_(m), today_(today) {}
  std::string title() const override { return ui::tr(ui::A::Phases); }
  void build(std::vector<FormField>& out) override {
    for (size_t i = 0; i < m_.phases.size(); ++i) {
      const core::Phase& p = m_.phases[i];
      out.push_back(act(fid::PlPhase0 + static_cast<int>(i), fmt("%d: %s", static_cast<int>(i) + 1, dateRange(p.fromDay, p.toDay).c_str()),
                           phaseSummary(p)));
    }
    if (m_.phases.size() < core::kMaxPhases) out.push_back(act(fid::PlAdd, ui::tr(ui::A::AddPhase), ui::tr(ui::A::TaperTitrate)));
    if (m_.phases.size() > 1) out.push_back(act(fid::PlRemove, ui::tr(ui::A::RemoveLastPhase), "", true));
  }
  FormResult action(int id) override {
    if (id == fid::FtDone) return FormResult::pop();
    if (id == fid::PlAdd && m_.phases.size() < core::kMaxPhases) {
      core::Phase& last = m_.phases.back();
      const int32_t lastFrom = last.fromDay != core::kNoDate ? last.fromDay : (m_.startDay != core::kNoDate ? m_.startDay : today_);
      if (last.toDay == core::kNoDate) last.toDay = std::max(lastFrom, today_) + 13;
      core::Phase n = last;
      n.fromDay = last.toDay + 1;
      n.toDay = core::kNoDate;
      m_.phases.push_back(n);
      return FormResult::stay(ui::tr(ui::A::PhaseAddedEditItsDose));
    }
    if (id == fid::PlRemove && m_.phases.size() > 1) {
      m_.phases.pop_back();
      return FormResult::stay(ui::tr(ui::A::PhaseRemoved));
    }
    const int i = id - fid::PlPhase0;
    if (i >= 0 && i < static_cast<int>(m_.phases.size()))
      return FormResult::push(std::unique_ptr<FormProvider>(new PhaseEditPage(m_, static_cast<size_t>(i), today_)));
    return FormResult::stay();
  }
  void footer(std::vector<FooterButton>& out) override { out.push_back({fid::FtDone, ui::tr(ui::A::Done), true}); }

 private:
  core::Medication& m_;
  int32_t today_;
};

// ---------- Medication editor ----------
class MedEditPage : public FormProvider {
 public:
  MedEditPage(AdminService& s, const core::Medication& m, bool isNew) : s_(s), m_(m), isNew_(isNew) {}
  std::string title() const override { return isNew_ ? ui::tr(ui::A::NewMedication) : ui::tr(ui::A::EditMedication); }
  PinScope pinScope() const override { return PinScope::Profile; }
  uint8_t pinProfile() const override { return m_.profileId; }
  void build(std::vector<FormField>& out) override {
    const core::Database db = s_.database();
    profiles_.clear();
    std::vector<std::string> names;
    int pidx = 0;
    for (const auto& p : db.profiles) {
      if (p.id == m_.profileId) pidx = static_cast<int>(profiles_.size());
      profiles_.push_back(p.id);
      names.push_back(p.name);
    }
    FormField n = text(fid::MedName, ui::tr(ui::A::Name), m_.name);
    if (m_.name.empty()) n.value = ui::tr(ui::A::Required);
    out.push_back(n);
    out.push_back(text(fid::MedDose, ui::tr(ui::A::Dose), m_.doseText));
    // Times on the first page: one row per dose time, "+ Add time" opens HH:MM.
    if (m_.phases.size() == 1 && m_.phases.front().rule.type != core::ScheduleType::Prn)
      addTimeRows(out, m_.phases.front().rule, fid::MedTime0, fid::MedAddTime);
    else if (m_.phases.size() > 1)
      out.push_back(info(fid::MedTimesNote, ui::tr(ui::A::TimesInSchedule)));
    out.push_back(choice(fid::MedPerson, ui::tr(ui::A::Person), pidx, names));
    std::vector<std::string> forms;
    for (int f = 0; f <= static_cast<int>(core::MedForm::Other); ++f) forms.push_back(core::medFormName(static_cast<core::MedForm>(f)));
    out.push_back(choice(fid::MedForm, ui::tr(ui::A::Form), static_cast<int>(m_.form), forms));
    out.push_back(text(fid::MedInstr, ui::tr(ui::A::Instructions), m_.instructions, core::kMaxTextBytes));
    out.push_back(text(fid::MedNotes, ui::tr(ui::A::Notes), m_.notes, core::kMaxTextBytes));
    std::vector<std::string> tpl{ui::tr(ui::A::Choose)};
    for (int t = 0; t < static_cast<int>(core::ScheduleTemplate::Count); ++t)
      tpl.push_back(core::scheduleTemplateName(static_cast<core::ScheduleTemplate>(t)));
    out.push_back(choice(fid::MedTemplate, ui::tr(ui::A::Template), 0, tpl));
    out.push_back(act(fid::MedPhases, m_.phases.size() > 1 ? fmt(ui::tr(ui::A::ScheduleNPhases), static_cast<int>(m_.phases.size())) : ui::tr(ui::A::Schedule),
                         m_.phases.empty() ? "-" : phaseSummary(m_.phases.front())));
    out.push_back(dateF(fid::MedStart, ui::tr(ui::A::StartDate), m_.startDay));
    out.push_back(toggle(fid::MedHasEnd, ui::tr(ui::A::EndDate), m_.endDay != core::kNoDate));
    if (m_.endDay != core::kNoDate) out.push_back(dateF(fid::MedEnd, ui::tr(ui::A::LastDay), m_.endDay));
    FormField c = number(fid::MedCourse, ui::tr(ui::A::CourseLength), m_.courseDays, 0, 365, 1, ui::tr(ui::A::Days));
    if (m_.courseDays == 0) c.value = ui::tr(ui::A::Off);
    c.enabled = m_.endDay == core::kNoDate;
    out.push_back(c);
    out.push_back(toggle(fid::MedCritical, ui::tr(ui::A::CriticalSoundsInQuietHours), m_.critical));
    out.push_back(number(fid::MedGrace, ui::tr(ui::A::MissedAfter), m_.graceMin, 15, 240, 15, ui::tr(ui::A::Min)));
    out.push_back(number(fid::MedOnTime, ui::tr(ui::A::OnTimeWithin), m_.onTimeMin, 5, 60, 5, ui::tr(ui::A::Min)));
    out.push_back(number(fid::MedRepeatEvery, ui::tr(ui::A::RepeatAlarmEvery), m_.ladderIntervalMin, 1, 30, 1, ui::tr(ui::A::Min)));
    out.push_back(number(fid::MedRepeats, ui::tr(ui::A::AlarmRepeats), m_.ladderCount, 1, 12, 1, "x"));
    out.push_back(choice(fid::MedSound, ui::tr(ui::A::AlertSound), m_.alertPattern == 255 ? 0 : m_.alertPattern + 1, patternOptions(true)));
    FormField v = number(fid::MedVolume, ui::tr(ui::A::Volume), m_.alertVolume, 0, 100, 10, "%");
    if (m_.alertVolume == 0) v.value = ui::tr(ui::A::PersonDefault);
    out.push_back(v);
    out.push_back(toggle(fid::MedTrack, ui::tr(ui::A::TrackSupply), m_.supply >= 0));
    if (m_.supply >= 0) {
      out.push_back(number(fid::MedSupply, ui::tr(ui::A::OnHand), m_.supply, 0, 999, 1, ui::tr(ui::A::Units)));
      out.push_back(number(fid::MedLow, ui::tr(ui::A::LowStockAt), m_.lowStockThreshold, 0, 99, 1, ui::tr(ui::A::Units)));
    }
    out.push_back(toggle(fid::MedHasRefill, ui::tr(ui::A::RefillReminderDate), m_.refillDay != core::kNoDate));
    if (m_.refillDay != core::kNoDate) out.push_back(dateF(fid::MedRefill, ui::tr(ui::A::RefillOn), m_.refillDay));
    if (!isNew_) out.push_back(act(fid::MedArchive, m_.active ? ui::tr(ui::A::ArchiveMedication) : ui::tr(ui::A::RestoreMedication),
                                      m_.active ? ui::tr(ui::A::StopsReminders) : "", m_.active));
  }
  FormResult setText(int id, const std::string& t) override {
    if (id == fid::MedName) m_.name = t;
    else if (id == fid::MedDose) m_.doseText = t;
    else if (id == fid::MedInstr) m_.instructions = t;
    else if (id == fid::MedNotes) m_.notes = t;
    return FormResult::stay();
  }
  FormResult setValue(int id, int v) override {
    switch (id) {
      case fid::MedPerson:
        if (v >= 0 && v < static_cast<int>(profiles_.size())) m_.profileId = profiles_[static_cast<size_t>(v)];
        break;
      case fid::MedForm: m_.form = static_cast<core::MedForm>(v); break;
      case fid::MedTemplate:
        if (v > 0) {
          core::applyScheduleTemplate(m_, static_cast<core::ScheduleTemplate>(v - 1));
          return FormResult::stay(std::string(ui::tr(ui::A::Schedule2)) + core::scheduleTemplateName(static_cast<core::ScheduleTemplate>(v - 1)));
        }
        break;
      case fid::MedStart:
        m_.startDay = v;
        if (m_.endDay != core::kNoDate && m_.endDay < v) m_.endDay = v;
        break;
      case fid::MedHasEnd: m_.endDay = v ? m_.startDay + 29 : core::kNoDate; if (v) m_.courseDays = 0; break;
      case fid::MedEnd:
        if (v < m_.startDay) return FormResult::stay(ui::tr(ui::A::LastDayIsBeforeThe), true);
        m_.endDay = v;
        break;
      case fid::MedCourse: m_.courseDays = static_cast<uint16_t>(v); break;
      case fid::MedCritical: m_.critical = v != 0; break;
      case fid::MedGrace:
        m_.graceMin = static_cast<uint16_t>(v);
        if (m_.onTimeMin > m_.graceMin) m_.onTimeMin = m_.graceMin;
        break;
      case fid::MedOnTime:
        if (v > m_.graceMin) return FormResult::stay(ui::tr(ui::A::MustNotExceedMissedAfter), true);
        m_.onTimeMin = static_cast<uint16_t>(v);
        break;
      case fid::MedRepeatEvery: m_.ladderIntervalMin = static_cast<uint8_t>(v); break;
      case fid::MedRepeats: m_.ladderCount = static_cast<uint8_t>(v); break;
      case fid::MedSound: m_.alertPattern = v == 0 ? 255 : static_cast<uint8_t>(v - 1); break;
      case fid::MedVolume: m_.alertVolume = static_cast<uint8_t>(v); break;
      case fid::MedTrack: m_.supply = v ? 30 : -1; break;
      case fid::MedSupply: m_.supply = v; break;
      case fid::MedLow: m_.lowStockThreshold = v; break;
      case fid::MedHasRefill: m_.refillDay = v ? s_.today() + 28 : core::kNoDate; break;
      case fid::MedRefill: m_.refillDay = v; break;
      case fid::MedAddTime:
        if (m_.phases.size() == 1) return addDoseTime(m_.phases.front().rule, v);
        break;
      default: break;
    }
    return FormResult::stay();
  }
  FormResult action(int id) override {
    if (id == fid::MedPhases) return FormResult::push(std::unique_ptr<FormProvider>(new PhaseListPage(m_, s_.today())));
    const int ti = id - fid::MedTime0;
    if (m_.phases.size() == 1 && ti >= 0 && ti < static_cast<int>(m_.phases.front().rule.times.size()))
      return FormResult::push(std::unique_ptr<FormProvider>(new TimeEditPage(m_.phases.front().rule, static_cast<size_t>(ti))));
    if (id == fid::FtCancel) return FormResult::pop();
    if (id == fid::MedArchive) {
      const std::string e = s_.setMedicationActive(m_.id, !m_.active);
      if (!e.empty()) return FormResult::stay(cap(e), true);
      return FormResult::pop(m_.active ? ui::tr(ui::A::ArchivedRemindersStopped) : ui::tr(ui::A::Restored));
    }
    if (id == fid::FtSave) {
      if (m_.name.empty()) return FormResult::stay(ui::tr(ui::A::EnterAName), true);
      const std::string e = s_.saveMedication(m_);
      if (!e.empty()) return FormResult::stay(cap(e), true);
      return FormResult::pop(ui::tr(ui::A::Saved));
    }
    return FormResult::stay();
  }
  void footer(std::vector<FooterButton>& out) override {
    out.push_back({fid::FtCancel, ui::tr(ui::A::Cancel), false});
    out.push_back({fid::FtSave, ui::tr(ui::A::Save), true});
  }
  const core::Medication& draft() const { return m_; }

 private:
  AdminService& s_;
  core::Medication m_;
  bool isNew_;
  std::vector<uint8_t> profiles_;
};

// ---------- Profiles ----------
class ProfileListPage : public FormProvider {
 public:
  explicit ProfileListPage(AdminService& s) : s_(s) {}
  std::string title() const override { return ui::tr(ui::A::People); }
  void build(std::vector<FormField>& out) override {
    const core::Database db = s_.database();
    ids_.clear();
    for (const auto& p : db.profiles) {
      std::string v = p.initials;
      if (!p.pinHash.empty()) v += ui::tr(ui::A::PIN);
      out.push_back(act(fid::ListItem0 + static_cast<int>(ids_.size()), p.name, v));
      ids_.push_back(p.id);
    }
    out.push_back(info(2, fmt(ui::tr(ui::A::NOfNPeople), static_cast<int>(ids_.size()), static_cast<int>(core::kMaxProfiles))));
  }
  FormResult action(int id) override {
    const core::Database db = s_.database();
    if (id == fid::FtAdd) {
      if (db.profiles.size() >= core::kMaxProfiles) return FormResult::stay(ui::tr(ui::A::N8PeopleIsTheMaximum), true);
      core::Profile p;
      return FormResult::push(makeProfileEditPage(s_, p, true));
    }
    const int i = id - fid::ListItem0;
    if (i >= 0 && i < static_cast<int>(ids_.size())) {
      const core::Profile* p = db.profile(ids_[static_cast<size_t>(i)]);
      if (p) return FormResult::push(makeProfileEditPage(s_, *p, false));
    }
    return FormResult::stay();
  }
  void footer(std::vector<FooterButton>& out) override { out.push_back({fid::FtAdd, ui::tr(ui::A::AddPerson), true}); }

 private:
  AdminService& s_;
  std::vector<uint8_t> ids_;
};

class ProfileEditPage : public FormProvider {
 public:
  ProfileEditPage(AdminService& s, const core::Profile& p, bool isNew) : s_(s), p_(p), isNew_(isNew) {}
  std::string title() const override { return isNew_ ? ui::tr(ui::A::NewPerson) : ui::tr(ui::A::EditPerson); }
  PinScope pinScope() const override { return isNew_ ? PinScope::None : PinScope::Profile; }
  uint8_t pinProfile() const override { return p_.id; }
  void build(std::vector<FormField>& out) override {
    FormField n = text(fid::PrName, ui::tr(ui::A::Name), p_.name);
    if (p_.name.empty()) n.value = ui::tr(ui::A::Required);
    out.push_back(n);
    FormField i = text(fid::PrInitials, ui::tr(ui::A::Initials), p_.initials, 8);
    if (p_.initials.empty()) i.value = ui::tr(ui::A::FromName);
    out.push_back(i);
    out.push_back(toggle(fid::PrPrivacy, ui::tr(ui::A::HideNamesWhenLocked), p_.privacy));
    const bool quiet = p_.quietStartMin != p_.quietEndMin;
    out.push_back(toggle(fid::PrQuiet, ui::tr(ui::A::QuietHours), quiet));
    if (quiet) {
      out.push_back(timeF(fid::PrQuietFrom, ui::tr(ui::A::QuietFrom), p_.quietStartMin));
      out.push_back(timeF(fid::PrQuietTo, ui::tr(ui::A::QuietUntil), p_.quietEndMin));
    }
    for (size_t r = 0; r < core::kRoutineCount; ++r)
      out.push_back(timeF(fid::PrRoutine0 + static_cast<int>(r), core::routineName(static_cast<core::Routine>(r + 1)), p_.routineMin[r]));
    out.push_back(choice(fid::PrSound, ui::tr(ui::A::AlertSound), p_.alertPattern, patternOptions(false)));
    out.push_back(number(fid::PrVolume, ui::tr(ui::A::Volume), p_.alertVolume, 10, 100, 10, "%"));
    out.push_back(act(fid::PrTest, ui::tr(ui::A::TestSound), ui::tr(ui::A::PlayOnce)));
    out.push_back(act(fid::PrPin, p_.pinHash.empty() ? ui::tr(ui::A::SetPIN) : ui::tr(ui::A::ChangePIN), p_.pinHash.empty() ? ui::tr(ui::A::None) : ui::tr(ui::A::Set)));
    if (!p_.pinHash.empty()) out.push_back(act(fid::PrClearPin, ui::tr(ui::A::RemovePIN), "", true));
  }
  FormResult setText(int id, const std::string& t) override {
    if (pin_.handles(id))
      return pin_.step(id, t, [this](const std::string& pin) {
        core::setPin(p_.pinSalt, p_.pinHash, pin, s_.entropy());
        return std::string();
      });
    if (id == fid::PrName) p_.name = t;
    else if (id == fid::PrInitials) p_.initials = t;
    return FormResult::stay();
  }
  FormResult setValue(int id, int v) override {
    if (id == fid::PrPrivacy) p_.privacy = v != 0;
    else if (id == fid::PrQuiet) {
      if (v) {
        p_.quietStartMin = 22 * 60;
        p_.quietEndMin = 7 * 60;
      } else {
        p_.quietStartMin = p_.quietEndMin = 0;
      }
    } else if (id == fid::PrQuietFrom) p_.quietStartMin = static_cast<int16_t>(v);
    else if (id == fid::PrQuietTo) p_.quietEndMin = static_cast<int16_t>(v);
    else if (id >= fid::PrRoutine0 && id < fid::PrRoutine0 + static_cast<int>(core::kRoutineCount))
      p_.routineMin[static_cast<size_t>(id - fid::PrRoutine0)] = static_cast<int16_t>(v);
    else if (id == fid::PrSound) p_.alertPattern = static_cast<uint8_t>(v);
    else if (id == fid::PrVolume) p_.alertVolume = static_cast<uint8_t>(v);
    return FormResult::stay();
  }
  FormResult action(int id) override {
    if (id == fid::FtCancel) return FormResult::pop();
    if (id == fid::PrPin) return pin_.start();
    if (id == fid::PrClearPin) {
      p_.pinSalt.clear();
      p_.pinHash.clear();
      return FormResult::stay(ui::tr(ui::A::PINRemovedSaveToKeep));
    }
    if (id == fid::PrTest) {
      if (!s_.hooks().testAlert) return FormResult::stay(ui::tr(ui::A::NoSoundOutputOnThis), true);
      core::PersonAlert a;
      a.profileId = p_.id;
      a.pattern = static_cast<core::PatternId>(p_.alertPattern);
      a.volume = p_.alertVolume;
      s_.hooks().testAlert(a);
      return FormResult::stay(ui::tr(ui::A::PlayingTestSound));
    }
    if (id == fid::FtSave) {
      if (p_.initials.empty()) p_.initials = initialsFrom(p_.name);
      if (p_.name.empty()) return FormResult::stay(ui::tr(ui::A::EnterAName), true);
      const std::string e = s_.saveProfile(p_);
      if (!e.empty()) return FormResult::stay(cap(e), true);
      return FormResult::pop(ui::tr(ui::A::Saved));
    }
    return FormResult::stay();
  }
  void footer(std::vector<FooterButton>& out) override {
    out.push_back({fid::FtCancel, ui::tr(ui::A::Cancel), false});
    out.push_back({fid::FtSave, ui::tr(ui::A::Save), true});
  }

 private:
  AdminService& s_;
  core::Profile p_;
  bool isNew_;
  PinFlow pin_{900, 901, ""};
};

// ---------- Settings ----------
class SettingsPage : public FormProvider {
 public:
  explicit SettingsPage(AdminService& s) : s_(s) {}
  std::string title() const override { return ui::tr(ui::A::Settings); }
  PinScope pinScope() const override { return PinScope::Admin; }
  void build(std::vector<FormField>& out) override {
    const core::Settings st = s_.database().settings;
    const BatteryInfo b = s_.batteryInfo();
    out.push_back(act(static_cast<int>(SettingsSection::Time), ui::tr(ui::A::TimeTimeZone), st.tzName));
    out.push_back(act(static_cast<int>(SettingsSection::Sound), ui::tr(ui::A::SoundAlarms)));
    out.push_back(act(static_cast<int>(SettingsSection::Quiet), ui::tr(ui::A::QuietHours)));
    out.push_back(act(static_cast<int>(SettingsSection::Display), ui::tr(ui::A::Display)));
    out.push_back(act(static_cast<int>(SettingsSection::Lock), ui::tr(ui::A::LockPIN), st.adminPinHash.empty() ? ui::tr(ui::A::Off2) : ui::tr(ui::A::On)));
    out.push_back(act(static_cast<int>(SettingsSection::Battery), ui::tr(ui::A::Battery), b.known ? fmt("%d%%", b.pct) : ui::tr(ui::A::Unknown)));
    out.push_back(act(static_cast<int>(SettingsSection::Storage), ui::tr(ui::A::Storage)));
    out.push_back(act(static_cast<int>(SettingsSection::Backup), ui::tr(ui::A::BackupRestore), s_.sdAvailable() ? ui::tr(ui::A::SDReady) : ui::tr(ui::A::NoSD)));
    const NetStatus ns = s_.net().status();
    out.push_back(act(static_cast<int>(SettingsSection::Wifi), ui::tr(ui::A::WiFi), ns.configured ? ns.ssid : ui::tr(ui::A::NotSet)));
    out.push_back(act(static_cast<int>(SettingsSection::Portal), ui::tr(ui::A::WebPortal), ns.portal ? ui::tr(ui::A::Running) : ui::tr(ui::A::Off2)));
    out.push_back(act(static_cast<int>(SettingsSection::Notify), ui::tr(ui::A::CaregiverAlerts), st.notifyEnabled ? ui::tr(ui::A::On) : ui::tr(ui::A::Off2)));
    out.push_back(act(static_cast<int>(SettingsSection::Update), ui::tr(ui::A::SoftwareUpdate)));
    out.push_back(act(static_cast<int>(SettingsSection::About), ui::tr(ui::A::About), s_.version()));
    out.push_back(act(static_cast<int>(SettingsSection::Reset), ui::tr(ui::A::FactoryReset)));
  }
  FormResult action(int id) override {
    if (id >= 1 && id <= static_cast<int>(SettingsSection::Reset))
      return FormResult::push(makeSettingsSection(s_, static_cast<SettingsSection>(id)));
    return FormResult::stay();
  }

 private:
  AdminService& s_;
};

class SectionPage : public FormProvider {
 public:
  SectionPage(AdminService& s, SettingsSection sec) : s_(s), sec_(sec) {}
  bool poll(FormResult& out) override {
    NetJobState st;
    const int r = job_.poll(s_.net(), st);
    if (r == 0) return false;
    if (r == 1) {
      out = FormResult::stay();  // progress: redraw the "... N s" value
      return true;
    }
    if (!st.error.empty()) out = FormResult::stay(st.error, true);
    else out = FormResult::stay(ui::tr(st.kind == NetJob::SyncTime ? ui::A::TimeSynchronised : ui::A::ConnectedRadioOffAgain));
    return true;
  }
  std::string title() const override {
    switch (sec_) {
      case SettingsSection::Time: return ui::tr(ui::A::TimeTimeZone);
      case SettingsSection::Sound: return ui::tr(ui::A::SoundAlarms);
      case SettingsSection::Quiet: return ui::tr(ui::A::QuietHours);
      case SettingsSection::Display: return ui::tr(ui::A::Display);
      case SettingsSection::Lock: return ui::tr(ui::A::LockPIN);
      case SettingsSection::Battery: return ui::tr(ui::A::Battery);
      case SettingsSection::Storage: return ui::tr(ui::A::Storage);
      case SettingsSection::Backup: return ui::tr(ui::A::BackupRestore);
      case SettingsSection::Wifi: return ui::tr(ui::A::WiFi);
      case SettingsSection::Update: return ui::tr(ui::A::SoftwareUpdate);
      case SettingsSection::Portal: return ui::tr(ui::A::WebPortal);
      case SettingsSection::Notify: return ui::tr(ui::A::CaregiverAlerts);
      case SettingsSection::About: return ui::tr(ui::A::About);
      case SettingsSection::Reset: return ui::tr(ui::A::FactoryReset);
    }
    return ui::tr(ui::A::Settings);
  }
  PinScope pinScope() const override { return PinScope::Admin; }

  void build(std::vector<FormField>& out) override {
    const core::Database db = s_.database();
    const core::Settings& st = db.settings;
    switch (sec_) {
      case SettingsSection::Time: {
        const int idx = core::tzPresetIndex(st.tzName);
        out.push_back(choice(fid::TmZone, ui::tr(ui::A::TimeZone), idx < 0 ? core::tzPresetCount() : idx, tzOptions()));
        FormField px = text(fid::TmPosix, ui::tr(ui::A::POSIXRule), st.tz, 64);
        px.enabled = idx < 0;
        out.push_back(px);
        const core::LocalTime lt = s_.localNow();
        out.push_back(info(fid::TmNow, ui::tr(ui::A::Now), fmtShortDate(lt.days) + " " + fmtHm(lt.secOfDay / 60)));
        {
          const ui::DateTimeValue dv = clockDefault(s_);
          FormField f = dateTimeF(fid::TmClock, ui::tr(ui::A::SetDateAndTime), dv.day, dv.minute);
          f.value = " ";  // the current time is shown in the row above
          out.push_back(f);
        }
        out.push_back(toggle(fid::TmAuto, ui::tr(ui::A::NetworkTime), st.autoTimeSync));
        {
          const NetStatus ns = s_.net().status();
          FormField f = act(fid::TmSync, ui::tr(ui::A::SyncTimeNow),
                            job_.busy(NetJob::SyncTime) ? fmt(ui::tr(ui::A::SyncingTimeN), job_.elapsed())
                            : !ns.configured ? ui::tr(ui::A::NeedsWiFi)
                                           : (ns.lastSyncUtc ? ui::tr(ui::A::Last) + fmtShortDate(core::utcToLocal(s_.model().tz(), ns.lastSyncUtc).days) : ui::tr(ui::A::Never)));
          f.enabled = !job_.busy();
          out.push_back(f);
        }
        break;
      }
      case SettingsSection::Sound:
        out.push_back(info(fid::SnInfo, ui::tr(ui::A::Buzzer), ui::tr(ui::A::BuiltIn)));
        out.push_back(number(fid::SnLadderEvery, ui::tr(ui::A::NewMedsRepeatEvery), st.defaultLadderIntervalMin, 1, 30, 1, ui::tr(ui::A::Min)));
        out.push_back(number(fid::SnLadderCount, ui::tr(ui::A::NewMedsRepeats), st.defaultLadderCount, 1, 12, 1, "x"));
        out.push_back(choice(fid::SnPattern, ui::tr(ui::A::TestSound), testPattern_, patternOptions(false)));
        out.push_back(number(fid::SnVolume, ui::tr(ui::A::TestVolume), testVolume_, 10, 100, 10, "%"));
        out.push_back(act(fid::SnPlay, ui::tr(ui::A::PlayTest)));
        out.push_back(info(fid::SnNote, ui::tr(ui::A::EachPersonsSoundPeoplePerson)));
        break;
      case SettingsSection::Quiet:
        quietIds_.clear();
        for (const auto& p : db.profiles) {
          const int b = 10 + static_cast<int>(quietIds_.size()) * 2;
          out.push_back(timeF(b, p.name + ui::tr(ui::A::QuietFrom2), p.quietStartMin));
          out.push_back(timeF(b + 1, p.name + ui::tr(ui::A::Until), p.quietEndMin));
          quietIds_.push_back(p.id);
        }
        out.push_back(info(1, ui::tr(ui::A::SameStartAndEndNo)));
        out.push_back(info(2, ui::tr(ui::A::CriticalMedicationsStillSound)));
        break;
      case SettingsSection::Display:
        out.push_back(toggle(fid::DsLarge, ui::tr(ui::A::LargeText), s_.hooks().largeText ? s_.hooks().largeText() : false));
        out.push_back(number(fid::DsFullEvery, ui::tr(ui::A::FullRefreshEvery), st.refreshFullEvery, 5, 50, 1, ui::tr(ui::A::Updates)));
        out.push_back(toggle(fid::DsNightly, ui::tr(ui::A::NightlyFullRefresh), st.nightlyFullRefresh));
        break;
      case SettingsSection::Lock:
        out.push_back(info(fid::LkStatus, ui::tr(ui::A::SettingsEditorPIN), st.adminPinHash.empty() ? ui::tr(ui::A::Off) : ui::tr(ui::A::On2)));
        out.push_back(act(fid::LkSet, st.adminPinHash.empty() ? ui::tr(ui::A::SetPIN) : ui::tr(ui::A::ChangePIN)));
        if (!st.adminPinHash.empty()) out.push_back(act(fid::LkClear, ui::tr(ui::A::RemovePIN), "", true));
        out.push_back(info(fid::LkNote, ui::tr(ui::A::PersonPINsPeoplePerson)));
        break;
      case SettingsSection::Battery: {
        const BatteryInfo b = s_.batteryInfo();
        out.push_back(info(1, ui::tr(ui::A::Charge), b.known ? fmt("%d%%", b.pct) : ui::tr(ui::A::Unknown)));
        out.push_back(info(2, ui::tr(ui::A::Voltage), b.mv > 0 ? fmt("%d.%02d V", b.mv / 1000, (b.mv % 1000) / 10) : "-"));
        out.push_back(info(3, ui::tr(ui::A::Current), b.mv > 0 ? fmt(ui::tr(ui::A::NMA), b.ma) : "-"));
        out.push_back(info(4, ui::tr(ui::A::Power), b.usb ? (b.charging ? ui::tr(ui::A::USBCharging) : ui::tr(ui::A::USB)) : ui::tr(ui::A::Battery)));
        out.push_back(info(5, ui::tr(ui::A::EstimatedRuntime), b.estimate.empty() ? "-" : b.estimate));
        break;
      }
      case SettingsSection::Storage: {
        const StorageInfo si = s_.storageInfo();
        out.push_back(info(1, ui::tr(ui::A::InternalFlash), si.internalOk ? ui::tr(ui::A::OK) : ui::tr(ui::A::NotMounted)));
        out.push_back(info(2, ui::tr(ui::A::Health), si.health));
        out.push_back(info(3, ui::tr(ui::A::PeopleMedications), fmt("%d / %d", si.profiles, si.meds)));
        out.push_back(info(4, ui::tr(ui::A::HistoryEntries), fmt("%u", static_cast<unsigned>(si.logEntries))));
        out.push_back(info(5, ui::tr(ui::A::SDCard), si.sdPresent ? fmt(ui::tr(ui::A::PresentNBackups), si.backups) : ui::tr(ui::A::NotPresent)));
        break;
      }
      case SettingsSection::Backup: {
        out.push_back(info(fid::BkSd, ui::tr(ui::A::SDCard), s_.sdAvailable() ? ui::tr(ui::A::Ready) : ui::tr(ui::A::InsertAnSDCard)));
        out.push_back(act(fid::BkNow, ui::tr(ui::A::BackUpNow)));
        out.push_back(toggle(fid::BkAuto, ui::tr(ui::A::NightlyBackup), st.autoBackup));
        if (st.autoBackup) {
          const storage::AutoBackupState ab = s_.autoBackupState();
          out.push_back(info(fid::BkAutoLast, ui::tr(ui::A::LastAutoBackup), autoBackupWhen(s_, ab)));
          out.push_back(info(fid::BkAutoStatus, ui::tr(ui::A::AbStatus), autoBackupStatusText(ab.status)));
          if (!ab.lastName.empty()) out.push_back(info(fid::BkAutoFile, ui::tr(ui::A::AbFile), ab.lastName));
        }
        files_ = s_.backups();
        for (size_t i = 0; i < files_.size() && i < 20; ++i)
          out.push_back(act(fid::BkItem0 + static_cast<int>(i), files_[i], ui::tr(ui::A::Restore), true));
        break;
      }
      case SettingsSection::Wifi: {
        const NetStatus ns = s_.net().status();
        out.push_back(info(fid::WfStatus, ui::tr(ui::A::Status), ns.text));
        out.push_back(toggle(fid::WfEnabled, ui::tr(ui::A::UseWiFi), st.wifiEnabled));
        out.push_back(info(fid::WfSsid, ui::tr(ui::A::Network), ns.configured ? ns.ssid : ui::tr(ui::A::NotSet)));
        if (ns.provisioning) {
          out.push_back(info(fid::WfApName, ui::tr(ui::A::N1JoinHotspot), ns.apSsid));
          out.push_back(info(fid::WfApPass, ui::tr(ui::A::HotspotPassword), ns.apPass));
          out.push_back(info(fid::WfApUrl, ui::tr(ui::A::N2Open), "http://192.168.4.1"));
          out.push_back(act(fid::WfPhoneStop, ui::tr(ui::A::StopPhoneSetup)));
        } else {
          out.push_back(act(fid::WfPhone, ui::tr(ui::A::SetUpWithPhone), ui::tr(ui::A::Hotspot)));
        }
        FormField ms = text(fid::WfManualSsid, ui::tr(ui::A::EnterNetworkName), pendingSsid_, 32);
        if (pendingSsid_.empty()) ms.value = ui::tr(ui::A::OnDevice);
        out.push_back(ms);
        FormField mp = text(fid::WfManualPass, ui::tr(ui::A::EnterPassword), "", 64);
        mp.password = true;
        mp.value = pendingSsid_.empty() ? ui::tr(ui::A::NameFirst) : ui::tr(ui::A::ThenSave);
        mp.enabled = !pendingSsid_.empty();
        out.push_back(mp);
        {
          FormField f = act(fid::WfTest, ui::tr(ui::A::TestConnection), job_.busy(NetJob::TestConnection) ? fmt(ui::tr(ui::A::ConnectingN), job_.elapsed()) : "");
          f.enabled = !job_.busy();
          out.push_back(f);
        }
        if (ns.configured) out.push_back(act(fid::WfForget, ui::tr(ui::A::ForgetNetwork), "", true));
        break;
      }
      case SettingsSection::Portal: {
        const NetStatus ns = s_.net().status();
        out.push_back(info(fid::PoStatus, ui::tr(ui::A::WebPortal), ns.portal ? ui::tr(ui::A::Running) : ui::tr(ui::A::Stopped)));
        if (ns.portal) {
          out.push_back(info(fid::PoUrl, ui::tr(ui::A::Address), ns.portalUrl));
          out.push_back(info(fid::PoIp, ui::tr(ui::A::Or), ns.portalIp));
          out.push_back(info(fid::PoCode, ui::tr(ui::A::OneTimeCode), ns.portalCode));
          out.push_back(info(fid::PoTime, ui::tr(ui::A::StopsWhenIdleIn), fmt(ui::tr(ui::A::NMin), ns.portalMinutesLeft)));
          out.push_back(act(fid::PoStop, ui::tr(ui::A::StopPortal)));
        } else {
          out.push_back(act(fid::PoStart, ui::tr(ui::A::StartPortal), ns.configured ? "" : ui::tr(ui::A::SetUpWiFiFirst)));
        }
        out.push_back(info(fid::PoNoTls, ui::tr(ui::A::NoTLSUseOnlyOn)));
        break;
      }
      case SettingsSection::Notify: {
        out.push_back(toggle(fid::NoEnabled, ui::tr(ui::A::MissedDosePush), st.notifyEnabled));
        FormField u = text(fid::NoUrl, ui::tr(ui::A::URLNtfyWebhook), st.notifyUrl, 160);
        if (st.notifyUrl.empty()) u.value = "-";
        out.push_back(u);
        out.push_back(toggle(fid::NoNames, ui::tr(ui::A::IncludeNames), st.notifyNames));
        out.push_back(act(fid::NoTest, ui::tr(ui::A::SendTest)));
        out.push_back(info(fid::NoNote, ui::tr(ui::A::OffInitialsAndCountsOnly)));
        break;
      }
      case SettingsSection::Update:
        out.push_back(info(fid::UpVersion, ui::tr(ui::A::Installed), s_.version()));
        out.push_back(info(fid::UpHow, ui::tr(ui::A::UploadFirmwareInTheWeb)));
        out.push_back(info(fid::UpVerify, ui::tr(ui::A::SHA256CheckedRollsBack)));
        out.push_back(act(fid::UpPortal, s_.net().status().portal ? ui::tr(ui::A::WebPortalRunning) : ui::tr(ui::A::StartWebPortal)));
        break;
      case SettingsSection::About:
        out.push_back(info(1, ui::tr(ui::A::RxNudge), s_.version()));
        out.push_back(info(2, ui::tr(ui::A::AReminderToolNotA)));
        out.push_back(info(3, ui::tr(ui::A::FollowYourDoctorsInstructions)));
        out.push_back(info(4, ui::tr(ui::A::DoNotRelyOnSound)));
        out.push_back(info(5, ui::tr(ui::A::DataStaysOnThisDevice)));
        out.push_back(info(6, ui::tr(ui::A::FontAtkinsonHyperlegibleSILOFL)));
        break;
      case SettingsSection::Reset: {
        out.push_back(info(fid::RsWarn, ui::tr(ui::A::ErasesPeopleMedsHistorySettings)));
        FormField c = text(fid::RsConfirm, ui::tr(ui::A::TypeRESETToConfirm), confirm_, 8);
        if (confirm_.empty()) c.value = "-";
        out.push_back(c);
        FormField e = act(fid::RsErase, ui::tr(ui::A::EraseEverything), "", true);
        e.enabled = confirm_ == "RESET";
        out.push_back(e);
        break;
      }
    }
  }

  FormResult setValue(int id, int v) override {
    core::Database db = s_.database();
    core::Settings st = db.settings;
    switch (sec_) {
      case SettingsSection::Time:
        if (id == fid::TmZone) {
          if (v >= 0 && v < core::tzPresetCount())
            return result(s_.setTimezone(core::tzPreset(v).name, core::tzPreset(v).posix), std::string(ui::tr(ui::A::TimeZone2)) + core::tzPreset(v).name);
          st.tzName = "Custom";
          return result(s_.saveSettings(st), ui::tr(ui::A::EnterThePOSIXRule));
        }
        if (id == fid::TmClock) return result(setClockFromEntry(s_, v), ui::tr(ui::A::ClockSet));
        if (id == fid::TmAuto) {
          st.autoTimeSync = v != 0;
          return result(s_.saveSettings(st), "");
        }
        break;
      case SettingsSection::Sound:
        if (id == fid::SnLadderEvery) st.defaultLadderIntervalMin = static_cast<uint8_t>(v);
        else if (id == fid::SnLadderCount) st.defaultLadderCount = static_cast<uint8_t>(v);
        else {
          if (id == fid::SnPattern) testPattern_ = v;
          if (id == fid::SnVolume) testVolume_ = v;
          break;
        }
        return result(s_.saveSettings(st), ui::tr(ui::A::Saved));
      case SettingsSection::Quiet: {
        const int i = (id - 10) / 2;
        if (id < 10 || i >= static_cast<int>(quietIds_.size())) break;
        core::Profile* p = db.profile(quietIds_[static_cast<size_t>(i)]);
        if (!p) break;
        if ((id - 10) % 2 == 0) p->quietStartMin = static_cast<int16_t>(v);
        else p->quietEndMin = static_cast<int16_t>(v);
        core::Profile copy = *p;
        return result(s_.saveProfile(copy), ui::tr(ui::A::Saved));
      }
      case SettingsSection::Display:
        if (id == fid::DsLarge) {
          if (s_.hooks().setLargeText) s_.hooks().setLargeText(v != 0);
          return FormResult::stay();
        }
        if (id == fid::DsFullEvery) st.refreshFullEvery = static_cast<uint8_t>(v);
        if (id == fid::DsNightly) st.nightlyFullRefresh = v != 0;
        return result(s_.saveSettings(st), ui::tr(ui::A::Saved));
      case SettingsSection::Backup:
        if (id == fid::BkAuto) {
          st.autoBackup = v != 0;
          return result(s_.saveSettings(st), ui::tr(ui::A::Saved));
        }
        break;
      case SettingsSection::Wifi:
        if (id == fid::WfEnabled) {
          st.wifiEnabled = v != 0;
          const std::string e = s_.saveSettings(st);
          if (!e.empty()) return FormResult::stay(cap(e), true);
          if (!st.wifiEnabled) {
            s_.net().stopPortal();
            s_.net().stopProvisioning();
          }
          return FormResult::stay(st.wifiEnabled && !s_.net().status().available ? RadioOffNetService::kMsg : ui::tr(ui::A::Saved));
        }
        break;
      case SettingsSection::Notify:
        if (id == fid::NoEnabled) {
          if (v && st.notifyUrl.empty()) return FormResult::stay(ui::tr(ui::A::EnterTheURLFirst), true);
          st.notifyEnabled = v != 0;
        } else if (id == fid::NoNames) {
          st.notifyNames = v != 0;
        } else {
          break;
        }
        return result(s_.saveSettings(st), ui::tr(ui::A::Saved));
      default: break;
    }
    return FormResult::stay();
  }

  FormResult setText(int id, const std::string& t) override {
    if (pin_.handles(id)) return pin_.step(id, t, [this](const std::string& p) { return s_.setAdminPin(p); });
    core::Settings st = s_.database().settings;
    if (sec_ == SettingsSection::Time && id == fid::TmPosix) {
      core::PosixTz tz;
      std::string err;
      if (!core::parsePosixTz(t, tz, &err)) return FormResult::stay(ui::tr(ui::A::InvalidRule) + err, true);
      return result(s_.setTimezone("Custom", t), ui::tr(ui::A::TimeZoneRuleSaved));
    }
    if (sec_ == SettingsSection::Wifi && id == fid::WfManualSsid) {
      if (t.empty() || t.size() > 32) return FormResult::stay(ui::tr(ui::A::NetworkName1To32), true);
      pendingSsid_ = t;
      return FormResult::stay(ui::tr(ui::A::NowEnterThePassword));
    }
    if (sec_ == SettingsSection::Wifi && id == fid::WfManualPass) {
      const std::string e = s_.net().saveCredentials(pendingSsid_, t);
      if (!e.empty()) return FormResult::stay(e, true);
      pendingSsid_.clear();
      st.wifiEnabled = true;
      return result(s_.saveSettings(st), ui::tr(ui::A::NetworkSaved));
    }
    if (sec_ == SettingsSection::Notify && id == fid::NoUrl) {
      const std::string e = validateNotifyUrl(t);
      if (!e.empty()) return FormResult::stay(e, true);
      st.notifyUrl = t;
      if (t.empty()) st.notifyEnabled = false;
      return result(s_.saveSettings(st), ui::tr(ui::A::Saved));
    }
    if (sec_ == SettingsSection::Reset && id == fid::RsConfirm) {
      confirm_ = t;
      if (t != "RESET") return FormResult::stay(ui::tr(ui::A::TypeRESETInCapitals), true);
    }
    return FormResult::stay();
  }

  FormResult action(int id) override {
    switch (sec_) {
      case SettingsSection::Time:
        if (id == fid::TmSync) {
          const std::string e = job_.start(s_.net(), NetJob::SyncTime);
          return e.empty() ? FormResult::stay(ui::tr(ui::A::StartingEllipsis)) : FormResult::stay(e, true);
        }
        break;
      case SettingsSection::Sound:
        if (id == fid::SnPlay) {
          if (!s_.hooks().testAlert) return FormResult::stay(ui::tr(ui::A::NoSoundOutputOnThis), true);
          core::PersonAlert a;
          a.pattern = static_cast<core::PatternId>(testPattern_);
          a.volume = static_cast<uint8_t>(testVolume_);
          s_.hooks().testAlert(a);
          return FormResult::stay(ui::tr(ui::A::PlayingTestSound));
        }
        break;
      case SettingsSection::Lock:
        if (id == fid::LkSet) return pin_.start();
        if (id == fid::LkClear) return result(s_.setAdminPin(""), ui::tr(ui::A::PINRemoved));
        break;
      case SettingsSection::Backup: {
        if (id == fid::BkNow) {
          std::string name;
          const std::string e = s_.backupNow(&name);
          return e.empty() ? FormResult::stay(ui::tr(ui::A::Saved2) + name) : FormResult::stay(e, true);
        }
        const int i = id - fid::BkItem0;
        if (i >= 0 && i < static_cast<int>(files_.size())) {
          const std::string e = s_.restore(files_[static_cast<size_t>(i)]);
          if (!e.empty() && e.rfind(ui::tr(ui::A::Restored), 0) != 0) return FormResult::stay(e, true);
          return FormResult::closeAll(e.empty() ? ui::tr(ui::A::BackupRestored) : e);
        }
        break;
      }
      case SettingsSection::Wifi:
        if (id == fid::WfPhone) {
          const std::string e = s_.net().startProvisioning();
          return e.empty() ? FormResult::stay(ui::tr(ui::A::HotspotOnJoinItWith)) : FormResult::stay(e, true);
        }
        if (id == fid::WfPhoneStop) {
          const std::string e = s_.net().stopProvisioning();
          return e.empty() ? FormResult::stay(ui::tr(ui::A::HotspotOff)) : FormResult::stay(e, true);
        }
        if (id == fid::WfTest) {
          const std::string e = job_.start(s_.net(), NetJob::TestConnection);
          return e.empty() ? FormResult::stay(ui::tr(ui::A::StartingEllipsis)) : FormResult::stay(e, true);
        }
        if (id == fid::WfForget) {
          const std::string e = s_.net().forgetCredentials();
          return e.empty() ? FormResult::stay(ui::tr(ui::A::NetworkForgotten)) : FormResult::stay(e, true);
        }
        break;
      case SettingsSection::Portal:
        if (id == fid::PoStart) {
          const std::string e = s_.net().startPortal();
          return e.empty() ? FormResult::stay(ui::tr(ui::A::PortalStarted)) : FormResult::stay(e, true);
        }
        if (id == fid::PoStop) {
          const std::string e = s_.net().stopPortal();
          return e.empty() ? FormResult::stay(ui::tr(ui::A::PortalStoppedRadioOff)) : FormResult::stay(e, true);
        }
        break;
      case SettingsSection::Notify:
        if (id == fid::NoTest) {
          const std::string e = s_.net().sendTestNotification();
          return e.empty() ? FormResult::stay(ui::tr(ui::A::TestSent)) : FormResult::stay(e, true);
        }
        break;
      case SettingsSection::Update:
        if (id == fid::UpPortal) return FormResult::replace(makeSettingsSection(s_, SettingsSection::Portal));
        break;
      case SettingsSection::Reset:
        if (id == fid::RsErase) {
          if (confirm_ != "RESET") return FormResult::stay(ui::tr(ui::A::TypeRESETFirst), true);
          const std::string e = s_.factoryReset();
          return FormResult::closeAll(e.empty() ? ui::tr(ui::A::EverythingErased) : e);
        }
        break;
      default: break;
    }
    return FormResult::stay();
  }

 private:
  JobWatch job_;
  AdminService& s_;
  SettingsSection sec_;
  int testPattern_ = 0, testVolume_ = 70;
  std::vector<uint8_t> quietIds_;
  std::vector<std::string> files_;
  std::string confirm_;
  std::string pendingSsid_;
  PinFlow pin_{900, 901, ""};
};

// ---------- Setup wizard ----------
class WizardPage : public FormProvider {
 public:
  enum class Step : int { Safety = 0, Language, TimeZone, Wifi, Clock, Profile, Pin, Import, Done };
  static constexpr int kSteps = 9;
  explicit WizardPage(AdminService& s) : s_(s) {
    const core::Database db = s_.database();
    tzIdx_ = core::tzPresetIndex(db.settings.tzName);
    if (tzIdx_ < 0) tzIdx_ = core::kDefaultTzPreset;
    posix_ = core::tzPreset(tzIdx_).posix;
    if (!db.profiles.empty()) {
      profile_ = db.profiles.front();
      profileSaved_ = true;
    }
  }
  bool poll(FormResult& out) override {
    NetJobState st;
    const int r = job_.poll(s_.net(), st);
    if (r == 0) return false;
    if (r == 1) {
      out = FormResult::stay();
      return true;
    }
    if (!st.error.empty()) {
      out = FormResult::stay(st.error, true);
      return true;
    }
    clockSet_ = true;
    out = FormResult::stay(ui::tr(ui::A::TimeSynchronised));
    return true;
  }
  std::string title() const override {
    static const char* kT[] = {ui::tr(ui::A::Welcome), ui::tr(ui::A::Language), ui::tr(ui::A::TimeZone), ui::tr(ui::A::WiFiOptional), ui::tr(ui::A::Clock), ui::tr(ui::A::FirstPerson),
                               ui::tr(ui::A::PINOptional), ui::tr(ui::A::ImportOptional), ui::tr(ui::A::Ready2)};
    return fmt("%s  %d/%d", kT[static_cast<int>(step_)], static_cast<int>(step_) + 1, kSteps);
  }
  bool canGoBack() const override { return false; }
  Step step() const { return step_; }

  void build(std::vector<FormField>& out) override {
    switch (step_) {
      case Step::Safety:
        out.push_back(info(20, ui::tr(ui::A::RxNudgeRemindsYouToTake)));
        out.push_back(info(21, ui::tr(ui::A::ItIsNotAMedical)));
        out.push_back(info(22, ui::tr(ui::A::DoctorsAndPharmacistsInstructions)));
        out.push_back(toggle(fid::WzAccept, ui::tr(ui::A::IUnderstand), accepted_));
        break;
      case Step::Language:
        out.push_back(choice(fid::WzLanguage, ui::tr(ui::A::Language), 0, {ui::tr(ui::A::English)}));
        out.push_back(info(20, ui::tr(ui::A::MoreLanguagesStringsTableLater)));
        break;
      case Step::TimeZone: {
        out.push_back(choice(fid::WzTz, ui::tr(ui::A::TimeZone), tzIdx_, tzOptions()));
        FormField px = text(fid::WzPosix, ui::tr(ui::A::POSIXRule), posix_, 64);
        px.enabled = tzIdx_ >= core::tzPresetCount();
        out.push_back(px);
        break;
      }
      case Step::Wifi: {
        const NetStatus ns = s_.net().status();
        out.push_back(info(fid::WzWifiStatus, ui::tr(ui::A::Network), ns.configured ? ns.ssid : ui::tr(ui::A::NotSetUp)));
        if (ns.provisioning) {
          out.push_back(info(fid::WzWifiAp, ui::tr(ui::A::N1JoinHotspot), ns.apSsid));
          out.push_back(info(fid::WzWifiApPass, ui::tr(ui::A::Password), ns.apPass));
          out.push_back(info(fid::WzWifiUrl, ui::tr(ui::A::N2Open), "http://192.168.4.1"));
          out.push_back(act(fid::WzWifiStop, ui::tr(ui::A::StopHotspot)));
        } else {
          out.push_back(act(fid::WzWifiPhone, ui::tr(ui::A::SetUpWithPhone), ns.available ? ui::tr(ui::A::Hotspot) : ui::tr(ui::A::NotAvailable)));
          out.push_back(info(20, ui::tr(ui::A::OptionalWiFiKeepsThe)));
        }
        break;
      }
      case Step::Clock: {
        const NetStatus ns = s_.net().status();
        // With Wi-Fi the internet time is the first, filled button: manual entry is the fallback.
        if (ns.configured) {
          FormField f = act(fid::WzNtp, ui::tr(ui::A::GetTimeFromInternet),
                            job_.busy() ? fmt(ui::tr(ui::A::SyncingTimeN), job_.elapsed()) : clockSet_ ? ui::tr(ui::A::Done2) : "");
          f.enabled = !job_.busy();
          f.primary = !clockSet_;
          out.push_back(f);
          out.push_back(info(fid::WzClockHint, ui::tr(ui::A::RecommendedWiFiTime)));
        }
        {
          const ui::DateTimeValue dv = clockDefault(s_);
          FormField f = dateTimeF(fid::WzClock, ns.configured ? ui::tr(ui::A::OrSetByHand) : ui::tr(ui::A::SetDateAndTime), dv.day, dv.minute);
          f.value = clockSet_ ? std::string(ui::tr(ui::A::Done2)) : std::string(" ");
          out.push_back(f);
        }
        if (clockSet_ || !s_.clockUnset()) {
          const core::LocalTime lt = s_.localNow();
          out.push_back(info(fid::WzClockNow, ui::tr(ui::A::Now), fmtShortDate(lt.days) + " " + fmtHm(lt.secOfDay / 60)));
        }
        if (!ns.configured) out.push_back(info(fid::WzNetNote, ui::tr(ui::A::WiFiTimeSyncSettings)));
        break;
      }
      case Step::Profile: {
        FormField n = text(fid::WzName, ui::tr(ui::A::Name), profile_.name);
        if (profile_.name.empty()) n.value = ui::tr(ui::A::Required);
        out.push_back(n);
        FormField i = text(fid::WzInitials, ui::tr(ui::A::Initials), profile_.initials, 8);
        if (profile_.initials.empty()) i.value = ui::tr(ui::A::FromName);
        out.push_back(i);
        out.push_back(info(20, ui::tr(ui::A::AddMorePeopleLaterMenu)));
        break;
      }
      case Step::Pin:
        out.push_back(info(20, ui::tr(ui::A::APINLocksTheEditors)));
        out.push_back(act(fid::WzPin, s_.adminPinSet() ? ui::tr(ui::A::ChangePIN) : ui::tr(ui::A::SetPIN), s_.adminPinSet() ? ui::tr(ui::A::Set) : ui::tr(ui::A::None)));
        break;
      case Step::Import: {
        files_ = s_.backups();
        std::vector<std::string> o{ui::tr(ui::A::StartFresh)};
        for (size_t i = 0; i < files_.size() && i < 20; ++i) o.push_back(files_[i]);
        out.push_back(choice(fid::WzImport, ui::tr(ui::A::RestoreFromSD), importIdx_, o));
        out.push_back(info(20, s_.sdAvailable() ? (files_.empty() ? ui::tr(ui::A::NoBackupsFoundOnThe) : ui::tr(ui::A::RestoringReplacesThePersonAbove))
                                                : ui::tr(ui::A::NoSDCardInserted)));
        break;
      }
      case Step::Done:
        out.push_back(info(20, ui::tr(ui::A::SetupIsComplete)));
        out.push_back(info(21, ui::tr(ui::A::AddMedicationsMenuMedications)));
        break;
    }
  }

  FormResult setValue(int id, int v) override {
    if (id == fid::WzAccept) accepted_ = v != 0;
    else if (id == fid::WzTz) {
      tzIdx_ = v;
      if (v < core::tzPresetCount()) posix_ = core::tzPreset(v).posix;
    } else if (id == fid::WzClock) {
      const std::string e = setClockFromEntry(s_, v);
      if (!e.empty()) return FormResult::stay(cap(e), true);
      clockSet_ = true;
      return FormResult::stay(ui::tr(ui::A::ClockSet));
    } else if (id == fid::WzImport) importIdx_ = v;
    return FormResult::stay();
  }
  FormResult setText(int id, const std::string& t) override {
    if (pin_.handles(id)) return pin_.step(id, t, [this](const std::string& p) { return s_.setAdminPin(p); });
    if (id == fid::WzPosix) {
      core::PosixTz tz;
      std::string err;
      if (!core::parsePosixTz(t, tz, &err)) return FormResult::stay(ui::tr(ui::A::InvalidRule) + err, true);
      posix_ = t;
    } else if (id == fid::WzName) profile_.name = t;
    else if (id == fid::WzInitials) profile_.initials = t;
    return FormResult::stay();
  }
  FormResult action(int id) override {
    if (id == fid::WzWifiPhone) {
      const std::string e = s_.net().startProvisioning();
      return e.empty() ? FormResult::stay(ui::tr(ui::A::HotspotOn)) : FormResult::stay(e, true);
    }
    if (id == fid::WzWifiStop) return result(s_.net().stopProvisioning(), ui::tr(ui::A::HotspotOff));
    if (id == fid::WzNtp) {
      const std::string e = job_.start(s_.net(), NetJob::SyncTime);
      return e.empty() ? FormResult::stay(ui::tr(ui::A::StartingEllipsis)) : FormResult::stay(e, true);
    }
    if (id == fid::WzPin) return pin_.start();
    if (id == fid::FtBack) {
      if (step_ != Step::Safety) step_ = static_cast<Step>(static_cast<int>(step_) - 1);
      return FormResult::stay();
    }
    if (id != fid::FtNext) return FormResult::stay();
    switch (step_) {
      case Step::Safety:
        if (!accepted_) return FormResult::stay(ui::tr(ui::A::PleaseConfirmIUnderstand), true);
        break;
      case Step::Language: break;
      case Step::Wifi:
        if (s_.net().status().provisioning) s_.net().stopProvisioning();
        break;
      case Step::TimeZone: {
        const bool custom = tzIdx_ >= core::tzPresetCount();
        const std::string e = s_.setTimezone(custom ? "Custom" : core::tzPreset(tzIdx_).name, posix_);
        if (!e.empty()) return FormResult::stay(cap(e), true);
        break;
      }
      case Step::Clock:
        // Never set the clock silently from a default: the user picks internet time or enters it.
        if (!clockSet_ && s_.clockUnset()) return FormResult::stay(ui::tr(ui::A::SetTheDateAndTimeFirst), true);
        break;
      case Step::Profile: {
        if (profile_.name.empty()) return FormResult::stay(ui::tr(ui::A::EnterAName), true);
        if (profile_.initials.empty()) profile_.initials = initialsFrom(profile_.name);
        const std::string e = s_.saveProfile(profile_);
        if (!e.empty()) return FormResult::stay(cap(e), true);
        profileSaved_ = true;
        break;
      }
      case Step::Pin: break;
      case Step::Import:
        if (importIdx_ > 0 && importIdx_ <= static_cast<int>(files_.size())) {
          const std::string e = s_.restore(files_[static_cast<size_t>(importIdx_ - 1)]);
          if (!e.empty() && e.rfind(ui::tr(ui::A::Restored), 0) != 0) return FormResult::stay(e, true);
          return FormResult::closeAll(e.empty() ? ui::tr(ui::A::BackupRestored) : e);
        }
        break;
      case Step::Done: {
        const std::string e = s_.markSetupDone();
        if (!e.empty()) return FormResult::stay(cap(e), true);
        return FormResult::closeAll(ui::tr(ui::A::SetupComplete));
      }
    }
    step_ = static_cast<Step>(static_cast<int>(step_) + 1);
    // Wi-Fi is set up and the clock is unknown: fetch the time right away (runs in the background).
    if (step_ == Step::Clock && !clockSet_ && s_.clockUnset() && s_.net().status().configured && !job_.busy()) {
      if (job_.start(s_.net(), NetJob::SyncTime).empty()) return FormResult::stay(ui::tr(ui::A::StartingEllipsis));
    }
    return FormResult::stay();
  }
  void footer(std::vector<FooterButton>& out) override {
    if (step_ != Step::Safety) out.push_back({fid::FtBack, ui::tr(ui::A::Back), false});
    const char* next = step_ == Step::Done ? ui::tr(ui::A::Start2) : (step_ == Step::Pin || step_ == Step::Import) ? ui::tr(ui::A::NextSkip) : ui::tr(ui::A::Next);
    if (step_ == Step::Import && importIdx_ > 0) next = ui::tr(ui::A::Restore);
    out.push_back({fid::FtNext, next, true});
  }

 private:
  JobWatch job_;
  AdminService& s_;
  Step step_ = Step::Safety;
  bool accepted_ = false;
  int tzIdx_ = 0;
  std::string posix_;
  bool clockSet_ = false;
  core::Profile profile_;
  bool profileSaved_ = false;
  int importIdx_ = 0;
  std::vector<std::string> files_;
  PinFlow pin_{900, 901, ""};
};

}  // namespace

std::string phaseSummary(const core::Phase& ph) {
  const core::ScheduleRule& r = ph.rule;
  if (r.type == core::ScheduleType::Prn) return fmt(ui::tr(ui::A::AsNeededMaxN24h), r.prnMaxPer24h);
  std::string times;
  for (size_t i = 0; i < r.times.size() && i < 3; ++i) {
    if (i) times += ", ";
    const core::DoseTime& t = r.times[i];
    times += t.routine == core::Routine::None ? fmtHm(t.minute) : core::routineName(t.routine);
  }
  if (r.times.size() > 3) times += fmt(" +%d", static_cast<int>(r.times.size()) - 3);
  switch (r.type) {
    case core::ScheduleType::Weekdays: {
      int n = 0;
      for (int b = 0; b < 7; ++b) n += (r.weekdayMask >> b) & 1;
      return fmt(ui::tr(ui::A::NDaysWk), n) + times;
    }
    case core::ScheduleType::EveryN: return fmt(ui::tr(ui::A::EveryNN), r.intervalN, r.intervalWeeks ? ui::tr(ui::A::Wk) : ui::tr(ui::A::D)) + times;
    case core::ScheduleType::SpecificDates: return fmt(ui::tr(ui::A::NDates), static_cast<int>(r.dates.size())) + times;
    case core::ScheduleType::Cycle: return fmt(ui::tr(ui::A::NOnNOff), r.cycleOn, r.cycleOff) + times;
    default: return ui::tr(ui::A::Daily) + times;
  }
}

std::unique_ptr<FormProvider> makeMenuPage(AdminService& s) { return std::unique_ptr<FormProvider>(new MenuPage(s)); }
std::unique_ptr<FormProvider> makeMedListPage(AdminService& s) { return std::unique_ptr<FormProvider>(new MedListPage(s)); }
std::unique_ptr<FormProvider> makeMedEditPage(AdminService& s, const core::Medication& m, bool isNew) {
  return std::unique_ptr<FormProvider>(new MedEditPage(s, m, isNew));
}
std::unique_ptr<FormProvider> makeProfileListPage(AdminService& s) { return std::unique_ptr<FormProvider>(new ProfileListPage(s)); }
std::unique_ptr<FormProvider> makeProfileEditPage(AdminService& s, const core::Profile& p, bool isNew) {
  return std::unique_ptr<FormProvider>(new ProfileEditPage(s, p, isNew));
}
std::unique_ptr<FormProvider> makeSettingsPage(AdminService& s) { return std::unique_ptr<FormProvider>(new SettingsPage(s)); }
std::unique_ptr<FormProvider> makeSettingsSection(AdminService& s, SettingsSection sec) {
  return std::unique_ptr<FormProvider>(new SectionPage(s, sec));
}
std::unique_ptr<FormProvider> makeWizard(AdminService& s) { return std::unique_ptr<FormProvider>(new WizardPage(s)); }

}  // namespace app
