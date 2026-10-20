#pragma once
// Form providers for the medication manager, profile manager, settings and the
// first-boot setup wizard. All edits go through AdminService.
#include <memory>
#include <string>

#include "app_core/admin_service.h"
#include "ui_core/form.h"

namespace app {

std::unique_ptr<ui::FormProvider> makeMenuPage(AdminService& s);
std::unique_ptr<ui::FormProvider> makeMedListPage(AdminService& s);
std::unique_ptr<ui::FormProvider> makeMedEditPage(AdminService& s, const core::Medication& m, bool isNew);
std::unique_ptr<ui::FormProvider> makeProfileListPage(AdminService& s);
std::unique_ptr<ui::FormProvider> makeProfileEditPage(AdminService& s, const core::Profile& p, bool isNew);
std::unique_ptr<ui::FormProvider> makeSettingsPage(AdminService& s);
enum class SettingsSection : int {
  Time = 1, Sound, Quiet, Display, Lock, Battery, Storage, Backup, Wifi, Portal, Notify, Update, About, Reset
};
std::unique_ptr<ui::FormProvider> makeSettingsSection(AdminService& s, SettingsSection sec);
std::unique_ptr<ui::FormProvider> makeWizard(AdminService& s);

// One-line schedule summary ("Daily 08:00, 20:00").
std::string phaseSummary(const core::Phase& ph);
// "" when the caregiver push URL is acceptable (empty clears it).
std::string validateNotifyUrl(const std::string& url);

// Field ids used by tests and previews.
namespace fid {
// medication editor
enum : int { MedName = 1, MedPerson, MedDose, MedForm, MedInstr, MedNotes, MedTemplate, MedPhases, MedStart, MedHasEnd,
             MedEnd, MedCourse, MedCritical, MedGrace, MedOnTime, MedRepeatEvery, MedRepeats, MedSound, MedVolume,
             MedTrack, MedSupply, MedLow, MedHasRefill, MedRefill, MedArchive, MedTimesNote,
             MedTime0 = 200, MedAddTime = 220 };
// one dose time (opened from a Times list)
enum : int { TeAnchor = 1, TeTime, TeOffset, TeDelete };
// phase editor
enum : int { PhHasFrom = 1, PhFrom, PhHasTo, PhTo, PhDose, PhType, PhTime0 = 20, PhAddTime = 40, PhWeekday0 = 60, PhEveryN = 70, PhEveryUnit, PhDateCount, PhCycleOn, PhCycleOff,
             PhPrnInterval, PhPrnMax, PhDate0 = 80 };
// phase list
enum : int { PlAdd = 1, PlRemove = 2, PlPhase0 = 100 };
// profile editor
enum : int { PrName = 1, PrInitials, PrPrivacy, PrQuiet, PrQuietFrom, PrQuietTo, PrRoutine0 = 10, PrSound = 20, PrVolume,
             PrTest, PrPin = 30, PrClearPin };
// wizard
enum : int { WzAccept = 1, WzLanguage, WzTz, WzPosix, WzClock, WzClockNow, WzClockHint, WzNetNote, WzName, WzInitials, WzPin,
             WzImport, WzWifiStatus, WzWifiPhone, WzWifiStop, WzWifiAp, WzWifiApPass, WzWifiUrl, WzNtp };
// settings sub pages
enum : int { TmZone = 1, TmPosix, TmNow, TmClock, TmAuto, TmSync };
enum : int { SnInfo = 1, SnLadderEvery, SnLadderCount, SnPattern, SnVolume, SnPlay, SnNote };
enum : int { DsLarge = 1, DsFullEvery, DsNightly };
enum : int { LkStatus = 1, LkSet, LkClear, LkNote };
enum : int { BkSd = 1, BkNow, BkAuto, BkAutoLast, BkAutoStatus, BkAutoFile, BkItem0 = 100 };
enum : int { WfStatus = 1, WfEnabled, WfSsid, WfPhone, WfPhoneStop, WfApName, WfApPass, WfApUrl, WfManualSsid, WfManualPass,
             WfTest, WfForget };
enum : int { PoStatus = 1, PoStart, PoStop, PoUrl, PoIp, PoCode, PoTime, PoNoTls };
enum : int { NoEnabled = 1, NoUrl, NoNames, NoTest, NoNote };
enum : int { UpVersion = 1, UpHow, UpVerify, UpPortal };
enum : int { RsWarn = 1, RsConfirm, RsErase };
// list pages
enum : int { ListShowArchived = 1, ListItem0 = 100 };
// footer
enum : int { FtSave = 1000, FtCancel, FtAdd, FtDone, FtNext, FtBack };
}  // namespace fid

class AdminFormFactory : public ui::IFormFactory {
 public:
  explicit AdminFormFactory(AdminService& s) : s_(s) {}
  std::unique_ptr<ui::FormProvider> menu() override { return makeMenuPage(s_); }
  std::unique_ptr<ui::FormProvider> wizard() override { return makeWizard(s_); }
  bool setupNeeded() override { return s_.setupNeeded(); }
  ui::IPinGate* gate() override { return &s_; }

 private:
  AdminService& s_;
};

}  // namespace app
