#include "net_core/portal_api.h"

#include <cstdlib>

#include "app_core/admin_screens.h"
#include "app_core/format.h"
#include "core/civil.h"
#include "core/codec.h"
#include "net_core/api_json.h"
#include "net_core/web_assets.h"
#include "storage/export.h"

namespace net {

using core::JsonValue;

std::string urlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') o.push_back(' ');
    else if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
             isxdigit(static_cast<unsigned char>(s[i + 2]))) {
      o.push_back(static_cast<char>(strtol(s.substr(i + 1, 2).c_str(), nullptr, 16)));
      i += 2;
    } else {
      o.push_back(s[i]);
    }
  }
  return o;
}

std::string queryParam(const std::string& q, const std::string& key) {
  size_t i = 0;
  while (i <= q.size()) {
    size_t amp = q.find('&', i);
    if (amp == std::string::npos) amp = q.size();
    const std::string part = q.substr(i, amp - i);
    const size_t eq = part.find('=');
    if (urlDecode(part.substr(0, eq)) == key) return eq == std::string::npos ? "" : urlDecode(part.substr(eq + 1));
    i = amp + 1;
  }
  return "";
}

const char* statusText(int s) {
  switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "";
  }
}

HttpResponse jsonResponse(int status, const JsonValue& j) {
  HttpResponse r;
  r.status = status;
  r.body = j.dump();
  return r;
}

HttpResponse errorResponse(int status, const std::string& m) { return jsonResponse(status, errorJson(m)); }

static HttpResponse okJson() {
  JsonValue j = JsonValue::object();
  j.set("ok", JsonValue::boolean(true));
  return jsonResponse(200, j);
}

static void addSecurityHeaders(HttpResponse& r) {
  r.headers.push_back({"Cache-Control", "no-store"});
  r.headers.push_back({"X-Content-Type-Options", "nosniff"});
  r.headers.push_back({"X-Frame-Options", "DENY"});
  r.headers.push_back({"Referrer-Policy", "no-referrer"});
  r.headers.push_back({"Content-Security-Policy",
                       "default-src 'self'; style-src 'self' 'unsafe-inline'; img-src 'self' data:; frame-ancestors 'none'; "
                       "form-action 'self'"});
}

// "/api/meds/12/archive" -> segments
static std::vector<std::string> segments(const std::string& path) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < path.size()) {
    if (path[i] == '/') {
      i++;
      continue;
    }
    const size_t j = path.find('/', i);
    out.push_back(path.substr(i, j == std::string::npos ? std::string::npos : j - i));
    if (j == std::string::npos) break;
    i = j;
  }
  return out;
}

static int parseId(const std::string& s) {
  if (s.empty() || s.size() > 5) return -1;
  for (char c : s)
    if (c < '0' || c > '9') return -1;
  return atoi(s.c_str());
}

PortalApi::PortalApi(app::AdminService& admin, PortalSecurity& sec, IOtaTarget* ota, std::string project)
    : admin_(admin), sec_(sec), ota_(ota), project_(std::move(project)) {}

HttpResponse PortalApi::handle(const HttpRequest& r, uint32_t nowMs) {
  HttpResponse resp = route(r, nowMs);
  addSecurityHeaders(resp);
  return resp;
}

HttpResponse PortalApi::asset(const std::string& path) {
  HttpResponse r;
  if (path != "/" && path != "/app.js") return errorResponse(404, "not found");  // setup.html is for the hotspot only
  const WebAsset* a = findWebAsset(path == "/" ? "/index.html" : path);
  if (!a) return errorResponse(404, "not found");
  r.contentType = a->contentType;
  r.body.assign(reinterpret_cast<const char*>(a->data), a->size);
  r.gzip = true;
  return r;
}

HttpResponse PortalApi::route(const HttpRequest& r, uint32_t nowMs) {
  const std::string& m = r.meta.method;
  const bool isApi = r.path.compare(0, 5, "/api/") == 0;
  const bool open = !isApi || r.path == "/api/info" || r.path == "/api/login";
  const Verdict v = sec_.check(r.meta, !open, nowMs);
  if (v != Verdict::Ok) {
    HttpResponse e = errorResponse(verdictStatus(v), verdictName(v));
    if (v == Verdict::RateLimited) e.headers.push_back({"Retry-After", "60"});
    return e;
  }
  if (!isApi) {
    if (m != "GET") return errorResponse(405, "method not allowed");
    return asset(r.path);
  }
  if (r.path == "/api/info" && m == "GET") {
    JsonValue j = JsonValue::object();
    j.set("name", JsonValue::string("RxNudge"));
    j.set("version", JsonValue::string(admin_.version()));
    j.set("tls", JsonValue::boolean(false));
    j.set("note", JsonValue::string("No TLS: traffic on this network is not encrypted. Use only on your home Wi-Fi."));
    return jsonResponse(200, j);
  }
  if (r.path == "/api/login") return m == "POST" ? login(r, nowMs) : errorResponse(405, "method not allowed");
  if (r.path == "/api/session" && m == "GET") {
    std::string csrf;
    sec_.check(r.meta, true, nowMs, &csrf);
    JsonValue j = JsonValue::object();
    j.set("csrf", JsonValue::string(csrf));
    return jsonResponse(200, j);
  }
  if (r.path == "/api/logout" && m == "POST") {
    sec_.logout(r.meta);
    HttpResponse o = okJson();
    o.headers.push_back({"Set-Cookie", std::string(PortalSecurity::kCookie) + "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0"});
    return o;
  }
  const std::vector<std::string> seg = segments(r.path);  // "api", ...
  const std::string res = seg.size() > 1 ? seg[1] : "";
  const int id = seg.size() > 2 ? parseId(seg[2]) : 0;
  if (seg.size() > 2 && id < 0) return errorResponse(404, "not found");
  if (res == "state" && m == "GET") return state();
  if (res == "profiles") {
    if (seg.size() == 2 && m == "POST") return saveProfile(r, 0);
    if (seg.size() == 3 && m == "PUT") return saveProfile(r, id);
    if (seg.size() == 3 && m == "DELETE") return deleteProfile(id);
  }
  if (res == "meds") {
    if (seg.size() == 2 && m == "POST") return saveMed(r, 0);
    if (seg.size() == 3 && m == "PUT") return saveMed(r, id);
    if (seg.size() == 3 && m == "DELETE") return deleteMed(id);
    if (seg.size() == 4 && seg[3] == "archive" && m == "POST") return archiveMed(r, id);
    if (seg.size() == 4 && seg[3] == "schedule" && m == "PUT") return saveSchedule(r, id);
  }
  if (res == "history" && m == "GET") return history(r);
  if (res == "export.csv" && m == "GET") return exportData(r, true);
  if (res == "export.json" && m == "GET") return exportData(r, false);
  if (res == "backup" && m == "GET") return backup();
  if (res == "restore" && m == "POST") return restore(r);
  if (res == "notify" && m == "GET") return notifyGet();
  if (res == "notify" && m == "PUT") return notifyPut(r);
  if (res == "ota" && m == "POST") {
    bool ok = false;
    HttpResponse b = otaBegin(r, r.body.size(), nowMs, &ok);
    if (!ok) return b;
    std::string err;
    if (!otaChunk(reinterpret_cast<const uint8_t*>(r.body.data()), r.body.size(), &err)) return errorResponse(422, err);
    return otaFinish(nowMs);
  }
  return errorResponse(404, "not found");
}

HttpResponse PortalApi::login(const HttpRequest& r, uint32_t nowMs) {
  JsonValue j;
  std::string err;
  if (!parseBody(r.body, 256, j, &err)) return errorResponse(400, err);
  const LoginResult lr = sec_.login(r.meta, j["code"].asString(), nowMs);
  if (!lr.ok) {
    HttpResponse e = errorResponse(lr.error == "locked" ? 429 : 401, lr.error == "locked" ? "too many attempts, wait 5 minutes"
                                                                                         : "wrong or expired code");
    if (lr.retryAfterMs) e.headers.push_back({"Retry-After", std::to_string((lr.retryAfterMs + 999) / 1000)});
    return e;
  }
  JsonValue o = JsonValue::object();
  o.set("csrf", JsonValue::string(lr.csrf));
  HttpResponse resp = jsonResponse(200, o);
  resp.headers.push_back({"Set-Cookie", std::string(PortalSecurity::kCookie) + "=" + lr.token +
                                            "; Path=/; HttpOnly; SameSite=Strict; Max-Age=7200"});
  return resp;
}

HttpResponse PortalApi::state() {
  const core::Database db = admin_.database();
  JsonValue j = JsonValue::object();
  j.set("version", JsonValue::string(admin_.version()));
  const core::LocalTime lt = admin_.localNow();
  j.set("today", JsonValue::string(core::formatDate(lt.days)));
  j.set("now", JsonValue::string(core::formatHm(lt.secOfDay / 60)));
  j.set("tzName", JsonValue::string(db.settings.tzName));
  j.set("maxProfiles", JsonValue::integer(static_cast<int64_t>(core::kMaxProfiles)));
  JsonValue ps = JsonValue::array();
  for (const auto& p : db.profiles) ps.push(profileToApi(p));
  j.set("profiles", ps);
  JsonValue ms = JsonValue::array();
  for (const auto& m : db.meds) ms.push(medicationToApi(m));
  j.set("meds", ms);
  return jsonResponse(200, j);
}

HttpResponse PortalApi::saveProfile(const HttpRequest& r, int id) {
  JsonValue j;
  std::string err;
  if (!parseBody(r.body, kMaxJsonBody, j, &err)) return errorResponse(400, err);
  const core::Database db = admin_.database();
  const core::Profile* existing = nullptr;
  if (id) {
    existing = db.profile(static_cast<uint8_t>(id));
    if (!existing) return errorResponse(404, "person not found");
  } else if (db.profiles.size() >= core::kMaxProfiles) {
    return errorResponse(409, "8 people is the maximum");
  }
  core::Profile p;
  if (!profileFromApi(j, existing, p, &err)) return errorResponse(422, err);
  err = admin_.saveProfile(p);
  if (!err.empty()) return errorResponse(422, err);
  return jsonResponse(id ? 200 : 201, profileToApi(p));
}

HttpResponse PortalApi::deleteProfile(int id) {
  const std::string e = admin_.deleteProfile(static_cast<uint8_t>(id));
  if (e == "Person not found") return errorResponse(404, e);
  if (!e.empty()) return errorResponse(409, e);
  return okJson();
}

HttpResponse PortalApi::saveMed(const HttpRequest& r, int id) {
  JsonValue j;
  std::string err;
  if (!parseBody(r.body, kMaxJsonBody, j, &err)) return errorResponse(400, err);
  if (id && !admin_.database().med(static_cast<uint16_t>(id))) return errorResponse(404, "medication not found");
  core::Medication m;
  if (!medicationFromApi(j, static_cast<uint16_t>(id), m, &err)) return errorResponse(422, err);
  err = admin_.saveMedication(m);
  if (!err.empty()) return errorResponse(422, err);
  return jsonResponse(id ? 200 : 201, medicationToApi(m));
}

HttpResponse PortalApi::deleteMed(int id) {
  const std::string e = admin_.deleteMedication(static_cast<uint16_t>(id));
  if (e == "Medication not found") return errorResponse(404, e);
  if (!e.empty()) return errorResponse(409, e);
  return okJson();
}

HttpResponse PortalApi::archiveMed(const HttpRequest& r, int id) {
  JsonValue j;
  std::string err;
  if (!parseBody(r.body, 256, j, &err)) return errorResponse(400, err);
  if (j["active"].type() != JsonValue::Type::Bool) return errorResponse(422, "active must be true or false");
  const std::string e = admin_.setMedicationActive(static_cast<uint16_t>(id), j["active"].asBool());
  if (e == "Medication not found") return errorResponse(404, e);
  if (!e.empty()) return errorResponse(422, e);
  return okJson();
}

HttpResponse PortalApi::saveSchedule(const HttpRequest& r, int id) {
  JsonValue j;
  std::string err;
  if (!parseBody(r.body, kMaxJsonBody, j, &err)) return errorResponse(400, err);
  core::Database db = admin_.database();
  core::Medication* m = db.med(static_cast<uint16_t>(id));
  if (!m) return errorResponse(404, "medication not found");
  std::vector<core::Phase> phases;
  if (!phasesFromApi(j["phases"], phases, &err)) return errorResponse(422, err);
  core::Medication copy = *m;
  copy.phases = phases;
  err = admin_.saveMedication(copy);
  if (!err.empty()) return errorResponse(422, err);
  return jsonResponse(200, medicationToApi(copy));
}

HttpResponse PortalApi::history(const HttpRequest& r) {
  const int profile = atoi(queryParam(r.query, "profile").c_str());
  int days = atoi(queryParam(r.query, "days").c_str());
  if (days <= 0 || days > 366) days = 14;
  const core::Database db = admin_.database();
  const int64_t since = admin_.nowUtc() - static_cast<int64_t>(days) * 86400;
  JsonValue j = JsonValue::object();
  j.set("entries", historyToApi(admin_.recentLog(), db, admin_.timezone(), profile, since, 500));
  return jsonResponse(200, j);
}

static std::string stamp(const core::LocalTime& lt) {
  const core::Ymd d = core::civilFromDays(lt.days);
  return app::fmt("%04d%02d%02d-%02d%02d", d.y, d.m, d.d, lt.secOfDay / 3600, (lt.secOfDay / 60) % 60);
}

HttpResponse PortalApi::exportData(const HttpRequest& r, bool csv) {
  storage::ExportOptions o;
  o.includeNames = queryParam(r.query, "names") == "1";
  o.profileId = atoi(queryParam(r.query, "profile").c_str());
  const core::Database db = admin_.database();
  const std::vector<core::DoseLogEntry> log = admin_.recentLog();
  HttpResponse resp;
  resp.contentType = csv ? "text/csv; charset=utf-8" : "application/json; charset=utf-8";
  resp.body = csv ? storage::exportCsv(log, db, o) : storage::exportJson(log, db, o);
  resp.headers.push_back({"Content-Disposition", std::string("attachment; filename=\"rxnudge-history-") +
                                                     stamp(admin_.localNow()) + (csv ? ".csv\"" : ".json\"")});
  return resp;
}

HttpResponse PortalApi::backup() {
  HttpResponse resp;
  resp.body = admin_.backupJson();
  resp.headers.push_back({"Content-Disposition", "attachment; filename=\"backup-" + stamp(admin_.localNow()) + ".json\""});
  return resp;
}

HttpResponse PortalApi::restore(const HttpRequest& r) {
  if (r.body.size() > kMaxRestoreBody) return errorResponse(413, "backup file too large");
  const std::string e = admin_.restoreJson(r.body);
  if (!e.empty() && e.rfind("Restored", 0) != 0) return errorResponse(422, e);
  JsonValue j = JsonValue::object();
  j.set("ok", JsonValue::boolean(true));
  j.set("message", JsonValue::string(e.empty() ? "Backup restored" : e));
  return jsonResponse(200, j);
}

HttpResponse PortalApi::notifyGet() {
  const core::Settings s = admin_.database().settings;
  JsonValue j = JsonValue::object();
  j.set("enabled", JsonValue::boolean(s.notifyEnabled));
  j.set("url", JsonValue::string(s.notifyUrl));
  j.set("includeNames", JsonValue::boolean(s.notifyNames));
  return jsonResponse(200, j);
}

HttpResponse PortalApi::notifyPut(const HttpRequest& r) {
  JsonValue j;
  std::string err;
  if (!parseBody(r.body, 1024, j, &err)) return errorResponse(400, err);
  core::Settings s = admin_.database().settings;
  if (j.has("url")) {
    if (!j["url"].isString()) return errorResponse(422, "url must be a string");
    err = app::validateNotifyUrl(j["url"].asString());
    if (!err.empty()) return errorResponse(422, err);
    s.notifyUrl = j["url"].asString();
  }
  for (const char* k : {"enabled", "includeNames"})
    if (j.has(k) && j[k].type() != JsonValue::Type::Bool) return errorResponse(422, std::string(k) + " must be true or false");
  if (j.has("enabled")) s.notifyEnabled = j["enabled"].asBool();
  if (j.has("includeNames")) s.notifyNames = j["includeNames"].asBool();
  if (s.notifyEnabled && s.notifyUrl.empty()) return errorResponse(422, "enter the URL first");
  err = admin_.saveSettings(s);
  if (!err.empty()) return errorResponse(422, err);
  return notifyGet();
}

HttpResponse PortalApi::otaBegin(const HttpRequest& r, size_t size, uint32_t nowMs, bool* ok) {
  *ok = false;
  const Verdict v = sec_.check(r.meta, true, nowMs);
  if (v != Verdict::Ok) return errorResponse(verdictStatus(v), verdictName(v));
  if (!ota_) return errorResponse(503, "updates are not available on this build");
  otaSession_.reset(new OtaSession(*ota_, project_));
  std::string err;
  if (!otaSession_->begin(size, r.sha256, &err)) return errorResponse(422, err);
  *ok = true;
  return okJson();
}

bool PortalApi::otaChunk(const uint8_t* p, size_t n, std::string* err) {
  if (!otaSession_) {
    if (err) *err = "no upload in progress";
    return false;
  }
  return otaSession_->write(p, n, err);
}

HttpResponse PortalApi::otaFinish(uint32_t nowMs) {
  sec_.touch(nowMs);
  if (!otaSession_) return errorResponse(400, "no upload in progress");
  std::string err;
  const bool ok = otaSession_->finish(&err);
  const AppDescriptor d = otaSession_->descriptor();
  otaSession_.reset();
  if (!ok) return errorResponse(422, err);
  reboot_ = true;
  JsonValue j = JsonValue::object();
  j.set("ok", JsonValue::boolean(true));
  j.set("version", JsonValue::string(d.version));
  j.set("message", JsonValue::string("Verified. Restarting into the new firmware; it rolls back if it fails to start."));
  return jsonResponse(200, j);
}

void PortalApi::otaAbort() {
  if (otaSession_) otaSession_->abort();
  otaSession_.reset();
}

}  // namespace net
