#include <unity.h>

#include <cstdlib>
#include <unistd.h>

#include "../common/fixtures.h"
#include "core/codec.h"
#include "core/json.h"
#include "core/privacy.h"
#include "storage/export.h"
#include "storage/host_fs.h"
#include "storage/log_store.h"
#include "storage/mem_fs.h"
#include "storage/migrations.h"
#include "storage/record_store.h"
#include "storage/repository.h"

using namespace core;
using namespace storage;
using fx::at;
using fx::day;

void setUp() {}
void tearDown() {}

static Database sampleDb() {
  Database db = fx::singleMedDb(fx::fixedRule({at(8), at(20)}), day(2026, 6, 1));
  db.meds[0].name = "Ibuprofen \xC3\xBC\xE2\x82\xAC";  // UTF-8
  db.meds[0].instructions = "with food, \"not\" fasting";
  db.meds[0].critical = true;
  db.meds[0].supply = 30;
  db.meds[0].refillDay = day(2026, 7, 1);
  ScheduleRule prn;
  prn.type = ScheduleType::Prn;
  prn.prnMaxPer24h = 2;
  db.meds.push_back(fx::med(2, 1, day(2026, 6, 1), prn));
  db.profiles[0].pinSalt = "00ff";
  db.profiles[0].pinHash = hashPin("1234", "00ff");
  return db;
}

static DoseLogEntry entry(int64_t recorded, DoseAction a, uint16_t med = 1) {
  DoseLogEntry e;
  e.profileId = 1;
  e.medId = med;
  e.scheduledUtc = recorded - 120;
  e.eventId = makeEventId(1, med, e.scheduledUtc);
  e.action = a;
  e.actionUtc = recorded - 60;
  e.recordedUtc = recorded;
  e.source = ActionSource::Button;
  return e;
}

static void test_json_roundtrip_and_errors() {
  JsonValue v;
  TEST_ASSERT_TRUE(parseJson("{\"a\":[1,-2,3.5,true,null,\"x\\u00fc\\ud83d\\ude00\\n\"],\"b\":{\"c\":9007199254740993}}", v));
  TEST_ASSERT_EQUAL(-2, v["a"].at(1).asInt());
  TEST_ASSERT_TRUE(v["a"].at(2).asDouble() == 3.5);
  TEST_ASSERT_EQUAL_STRING("x\xC3\xBC\xF0\x9F\x98\x80\n", v["a"].at(5).asString().c_str());
  TEST_ASSERT_TRUE(v["b"]["c"].asInt() == 9007199254740993LL);
  JsonValue w;
  TEST_ASSERT_TRUE(parseJson(v.dump(), w));
  TEST_ASSERT_EQUAL_STRING(v.dump().c_str(), w.dump().c_str());
  TEST_ASSERT_TRUE(parseJson(v.dump(true), w));
  TEST_ASSERT_FALSE(parseJson("{\"a\":1,}", w));
  TEST_ASSERT_FALSE(parseJson("[1 2]", w));
  TEST_ASSERT_FALSE(parseJson("\"\\ud800\"", w));
  TEST_ASSERT_FALSE(parseJson("{} x", w));
  TEST_ASSERT_FALSE(parseJson(std::string(100, '['), w));  // depth limit
}

static void test_codec_database_roundtrip() {
  Database db = sampleDb();
  Database out;
  std::string err;
  JsonValue j;
  TEST_ASSERT_TRUE(parseJson(encodeDatabase(db).dump(), j));
  TEST_ASSERT_TRUE_MESSAGE(decodeDatabase(j, out, &err), err.c_str());
  TEST_ASSERT_EQUAL_STRING(encodeDatabase(db).dump().c_str(), encodeDatabase(out).dump().c_str());
  TEST_ASSERT_EQUAL_STRING(db.meds[0].name.c_str(), out.meds[0].name.c_str());
  TEST_ASSERT_EQUAL(ScheduleType::Prn, out.meds[1].phases[0].rule.type);
  TEST_ASSERT_TRUE(checkPin(out.profiles[0], "1234"));
  int32_t d;
  TEST_ASSERT_FALSE(parseDate("2026-02-29", d));
  TEST_ASSERT_TRUE(parseDate("2028-02-29", d));
  TEST_ASSERT_EQUAL_STRING("2028-02-29", formatDate(d).c_str());
}

static void test_record_crc_detects_corruption() {
  const std::string rec = encodeRecord(2, "hello");
  uint16_t schema;
  std::string payload;
  TEST_ASSERT_TRUE(decodeRecord(rec, schema, payload));
  TEST_ASSERT_EQUAL(2, schema);
  TEST_ASSERT_EQUAL_STRING("hello", payload.c_str());
  for (size_t i = 0; i < rec.size(); i++) {
    if (i == 4 || i == 5 || i == 6 || i == 7) continue;  // schema/flags are not CRC-covered (validated by caller)
    std::string bad = rec;
    bad[i] = static_cast<char>(bad[i] ^ 0x01);
    TEST_ASSERT_FALSE(decodeRecord(bad, schema, payload));
  }
  TEST_ASSERT_FALSE(decodeRecord(rec.substr(0, rec.size() - 1), schema, payload));
}

static void test_atomic_write_keeps_last_known_good() {
  MemFs fs;
  RecordStore rs(fs);
  TEST_ASSERT_TRUE(rs.write("/RxNudge/x.rec", 2, "v1"));
  TEST_ASSERT_TRUE(rs.write("/RxNudge/x.rec", 2, "v2"));
  TEST_ASSERT_FALSE(fs.exists("/RxNudge/x.rec.tmp"));
  uint16_t s;
  std::string p;
  TEST_ASSERT_EQUAL(LoadStatus::Ok, rs.read("/RxNudge/x.rec", s, p));
  TEST_ASSERT_EQUAL_STRING("v2", p.c_str());
  TEST_ASSERT_TRUE(decodeRecord(fs.files["/RxNudge/x.rec.bak"], s, p));
  TEST_ASSERT_EQUAL_STRING("v1", p.c_str());
  // failed write (torn temp) leaves the good copy untouched
  fs.failWriteAfter = 0;
  TEST_ASSERT_FALSE(rs.write("/RxNudge/x.rec", 2, "v3-long-payload"));
  fs.failWriteAfter = -1;
  TEST_ASSERT_EQUAL(LoadStatus::Ok, rs.read("/RxNudge/x.rec", s, p));
  TEST_ASSERT_EQUAL_STRING("v2", p.c_str());
}

static void test_corruption_recovery_from_backup() {
  MemFs fs;
  RecordStore rs(fs);
  rs.write("/r.rec", 2, "good-old");
  rs.write("/r.rec", 2, "good-new");
  fs.flipByte("/r.rec", 20);
  uint16_t s;
  std::string p;
  TEST_ASSERT_EQUAL(LoadStatus::RecoveredFromBackup, rs.read("/r.rec", s, p));
  TEST_ASSERT_EQUAL_STRING("good-old", p.c_str());
  TEST_ASSERT_TRUE(fs.exists("/r.rec.bad"));  // corrupt copy kept for diagnosis
  TEST_ASSERT_EQUAL(LoadStatus::Ok, rs.read("/r.rec", s, p));  // main repaired
  // next write does not rotate a corrupt main over the good backup
  fs.flipByte("/r.rec", 20);
  TEST_ASSERT_TRUE(rs.write("/r.rec", 2, "fresh"));
  TEST_ASSERT_TRUE(decodeRecord(fs.files["/r.rec.bak"], s, p));
  TEST_ASSERT_EQUAL_STRING("good-old", p.c_str());
  // everything corrupt -> visible Corrupt, nothing loaded
  fs.flipByte("/r.rec", 20);
  fs.flipByte("/r.rec.bak", 20);
  TEST_ASSERT_EQUAL(LoadStatus::Corrupt, rs.read("/r.rec", s, p));
  TEST_ASSERT_EQUAL(LoadStatus::NotFound, rs.read("/none.rec", s, p));
}

static void test_power_loss_between_renames() {
  MemFs fs;
  RecordStore rs(fs);
  rs.write("/r.rec", 2, "old");
  fs.failRenameAfter = 1;  // main->bak succeeds, tmp->main "loses power"
  TEST_ASSERT_FALSE(rs.write("/r.rec", 2, "new"));
  fs.failRenameAfter = -1;
  TEST_ASSERT_FALSE(fs.exists("/r.rec"));
  uint16_t s;
  std::string p;
  TEST_ASSERT_EQUAL(LoadStatus::RecoveredFromTemp, rs.read("/r.rec", s, p));
  TEST_ASSERT_EQUAL_STRING("new", p.c_str());
  TEST_ASSERT_EQUAL(LoadStatus::Ok, rs.read("/r.rec", s, p));
}

static void test_repository_roundtrip_and_health() {
  MemFs fs;
  FsRepository repo(fs);
  Database db;
  TEST_ASSERT_EQUAL(LoadStatus::NotFound, repo.loadDatabase(db));
  TEST_ASSERT_TRUE(repo.saveDatabase(sampleDb()));
  TEST_ASSERT_TRUE(repo.saveDatabase(sampleDb()));
  FsRepository repo2(fs);
  TEST_ASSERT_EQUAL(LoadStatus::Ok, repo2.loadDatabase(db));
  TEST_ASSERT_TRUE(repo2.health().healthy());
  TEST_ASSERT_EQUAL(2, db.meds.size());
  // corrupt main -> recovered with a visible warning
  fs.flipByte(repo.dbPath(), 30);
  FsRepository repo3(fs);
  TEST_ASSERT_EQUAL(LoadStatus::RecoveredFromBackup, repo3.loadDatabase(db));
  TEST_ASSERT_FALSE(repo3.health().healthy());
  TEST_ASSERT_TRUE(repo3.health().lastError.size() > 0);
  // invalid database refused
  Database bad = sampleDb();
  bad.meds[0].phases.clear();
  TEST_ASSERT_FALSE(repo3.saveDatabase(bad));
  // both copies corrupt -> Corrupt and db untouched
  fs.flipByte(repo.dbPath(), 30);
  fs.flipByte(repo.dbPath() + ".bak", 30);
  Database keep = sampleDb();
  FsRepository repo4(fs);
  TEST_ASSERT_EQUAL(LoadStatus::Corrupt, repo4.loadDatabase(keep));
  TEST_ASSERT_EQUAL(2, keep.meds.size());
  TEST_ASSERT_FALSE(repo4.health().healthy());
}

static void test_snapshot_persistence() {
  MemFs fs;
  FsRepository repo(fs);
  EngineSnapshot s;
  s.generatedUntilUtc = 123;
  DoseInstance d;
  d.eventId = 0xFF00112233445566ULL;
  d.state = DoseState::Snoozed;
  s.instances.push_back(d);
  s.lastTaken.emplace_back(4, 999);
  TEST_ASSERT_TRUE(repo.saveSnapshot(s));
  EngineSnapshot out;
  TEST_ASSERT_EQUAL(LoadStatus::Ok, repo.loadSnapshot(out));
  TEST_ASSERT_TRUE(out.instances[0].eventId == d.eventId);
  TEST_ASSERT_EQUAL(DoseState::Snoozed, out.instances[0].state);
  TEST_ASSERT_EQUAL(999, out.lastTaken[0].second);
}

static const char* kV1Doc =
    "{\"tz\":\"EST5EDT,M3.2.0,M11.1.0\","
    "\"profiles\":[{\"id\":1,\"name\":\"Oma\",\"initials\":\"OM\"}],"
    "\"meds\":[{\"id\":7,\"profile\":1,\"name\":\"Aspirin\",\"dose\":\"100 mg\",\"form\":\"pill\","
    "\"start\":\"2025-01-01\",\"grace\":45,\"times\":[\"08:00\",\"19:30\"]}]}";

static void test_migration_v1_to_v2() {
  JsonValue doc;
  TEST_ASSERT_TRUE(parseJson(kV1Doc, doc));
  TEST_ASSERT_EQUAL(1, documentVersion(doc));
  int steps = 0;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(migrateToCurrent(doc, &err, &steps), err.c_str());
  TEST_ASSERT_EQUAL(1, steps);
  TEST_ASSERT_EQUAL(2, doc["schemaVersion"].asInt());
  Database db;
  TEST_ASSERT_TRUE_MESSAGE(decodeDatabase(doc, db, &err), err.c_str());
  TEST_ASSERT_EQUAL(45, db.meds[0].graceMin);
  TEST_ASSERT_EQUAL(1, db.meds[0].phases.size());
  TEST_ASSERT_EQUAL(2, db.meds[0].phases[0].rule.times.size());
  TEST_ASSERT_EQUAL(19 * 60 + 30, db.meds[0].phases[0].rule.times[1].minute);
  TEST_ASSERT_TRUE(db.profiles[0].privacy);
  TEST_ASSERT_EQUAL_STRING("EST5EDT,M3.2.0,M11.1.0", db.settings.tz.c_str());
  // newer than supported -> refused
  JsonValue future;
  parseJson("{\"schemaVersion\":99}", future);
  TEST_ASSERT_FALSE(migrateToCurrent(future, &err));
}

static void test_repository_migrates_stored_v1() {
  MemFs fs;
  RecordStore rs(fs);
  rs.write("/RxNudge/db.rec", 1, kV1Doc);
  FsRepository repo(fs);
  Database db;
  TEST_ASSERT_EQUAL(LoadStatus::Ok, repo.loadDatabase(db));
  TEST_ASSERT_EQUAL(1, repo.health().migrationsRun);
  TEST_ASSERT_TRUE(fs.exists("/RxNudge/db.rec.v1"));  // pre-migration copy kept
  uint16_t schema;
  std::string payload;
  TEST_ASSERT_EQUAL(LoadStatus::Ok, rs.read("/RxNudge/db.rec", schema, payload));
  TEST_ASSERT_EQUAL(2, schema);
  FsRepository again(fs);
  TEST_ASSERT_EQUAL(LoadStatus::Ok, again.loadDatabase(db));
  TEST_ASSERT_EQUAL(0, again.health().migrationsRun);
  TEST_ASSERT_EQUAL(45, db.meds[0].graceMin);
}

static void test_dose_log_monthly_rotation_and_torn_record() {
  MemFs fs;
  FsDoseLog log(fs);
  TEST_ASSERT_TRUE(log.append(entry(fx::utc(2026, 9, 30, 23, 59), DoseAction::Taken)));
  TEST_ASSERT_TRUE(log.append(entry(fx::utc(2026, 10, 1, 0, 1), DoseAction::Skipped)));
  TEST_ASSERT_TRUE(log.append(entry(fx::utc(2026, 10, 2), DoseAction::Missed)));
  auto months = log.months();
  TEST_ASSERT_EQUAL(2, months.size());
  TEST_ASSERT_EQUAL_STRING("2026-09.log", months[0].c_str());
  TEST_ASSERT_TRUE(fs.exists("/RxNudge/logs/2026-10.log"));
  // power cut mid-append: half a record, then later appends still readable
  fs.failWriteAfter = 0;
  TEST_ASSERT_FALSE(log.append(entry(fx::utc(2026, 10, 3), DoseAction::Taken)));
  fs.failWriteAfter = -1;
  // correction = new entry
  TEST_ASSERT_TRUE(log.append(entry(fx::utc(2026, 10, 4), DoseAction::Undo)));
  size_t bad = 0;
  auto all = log.readAll(&bad);
  TEST_ASSERT_EQUAL(4, all.size());
  TEST_ASSERT_EQUAL(1, bad);
  TEST_ASSERT_EQUAL(DoseAction::Taken, all[0].action);
  TEST_ASSERT_EQUAL(DoseAction::Undo, all[3].action);
  TEST_ASSERT_EQUAL(1, log.failedAppends());
  // flipped byte in a record -> CRC rejects only that record
  fs.flipByte("/RxNudge/logs/2026-09.log", 10);
  all = log.readAll(&bad);
  TEST_ASSERT_EQUAL(3, all.size());
  TEST_ASSERT_EQUAL(2, bad);
}

static void test_logs_and_default_exports_contain_no_med_names() {
  MemFs fs;
  Database db = sampleDb();
  FsDoseLog log(fs);
  log.append(entry(fx::utc(2026, 10, 1), DoseAction::Taken));
  for (const auto& kv : fs.files) TEST_ASSERT_EQUAL(std::string::npos, kv.second.find("Ibuprofen"));
  auto entries = log.readAll();
  TEST_ASSERT_EQUAL(std::string::npos, exportCsv(entries, db).find("Ibuprofen"));
  TEST_ASSERT_EQUAL(std::string::npos, exportJson(entries, db).find("Ibuprofen"));
  ExportOptions o;
  o.includeNames = true;
  TEST_ASSERT_TRUE(exportCsv(entries, db, o).find("Ibuprofen") != std::string::npos);
  TEST_ASSERT_EQUAL_STRING("took med#1 at 8", redactMedNames("took " + db.meds[0].name + " at 8", db, false).c_str());
  TEST_ASSERT_EQUAL_STRING("med#1", medLogLabel(db.meds[0], false).c_str());
  TEST_ASSERT_FALSE(kLogMedNames);
}

static void test_csv_and_json_export_format() {
  Database db = sampleDb();
  std::vector<DoseLogEntry> e = {entry(fx::utc(2026, 10, 1, 6), DoseAction::TakenLate)};
  e[0].note = "=cmd,\"x\"";
  const std::string csv = exportCsv(e, db);
  TEST_ASSERT_EQUAL(0, csv.find("event_id,profile,med_id,scheduled_utc,action,action_utc,recorded_utc,source,note\r\n"));
  TEST_ASSERT_TRUE(csv.find(",2026-10-01T05:58:00Z,taken_late,2026-10-01T05:59:00Z,2026-10-01T06:00:00Z,button,") !=
                   std::string::npos);
  TEST_ASSERT_TRUE(csv.find("\"'=cmd,\"\"x\"\"\"") != std::string::npos);
  JsonValue j;
  TEST_ASSERT_TRUE(parseJson(exportJson(e, db), j));
  TEST_ASSERT_EQUAL(1, j["entries"].size());
  TEST_ASSERT_EQUAL_STRING("taken_late", j["entries"].at(0)["action"].asString().c_str());
  ExportOptions o;
  o.profileId = 2;
  TEST_ASSERT_TRUE(parseJson(exportJson(e, db, o), j));
  TEST_ASSERT_EQUAL(0, j["entries"].size());
}

static void test_backup_restore() {
  Database db = sampleDb();
  std::vector<DoseLogEntry> log = {entry(fx::utc(2026, 10, 1), DoseAction::Taken),
                                   entry(fx::utc(2026, 10, 2), DoseAction::Missed, 2)};
  const std::string backup = makeBackup(db, log);
  Database out;
  std::vector<DoseLogEntry> outLog;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(restoreBackup(backup, out, outLog, &err), err.c_str());
  TEST_ASSERT_EQUAL_STRING(encodeDatabase(db).dump().c_str(), encodeDatabase(out).dump().c_str());
  TEST_ASSERT_EQUAL(2, outLog.size());
  TEST_ASSERT_TRUE(outLog[1].eventId == log[1].eventId);
  // a broken backup changes nothing
  Database keep = sampleDb();
  std::vector<DoseLogEntry> keepLog = log;
  TEST_ASSERT_FALSE(restoreBackup(backup.substr(0, backup.size() / 2), keep, keepLog, &err));
  TEST_ASSERT_FALSE(restoreBackup("{\"format\":\"other\"}", keep, keepLog, &err));
  TEST_ASSERT_EQUAL(2, keepLog.size());
  // a v1-era backup is migrated on restore
  const std::string v1 = std::string("{\"format\":\"rxnudge-backup\",\"database\":") + kV1Doc + ",\"log\":[]}";
  int steps = 0;
  TEST_ASSERT_TRUE_MESSAGE(restoreBackup(v1, out, outLog, &err, &steps), err.c_str());
  TEST_ASSERT_EQUAL(1, steps);
  TEST_ASSERT_EQUAL(45, out.meds[0].graceMin);
}

static void test_host_fs_atomic_store() {
  char tmpl[] = "/tmp/mt_hostfs_XXXXXX";
  TEST_ASSERT_NOT_NULL(mkdtemp(tmpl));
  HostFs fs(tmpl);
  FsRepository repo(fs);
  TEST_ASSERT_TRUE(repo.saveDatabase(sampleDb()));
  TEST_ASSERT_TRUE(repo.saveDatabase(sampleDb()));
  Database db;
  TEST_ASSERT_EQUAL(LoadStatus::Ok, repo.loadDatabase(db));
  TEST_ASSERT_TRUE(fs.exists("/RxNudge/db.rec.bak"));
  FsDoseLog log(fs);
  TEST_ASSERT_TRUE(log.append(entry(fx::utc(2026, 10, 1), DoseAction::Taken)));
  TEST_ASSERT_EQUAL(1, log.readAll().size());
  std::string cmd = std::string("rm -rf ") + tmpl;
  TEST_ASSERT_EQUAL(0, std::system(cmd.c_str()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_json_roundtrip_and_errors);
  RUN_TEST(test_codec_database_roundtrip);
  RUN_TEST(test_record_crc_detects_corruption);
  RUN_TEST(test_atomic_write_keeps_last_known_good);
  RUN_TEST(test_corruption_recovery_from_backup);
  RUN_TEST(test_power_loss_between_renames);
  RUN_TEST(test_repository_roundtrip_and_health);
  RUN_TEST(test_snapshot_persistence);
  RUN_TEST(test_migration_v1_to_v2);
  RUN_TEST(test_repository_migrates_stored_v1);
  RUN_TEST(test_dose_log_monthly_rotation_and_torn_record);
  RUN_TEST(test_logs_and_default_exports_contain_no_med_names);
  RUN_TEST(test_csv_and_json_export_format);
  RUN_TEST(test_backup_restore);
  RUN_TEST(test_host_fs_atomic_store);
  return UNITY_END();
}
