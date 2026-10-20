// Phase 5: keyboard model, form host, medication/profile editors, PIN gating,
// setup wizard, settings, factory reset and SD backup/restore round trip.
#include <unity.h>

#include <cstdio>
#include <cstdlib>
#include <memory>

#include "../common/fake_net.h"
#include "../common/fixtures.h"
#include "app_core/admin_screens.h"
#include "app_core/admin_service.h"
#include "app_core/app_port.h"
#include "core/demo_data.h"
#include "core/presets.h"
#include "storage/export.h"
#include "storage/log_store.h"
#include "storage/maintenance.h"
#include "storage/mem_fs.h"
#include "storage/repository.h"
#include "ui_core/controller.h"
#include "ui_core/form_host.h"
#include "ui_core/keyboard.h"
#include "ui_core/refresh.h"

using namespace ui;
namespace fid = app::fid;

void setUp() {}
void tearDown() {}

static int64_t berlin(int d, int h, int m = 0) { return fx::berlinLocal(2026, 10, d, h, m); }

struct Rig {
  core::FakeClock clk;
  core::Database db;
  storage::MemFs internal, sd;
  storage::FsRepository repo{internal};
  storage::FsDoseLog log{internal};
  std::unique_ptr<app::AppModel> model;
  FakeNetService net;
  std::unique_ptr<app::AdminService> svc;
  std::unique_ptr<app::AdminFormFactory> factory;
  std::unique_ptr<app::ModelPort> port;
  std::unique_ptr<UiController> ui;
  FormHost host;
  uint8_t buf[Canvas::kBytes];
  Canvas canvas{buf};
  uint32_t ms = 1000;
  int resets = 0;
  bool large = false;
  std::vector<core::PersonAlert> played;
  storage::AutoBackupState ab;  // what the device keeps in NVS

  static bool portSetClock(void* ctx, int64_t utc) { return static_cast<Rig*>(ctx)->setClock(utc); }
  bool setClock(int64_t utc) {
    clk.set(utc);
    model->status.clockUnset = false;
    if (!model->engineRunning()) model->startEngine(utc);
    return true;
  }

  explicit Rig(int64_t now, bool demo = true) : clk(now) {
    if (demo) {
      const core::LocalTime lt = core::utcToLocal(fx::berlin(), now);
      db = core::makeDemoDatabase(lt.days - 20);
      db.settings.setupDone = true;
    }
    model.reset(new app::AppModel(clk, db, &log));
    if (demo) {
      model->startEngine(now);
      model->process();
    }
    app::AdminHooks h;
    h.setClockUtc = [this](int64_t utc) { return setClock(utc); };
    h.afterFactoryReset = [this]() { ++resets; };
    h.autoBackupState = [this]() { return ab; };
    h.setLargeText = [this](bool on) { large = on; };
    h.largeText = [this]() { return large; };
    h.testAlert = [this](const core::PersonAlert& a) { played.push_back(a); };
    h.entropy = []() { return 0x1234ull; };
    net.writeClock = [this](int64_t utc) { return setClock(utc); };
    svc.reset(new app::AdminService(*model, &repo, &internal, &sd, net, nullptr, h, "0.5.0"));
    factory.reset(new app::AdminFormFactory(*svc));
    host.setGate(svc.get());
    port.reset(new app::ModelPort(*model, &Rig::portSetClock, this));
    ui.reset(new UiController(*port));
    ui->setFormFactory(factory.get());
  }
  void open(std::unique_ptr<FormProvider> p) { host.open(std::move(p), ms); }
  // Lets a background network job finish (one UI tick per second, as on the device).
  void finishJob() {
    for (int i = 0; i < 5; i++) {
      ms += 1000;
      host.tick(ms);
    }
  }
  void draw() { host.render(canvas, themeFor(false)); }
  void step(uint32_t dt = 100) {
    ms += dt;
    model->process();
    ui->tick(ms);
    ui->render(canvas);
  }
  bool activate(int id) { return host.activateField(id, ms += 200); }
  bool type(int id, const std::string& t) { return activate(id) && host.submitKeyboard(t, ms += 200); }
  // replaces the field's current text
  bool retype(int id, const std::string& t) {
    if (!activate(id)) return false;
    while (!host.keyboard().text().empty()) host.keyboard().backspace();
    return host.submitKeyboard(t, ms += 200);
  }
  bool stepper(int id, int v) { return activate(id) && host.entrySave(v, ms += 200); }
  bool pick(int id, int idx) { return activate(id) && host.pickOption(idx, ms += 200); }
  bool footer(int id) { return host.pressFooter(id, ms += 200); }
  const FormField* field(int id) {
    for (const auto& f : host.fields())
      if (f.id == id) return &f;
    return nullptr;
  }
};

// ---------------------------------------------------------------- keyboard
void test_keyboard_utf8_and_layers() {
  KeyboardModel kb("", 12, false, false);
  TEST_ASSERT_EQUAL(static_cast<int>(KbAction::Edited), static_cast<int>(kb.typeText("Müßig")));
  TEST_ASSERT_EQUAL_STRING("Müßig", kb.text().c_str());
  TEST_ASSERT_EQUAL(5u, kb.codepoints());
  kb.backspace();
  kb.backspace();
  kb.backspace();  // removes 'ß' (two bytes) as one character
  TEST_ASSERT_EQUAL_STRING("Mü", kb.text().c_str());
  TEST_ASSERT_EQUAL(static_cast<int>(KbAction::Edited), static_cast<int>(kb.typeText("Ärger ö")));
  TEST_ASSERT_EQUAL(static_cast<int>(KbAction::Rejected), static_cast<int>(kb.typeText("xyz")));  // 12-byte limit
  TEST_ASSERT_TRUE(kb.text().size() <= 12);
  // US QWERTY main layout
  KeyboardModel k2("", 48, false, false);
  auto pressText = [&](const std::string& t) {
    for (size_t i = 0; i < k2.keys().size(); i++)
      if (k2.keys()[i].kind == KeyKind::Char && k2.keys()[i].text == t) return k2.press(i), true;
    return false;
  };
  auto pressLayer = [&](const std::string& target) {
    for (size_t i = 0; i < k2.keys().size(); i++)
      if (k2.keys()[i].kind == KeyKind::Layer && k2.keys()[i].text == target) return k2.press(i), true;
    return false;
  };
  auto pressShift = [&]() {
    for (size_t i = 0; i < k2.keys().size(); i++)
      if (k2.keys()[i].kind == KeyKind::Shift) return k2.press(i), true;
    return false;
  };
  const char* row1[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"};
  for (int i = 0; i < 10; i++) TEST_ASSERT_EQUAL_STRING(row1[i], k2.keys()[static_cast<size_t>(i)].text.c_str());
  TEST_ASSERT_EQUAL_STRING("a", k2.keys()[10].text.c_str());
  TEST_ASSERT_EQUAL_STRING("z", k2.keys()[19].text.c_str());  // bottom row starts with z (not y)
  for (const auto& k : k2.keys()) TEST_ASSERT_TRUE(k.text != "ö" && k.text != "ß");  // no accents on the main layer
  // accents: Symbols -> àé layer; one-shot shift gives capitals
  TEST_ASSERT_TRUE(pressText("m"));
  TEST_ASSERT_TRUE(pressLayer("sym"));
  TEST_ASSERT_EQUAL(static_cast<int>(KbLayer::Symbols), static_cast<int>(k2.layer()));
  TEST_ASSERT_TRUE(pressLayer("acc"));
  TEST_ASSERT_EQUAL(static_cast<int>(KbLayer::Accents), static_cast<int>(k2.layer()));
  TEST_ASSERT_TRUE(pressText("ü"));
  TEST_ASSERT_TRUE(pressShift());
  TEST_ASSERT_EQUAL(static_cast<int>(KbLayer::AccentsUpper), static_cast<int>(k2.layer()));
  TEST_ASSERT_TRUE(pressText("Ö"));
  TEST_ASSERT_EQUAL(static_cast<int>(KbLayer::Accents), static_cast<int>(k2.layer()));
  TEST_ASSERT_TRUE(pressText("ß"));
  TEST_ASSERT_TRUE(pressLayer("sym"));
  TEST_ASSERT_TRUE(pressLayer("abc"));
  TEST_ASSERT_TRUE(pressShift());
  TEST_ASSERT_TRUE(pressText("Y"));
  TEST_ASSERT_EQUAL(static_cast<int>(KbLayer::Lower), static_cast<int>(k2.layer()));
  TEST_ASSERT_EQUAL_STRING("müÖßY", k2.text().c_str());
  // every key is at least 72 px tall and inside the key area
  for (const auto& k : k2.keys()) {
    TEST_ASSERT_TRUE(k.r.h >= 72);
    TEST_ASSERT_TRUE(k.r.y >= kKbTop);
    TEST_ASSERT_TRUE(k.r.bottom() <= Canvas::kH);
  }
}

void test_keyboard_numeric_and_password() {
  KeyboardModel kb("", 8, true, true);
  TEST_ASSERT_EQUAL(static_cast<int>(KbLayer::Numeric), static_cast<int>(kb.layer()));
  TEST_ASSERT_EQUAL(static_cast<int>(KbAction::Rejected), static_cast<int>(kb.typeText("a")));
  kb.typeText("1234");
  TEST_ASSERT_EQUAL_STRING("****", kb.display().c_str());
  TEST_ASSERT_EQUAL_STRING("1234", kb.text().c_str());
  for (const auto& k : kb.keys()) TEST_ASSERT_TRUE(k.r.w >= 83 && k.r.h >= 72);
}

void test_keyboard_keypress_is_partial_update() {
  Rig r(berlin(14, 12));
  core::Medication m = r.svc->newMedication(1);
  r.open(app::makeMedEditPage(*r.svc, m, true));
  TEST_ASSERT_TRUE(r.activate(fid::MedName));
  TEST_ASSERT_EQUAL(static_cast<int>(FormHost::Mode::Keyboard), static_cast<int>(r.host.mode()));
  r.draw();
  std::vector<uint8_t> before(r.buf, r.buf + Canvas::kBytes);
  KeyboardModel& kb = r.host.keyboard();
  size_t idx = 0;
  for (size_t i = 0; i < kb.keys().size(); i++)
    if (kb.keys()[i].text == "a") idx = i;
  InputEvent e;
  e.type = InputType::Tap;
  e.x = static_cast<int16_t>(kb.keys()[idx].r.cx());
  e.y = static_cast<int16_t>(kb.keys()[idx].r.cy());
  r.host.handle(e, r.ms += 100);
  r.draw();
  std::vector<Rect> dirty;
  diffFrames(before.data(), r.buf, dirty);
  TEST_ASSERT_FALSE(dirty.empty());
  const Rect field = kbFieldRect(), keys = kbKeysRect();
  for (const Rect& d : dirty) {
    TEST_ASSERT_TRUE_MESSAGE(d.y >= field.y - 16, "keypress must not touch the title bar");
    TEST_ASSERT_TRUE(d.bottom() <= keys.bottom() + 16);
  }
  TEST_ASSERT_EQUAL(101, r.host.screenType());  // same screen type: fast refresh
  TEST_ASSERT_EQUAL_STRING("a", kb.text().c_str());
}

// ---------------------------------------------------------------- medication editor
void test_med_editor_validation_and_save() {
  Rig r(berlin(14, 12));
  const size_t n0 = r.svc->database().meds.size();
  r.open(app::makeMedEditPage(*r.svc, r.svc->newMedication(1), true));
  TEST_ASSERT_TRUE(r.footer(fid::FtSave));
  TEST_ASSERT_TRUE(r.host.toastIsError());  // no name
  TEST_ASSERT_TRUE(r.type(fid::MedName, "Ibuprofen ä"));
  TEST_ASSERT_TRUE(r.type(fid::MedDose, "400 mg"));
  TEST_ASSERT_TRUE(r.pick(fid::MedTemplate, 1 + static_cast<int>(core::ScheduleTemplate::TwiceDaily)));
  // on-time window larger than the grace window is refused
  TEST_ASSERT_TRUE(r.stepper(fid::MedGrace, 30));
  TEST_ASSERT_TRUE(r.stepper(fid::MedOnTime, 45));
  TEST_ASSERT_TRUE(r.host.toastIsError());
  // end before start refused
  TEST_ASSERT_TRUE(r.activate(fid::MedHasEnd));
  const int start = r.field(fid::MedStart)->num;
  TEST_ASSERT_TRUE(r.stepper(fid::MedEnd, start - 3));
  TEST_ASSERT_TRUE(r.host.toastIsError());
  TEST_ASSERT_TRUE(r.activate(fid::MedHasEnd));  // off again
  TEST_ASSERT_TRUE(r.activate(fid::MedTrack));
  TEST_ASSERT_TRUE(r.stepper(fid::MedSupply, 20));
  TEST_ASSERT_TRUE(r.pick(fid::MedSound, 3));
  TEST_ASSERT_TRUE(r.footer(fid::FtSave));
  TEST_ASSERT_FALSE_MESSAGE(r.host.toastIsError(), r.host.toast().c_str());
  TEST_ASSERT_EQUAL(0u, r.host.depth());
  const core::Database db = r.svc->database();
  TEST_ASSERT_EQUAL(n0 + 1, db.meds.size());
  const core::Medication& m = db.meds.back();
  TEST_ASSERT_EQUAL_STRING("Ibuprofen ä", m.name.c_str());
  TEST_ASSERT_EQUAL(2u, m.phases[0].rule.times.size());
  TEST_ASSERT_EQUAL(30, m.graceMin);
  TEST_ASSERT_EQUAL(20, m.supply);
  TEST_ASSERT_EQUAL(2, m.alertPattern);
  TEST_ASSERT_TRUE(m.id > 0);
  // persisted
  core::Database loaded;
  r.repo.loadDatabase(loaded);
  TEST_ASSERT_NOT_NULL(loaded.med(m.id));
  // future doses were generated for the new medication
  r.model->process();
  bool gen = false;
  for (const auto& d : r.model->engine().instances()) gen = gen || d.medId == m.id;
  TEST_ASSERT_TRUE(gen);
}

void test_med_schedule_types_and_phases() {
  Rig r(berlin(14, 12));
  const int32_t today = r.svc->today();
  for (int t = 0; t < static_cast<int>(core::ScheduleTemplate::Count); ++t) {
    core::Medication m = r.svc->newMedication(2);
    m.name = core::scheduleTemplateName(static_cast<core::ScheduleTemplate>(t));
    core::applyScheduleTemplate(m, static_cast<core::ScheduleTemplate>(t));
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", r.svc->saveMedication(m).c_str(), m.name.c_str());
  }
  // Phase editor: every schedule type through the UI, then a taper with 2 phases
  core::Medication m = r.svc->newMedication(1);
  m.name = "Prednisolon";
  r.open(app::makeMedEditPage(*r.svc, m, true));
  TEST_ASSERT_TRUE(r.activate(fid::MedPhases));
  TEST_ASSERT_TRUE(r.activate(fid::PlPhase0));
  for (int type = 0; type <= static_cast<int>(core::ScheduleType::Prn); ++type) {
    TEST_ASSERT_TRUE(r.pick(fid::PhType, type));
    TEST_ASSERT_NOT_NULL(r.field(fid::PhType));
    TEST_ASSERT_EQUAL(type, r.field(fid::PhType)->num);
  }
  TEST_ASSERT_NOT_NULL(r.field(fid::PhPrnMax));
  TEST_ASSERT_TRUE(r.pick(fid::PhType, static_cast<int>(core::ScheduleType::Weekdays)));
  for (int d = 0; d < 7; ++d) r.activate(fid::PhWeekday0 + d);  // the last one is refused
  TEST_ASSERT_TRUE(r.host.toastIsError());
  TEST_ASSERT_TRUE(r.pick(fid::PhType, static_cast<int>(core::ScheduleType::Routine)));
  TEST_ASSERT_NOT_NULL(r.field(fid::PhTime0));
  TEST_ASSERT_TRUE(r.activate(fid::PhTime0));  // Time 1 page: anchored to a routine now
  TEST_ASSERT_NOT_NULL(r.field(fid::TeOffset));
  TEST_ASSERT_NULL(r.field(fid::TeTime));
  TEST_ASSERT_TRUE(r.stepper(fid::TeOffset, -30));
  TEST_ASSERT_TRUE(r.footer(fid::FtDone));
  TEST_ASSERT_TRUE(r.type(fid::PhDose, "20 mg"));
  TEST_ASSERT_TRUE(r.footer(fid::FtDone));
  TEST_ASSERT_TRUE(r.activate(fid::PlAdd));
  TEST_ASSERT_TRUE(r.activate(fid::PlPhase0 + 1));
  TEST_ASSERT_TRUE(r.retype(fid::PhDose, "10 mg"));
  TEST_ASSERT_TRUE(r.footer(fid::FtDone));
  TEST_ASSERT_TRUE(r.footer(fid::FtDone));
  TEST_ASSERT_TRUE(r.footer(fid::FtSave));
  TEST_ASSERT_FALSE_MESSAGE(r.host.toastIsError(), r.host.toast().c_str());
  const core::Database db = r.svc->database();
  const core::Medication& s = db.meds.back();
  TEST_ASSERT_EQUAL(2u, s.phases.size());
  TEST_ASSERT_EQUAL(static_cast<int>(core::ScheduleType::Routine), static_cast<int>(s.phases[0].rule.type));
  TEST_ASSERT_EQUAL(-30, s.phases[0].rule.times[0].offsetMin);
  TEST_ASSERT_EQUAL_STRING("10 mg", s.phases[1].doseText.c_str());
  TEST_ASSERT_EQUAL(s.phases[0].toDay + 1, s.phases[1].fromDay);
  TEST_ASSERT_TRUE(s.phases[0].toDay >= today);
}

void test_med_archive_and_list() {
  Rig r(berlin(14, 12));
  const core::Database db0 = r.svc->database();
  const uint16_t id = db0.meds.front().id;
  r.open(app::makeMedListPage(*r.svc));
  TEST_ASSERT_TRUE(r.activate(fid::ListItem0));
  TEST_ASSERT_TRUE(r.activate(fid::MedArchive));  // arms
  TEST_ASSERT_TRUE(r.svc->database().med(id)->active);
  TEST_ASSERT_TRUE(r.activate(fid::MedArchive));  // confirms
  TEST_ASSERT_FALSE(r.svc->database().med(id)->active);
  TEST_ASSERT_EQUAL(1u, r.host.depth());
  for (const auto& f : r.host.fields()) TEST_ASSERT_TRUE(f.label != db0.meds.front().name || f.id < fid::ListItem0);
  TEST_ASSERT_TRUE(r.activate(fid::ListShowArchived));
  bool shown = false;
  for (const auto& f : r.host.fields()) shown = shown || f.label == db0.meds.front().name;
  TEST_ASSERT_TRUE(shown);
  for (const auto& d : r.model->engine().instances())
    if (d.medId == id) TEST_ASSERT_TRUE(d.scheduledUtc <= r.clk.nowUtc());  // no future doses
}

// ---------------------------------------------------------------- profiles and PINs
void test_profile_limit_and_editor() {
  Rig r(berlin(14, 12));
  while (r.svc->database().profiles.size() < core::kMaxProfiles) {
    core::Profile p;
    p.name = "Person";
    p.initials = "P";
    TEST_ASSERT_EQUAL_STRING("", r.svc->saveProfile(p).c_str());
  }
  core::Profile extra;
  extra.name = "Nine";
  extra.initials = "N";
  TEST_ASSERT_FALSE(r.svc->saveProfile(extra).empty());
  r.open(app::makeProfileListPage(*r.svc));
  TEST_ASSERT_TRUE(r.footer(fid::FtAdd));
  TEST_ASSERT_TRUE(r.host.toastIsError());
  // edit the first person: quiet hours, routine, sound, PIN (mismatch then match)
  TEST_ASSERT_TRUE(r.activate(fid::ListItem0));
  TEST_ASSERT_TRUE(r.retype(fid::PrName, "Jürgen Weiß"));
  TEST_ASSERT_TRUE(r.stepper(fid::PrQuietFrom, 23 * 60));
  TEST_ASSERT_TRUE(r.stepper(fid::PrRoutine0 + 1, 8 * 60));
  TEST_ASSERT_TRUE(r.pick(fid::PrSound, 2));
  TEST_ASSERT_TRUE(r.activate(fid::PrTest));
  TEST_ASSERT_EQUAL(1u, r.played.size());
  TEST_ASSERT_TRUE(r.activate(fid::PrPin));
  TEST_ASSERT_TRUE(r.host.submitKeyboard("1357", r.ms += 100));
  TEST_ASSERT_TRUE(r.host.submitKeyboard("1358", r.ms += 100));
  TEST_ASSERT_TRUE(r.host.toastIsError());
  TEST_ASSERT_TRUE(r.activate(fid::PrPin));
  TEST_ASSERT_TRUE(r.host.submitKeyboard("12", r.ms += 100));
  TEST_ASSERT_TRUE(r.host.toastIsError());  // too short
  TEST_ASSERT_TRUE(r.activate(fid::PrPin));
  r.host.submitKeyboard("1357", r.ms += 100);
  r.host.submitKeyboard("1357", r.ms += 100);
  TEST_ASSERT_FALSE(r.host.toastIsError());
  TEST_ASSERT_TRUE(r.footer(fid::FtSave));
  TEST_ASSERT_FALSE_MESSAGE(r.host.toastIsError(), r.host.toast().c_str());
  const core::Database dbp = r.svc->database();
  const core::Profile cp = dbp.profiles.front();
  TEST_ASSERT_EQUAL_STRING("Jürgen Weiß", cp.name.c_str());
  TEST_ASSERT_EQUAL(23 * 60, cp.quietStartMin);
  TEST_ASSERT_EQUAL(8 * 60, cp.routineMin[1]);
  TEST_ASSERT_EQUAL(2, cp.alertPattern);
  TEST_ASSERT_TRUE(core::verifyPin(cp.pinSalt, cp.pinHash, "1357"));
  TEST_ASSERT_TRUE(r.svc->checkPin(PinScope::Profile, 1, "1357"));
  TEST_ASSERT_EQUAL_STRING("", r.svc->setProfilePin(2, "8642").c_str());
  TEST_ASSERT_FALSE(r.svc->checkPin(PinScope::Profile, 2, "1357"));  // one person's PIN does not open another
  TEST_ASSERT_TRUE(r.svc->checkPin(PinScope::Profile, 2, "8642"));
  TEST_ASSERT_FALSE(r.svc->checkPin(PinScope::Admin, 0, "8642"));
}

void test_pin_helpers_and_gate() {
  Rig r(berlin(14, 12));
  TEST_ASSERT_TRUE(core::pinFormatValid("0000"));
  TEST_ASSERT_TRUE(core::pinFormatValid("12345678"));
  TEST_ASSERT_FALSE(core::pinFormatValid("123"));
  TEST_ASSERT_FALSE(core::pinFormatValid("123456789"));
  TEST_ASSERT_FALSE(core::pinFormatValid("12a4"));
  TEST_ASSERT_FALSE(r.svc->pinRequired(PinScope::Admin, 0));
  TEST_ASSERT_FALSE(r.svc->setAdminPin("12").empty());
  TEST_ASSERT_EQUAL_STRING("", r.svc->setAdminPin("2468").c_str());
  TEST_ASSERT_TRUE(r.svc->pinRequired(PinScope::Admin, 0));
  TEST_ASSERT_TRUE(r.svc->pinRequired(PinScope::Profile, 1));  // the admin PIN guards every person
  TEST_ASSERT_TRUE(r.svc->checkPin(PinScope::Profile, 1, "2468"));
  // salt differs per set: the same PIN gives a different hash
  const std::string h1 = r.svc->database().settings.adminPinHash;
  r.svc->setAdminPin("2468");
  TEST_ASSERT_TRUE(h1 != r.svc->database().settings.adminPinHash);
  // the wifi password never leaves the device; the PIN hash does (restore keeps the lock)
  TEST_ASSERT_EQUAL_STRING("", r.svc->setAdminPin("").c_str());
  TEST_ASSERT_FALSE(r.svc->pinRequired(PinScope::Admin, 0));
}

void test_pin_gating_in_host() {
  Rig r(berlin(14, 12));
  r.svc->setAdminPin("2468");
  r.ui->openMenu(r.ms);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Form), static_cast<int>(r.ui->screen()));
  FormHost& h = r.ui->form();
  TEST_ASSERT_EQUAL(static_cast<int>(FormHost::Mode::Pin), static_cast<int>(h.mode()));
  for (int i = 0; i < FormHost::kMaxPinFails; ++i) h.submitKeyboard("0000", r.ms += 100);
  TEST_ASSERT_EQUAL(0u, h.depth());
  h.submitKeyboard("2468", r.ms += 100);  // locked out for 60 s even with the right PIN
  TEST_ASSERT_EQUAL(0u, h.depth());
  r.ms += FormHost::kPinLockMs + 10;
  h.submitKeyboard("2468", r.ms);
  TEST_ASSERT_EQUAL(1u, h.depth());
  TEST_ASSERT_EQUAL_STRING("Menu", h.top()->title().c_str());
  // Settings and editors open without asking again in this session
  TEST_ASSERT_TRUE(h.activateField(3, r.ms += 100));
  TEST_ASSERT_EQUAL(2u, h.depth());
  // idle close relocks
  r.ms += FormHost::kIdleCloseMs + 1000;
  r.ui->tick(r.ms);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
  r.ui->openMenu(r.ms);
  TEST_ASSERT_EQUAL(static_cast<int>(FormHost::Mode::Pin), static_cast<int>(r.ui->form().mode()));
  // cancel returns home
  InputEvent e;
  e.type = InputType::Tap;
  for (const auto& k : r.ui->form().keyboard().keys())
    if (k.kind == KeyKind::Cancel) {
      e.x = static_cast<int16_t>(k.r.cx());
      e.y = static_cast<int16_t>(k.r.cy());
    }
  r.ui->handle(e, r.ms += 100);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
}

// ---------------------------------------------------------------- wizard
void test_wizard_flow() {
  Rig r(berlin(14, 12), false);
  r.model->status.clockUnset = true;
  TEST_ASSERT_TRUE(r.svc->setupNeeded());
  r.step();
  TEST_ASSERT_TRUE(r.ui->wizardActive());  // wizard wins over the clock screen
  FormHost& h = r.ui->form();
  auto next = [&]() { return h.pressFooter(fid::FtNext, r.ms += 100); };
  next();
  TEST_ASSERT_TRUE(h.toastIsError());  // safety notice must be acknowledged
  TEST_ASSERT_FALSE(h.back(r.ms += 100) && h.depth() == 0);
  TEST_ASSERT_EQUAL(1u, h.depth());
  h.activateField(fid::WzAccept, r.ms += 100);
  next();
  TEST_ASSERT_NOT_NULL(h.top());
  TEST_ASSERT_TRUE(h.top()->title().find("Language") == 0);
  next();
  // time zone: Berlin is the default
  bool berlinDefault = false;
  for (const auto& f : h.fields())
    if (f.id == fid::WzTz) berlinDefault = f.num == core::kDefaultTzPreset;
  TEST_ASSERT_TRUE(berlinDefault);
  // custom POSIX: invalid refused, valid accepted
  h.activateField(fid::WzTz, r.ms += 100);
  h.pickOption(core::tzPresetCount(), r.ms += 100);
  h.activateField(fid::WzPosix, r.ms += 100);
  h.submitKeyboard("garbage", r.ms += 100);
  TEST_ASSERT_TRUE(h.toastIsError());
  h.activateField(fid::WzTz, r.ms += 100);
  h.pickOption(core::kDefaultTzPreset, r.ms += 100);
  next();
  TEST_ASSERT_EQUAL_STRING("Europe/Berlin", r.svc->database().settings.tzName.c_str());
  // Wi-Fi (optional): the phone hotspot starts and stops with the step
  TEST_ASSERT_TRUE(h.top()->title().find("Wi-Fi") == 0);
  h.activateField(fid::WzWifiPhone, r.ms += 100);
  TEST_ASSERT_TRUE(r.net.provisioning);
  next();
  TEST_ASSERT_FALSE(r.net.provisioning);
  // clock
  next();
  TEST_ASSERT_TRUE(h.toastIsError());  // never continues with an unset clock
  h.activateField(fid::WzClock, r.ms += 100);
  TEST_ASSERT_EQUAL(static_cast<int>(FormHost::Mode::Entry), static_cast<int>(h.mode()));
  h.entrySave(fx::day(2026, 10, 18) * 1440 + 9 * 60 + 30, r.ms += 100);
  TEST_ASSERT_FALSE(h.toastIsError());
  next();
  TEST_ASSERT_FALSE(r.model->status.clockUnset);
  TEST_ASSERT_EQUAL(fx::berlinLocal(2026, 10, 18, 9, 30), r.clk.nowUtc());
  // first profile
  next();
  TEST_ASSERT_TRUE(h.toastIsError());  // name required
  h.activateField(fid::WzName, r.ms += 100);
  h.submitKeyboard("Anna Müller", r.ms += 100);
  next();
  TEST_ASSERT_EQUAL(1u, r.svc->database().profiles.size());
  TEST_ASSERT_EQUAL_STRING("AM", r.svc->database().profiles[0].initials.c_str());
  // optional PIN
  h.activateField(fid::WzPin, r.ms += 100);
  h.submitKeyboard("9753", r.ms += 100);
  h.submitKeyboard("9753", r.ms += 100);
  TEST_ASSERT_TRUE(r.svc->adminPinSet());
  next();
  // import (no backups) then done
  next();
  TEST_ASSERT_TRUE(h.top()->title().find("Ready") == 0);
  TEST_ASSERT_TRUE(r.svc->setupNeeded());
  next();
  TEST_ASSERT_FALSE(r.svc->setupNeeded());
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
  TEST_ASSERT_FALSE(r.ui->wizardActive());
  // the wizard does not come back
  r.step(5000);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
}

void test_wizard_back_and_import() {
  // a backup made on another device
  Rig src(berlin(14, 12));
  src.svc->backupNow();
  const std::vector<std::string> names = src.svc->backups();
  TEST_ASSERT_EQUAL(1u, names.size());
  std::string json;
  TEST_ASSERT_TRUE(storage::readBackupFile(src.sd, names[0], json));

  Rig r(berlin(14, 12), false);
  r.model->status.clockUnset = true;
  TEST_ASSERT_TRUE(storage::writeBackupFile(r.sd, names[0], json));
  r.step();
  FormHost& h = r.ui->form();
  auto next = [&]() { return h.pressFooter(fid::FtNext, r.ms += 100); };
  h.activateField(fid::WzAccept, r.ms += 100);
  next();
  next();
  h.pressFooter(fid::FtBack, r.ms += 100);  // back to language
  TEST_ASSERT_TRUE(h.top()->title().find("Language") == 0);
  next();
  next();  // tz
  next();  // wifi (skipped)
  next();  // clock: refused until a time is chosen
  TEST_ASSERT_TRUE(r.model->status.clockUnset);
  h.activateField(fid::WzClock, r.ms += 100);
  h.entrySave(fx::day(2026, 10, 14) * 1440 + 12 * 60, r.ms += 100);
  next();
  TEST_ASSERT_FALSE(r.model->status.clockUnset);
  h.activateField(fid::WzName, r.ms += 100);
  h.submitKeyboard("Temp", r.ms += 100);
  next();
  next();  // skip PIN
  h.activateField(fid::WzImport, r.ms += 100);
  h.pickOption(1, r.ms += 100);
  next();
  TEST_ASSERT_FALSE(r.svc->setupNeeded());
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
  TEST_ASSERT_EQUAL(src.svc->database().profiles.size(), r.svc->database().profiles.size());
  TEST_ASSERT_EQUAL(src.svc->database().meds.size(), r.svc->database().meds.size());
}

// ---------------------------------------------------------------- settings
void test_factory_reset_needs_strong_confirmation() {
  Rig r(berlin(14, 12));
  r.svc->backupNow();
  r.model->append(core::DoseLogEntry());
  r.repo.saveDatabase(r.svc->database());
  TEST_ASSERT_TRUE(r.internal.exists("/RxNudge/db.rec"));
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Reset));
  TEST_ASSERT_FALSE(r.field(fid::RsErase)->enabled);
  r.activate(fid::RsErase);
  r.activate(fid::RsErase);
  TEST_ASSERT_EQUAL(0, r.resets);
  r.type(fid::RsConfirm, "reset");
  TEST_ASSERT_TRUE(r.host.toastIsError());
  TEST_ASSERT_FALSE(r.field(fid::RsErase)->enabled);
  r.retype(fid::RsConfirm, "RESET");
  TEST_ASSERT_TRUE(r.field(fid::RsErase)->enabled);
  r.activate(fid::RsErase);  // arm
  TEST_ASSERT_EQUAL(0, r.resets);
  r.activate(fid::RsErase);  // confirm
  TEST_ASSERT_EQUAL(1, r.resets);
  TEST_ASSERT_EQUAL(0u, r.host.depth());
  TEST_ASSERT_TRUE(r.svc->setupNeeded());
  TEST_ASSERT_EQUAL(0u, r.svc->database().profiles.size());
  TEST_ASSERT_EQUAL(0u, r.model->recentLog().size());
  TEST_ASSERT_TRUE(r.internal.list("/RxNudge/logs").empty());
  TEST_ASSERT_EQUAL(1u, r.svc->backups().size());  // SD backups are kept
  // the UI goes to the wizard
  r.step();
  TEST_ASSERT_TRUE(r.ui->wizardActive());
}

void test_backup_restore_round_trip() {
  Rig r(berlin(14, 12));
  r.model->setRecentLog(core::makeDemoLog(r.db, r.model->tz(), r.clk.nowUtc() - 3600, 7));
  r.net.saveCredentials("Home", "geheim-Passwort");
  const core::Database before = r.svc->database();
  const size_t logN = r.model->recentLog().size();
  std::string name;
  TEST_ASSERT_EQUAL_STRING("", r.svc->backupNow(&name).c_str());
  TEST_ASSERT_EQUAL_STRING("backup-20261014-1200.json", name.c_str());
  std::string json;
  TEST_ASSERT_TRUE(storage::readBackupFile(r.sd, name, json));
  TEST_ASSERT_TRUE(json.find("geheim") == std::string::npos);
  // change things, then restore through the settings page
  core::Medication m = before.meds.front();
  m.name = "Changed";
  r.svc->saveMedication(m);
  r.svc->setMedicationActive(before.meds.back().id, false);
  r.model->setRecentLog({});
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Backup));
  r.activate(fid::BkItem0);
  r.activate(fid::BkItem0);
  TEST_ASSERT_FALSE_MESSAGE(r.host.toastIsError(), r.host.toast().c_str());
  const core::Database after = r.svc->database();
  TEST_ASSERT_EQUAL(before.meds.size(), after.meds.size());
  for (size_t i = 0; i < before.meds.size(); ++i) {
    TEST_ASSERT_EQUAL_STRING(before.meds[i].name.c_str(), after.meds[i].name.c_str());
    TEST_ASSERT_EQUAL(before.meds[i].active, after.meds[i].active);
    TEST_ASSERT_EQUAL(before.meds[i].phases.size(), after.meds[i].phases.size());
  }
  TEST_ASSERT_EQUAL(before.profiles.size(), after.profiles.size());
  TEST_ASSERT_EQUAL(logN, r.model->recentLog().size());
  TEST_ASSERT_EQUAL_STRING("geheim-Passwort", r.net.pass.c_str());  // NVS credentials untouched
  // the restored log was written to flash
  storage::FsDoseLog fresh(r.internal);
  const std::vector<core::DoseLogEntry> reread = fresh.readAll();
  TEST_ASSERT_EQUAL(logN, reread.size());
  // garbage is refused and nothing changes
  TEST_ASSERT_TRUE(storage::writeBackupFile(r.sd, "backup-20261015-0800.json", "{\"format\":\"nope\"}"));
  TEST_ASSERT_FALSE(r.svc->restore("backup-20261015-0800.json").empty());
  TEST_ASSERT_EQUAL(before.meds.size(), r.svc->database().meds.size());
}

void test_settings_backup_page() {
  Rig r(berlin(14, 12));
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Backup));
  TEST_ASSERT_NOT_NULL(r.field(fid::BkAuto));
  TEST_ASSERT_EQUAL(1, r.field(fid::BkAuto)->num);  // default ON
  TEST_ASSERT_EQUAL_STRING("Never", r.field(fid::BkAutoLast)->value.c_str());
  TEST_ASSERT_EQUAL_STRING("Runs after 03:00", r.field(fid::BkAutoStatus)->value.c_str());
  r.ab.status = storage::AutoBackupStatus::Ok;
  r.ab.lastBackupUtc = berlin(18, 3);
  r.ab.lastName = "auto-20261018.json";
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Backup));
  TEST_ASSERT_TRUE(r.field(fid::BkAutoLast)->value.find("03:00") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("auto-20261018.json", r.field(fid::BkAutoFile)->value.c_str());
  TEST_ASSERT_EQUAL_STRING("Saved", r.field(fid::BkAutoStatus)->value.c_str());
  r.ab.status = storage::AutoBackupStatus::Failed;
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Backup));
  TEST_ASSERT_EQUAL_STRING("Failed - check SD card", r.field(fid::BkAutoStatus)->value.c_str());
  // Toggle off: saved in the settings; the status rows disappear.
  r.activate(fid::BkAuto);
  TEST_ASSERT_FALSE_MESSAGE(r.host.toastIsError(), r.host.toast().c_str());
  TEST_ASSERT_FALSE(r.svc->database().settings.autoBackup);
  TEST_ASSERT_NULL(r.field(fid::BkAutoLast));
  // Backups never contain the Wi-Fi password (it lives in NVS).
  r.net.saveCredentials("Heimnetz", "secret-pass");
  TEST_ASSERT_TRUE(r.svc->backupJson().find("secret-pass") == std::string::npos);
}

void test_settings_pages_and_net_hooks() {
  Rig r(berlin(14, 12));
  // every settings page builds and has no empty labels
  for (int s = 1; s <= static_cast<int>(app::SettingsSection::Reset); ++s) {
    r.open(app::makeSettingsSection(*r.svc, static_cast<app::SettingsSection>(s)));
    TEST_ASSERT_FALSE(r.host.fields().empty());
    for (const auto& f : r.host.fields()) TEST_ASSERT_FALSE(f.label.empty());
    TEST_ASSERT_TRUE(r.host.top()->title().find("TODO") == std::string::npos);
  }
  // Wi-Fi: credentials go to the net service (NVS), never into the database
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Wifi));
  r.activate(fid::WfTest);
  TEST_ASSERT_TRUE(r.host.toastIsError());  // nothing configured
  TEST_ASSERT_FALSE(r.field(fid::WfManualPass)->enabled);
  r.type(fid::WfManualSsid, "Heimnetz");
  TEST_ASSERT_TRUE(r.field(fid::WfManualPass)->enabled);
  r.type(fid::WfManualPass, "short");
  TEST_ASSERT_TRUE(r.host.toastIsError());
  r.type(fid::WfManualPass, "pässwort-lang");  // the name is kept after a rejected password
  TEST_ASSERT_FALSE_MESSAGE(r.host.toastIsError(), r.host.toast().c_str());
  TEST_ASSERT_EQUAL_STRING("Heimnetz", r.net.ssid.c_str());
  TEST_ASSERT_TRUE(r.svc->database().settings.wifiEnabled);
  TEST_ASSERT_TRUE(r.svc->backupJson().find("pässwort") == std::string::npos);
  r.net.jobPolls = 2;
  r.activate(fid::WfTest);  // runs in the background: the row shows progress, the button is disabled
  TEST_ASSERT_FALSE(r.host.toastIsError());
  r.ms += 1000;
  r.host.tick(r.ms);
  TEST_ASSERT_FALSE(r.field(fid::WfTest)->enabled);
  TEST_ASSERT_EQUAL_STRING("Connecting... 1 s", r.field(fid::WfTest)->value.c_str());
  r.finishJob();
  TEST_ASSERT_FALSE(r.host.toastIsError());
  TEST_ASSERT_EQUAL_STRING("Connected; radio off again", r.host.toast().c_str());
  TEST_ASSERT_TRUE(r.field(fid::WfTest)->enabled);
  TEST_ASSERT_EQUAL(1, r.net.tests);
  r.net.jobPolls = 1;
  r.activate(fid::WfPhone);
  TEST_ASSERT_TRUE(r.net.provisioning);
  TEST_ASSERT_NOT_NULL(r.field(fid::WfApPass));
  r.activate(fid::WfPhoneStop);
  TEST_ASSERT_FALSE(r.net.provisioning);
  // Portal: code and the no-TLS note are shown
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Portal));
  r.activate(fid::PoStart);
  TEST_ASSERT_TRUE(r.net.portal);
  TEST_ASSERT_EQUAL_STRING("482913", r.field(fid::PoCode)->value.c_str());
  TEST_ASSERT_NOT_NULL(r.field(fid::PoNoTls));
  r.activate(fid::PoStop);
  TEST_ASSERT_FALSE(r.net.portal);
  // Caregiver push: URL validated, opt-in
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Notify));
  r.activate(fid::NoEnabled);
  TEST_ASSERT_TRUE(r.host.toastIsError());  // URL first
  r.type(fid::NoUrl, "ftp://x");
  TEST_ASSERT_TRUE(r.host.toastIsError());
  r.retype(fid::NoUrl, "https://ntfy.sh/oma-meds-7f3k");
  r.activate(fid::NoEnabled);
  TEST_ASSERT_TRUE(r.svc->database().settings.notifyEnabled);
  TEST_ASSERT_FALSE(r.svc->database().settings.notifyNames);
  r.activate(fid::NoTest);
  TEST_ASSERT_EQUAL(1, r.net.notifications);
  // Update page points to the portal
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Update));
  r.activate(fid::UpPortal);
  TEST_ASSERT_TRUE(r.host.top()->title() == "Web portal");
  // Time sync through NTP writes the clock
  r.net.ntpUtc = berlin(14, 12, 7);
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Time));
  r.activate(fid::TmSync);
  TEST_ASSERT_FALSE(r.host.toastIsError());
  r.finishJob();
  TEST_ASSERT_EQUAL_STRING("Time synchronised", r.host.toast().c_str());
  TEST_ASSERT_EQUAL(berlin(14, 12, 7), r.clk.nowUtc());
  // Time: custom POSIX, then clock
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Time));
  r.pick(fid::TmZone, core::tzPresetCount());
  TEST_ASSERT_TRUE(r.field(fid::TmPosix)->enabled);
  r.type(fid::TmPosix, "nonsense");
  TEST_ASSERT_TRUE(r.host.toastIsError());
  r.retype(fid::TmPosix, "GMT0BST,M3.5.0/1,M10.5.0");
  TEST_ASSERT_FALSE(r.host.toastIsError());
  TEST_ASSERT_EQUAL_STRING("Custom", r.svc->database().settings.tzName.c_str());
  r.pick(fid::TmZone, core::kDefaultTzPreset);
  TEST_ASSERT_EQUAL_STRING("Europe/Berlin", r.svc->database().settings.tzName.c_str());
  r.stepper(fid::TmClock, fx::day(2026, 10, 14) * 1440 + 13 * 60 + 5);
  TEST_ASSERT_EQUAL(berlin(14, 13, 5), r.clk.nowUtc());
  r.net.ntpUtc = 0;
  r.activate(fid::TmSync);
  r.finishJob();
  TEST_ASSERT_TRUE(r.host.toastIsError());
  // Display: refresh policy + large text hook
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Display));
  r.stepper(fid::DsFullEvery, 10);
  r.activate(fid::DsLarge);
  TEST_ASSERT_EQUAL(10, r.svc->database().settings.refreshFullEvery);
  TEST_ASSERT_TRUE(r.large);
  // Sound: test pattern plays through the hook; defaults for new meds
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Sound));
  r.stepper(fid::SnLadderEvery, 3);
  r.activate(fid::SnPlay);
  TEST_ASSERT_EQUAL(1u, r.played.size());
  TEST_ASSERT_EQUAL(3, r.svc->newMedication(1).ladderIntervalMin);
  // Quiet hours per person
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Quiet));
  r.stepper(10, 21 * 60);
  TEST_ASSERT_EQUAL(21 * 60, r.svc->database().profiles[0].quietStartMin);
  // Lock page: set and remove the admin PIN
  r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Lock));
  r.activate(fid::LkSet);
  r.host.submitKeyboard("4321", r.ms += 100);
  r.host.submitKeyboard("4321", r.ms += 100);
  TEST_ASSERT_TRUE(r.svc->adminPinSet());
  r.activate(fid::LkClear);
  r.activate(fid::LkClear);
  TEST_ASSERT_FALSE(r.svc->adminPinSet());
}


// Taps only (screen coordinates, like the device): rows on later pages need the page button.
struct Tapper {
  Rig& r;
  bool tapRect(const Rect* rc) {
    if (!rc) return false;
    InputEvent e;
    e.type = InputType::Tap;
    e.x = static_cast<int16_t>(rc->cx());
    e.y = static_cast<int16_t>(rc->cy());
    r.ui->handle(e, r.ms += 400);
    r.model->process();
    r.ui->render(r.canvas);
    return true;
  }
  bool hit(int id) {
    r.ui->render(r.canvas);
    return tapRect(r.ui->screen() == ScreenId::Form ? r.ui->form().hits().rectOf(id) : r.ui->hits().rectOf(id));
  }
  // A form row by field id, paging forward if needed.
  bool row(int fieldId) {
    for (int page = 0; page < 8; ++page) {
      r.ui->render(r.canvas);
      if (const Rect* rc = r.ui->form().hits().rectOf(FormHost::kHitRowBase + fieldId)) return tapRect(rc);
      if (!tapRect(r.ui->form().hits().rectOf(3))) return false;  // next page
    }
    return false;
  }
  bool footer(int index) { return hit(FormHost::kHitFooterBase + index); }
  const FormField* field(int id) {
    for (const auto& f : r.ui->form().fields())
      if (f.id == id) return &f;
    return nullptr;
  }
};

// Bug v1.0.2: a custom time could not be set. Drives the device UI with coordinate taps only.
void test_custom_time_from_home() {
  Rig r(berlin(14, 12, 30));
  r.step();
  Tapper t{r};
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
  TEST_ASSERT_TRUE(t.hit(kHitMenu));
  TEST_ASSERT_TRUE_MESSAGE(t.row(1), "Menu > Medications");
  TEST_ASSERT_EQUAL_STRING("Medications", r.ui->form().top()->title().c_str());
  TEST_ASSERT_TRUE_MESSAGE(t.footer(0), "Add medication");
  TEST_ASSERT_EQUAL_STRING("New medication", r.ui->form().top()->title().c_str());
  // The default time is visible on the first page of the editor: one fixed 08:00.
  r.ui->render(r.canvas);
  TEST_ASSERT_NOT_NULL_MESSAGE(r.ui->form().hits().rectOf(FormHost::kHitRowBase + fid::MedTime0), "Time 1 on page 1");
  TEST_ASSERT_NOT_NULL_MESSAGE(r.ui->form().hits().rectOf(FormHost::kHitRowBase + fid::MedAddTime), "+ Add time on page 1");
  TEST_ASSERT_EQUAL_STRING("08:00", t.field(fid::MedTime0)->value.c_str());
  TEST_ASSERT_NULL(t.field(fid::MedTime0 + 1));
  TEST_ASSERT_TRUE(t.row(fid::MedName));
  TEST_ASSERT_TRUE(r.ui->form().submitKeyboard("Vitamin D3", r.ms += 300));
  // Tap Time 1 -> its page -> "At" opens HH:MM; type 1 4 3 5 on the number pad.
  TEST_ASSERT_TRUE(t.row(fid::MedTime0));
  TEST_ASSERT_EQUAL_STRING("Time 1", r.ui->form().top()->title().c_str());
  TEST_ASSERT_TRUE(t.row(fid::TeTime));
  TEST_ASSERT_EQUAL(static_cast<int>(FormHost::Mode::Entry), static_cast<int>(r.ui->form().mode()));
  TEST_ASSERT_TRUE(t.hit(20 + kEntryBox0));  // hour box -> number pad
  const uint32_t padKey = r.ui->touchLayoutKey();
  for (int d : {1, 4, 3}) TEST_ASSERT_TRUE(t.hit(20 + kEntryKey0 + d));
  // typing keeps the touch layout (no touch guard between digits), only a new screen changes it
  TEST_ASSERT_EQUAL_UINT32(padKey, r.ui->touchLayoutKey());
  TEST_ASSERT_TRUE(t.hit(20 + kEntryKey0 + 5));
  if (r.ui->form().mode() == FormHost::Mode::Entry) TEST_ASSERT_TRUE(t.hit(10));  // Save
  TEST_ASSERT_TRUE(t.footer(0));  // Done
  TEST_ASSERT_EQUAL_STRING("14:35", t.field(fid::MedTime0)->value.c_str());
  // "+ Add time" opens HH:MM straight away; add 21:10, then delete it again from its page.
  TEST_ASSERT_TRUE(t.row(fid::MedAddTime));
  TEST_ASSERT_EQUAL(static_cast<int>(FormHost::Mode::Entry), static_cast<int>(r.ui->form().mode()));
  TEST_ASSERT_TRUE(t.hit(20 + kEntryBox0));
  for (int d : {2, 1, 1, 0}) TEST_ASSERT_TRUE(t.hit(20 + kEntryKey0 + d));
  if (r.ui->form().mode() == FormHost::Mode::Entry) TEST_ASSERT_TRUE(t.hit(10));
  TEST_ASSERT_NOT_NULL(t.field(fid::MedTime0 + 1));
  TEST_ASSERT_EQUAL_STRING("21:10", t.field(fid::MedTime0 + 1)->value.c_str());
  TEST_ASSERT_TRUE(t.row(fid::MedTime0 + 1));
  TEST_ASSERT_TRUE(t.row(fid::TeDelete));  // arms (danger rows confirm with a second tap)
  TEST_ASSERT_TRUE(t.row(fid::TeDelete));
  TEST_ASSERT_EQUAL_STRING("New medication", r.ui->form().top()->title().c_str());
  TEST_ASSERT_NULL(t.field(fid::MedTime0 + 1));
  // Save (footer: Cancel, Save) and check the stored schedule and Today.
  TEST_ASSERT_TRUE(t.footer(1));
  const core::Database db = r.svc->database();
  const core::Medication* m = nullptr;
  for (const auto& x : db.meds)
    if (x.name == "Vitamin D3") m = &x;
  TEST_ASSERT_NOT_NULL_MESSAGE(m, r.ui->form().toast().c_str());
  TEST_ASSERT_EQUAL(1u, m->phases.size());
  TEST_ASSERT_EQUAL(static_cast<int>(core::ScheduleType::FixedTimes), static_cast<int>(m->phases[0].rule.type));
  TEST_ASSERT_EQUAL(1u, m->phases[0].rule.times.size());
  TEST_ASSERT_EQUAL(14 * 60 + 35, m->phases[0].rule.times[0].minute);
  TEST_ASSERT_EQUAL(static_cast<int>(core::Routine::None), static_cast<int>(m->phases[0].rule.times[0].routine));
  r.model->process();
  const app::TodayVM vm = r.model->today(m->profileId);
  bool found = false;
  for (const auto& row : vm.rows) found |= row.medId == m->id && row.minute == 14 * 60 + 35 && row.time == "14:35";
  TEST_ASSERT_TRUE_MESSAGE(found, "14:35 is on Today");
}

// The time page can switch a time to a routine anchor and back; the last time cannot be deleted.
void test_time_page_anchor_and_delete() {
  Rig r(berlin(14, 12));
  core::Medication m = r.svc->newMedication(1);
  m.name = "Anchor";
  r.open(app::makeMedEditPage(*r.svc, m, true));
  TEST_ASSERT_TRUE(r.activate(fid::MedTime0));
  TEST_ASSERT_TRUE(r.pick(fid::TeAnchor, static_cast<int>(core::Routine::Lunch)));
  TEST_ASSERT_NOT_NULL(r.field(fid::TeOffset));
  TEST_ASSERT_TRUE(r.stepper(fid::TeOffset, 15));
  TEST_ASSERT_FALSE(r.field(fid::TeDelete)->enabled);
  TEST_ASSERT_TRUE(r.footer(fid::FtDone));
  TEST_ASSERT_TRUE(r.field(fid::MedTime0)->value.find("+15") != std::string::npos);
  TEST_ASSERT_TRUE(r.stepper(fid::MedAddTime, 20 * 60));
  TEST_ASSERT_TRUE(r.footer(fid::FtSave));
  TEST_ASSERT_FALSE_MESSAGE(r.host.toastIsError(), r.host.toast().c_str());
  const core::Database sdb = r.svc->database();
  const core::Medication& s = sdb.meds.back();
  TEST_ASSERT_EQUAL(2u, s.phases[0].rule.times.size());
  TEST_ASSERT_EQUAL(static_cast<int>(core::Routine::Lunch), static_cast<int>(s.phases[0].rule.times[0].routine));
  TEST_ASSERT_EQUAL(15, s.phases[0].rule.times[0].offsetMin);
  TEST_ASSERT_EQUAL(20 * 60, s.phases[0].rule.times[1].minute);
}

// Every list row must be tappable: row ids >= 100 used to collide with the footer hit ids.
void test_list_rows_tappable() {
  Rig r(berlin(14, 12, 30));
  r.step();
  Tapper t{r};
  TEST_ASSERT_TRUE(t.hit(kHitMenu));
  TEST_ASSERT_TRUE(t.row(1));
  TEST_ASSERT_TRUE(t.row(fid::ListItem0));
  TEST_ASSERT_EQUAL_STRING("Edit medication", r.ui->form().top()->title().c_str());
  TEST_ASSERT_TRUE(t.row(fid::MedPhases));
  TEST_ASSERT_TRUE(t.row(fid::PlPhase0));
  TEST_ASSERT_EQUAL_STRING("Phase 1", r.ui->form().top()->title().c_str());
}

void test_home_menu_button() {
  Rig r(berlin(14, 12, 30));
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
  const Rect* m = r.ui->hits().rectOf(kHitMenu);
  TEST_ASSERT_NOT_NULL(m);
  TEST_ASSERT_TRUE(m->w >= 83 && m->h >= 83);
  InputEvent e;
  e.type = InputType::Tap;
  e.x = static_cast<int16_t>(m->cx());
  e.y = static_cast<int16_t>(m->cy());
  r.ui->handle(e, r.ms += 100);
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Form), static_cast<int>(r.ui->screen()));
  TEST_ASSERT_EQUAL_STRING("Menu", r.ui->form().top()->title().c_str());
  r.ui->form().back(r.ms += 100);
  r.step();
  TEST_ASSERT_EQUAL(static_cast<int>(ScreenId::Home), static_cast<int>(r.ui->screen()));
}

// ---------------------------------------------------------------- previews
static void writePbm(const char* dir, const char* name, const uint8_t* buf) {
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.pbm", dir, name);
  FILE* f = fopen(path, "wb");
  if (!f) return;
  fprintf(f, "P4\n%d %d\n", Canvas::kW, Canvas::kH);
  for (int i = 0; i < Canvas::kBytes; i++) fputc(static_cast<uint8_t>(~buf[i]), f);
  fclose(f);
}

void test_previews() {
  const char* dir = getenv("MT_SCREENS_DIR");
  if (!dir) {
    TEST_PASS_MESSAGE("MT_SCREENS_DIR not set");
    return;
  }
  {
    Rig r(berlin(14, 12));
    core::Medication m = r.svc->database().meds.front();
    r.open(app::makeMedEditPage(*r.svc, m, false));
    r.draw();
    writePbm(dir, "p5_med_editor", r.buf);
    r.activate(fid::MedPhases);
    r.activate(fid::PlPhase0);
    r.draw();
    writePbm(dir, "p5_phase_editor", r.buf);
    r.host.back(r.ms += 100);
    r.host.back(r.ms += 100);
    r.activate(fid::MedName);
    r.host.keyboard().typeText(" Müller");
    r.draw();
    writePbm(dir, "p5_keyboard", r.buf);
    {
      KeyboardModel& kb = r.host.keyboard();
      for (const char* target : {"sym", "acc"})
        for (size_t i = 0; i < kb.keys().size(); i++)
          if (kb.keys()[i].kind == KeyKind::Layer && kb.keys()[i].text == target) {
            kb.press(i);
            break;
          }
      r.draw();
      writePbm(dir, "p5_keyboard_accents", r.buf);
    }
    r.open(app::makeMedListPage(*r.svc));
    r.draw();
    writePbm(dir, "p5_med_list", r.buf);
    r.open(app::makeProfileEditPage(*r.svc, r.svc->database().profiles.front(), false));
    r.draw();
    writePbm(dir, "p5_profile_editor", r.buf);
    r.open(app::makeSettingsPage(*r.svc));
    r.draw();
    writePbm(dir, "p5_settings", r.buf);
    r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Time));
    r.draw();
    writePbm(dir, "p5_settings_time", r.buf);
    r.net.saveCredentials("Heimnetz", "secret-pass");
    r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Wifi));
    r.draw();
    writePbm(dir, "p6_settings_wifi", r.buf);
    r.net.startProvisioning();
    r.draw();
    writePbm(dir, "p6_settings_wifi_hotspot", r.buf);
    r.net.stopProvisioning();
    r.net.startPortal();
    r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Portal));
    r.draw();
    writePbm(dir, "p6_settings_portal", r.buf);
    r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Notify));
    r.draw();
    writePbm(dir, "p6_settings_notify", r.buf);
    r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Reset));
    r.draw();
    writePbm(dir, "p5_settings_reset", r.buf);
    r.ab.status = storage::AutoBackupStatus::Ok;
    r.ab.lastBackupUtc = r.clk.nowUtc() - 9 * 3600;
    r.ab.lastName = "auto-20261018.json";
    r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Backup));
    r.draw();
    writePbm(dir, "p7_settings_backup", r.buf);
    r.open(app::makeSettingsSection(*r.svc, app::SettingsSection::Lock));
    r.activate(fid::LkSet);
    r.host.keyboard().typeText("12");
    r.draw();
    writePbm(dir, "p5_pin_entry", r.buf);
    {
      core::Medication n = r.svc->newMedication(1);
      n.name = "Vitamin D3";
      n.doseText = "1 tablet";
      n.phases[0].rule.times[0].minute = 14 * 60 + 35;
      r.open(app::makeMedEditPage(*r.svc, n, true));
      r.draw();
      writePbm(dir, "p9_med_times", r.buf);
      r.activate(fid::MedTime0);
      r.draw();
      writePbm(dir, "p9_time_page", r.buf);
      r.host.back(r.ms += 100);
      r.activate(fid::MedAddTime);
      r.draw();
      writePbm(dir, "p9_add_time_entry", r.buf);
    }
    r.open(app::makeMedEditPage(*r.svc, m, false));
    r.activate(fid::MedGrace);
    r.draw();
    writePbm(dir, "p8_entry_number", r.buf);
    r.host.back(r.ms += 100);
    r.activate(fid::MedStart);
    r.draw();
    writePbm(dir, "p8_entry_date", r.buf);
    r.host.entry().beginTyping(0);
    r.host.entry().typeDigit(2);
    r.draw();
    writePbm(dir, "p8_entry_date_numpad", r.buf);
    r.host.back(r.ms += 100);
    r.activate(fid::MedTemplate);
    r.draw();
    writePbm(dir, "p5_picker_templates", r.buf);
  }
  {
    Rig r(berlin(14, 12), false);
    r.model->status.clockUnset = true;
    r.step();
    FormHost& h = r.ui->form();
    const char* names[] = {"p5_wizard_1_safety", "p5_wizard_2_language", "p5_wizard_3_timezone", "p6_wizard_4_wifi",
                           "p5_wizard_5_clock",  "p5_wizard_6_profile",  "p5_wizard_7_pin",      "p5_wizard_8_import",
                           "p5_wizard_9_done"};
    h.activateField(fid::WzAccept, r.ms += 100);
    for (int i = 0; i < 9; ++i) {
      if (i == 3) h.activateField(fid::WzWifiPhone, r.ms += 100);
      if (i == 5) {
        h.activateField(fid::WzName, r.ms += 100);
        h.submitKeyboard("Anna Müller", r.ms += 100);
      }
      r.ui->render(r.canvas);
      writePbm(dir, names[i], r.buf);
      if (i == 4) {
        h.activateField(fid::WzClock, r.ms += 100);
        r.ui->render(r.canvas);
        writePbm(dir, "p8_wizard_5_clock_entry", r.buf);
        h.entrySave(fx::day(2026, 10, 14) * 1440 + 12 * 60, r.ms += 100);
      }
      if (i < 8) h.pressFooter(fid::FtNext, r.ms += 100);
    }
  }
  {
    // Wizard clock step with Wi-Fi set up: internet time first, fetched automatically.
    Rig r(berlin(14, 12), false);
    r.model->status.clockUnset = true;
    r.net.saveCredentials("Heimnetz", "secret-pass");
    r.net.jobPolls = 1000;  // still running in the picture
    r.step();
    FormHost& h = r.ui->form();
    h.activateField(fid::WzAccept, r.ms += 100);
    for (int i = 0; i < 4; ++i) h.pressFooter(fid::FtNext, r.ms += 100);
    r.ms += 3000;
    h.tick(r.ms);
    r.ui->render(r.canvas);
    writePbm(dir, "p8_wizard_5_clock_wifi", r.buf);
  }
  {
    Rig r(berlin(14, 12, 30));
    r.step();
    writePbm(dir, "p5_home_menu", r.buf);
  }
  TEST_PASS();
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_keyboard_utf8_and_layers);
  RUN_TEST(test_keyboard_numeric_and_password);
  RUN_TEST(test_keyboard_keypress_is_partial_update);
  RUN_TEST(test_med_editor_validation_and_save);
  RUN_TEST(test_med_schedule_types_and_phases);
  RUN_TEST(test_med_archive_and_list);
  RUN_TEST(test_profile_limit_and_editor);
  RUN_TEST(test_pin_helpers_and_gate);
  RUN_TEST(test_pin_gating_in_host);
  RUN_TEST(test_wizard_flow);
  RUN_TEST(test_wizard_back_and_import);
  RUN_TEST(test_factory_reset_needs_strong_confirmation);
  RUN_TEST(test_backup_restore_round_trip);
  RUN_TEST(test_settings_pages_and_net_hooks);
  RUN_TEST(test_settings_backup_page);
  RUN_TEST(test_home_menu_button);
  RUN_TEST(test_custom_time_from_home);
  RUN_TEST(test_time_page_anchor_and_delete);
  RUN_TEST(test_list_rows_tappable);
  RUN_TEST(test_previews);
  return UNITY_END();
}
