// Phase 6: SHA-256, portal security (sessions, CSRF, rate limit, LAN/host),
// request validation, JSON API mapping, NTP parsing and trust, OTA
// verification, caregiver notifications and the portal API end to end.
#include <unity.h>

#include <cstring>
#include <memory>

#include "../common/fake_net.h"
#include "../common/fixtures.h"
#include "app_core/admin_service.h"
#include "core/codec.h"
#include "core/demo_data.h"
#include "net_core/api_json.h"
#include "net_core/notify.h"
#include "net_core/ota_verify.h"
#include "net_core/portal_api.h"
#include "net_core/portal_security.h"
#include "net_core/sha256.h"
#include "net_core/time_sync.h"
#include "net_core/web_assets.h"
#include "net_core/wifi_creds.h"
#include "storage/log_store.h"
#include "storage/mem_fs.h"
#include "storage/repository.h"

using namespace net;
using core::JsonValue;

void setUp() {}
void tearDown() {}

// ---------------------------------------------------------------- SHA-256
static void test_sha256_vectors() {
  TEST_ASSERT_EQUAL_STRING("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", Sha256::hex("").c_str());
  TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Sha256::hex("abc").c_str());
  TEST_ASSERT_EQUAL_STRING("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
                           Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq").c_str());
  // one million 'a', fed in odd-sized chunks
  Sha256 h;
  std::string chunk(997, 'a');
  size_t left = 1000000;
  while (left) {
    const size_t n = left < chunk.size() ? left : chunk.size();
    h.update(reinterpret_cast<const uint8_t*>(chunk.data()), n);
    left -= n;
  }
  TEST_ASSERT_EQUAL_STRING("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", h.hexDigest().c_str());
  uint8_t d[32];
  TEST_ASSERT_TRUE(parseSha256Hex("  BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD\n", d));
  TEST_ASSERT_EQUAL_STRING("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", toHex(d, 32).c_str());
  TEST_ASSERT_FALSE(parseSha256Hex("ba7816bf", d));
  TEST_ASSERT_FALSE(parseSha256Hex(std::string(63, 'a') + "g", d));
  TEST_ASSERT_TRUE(constantTimeEqual(std::string("abc"), std::string("abc")));
  TEST_ASSERT_FALSE(constantTimeEqual(std::string("abc"), std::string("abd")));
  TEST_ASSERT_FALSE(constantTimeEqual(std::string("abc"), std::string("abcd")));
}

// ---------------------------------------------------------------- security
static uint64_t g_rng = 1;
static uint64_t rng() {
  g_rng = g_rng * 6364136223846793005ull + 1442695040888963407ull;
  return g_rng;
}

static RequestMeta meta(const std::string& method, const std::string& ip = "192.168.1.20") {
  RequestMeta m;
  m.method = method;
  m.ip = ip;
  m.host = "rx-nudge.local";
  return m;
}

static void test_lan_and_host_checks() {
  TEST_ASSERT_TRUE(isLanAddress("192.168.0.5"));
  TEST_ASSERT_TRUE(isLanAddress("10.1.2.3"));
  TEST_ASSERT_TRUE(isLanAddress("172.16.0.1"));
  TEST_ASSERT_TRUE(isLanAddress("172.31.255.1"));
  TEST_ASSERT_FALSE(isLanAddress("172.32.0.1"));
  TEST_ASSERT_FALSE(isLanAddress("8.8.8.8"));
  TEST_ASSERT_FALSE(isLanAddress("192.168.1"));
  TEST_ASSERT_FALSE(isLanAddress("192.168.1.300"));
  TEST_ASSERT_TRUE(hostAllowed("rx-nudge.local", ""));
  TEST_ASSERT_TRUE(hostAllowed("RX-Nudge.LOCAL:80", ""));
  TEST_ASSERT_TRUE(hostAllowed("192.168.1.50", "192.168.1.50"));
  TEST_ASSERT_FALSE(hostAllowed("192.168.1.51", "192.168.1.50"));
  TEST_ASSERT_FALSE(hostAllowed("evil.example", "192.168.1.50"));  // DNS rebinding
  TEST_ASSERT_FALSE(hostAllowed("rx-nudge.local:8080", ""));
  TEST_ASSERT_EQUAL_STRING("abc", cookieValue("x=1; mt_session=abc; y=2", "mt_session").c_str());
  TEST_ASSERT_EQUAL_STRING("", cookieValue("mt_sessionx=abc", "mt_session").c_str());
}

static void test_login_session_csrf() {
  PortalSecurity sec(rng);
  RequestMeta m = meta("GET");
  TEST_ASSERT_EQUAL(Verdict::Unauthorized, sec.check(m, false, 0));  // not started
  sec.start(1000);
  const std::string code = sec.code();
  TEST_ASSERT_EQUAL(6, code.size());
  const uint32_t gen = sec.codeGeneration();
  TEST_ASSERT_FALSE(sec.login(meta("POST"), "000000" == code ? "111111" : "000000", 1000).ok);
  LoginResult lr = sec.login(meta("POST"), code, 1100);
  TEST_ASSERT_TRUE(lr.ok);
  TEST_ASSERT_EQUAL(32, lr.token.size());
  TEST_ASSERT_NOT_EQUAL(lr.token, lr.csrf);
  // code is one-time
  TEST_ASSERT_NOT_EQUAL(gen, sec.codeGeneration());
  TEST_ASSERT_FALSE(sec.login(meta("POST"), code, 1200).ok);

  m.cookie = std::string("mt_session=") + lr.token;
  std::string csrf;
  TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(m, true, 2000, &csrf));
  TEST_ASSERT_EQUAL_STRING(lr.csrf.c_str(), csrf.c_str());
  RequestMeta p = m;
  p.method = "POST";
  TEST_ASSERT_EQUAL(Verdict::CsrfFailed, sec.check(p, true, 2000));
  p.csrf = lr.csrf;
  TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(p, true, 2000));
  p.origin = "http://evil.example";
  TEST_ASSERT_EQUAL(Verdict::CsrfFailed, sec.check(p, true, 2000));
  p.origin = "http://rx-nudge.local";
  TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(p, true, 2000));
  RequestMeta wan = m;
  wan.ip = "81.2.3.4";
  TEST_ASSERT_EQUAL(Verdict::NotLan, sec.check(wan, true, 2000));
  RequestMeta bad = m;
  bad.host = "attacker.example";
  TEST_ASSERT_EQUAL(Verdict::BadHost, sec.check(bad, true, 2000));
  RequestMeta forged = m;
  forged.cookie = "mt_session=" + std::string(32, '0');
  TEST_ASSERT_EQUAL(Verdict::Unauthorized, sec.check(forged, true, 2000));

  // idle expiry (15 min)
  TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(m, true, 2000 + 14 * 60000));
  TEST_ASSERT_EQUAL(Verdict::Unauthorized, sec.check(m, true, 2000 + 14 * 60000 + 15 * 60000 + 1));
  // logout
  LoginResult l2 = sec.login(meta("POST"), sec.code(), 3000000);
  RequestMeta m2 = meta("GET");
  m2.cookie = "mt_session=" + l2.token;
  TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(m2, true, 3000001));
  sec.logout(m2);
  TEST_ASSERT_EQUAL(Verdict::Unauthorized, sec.check(m2, true, 3000002));
  // stop drops everything
  LoginResult l3 = sec.login(meta("POST"), sec.code(), 3000003);
  m2.cookie = "mt_session=" + l3.token;
  sec.stop();
  sec.start(3000004);
  TEST_ASSERT_EQUAL(Verdict::Unauthorized, sec.check(m2, true, 3000005));
}

static void test_session_absolute_expiry_and_limit() {
  PortalSecurity sec(rng);
  sec.start(0);
  std::vector<std::string> tokens;
  for (int i = 0; i < 4; i++) tokens.push_back(sec.login(meta("POST"), sec.code(), 10 + i).token);
  TEST_ASSERT_EQUAL(3, sec.sessionCount(20));  // oldest evicted
  RequestMeta m = meta("GET");
  m.cookie = "mt_session=" + tokens[0];
  TEST_ASSERT_EQUAL(Verdict::Unauthorized, sec.check(m, true, 30));
  m.cookie = "mt_session=" + tokens[3];
  // keep it busy: absolute limit of 2 h still applies
  uint32_t t = 30;
  for (; t < 2UL * 3600000UL - 60000; t += 10 * 60000) TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(m, true, t));
  TEST_ASSERT_EQUAL(Verdict::Unauthorized, sec.check(m, true, 2UL * 3600000UL + 100));
}

static void test_login_lockout_and_code_rotation() {
  PortalSecurity sec(rng);
  sec.start(0);
  const std::string code = sec.code();
  const std::string wrong = code == "123456" ? "654321" : "123456";
  for (int i = 0; i < 5; i++) TEST_ASSERT_FALSE(sec.login(meta("POST"), wrong, 100 + i).ok);
  LoginResult lr = sec.login(meta("POST"), code, 200);  // locked even with the right code
  TEST_ASSERT_FALSE(lr.ok);
  TEST_ASSERT_EQUAL_STRING("locked", lr.error.c_str());
  TEST_ASSERT_TRUE(lr.retryAfterMs > 0);
  TEST_ASSERT_TRUE(sec.login(meta("POST"), code, 200 + 5 * 60000 + 1).ok);  // lock over
  // 15 failures in total from many addresses rotate the code
  const std::string code2 = sec.code();
  const uint32_t gen = sec.codeGeneration();
  const std::string wrong2 = code2 == "123456" ? "654321" : "123456";
  for (int i = 0; i < 15; i++) sec.login(meta("POST", "192.168.1." + std::to_string(100 + i / 4)), wrong2, 400000 + i);
  TEST_ASSERT_NOT_EQUAL(gen, sec.codeGeneration());
  TEST_ASSERT_FALSE(sec.login(meta("POST", "192.168.1.200"), code2, 400100).ok || sec.code() == code2);
}

static void test_rate_limit() {
  PortalSecurity sec(rng);
  sec.start(0);
  RequestMeta m = meta("GET");
  for (int i = 0; i < 120; i++) TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(m, false, 1000 + i));
  TEST_ASSERT_EQUAL(Verdict::RateLimited, sec.check(m, false, 2000));
  TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(meta("GET", "192.168.1.21"), false, 2000));  // per address
  TEST_ASSERT_EQUAL(Verdict::Ok, sec.check(m, false, 62000));                          // new window
  TEST_ASSERT_EQUAL(429, verdictStatus(Verdict::RateLimited));
  TEST_ASSERT_EQUAL(403, verdictStatus(Verdict::CsrfFailed));
  TEST_ASSERT_EQUAL(401, verdictStatus(Verdict::Unauthorized));
}

// ---------------------------------------------------------------- validation / mapping
static JsonValue parse(const std::string& s) {
  JsonValue j;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(parseBody(s, kMaxJsonBody, j, &err), err.c_str());
  return j;
}

static void test_request_validation() {
  JsonValue j;
  std::string err;
  TEST_ASSERT_FALSE(parseBody("", 100, j, &err));
  TEST_ASSERT_FALSE(parseBody("{\"a\":", 100, j, &err));
  TEST_ASSERT_FALSE(parseBody(std::string(200, ' ') + "{}", 100, j, &err));
  TEST_ASSERT_FALSE(parseBody("[1]", 100, j, &err));  // must be an object

  TEST_ASSERT_TRUE(validateProfileBody(parse("{\"name\":\"Anna\"}"), &err));
  TEST_ASSERT_FALSE(validateProfileBody(parse("{\"name\":\"\"}"), &err));
  TEST_ASSERT_FALSE(validateProfileBody(parse("{\"name\":7}"), &err));
  TEST_ASSERT_FALSE(validateProfileBody(parse("{\"name\":\"A\",\"pinHash\":\"x\"}"), &err));
  TEST_ASSERT_FALSE(validateProfileBody(parse("{\"name\":\"A\",\"routines\":{\"brunch\":\"10:00\"}}"), &err));
  TEST_ASSERT_FALSE(validateProfileBody(parse("{\"name\":\"A\",\"routines\":{\"wake\":\"25:00\"}}"), &err));
  TEST_ASSERT_TRUE(validateProfileBody(parse("{\"name\":\"Jos\xC3\xA9\",\"routines\":{\"wake\":\"06:30\"}}"), &err));
  TEST_ASSERT_FALSE(validateProfileBody(parse("{\"name\":\"bad\xC3\"}"), &err));  // broken UTF-8

  const char* ok = "{\"profile\":1,\"name\":\"Ibuprofen\",\"start\":\"2026-10-01\",\"phases\":[{\"rule\":{\"type\":\"fixed\","
                   "\"times\":[{\"at\":\"08:00\"}]}}]}";
  TEST_ASSERT_TRUE_MESSAGE(validateMedicationBody(parse(ok), &err), err.c_str());
  TEST_ASSERT_FALSE(validateMedicationBody(parse("{\"profile\":1,\"name\":\"X\",\"start\":\"2026-10-01\"}"), &err));
  TEST_ASSERT_FALSE(validateMedicationBody(parse("{\"profile\":9,\"name\":\"X\",\"start\":\"2026-10-01\",\"phases\":[]}"), &err));
  TEST_ASSERT_FALSE(validateMedicationBody(parse("{\"profile\":1.5,\"name\":\"X\",\"start\":\"2026-10-01\",\"phases\":[{\"rule\":{\"type\":\"prn\"}}]}"), &err));
  TEST_ASSERT_FALSE(validateRuleBody(parse("{\"type\":\"fixed\",\"times\":[{\"at\":\"8am\"}]}"), &err));
  TEST_ASSERT_FALSE(validateRuleBody(parse("{\"type\":\"every_n\",\"every\":0}"), &err));
  TEST_ASSERT_FALSE(validateRuleBody(parse("{\"type\":\"every_n\",\"every\":2,\"unit\":\"months\"}"), &err));
  TEST_ASSERT_FALSE(validateRuleBody(parse("{\"type\":\"weekdays\",\"weekdays\":128}"), &err));
  TEST_ASSERT_FALSE(validateRuleBody(parse("{\"type\":\"dates\",\"dates\":[\"2026-13-01\"]}"), &err));
  TEST_ASSERT_FALSE(validateRuleBody(parse("{\"type\":\"prn\",\"maxPer24h\":99}"), &err));
  TEST_ASSERT_TRUE(validateRuleBody(parse("{\"type\":\"routine\",\"times\":[{\"routine\":\"bed\",\"offset\":-30}]}"), &err));
  std::string many = "{\"type\":\"fixed\",\"times\":[";
  for (int i = 0; i < 13; i++) many += std::string(i ? "," : "") + "{\"at\":\"08:00\"}";
  TEST_ASSERT_FALSE(validateRuleBody(parse(many + "]}"), &err));
  core::Medication m;
  TEST_ASSERT_FALSE(medicationFromApi(parse("{\"profile\":1,\"name\":\"X\",\"form\":\"potion\",\"start\":\"2026-10-01\",\"phases\":[{\"rule\":{\"type\":\"prn\"}}]}"), 0, m, &err));
  TEST_ASSERT_EQUAL_STRING("unknown form 'potion'", err.c_str());
}

static void test_json_mapping() {
  core::Profile p;
  p.id = 2;
  p.name = "Anna";
  p.pinSalt = "salt";
  p.pinHash = "hash";
  const JsonValue j = profileToApi(p);
  TEST_ASSERT_FALSE(j.has("pinHash"));
  TEST_ASSERT_FALSE(j.has("pinSalt"));
  TEST_ASSERT_TRUE(j["hasPin"].asBool());
  // update keeps the existing PIN and id; initials are derived for new people
  core::Profile out;
  std::string err;
  TEST_ASSERT_TRUE(profileFromApi(parse("{\"name\":\"Anna Maria\",\"id\":99,\"hasPin\":false}"), &p, out, &err));
  TEST_ASSERT_EQUAL(2, out.id);
  TEST_ASSERT_EQUAL_STRING("hash", out.pinHash.c_str());
  TEST_ASSERT_EQUAL_STRING("Anna Maria", out.name.c_str());
  core::Profile fresh;
  TEST_ASSERT_TRUE(profileFromApi(parse("{\"name\":\"bob smith\"}"), nullptr, fresh, &err));
  TEST_ASSERT_EQUAL(0, fresh.id);
  TEST_ASSERT_EQUAL_STRING("BS", fresh.initials.c_str());
  TEST_ASSERT_TRUE(fresh.pinHash.empty());

  core::Medication m;
  TEST_ASSERT_TRUE(medicationFromApi(parse("{\"profile\":1,\"name\":\"Pred\",\"form\":\"pill\",\"start\":\"2026-10-01\",\"phases\":["
                                           "{\"to\":\"2026-10-05\",\"dose\":\"20 mg\",\"rule\":{\"type\":\"fixed\",\"times\":[{\"at\":\"08:00\"}]}},"
                                           "{\"from\":\"2026-10-06\",\"dose\":\"10 mg\",\"rule\":{\"type\":\"every_n\",\"every\":2,\"unit\":\"days\",\"times\":[{\"at\":\"08:00\"}]}}]}"),
                                     7, m, &err));
  TEST_ASSERT_EQUAL(7, m.id);
  TEST_ASSERT_EQUAL(2, m.phases.size());
  const JsonValue back = medicationToApi(m);
  TEST_ASSERT_EQUAL_STRING("Pred", back["name"].asString().c_str());
  TEST_ASSERT_EQUAL(2, back["phases"].size());
  TEST_ASSERT_EQUAL_STRING("every_n", back["phases"].at(1)["rule"]["type"].asString().c_str());
  std::vector<core::Phase> ph;
  TEST_ASSERT_TRUE(phasesFromApi(back["phases"], ph, &err));
  TEST_ASSERT_EQUAL(2, ph.size());
  TEST_ASSERT_EQUAL_STRING("{\"error\":\"x\"}", errorJson("x").dump().c_str());
}

// ---------------------------------------------------------------- NTP
static void putBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}
static void ntpReply(uint8_t* p, int64_t unixT, int stratum = 2, int li = 0, int mode = 4) {
  memset(p, 0, kNtpPacketSize);
  p[0] = uint8_t((li << 6) | (4 << 3) | mode);
  p[1] = uint8_t(stratum);
  putBe32(p + 40, uint32_t(unixT + kNtpUnixOffset));
  putBe32(p + 44, 0x80000000u);  // .5 s
}

static void test_ntp_packets() {
  uint8_t req[kNtpPacketSize];
  buildNtpRequest(req);
  TEST_ASSERT_EQUAL_HEX8(0x23, req[0]);  // LI 0, version 4, mode 3
  uint8_t p[kNtpPacketSize];
  const int64_t t = fx::berlinLocal(2026, 10, 18, 12, 0);
  ntpReply(p, t);
  int64_t u;
  uint32_t ms;
  std::string err;
  TEST_ASSERT_TRUE(parseNtpResponse(p, sizeof p, &u, &ms, &err));
  TEST_ASSERT_EQUAL_INT64(t, u);
  TEST_ASSERT_EQUAL(500, ms);
  TEST_ASSERT_FALSE(parseNtpResponse(p, 47, &u, &ms, &err));
  ntpReply(p, t, 0);  // kiss-o'-death
  TEST_ASSERT_FALSE(parseNtpResponse(p, sizeof p, &u, &ms, &err));
  ntpReply(p, t, 2, 3);  // unsynchronised
  TEST_ASSERT_FALSE(parseNtpResponse(p, sizeof p, &u, &ms, &err));
  ntpReply(p, t, 2, 0, 3);  // not a server reply
  TEST_ASSERT_FALSE(parseNtpResponse(p, sizeof p, &u, &ms, &err));
  ntpReply(p, fx::berlinLocal(2020, 1, 1, 0, 0));  // implausible year
  TEST_ASSERT_FALSE(parseNtpResponse(p, sizeof p, &u, &ms, &err));
  // era 1 (after 2036-02-07) still decodes
  const int64_t t2 = fx::berlinLocal(2040, 6, 1, 0, 0);
  ntpReply(p, t2);
  TEST_ASSERT_TRUE(parseNtpResponse(p, sizeof p, &u, &ms, &err));
  TEST_ASSERT_EQUAL_INT64(t2, u);
}

static void test_ntp_policy_and_trust() {
  const int64_t now = fx::berlinLocal(2026, 10, 18, 9, 0);
  TimeSyncState s;
  TEST_ASSERT_FALSE(syncDue(s, SyncReason::ColdBoot, now, false, true));  // no Wi-Fi
  TEST_ASSERT_FALSE(syncDue(s, SyncReason::ColdBoot, now, true, false));  // auto sync off
  TEST_ASSERT_TRUE(syncDue(s, SyncReason::ColdBoot, now, true, true));
  TEST_ASSERT_TRUE(syncDue(s, SyncReason::Manual, now, true, false));
  TEST_ASSERT_EQUAL(SyncTrust::NotTracked, syncTrust(s, now, true));
  recordSyncSuccess(s, now, now - 4);
  TEST_ASSERT_EQUAL(4, s.lastCorrectionSec);
  TEST_ASSERT_EQUAL(0, s.failures);
  TEST_ASSERT_FALSE(syncDue(s, SyncReason::Wake, now + 3600, true, true));
  TEST_ASSERT_TRUE(syncDue(s, SyncReason::Wake, now + 24 * 3600 + 1, true, true));
  TEST_ASSERT_TRUE(syncDue(s, SyncReason::DailySlot, now + 20 * 3600, true, true));
  TEST_ASSERT_EQUAL(SyncTrust::Fresh, syncTrust(s, now + 6 * 86400, true));
  TEST_ASSERT_EQUAL(SyncTrust::Stale, syncTrust(s, now + 7 * 86400 + 1, true));
  TEST_ASSERT_EQUAL(SyncTrust::NotTracked, syncTrust(s, now + 30 * 86400, false));
  // failures back off: 15 min doubling, capped at 6 h
  TEST_ASSERT_EQUAL_INT64(15 * 60, retryAfterSec(1));
  TEST_ASSERT_EQUAL_INT64(30 * 60, retryAfterSec(2));
  TEST_ASSERT_EQUAL_INT64(6 * 3600, retryAfterSec(20));
  const int64_t later = now + 2 * 86400;
  recordSyncFailure(s, later);
  TEST_ASSERT_EQUAL(1, s.failures);
  TEST_ASSERT_FALSE(syncDue(s, SyncReason::Wake, later + 60, true, true));
  TEST_ASSERT_TRUE(syncDue(s, SyncReason::Wake, later + 15 * 60 + 1, true, true));
  TEST_ASSERT_TRUE(syncDue(s, SyncReason::Manual, later + 60, true, true));
}

// ---------------------------------------------------------------- OTA
struct FakeOta : IOtaTarget {
  size_t cap = 1 << 20;
  std::string data;
  bool begun = false, committed = false, aborted = false, failWrite = false;
  size_t capacity() const override { return cap; }
  bool begin(size_t, std::string*) override {
    begun = true;
    data.clear();
    return true;
  }
  bool write(const uint8_t* p, size_t n, std::string* err) override {
    if (failWrite) {
      if (err) *err = "flash error";
      return false;
    }
    data.append(reinterpret_cast<const char*>(p), n);
    return true;
  }
  bool commit(std::string*) override { return committed = true; }
  void abort() override { aborted = true; }
};

static std::string fakeImage(size_t size, uint16_t chip = kChipIdEsp32S3, const char* project = "rxnudge", const char* ver = "0.6.0") {
  std::string img(size, '\0');
  for (size_t i = 0; i < size; i++) img[i] = char((i * 31 + 7) & 0xFF);
  img[0] = char(kEspImageMagic);
  img[12] = char(chip & 0xFF);
  img[13] = char(chip >> 8);
  uint8_t* d = reinterpret_cast<uint8_t*>(&img[32]);
  d[0] = 0x32, d[1] = 0x54, d[2] = 0xCD, d[3] = 0xAB;
  memset(d + 16, 0, 32);
  memcpy(d + 16, ver, strlen(ver));
  memset(d + 48, 0, 32);
  memcpy(d + 48, project, strlen(project));
  memset(d + 112, 0, 32);
  memcpy(d + 112, "v4.4.7", 6);
  return img;
}

static bool pushChunks(OtaSession& s, const std::string& img, size_t chunk, std::string* err) {
  for (size_t i = 0; i < img.size(); i += chunk)
    if (!s.write(reinterpret_cast<const uint8_t*>(img.data() + i), std::min(chunk, img.size() - i), err)) return false;
  return true;
}

static void test_ota_verification() {
  const std::string img = fakeImage(5000);
  const std::string sha = Sha256::hex(img);
  std::string err;
  {
    FakeOta t;
    OtaSession s(t, "rxnudge");
    TEST_ASSERT_TRUE(s.begin(img.size(), sha, &err));
    TEST_ASSERT_TRUE(pushChunks(s, img, 7, &err));  // header split across tiny chunks
    TEST_ASSERT_TRUE_MESSAGE(s.finish(&err), err.c_str());
    TEST_ASSERT_TRUE(t.committed);
    TEST_ASSERT_TRUE(t.data == img);
    TEST_ASSERT_EQUAL_STRING("0.6.0", s.descriptor().version.c_str());
    TEST_ASSERT_EQUAL_STRING("v4.4.7", s.descriptor().idfVersion.c_str());
  }
  {  // wrong hash: never committed
    FakeOta t;
    OtaSession s(t, "rxnudge");
    std::string bad = sha;
    bad[0] = bad[0] == 'a' ? 'b' : 'a';
    TEST_ASSERT_TRUE(s.begin(img.size(), bad, &err));
    TEST_ASSERT_TRUE(pushChunks(s, img, 512, &err));
    TEST_ASSERT_FALSE(s.finish(&err));
    TEST_ASSERT_FALSE(t.committed);
    TEST_ASSERT_TRUE(t.aborted);
  }
  {  // one flipped bit in the payload
    FakeOta t;
    OtaSession s(t, "rxnudge");
    std::string mod = img;
    mod[4000] ^= 1;
    TEST_ASSERT_TRUE(s.begin(mod.size(), sha, &err));
    TEST_ASSERT_TRUE(pushChunks(s, mod, 1000, &err));
    TEST_ASSERT_FALSE(s.finish(&err));
    TEST_ASSERT_FALSE(t.committed);
  }
  {  // missing / malformed hash, too big, too small
    FakeOta t;
    OtaSession s(t, "rxnudge");
    TEST_ASSERT_FALSE(s.begin(img.size(), "", &err));
    OtaSession s2(t, "rxnudge");
    t.cap = 1000;
    TEST_ASSERT_FALSE(s2.begin(img.size(), sha, &err));
    t.cap = 1 << 20;
    OtaSession s3(t, "rxnudge");
    TEST_ASSERT_FALSE(s3.begin(100, sha, &err));
  }
  {  // wrong chip, wrong project, not an image, short upload, extra data
    FakeOta t;
    const std::string esp32 = fakeImage(5000, 0);
    OtaSession s(t, "rxnudge");
    TEST_ASSERT_TRUE(s.begin(esp32.size(), Sha256::hex(esp32), &err));
    TEST_ASSERT_FALSE(pushChunks(s, esp32, 1000, &err));
    TEST_ASSERT_EQUAL_STRING("firmware is not for ESP32-S3", err.c_str());
    const std::string other = fakeImage(5000, kChipIdEsp32S3, "blink");
    OtaSession s2(t, "rxnudge");
    TEST_ASSERT_TRUE(s2.begin(other.size(), Sha256::hex(other), &err));
    TEST_ASSERT_FALSE(pushChunks(s2, other, 1000, &err));
    std::string junk = img;
    junk[0] = 'P';
    OtaSession s3(t, "rxnudge");
    TEST_ASSERT_TRUE(s3.begin(junk.size(), Sha256::hex(junk), &err));
    TEST_ASSERT_FALSE(pushChunks(s3, junk, 1000, &err));
    OtaSession s4(t, "rxnudge");
    TEST_ASSERT_TRUE(s4.begin(img.size(), sha, &err));
    TEST_ASSERT_TRUE(pushChunks(s4, img.substr(0, 4000), 1000, &err));
    TEST_ASSERT_FALSE(s4.finish(&err));
    OtaSession s5(t, "rxnudge");
    TEST_ASSERT_TRUE(s5.begin(4000, Sha256::hex(img.substr(0, 4000)), &err));
    TEST_ASSERT_FALSE(pushChunks(s5, img, 1000, &err));
    t.failWrite = true;
    OtaSession s6(t, "rxnudge");
    TEST_ASSERT_TRUE(s6.begin(img.size(), sha, &err));
    TEST_ASSERT_FALSE(pushChunks(s6, img, 1000, &err));
  }
}

// ---------------------------------------------------------------- notifications
static void test_notifications() {
  TEST_ASSERT_TRUE(isNtfyUrl("https://ntfy.sh/abc"));
  TEST_ASSERT_FALSE(isNtfyUrl("https://example.com/hook"));
  std::vector<MissedDose> md(1);
  md[0].profileId = 1;
  md[0].person = "Anna";
  md[0].initials = "A";
  md[0].med = "Metformin";
  md[0].scheduledUtc = fx::berlinLocal(2026, 10, 18, 8, 0);
  OutboundRequest r = buildMissedNotification("https://ntfy.sh/abc", false, md, fx::berlin());
  TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/abc", r.url.c_str());
  TEST_ASSERT_TRUE(r.body.find("Metformin") == std::string::npos);
  TEST_ASSERT_TRUE(r.body.find("Anna") == std::string::npos);
  TEST_ASSERT_TRUE(r.body.find("08:00") != std::string::npos);
  bool title = false;
  for (const auto& h : r.headers) {
    title = title || h.first == "Title";
    TEST_ASSERT_TRUE(h.second.find("Metformin") == std::string::npos);
  }
  TEST_ASSERT_TRUE(title);
  r = buildMissedNotification("https://ntfy.sh/abc", true, md, fx::berlin());
  TEST_ASSERT_TRUE(r.body.find("Metformin") != std::string::npos);
  r = buildMissedNotification("https://example.com/hook", false, md, fx::berlin());
  TEST_ASSERT_EQUAL_STRING("application/json", r.contentType.c_str());
  JsonValue j;
  std::string err;
  TEST_ASSERT_TRUE(core::parseJson(r.body, j, &err));
  TEST_ASSERT_EQUAL_STRING("missed_dose", j["event"].asString().c_str());
  TEST_ASSERT_TRUE(r.body.find("Metformin") == std::string::npos);
  r = buildMissedNotification("https://example.com/hook", true, md, fx::berlin());
  TEST_ASSERT_TRUE(r.body.find("Metformin") != std::string::npos);
  r = buildTestNotification("https://ntfy.sh/abc");
  TEST_ASSERT_FALSE(r.body.empty());
}

// ---------------------------------------------------------------- portal end to end
struct PortalRig {
  core::FakeClock clk;
  core::Database db;
  storage::MemFs internal, sd;
  storage::FsRepository repo{internal};
  storage::FsDoseLog log{internal};
  std::unique_ptr<app::AppModel> model;
  FakeNetService netSvc;
  std::unique_ptr<app::AdminService> svc;
  PortalSecurity sec{rng};
  FakeOta ota;
  std::unique_ptr<PortalApi> api;
  std::string cookie, csrf;
  uint32_t ms = 1000;

  PortalRig() : clk(fx::berlinLocal(2026, 10, 18, 9, 0)) {
    const core::LocalTime lt = core::utcToLocal(fx::berlin(), clk.nowUtc());
    db = core::makeDemoDatabase(lt.days - 20);
    db.settings.setupDone = true;
    model.reset(new app::AppModel(clk, db, &log));
    model->startEngine(clk.nowUtc());
    model->process();
    app::AdminHooks h;
    h.entropy = []() { return 0x42ull; };
    svc.reset(new app::AdminService(*model, &repo, &internal, &sd, netSvc, nullptr, h, "0.6.0"));
    api.reset(new PortalApi(*svc, sec, &ota, "rxnudge"));
    sec.setDeviceIp("192.168.1.50");
    sec.start(ms);
  }
  HttpResponse req(const std::string& method, const std::string& target, const std::string& body = "", bool auth = true) {
    HttpRequest r;
    const size_t q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string::npos) r.query = target.substr(q + 1);
    r.body = body;
    r.meta = meta(method);
    if (auth) {
      r.meta.cookie = "mt_session=" + cookie;
      r.meta.csrf = csrf;
    }
    ms += 10;
    return api->handle(r, ms);
  }
  void login() {
    HttpResponse r = req("POST", "/api/login", "{\"code\":\"" + sec.code() + "\"}", false);
    TEST_ASSERT_EQUAL(200, r.status);
    const std::string* sc = r.header("Set-Cookie");
    TEST_ASSERT_NOT_NULL(sc);
    TEST_ASSERT_TRUE(sc->find("HttpOnly") != std::string::npos);
    TEST_ASSERT_TRUE(sc->find("SameSite=Strict") != std::string::npos);
    cookie = sc->substr(11, sc->find(';') - 11);
    csrf = parse(r.body)["csrf"].asString();
  }
};

static JsonValue body(const HttpResponse& r) {
  JsonValue j;
  std::string err;
  TEST_ASSERT_TRUE_MESSAGE(core::parseJson(r.body, j, &err), r.body.c_str());
  return j;
}

static void test_portal_assets_and_auth() {
  PortalRig p;
  HttpResponse r = p.req("GET", "/", "", false);
  TEST_ASSERT_EQUAL(200, r.status);
  TEST_ASSERT_TRUE(r.gzip);
  TEST_ASSERT_EQUAL_HEX8(0x1F, uint8_t(r.body[0]));  // gzip magic
  TEST_ASSERT_TRUE(r.contentType.find("text/html") == 0);
  TEST_ASSERT_NOT_NULL(r.header("Content-Security-Policy"));
  TEST_ASSERT_EQUAL_STRING("DENY", r.header("X-Frame-Options")->c_str());
  TEST_ASSERT_EQUAL(200, p.req("GET", "/app.js", "", false).status);
  TEST_ASSERT_TRUE(webAssetsTotalBytes() < 16 * 1024);
  TEST_ASSERT_EQUAL(404, p.req("GET", "/setup.html", "", false).status);  // hotspot page only
  TEST_ASSERT_EQUAL(404, p.req("GET", "/nope", "", false).status);
  r = p.req("GET", "/api/info", "", false);
  TEST_ASSERT_EQUAL(200, r.status);
  TEST_ASSERT_FALSE(body(r)["tls"].asBool(true));
  TEST_ASSERT_EQUAL(401, p.req("GET", "/api/state", "", false).status);
  TEST_ASSERT_EQUAL(401, p.req("POST", "/api/login", "{\"code\":\"x\"}", false).status);
  TEST_ASSERT_EQUAL(400, p.req("POST", "/api/login", "nope", false).status);
  p.login();
  r = p.req("GET", "/api/state");
  TEST_ASSERT_EQUAL(200, r.status);
  const JsonValue st = body(r);
  TEST_ASSERT_EQUAL(p.db.profiles.size(), st["profiles"].size());
  TEST_ASSERT_TRUE(r.body.find("pinHash") == std::string::npos);
  TEST_ASSERT_EQUAL_STRING("2026-10-18", st["today"].asString().c_str());
  r = p.req("GET", "/api/session");
  TEST_ASSERT_EQUAL_STRING(p.csrf.c_str(), body(r)["csrf"].asString().c_str());
  // mutation without CSRF token
  const std::string saved = p.csrf;
  p.csrf = "";
  TEST_ASSERT_EQUAL(403, p.req("POST", "/api/profiles", "{\"name\":\"Eve\"}").status);
  p.csrf = saved;
  // wrong host / WAN
  HttpRequest x;
  x.path = "/api/state";
  x.meta = meta("GET", "93.184.216.34");
  x.meta.cookie = "mt_session=" + p.cookie;
  TEST_ASSERT_EQUAL(403, p.api->handle(x, p.ms).status);
  TEST_ASSERT_EQUAL(200, p.req("POST", "/api/logout", "{}").status);
  TEST_ASSERT_EQUAL(401, p.req("GET", "/api/state").status);
}

static void test_portal_crud() {
  PortalRig p;
  p.login();
  // add a person
  HttpResponse r = p.req("POST", "/api/profiles", "{\"name\":\"Grandpa Joe\",\"routines\":{\"wake\":\"06:00\"}}");
  TEST_ASSERT_EQUAL_MESSAGE(201, r.status, r.body.c_str());
  const int pid = int(body(r)["id"].asInt());
  TEST_ASSERT_TRUE(pid > 0);
  TEST_ASSERT_EQUAL(422, p.req("POST", "/api/profiles", "{\"name\":\"\"}").status);
  TEST_ASSERT_EQUAL(404, p.req("PUT", "/api/profiles/77", "{\"name\":\"X\"}").status);
  r = p.req("PUT", "/api/profiles/" + std::to_string(pid), "{\"name\":\"Grandpa\"}");
  TEST_ASSERT_EQUAL(200, r.status);
  TEST_ASSERT_EQUAL_STRING("Grandpa", p.svc->database().profile(uint8_t(pid))->name.c_str());
  // add a medication for him
  const std::string med = "{\"profile\":" + std::to_string(pid) +
                          ",\"name\":\"Aspirin\",\"dose\":\"100 mg\",\"form\":\"pill\",\"start\":\"2026-10-18\",\"phases\":[{\"rule\":"
                          "{\"type\":\"fixed\",\"times\":[{\"at\":\"20:00\"}]}}]}";
  r = p.req("POST", "/api/meds", med);
  TEST_ASSERT_EQUAL_MESSAGE(201, r.status, r.body.c_str());
  const int mid = int(body(r)["id"].asInt());
  TEST_ASSERT_NOT_NULL(p.svc->database().med(uint16_t(mid)));
  // the engine scheduled tonight's dose
  p.model->process();
  bool scheduled = false;
  for (const auto& d : p.model->engine().instances()) scheduled = scheduled || d.medId == mid;
  TEST_ASSERT_TRUE(scheduled);
  // schedule change only
  r = p.req("PUT", "/api/meds/" + std::to_string(mid) + "/schedule",
            "{\"phases\":[{\"rule\":{\"type\":\"weekdays\",\"weekdays\":1,\"times\":[{\"at\":\"07:00\"}]}}]}");
  TEST_ASSERT_EQUAL_MESSAGE(200, r.status, r.body.c_str());
  TEST_ASSERT_EQUAL(int(core::ScheduleType::Weekdays), int(p.svc->database().med(uint16_t(mid))->phases[0].rule.type));
  TEST_ASSERT_EQUAL(422, p.req("PUT", "/api/meds/" + std::to_string(mid) + "/schedule", "{\"phases\":[]}").status);
  // validation errors map to 422; unknown resources to 404
  TEST_ASSERT_EQUAL(422, p.req("POST", "/api/meds", "{\"profile\":1,\"name\":\"X\"}").status);
  TEST_ASSERT_EQUAL(404, p.req("PUT", "/api/meds/999", med).status);
  TEST_ASSERT_EQUAL(404, p.req("GET", "/api/meds/abc").status);
  // person with medications cannot be deleted; delete needs archive first
  TEST_ASSERT_EQUAL(409, p.req("DELETE", "/api/profiles/" + std::to_string(pid)).status);
  TEST_ASSERT_EQUAL(409, p.req("DELETE", "/api/meds/" + std::to_string(mid)).status);
  TEST_ASSERT_EQUAL(422, p.req("POST", "/api/meds/" + std::to_string(mid) + "/archive", "{\"active\":\"no\"}").status);
  TEST_ASSERT_EQUAL(200, p.req("POST", "/api/meds/" + std::to_string(mid) + "/archive", "{\"active\":false}").status);
  TEST_ASSERT_EQUAL(200, p.req("DELETE", "/api/meds/" + std::to_string(mid)).status);
  TEST_ASSERT_NULL(p.svc->database().med(uint16_t(mid)));
  TEST_ASSERT_EQUAL(200, p.req("DELETE", "/api/profiles/" + std::to_string(pid)).status);
  TEST_ASSERT_NULL(p.svc->database().profile(uint8_t(pid)));
  TEST_ASSERT_EQUAL(405, p.req("PUT", "/api/login", "{}").status);
}

static void test_portal_history_export_backup_restore() {
  PortalRig p;
  p.login();
  HttpResponse r = p.req("GET", "/api/history?days=30");
  TEST_ASSERT_EQUAL(200, r.status);
  TEST_ASSERT_TRUE(body(r)["entries"].isArray());
  r = p.req("GET", "/api/export.csv");
  TEST_ASSERT_EQUAL(200, r.status);
  TEST_ASSERT_TRUE(r.contentType.find("text/csv") == 0);
  const std::string* cd = r.header("Content-Disposition");
  TEST_ASSERT_NOT_NULL(cd);
  TEST_ASSERT_TRUE(cd->find("attachment; filename=\"rxnudge-history-20261018-0900.csv\"") == 0);
  r = p.req("GET", "/api/export.json?names=1");
  TEST_ASSERT_EQUAL(200, r.status);
  r = p.req("GET", "/api/backup");
  TEST_ASSERT_EQUAL(200, r.status);
  const std::string backup = r.body;
  TEST_ASSERT_TRUE(backup.find("wifiPass") == std::string::npos);
  // change something, then restore the backup
  const core::Database before = p.svc->database();
  TEST_ASSERT_EQUAL(201, p.req("POST", "/api/profiles", "{\"name\":\"Temp\"}").status);
  TEST_ASSERT_EQUAL(before.profiles.size() + 1, p.svc->database().profiles.size());
  TEST_ASSERT_EQUAL(422, p.req("POST", "/api/restore", "{\"not\":\"a backup\"}").status);
  TEST_ASSERT_EQUAL(before.profiles.size() + 1, p.svc->database().profiles.size());  // untouched
  r = p.req("POST", "/api/restore", backup);
  TEST_ASSERT_EQUAL_MESSAGE(200, r.status, r.body.c_str());
  TEST_ASSERT_EQUAL(before.profiles.size(), p.svc->database().profiles.size());
}

static void test_portal_notify_settings() {
  PortalRig p;
  p.login();
  HttpResponse r = p.req("GET", "/api/notify");
  TEST_ASSERT_FALSE(body(r)["enabled"].asBool(true));
  TEST_ASSERT_EQUAL(422, p.req("PUT", "/api/notify", "{\"enabled\":true}").status);  // no URL yet
  TEST_ASSERT_EQUAL(422, p.req("PUT", "/api/notify", "{\"url\":\"ftp://x\"}").status);
  r = p.req("PUT", "/api/notify", "{\"enabled\":true,\"url\":\"https://ntfy.sh/family-123\",\"includeNames\":false}");
  TEST_ASSERT_EQUAL_MESSAGE(200, r.status, r.body.c_str());
  const core::Settings s = p.svc->database().settings;
  TEST_ASSERT_TRUE(s.notifyEnabled);
  TEST_ASSERT_FALSE(s.notifyNames);
  TEST_ASSERT_EQUAL_STRING("https://ntfy.sh/family-123", s.notifyUrl.c_str());
}

static void test_portal_ota() {
  PortalRig p;
  p.login();
  const std::string img = fakeImage(4096);
  HttpRequest r;
  r.path = "/api/ota";
  r.meta = meta("POST");
  r.meta.cookie = "mt_session=" + p.cookie;
  r.meta.csrf = p.csrf;
  r.body = img;
  r.sha256 = std::string(64, '0');
  HttpResponse resp = p.api->handle(r, p.ms += 10);
  TEST_ASSERT_EQUAL(422, resp.status);
  TEST_ASSERT_FALSE(p.ota.committed);
  TEST_ASSERT_FALSE(p.api->rebootRequested());
  r.sha256 = "";
  TEST_ASSERT_EQUAL(422, p.api->handle(r, p.ms += 10).status);
  r.sha256 = Sha256::hex(img);
  r.meta.csrf = "";
  TEST_ASSERT_EQUAL(403, p.api->handle(r, p.ms += 10).status);
  r.meta.csrf = p.csrf;
  resp = p.api->handle(r, p.ms += 10);
  TEST_ASSERT_EQUAL_MESSAGE(200, resp.status, resp.body.c_str());
  TEST_ASSERT_TRUE(p.ota.committed);
  TEST_ASSERT_TRUE(p.api->rebootRequested());
  TEST_ASSERT_EQUAL_STRING("0.6.0", body(resp)["version"].asString().c_str());
  // streaming interface, as used by the device server
  PortalRig q;
  q.login();
  r.meta.cookie = "mt_session=" + q.cookie;
  r.meta.csrf = q.csrf;
  bool ok = false;
  q.api->otaBegin(r, img.size(), q.ms, &ok);
  TEST_ASSERT_TRUE(ok);
  std::string err;
  for (size_t i = 0; i < img.size(); i += 1000)
    TEST_ASSERT_TRUE(q.api->otaChunk(reinterpret_cast<const uint8_t*>(img.data() + i), std::min<size_t>(1000, img.size() - i), &err));
  TEST_ASSERT_EQUAL(200, q.api->otaFinish(q.ms).status);
  TEST_ASSERT_TRUE(q.ota.data == img);
}

static void test_wifi_credentials() {
  TEST_ASSERT_EQUAL_STRING("", validateCredentials("Home", "").c_str());  // open network
  TEST_ASSERT_EQUAL_STRING("", validateCredentials("Home", "correct horse").c_str());
  TEST_ASSERT_FALSE(validateCredentials("", "password1").empty());
  TEST_ASSERT_FALSE(validateCredentials(std::string(33, 'x'), "password1").empty());
  TEST_ASSERT_FALSE(validateCredentials("Home", "short").empty());
  TEST_ASSERT_FALSE(validateCredentials("Home", std::string(64, 'p')).empty());
  TEST_ASSERT_FALSE(validateCredentials("Home", std::string("pass\nword")).empty());
}

static void test_query_helpers() {
  TEST_ASSERT_EQUAL_STRING("2", queryParam("profile=2&days=7", "profile").c_str());
  TEST_ASSERT_EQUAL_STRING("7", queryParam("profile=2&days=7", "days").c_str());
  TEST_ASSERT_EQUAL_STRING("", queryParam("profile=2", "days").c_str());
  TEST_ASSERT_EQUAL_STRING("a b/c", queryParam("x=a+b%2Fc", "x").c_str());
  TEST_ASSERT_EQUAL_STRING("Too Many Requests", statusText(429));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_sha256_vectors);
  RUN_TEST(test_lan_and_host_checks);
  RUN_TEST(test_login_session_csrf);
  RUN_TEST(test_session_absolute_expiry_and_limit);
  RUN_TEST(test_login_lockout_and_code_rotation);
  RUN_TEST(test_rate_limit);
  RUN_TEST(test_request_validation);
  RUN_TEST(test_json_mapping);
  RUN_TEST(test_ntp_packets);
  RUN_TEST(test_ntp_policy_and_trust);
  RUN_TEST(test_ota_verification);
  RUN_TEST(test_notifications);
  RUN_TEST(test_portal_assets_and_auth);
  RUN_TEST(test_portal_crud);
  RUN_TEST(test_portal_history_export_backup_restore);
  RUN_TEST(test_portal_notify_settings);
  RUN_TEST(test_portal_ota);
  RUN_TEST(test_wifi_credentials);
  RUN_TEST(test_query_helpers);
  return UNITY_END();
}
