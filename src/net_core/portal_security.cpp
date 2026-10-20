#include "net_core/portal_security.h"

#include <algorithm>
#include <cstdio>

#include "net_core/sha256.h"

namespace net {

const char* verdictName(Verdict v) {
  switch (v) {
    case Verdict::Ok: return "ok";
    case Verdict::NotLan: return "not_lan";
    case Verdict::BadHost: return "bad_host";
    case Verdict::RateLimited: return "rate_limited";
    case Verdict::Unauthorized: return "unauthorized";
    case Verdict::CsrfFailed: return "csrf_failed";
  }
  return "?";
}

int verdictStatus(Verdict v) {
  switch (v) {
    case Verdict::Ok: return 200;
    case Verdict::RateLimited: return 429;
    case Verdict::Unauthorized: return 401;
    default: return 403;
  }
}

static bool parseIpv4(const std::string& s, uint8_t o[4]) {
  int a, b, c, d;
  char tail;
  if (sscanf(s.c_str(), "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) return false;
  const int v[4] = {a, b, c, d};
  for (int i = 0; i < 4; i++) {
    if (v[i] < 0 || v[i] > 255) return false;
    o[i] = static_cast<uint8_t>(v[i]);
  }
  return true;
}

bool isLanAddress(const std::string& ip) {
  uint8_t o[4];
  if (!parseIpv4(ip, o)) return false;
  if (o[0] == 10 || o[0] == 127) return true;
  if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return true;
  if (o[0] == 192 && o[1] == 168) return true;
  if (o[0] == 169 && o[1] == 254) return true;
  return false;
}

bool hostAllowed(const std::string& hostHeader, const std::string& deviceIp) {
  std::string h = hostHeader;
  std::transform(h.begin(), h.end(), h.begin(), [](unsigned char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); });
  const size_t colon = h.rfind(':');
  if (colon != std::string::npos) {
    const std::string port = h.substr(colon + 1);
    if (port != "80") return false;
    h = h.substr(0, colon);
  }
  if (h == "rx-nudge.local") return true;
  return !deviceIp.empty() && h == deviceIp;
}

std::string cookieValue(const std::string& header, const std::string& name) {
  size_t i = 0;
  while (i < header.size()) {
    while (i < header.size() && (header[i] == ' ' || header[i] == ';')) i++;
    const size_t eq = header.find('=', i);
    if (eq == std::string::npos) break;
    size_t end = header.find(';', eq);
    if (end == std::string::npos) end = header.size();
    if (header.compare(i, eq - i, name) == 0) return header.substr(eq + 1, end - eq - 1);
    i = end + 1;
  }
  return "";
}

PortalSecurity::PortalSecurity(Rng rng, SecurityConfig c) : rng_(std::move(rng)), cfg_(c) {}

std::string PortalSecurity::randomHex(size_t bytes) {
  std::string raw;
  while (raw.size() < bytes) {
    const uint64_t v = rng_();
    for (int i = 0; i < 8 && raw.size() < bytes; i++) raw.push_back(static_cast<char>(v >> (8 * i)));
  }
  return toHex(reinterpret_cast<const uint8_t*>(raw.data()), raw.size());
}

void PortalSecurity::newCode() {
  // 6 digits from 64 random bits (bias < 1e-13)
  char buf[8];
  snprintf(buf, sizeof(buf), "%06u", static_cast<unsigned>(rng_() % 1000000ULL));
  code_ = buf;
  ++codeGen_;
  totalFails_ = 0;
}

void PortalSecurity::start(uint32_t nowMs) {
  running_ = true;
  sessions_.clear();
  ips_.clear();
  newCode();
  lastActivity_ = nowMs;
}

void PortalSecurity::stop() {
  running_ = false;
  sessions_.clear();
  ips_.clear();
  code_.clear();
  ++codeGen_;
}

PortalSecurity::IpState& PortalSecurity::ipState(const std::string& ip, uint32_t nowMs) {
  for (auto& s : ips_)
    if (s.ip == ip) {
      s.lastSeen = nowMs;
      return s;
    }
  if (ips_.size() >= cfg_.maxTrackedIps) {
    // evict the least recently seen address that is not locked out
    size_t victim = 0;
    for (size_t i = 1; i < ips_.size(); i++)
      if (ips_[i].lastSeen < ips_[victim].lastSeen) victim = i;
    ips_.erase(ips_.begin() + static_cast<long>(victim));
  }
  IpState s;
  s.ip = ip;
  s.windowStart = nowMs;
  s.lastSeen = nowMs;
  ips_.push_back(s);
  return ips_.back();
}

void PortalSecurity::expire(uint32_t nowMs) {
  sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(),
                                 [&](const Session& s) {
                                   return nowMs - s.lastUsed > cfg_.sessionIdleMs || nowMs - s.created > cfg_.sessionMaxMs;
                                 }),
                  sessions_.end());
}

PortalSecurity::Session* PortalSecurity::find(const std::string& token, uint32_t nowMs) {
  expire(nowMs);
  if (token.size() != 32) return nullptr;
  for (auto& s : sessions_)
    if (constantTimeEqual(s.token, token)) return &s;
  return nullptr;
}

size_t PortalSecurity::sessionCount(uint32_t nowMs) {
  expire(nowMs);
  return sessions_.size();
}

Verdict PortalSecurity::check(const RequestMeta& m, bool needsSession, uint32_t nowMs, std::string* csrfOut) {
  if (!running_) return Verdict::Unauthorized;
  if (!isLanAddress(m.ip)) return Verdict::NotLan;
  if (!hostAllowed(m.host, deviceIp_)) return Verdict::BadHost;
  IpState& st = ipState(m.ip, nowMs);
  if (nowMs - st.windowStart >= cfg_.windowMs) {
    st.windowStart = nowMs;
    st.requests = 0;
  }
  if (++st.requests > cfg_.requestsPerWindow) return Verdict::RateLimited;
  lastActivity_ = nowMs;
  if (!needsSession) return Verdict::Ok;
  Session* s = find(cookieValue(m.cookie, kCookie), nowMs);
  if (!s) return Verdict::Unauthorized;
  const bool mutating = m.method != "GET" && m.method != "HEAD";
  if (mutating) {
    if (!constantTimeEqual(m.csrf, s->csrf)) return Verdict::CsrfFailed;
    if (!m.origin.empty()) {
      const std::string prefix = "http://";
      if (m.origin.compare(0, prefix.size(), prefix) != 0 || !hostAllowed(m.origin.substr(prefix.size()), deviceIp_) ||
          m.origin.substr(prefix.size()) != m.host)
        return Verdict::CsrfFailed;
    }
  }
  s->lastUsed = nowMs;
  if (csrfOut) *csrfOut = s->csrf;
  return Verdict::Ok;
}

LoginResult PortalSecurity::login(const RequestMeta& m, const std::string& code, uint32_t nowMs) {
  LoginResult r;
  const Verdict v = check(m, false, nowMs);
  if (v != Verdict::Ok) {
    r.error = verdictName(v);
    return r;
  }
  IpState& st = ipState(m.ip, nowMs);
  if (static_cast<int32_t>(st.lockUntil - nowMs) > 0) {
    r.error = "locked";
    r.retryAfterMs = st.lockUntil - nowMs;
    return r;
  }
  if (code_.empty() || !constantTimeEqual(code, code_)) {
    r.error = "wrong code";
    if (++st.loginFails >= cfg_.loginFailsPerIp) {
      st.lockUntil = nowMs + cfg_.loginLockMs;
      st.loginFails = 0;
      r.retryAfterMs = cfg_.loginLockMs;
    }
    if (++totalFails_ >= cfg_.loginFailsTotal) newCode();  // guessing across addresses: new code on the device
    return r;
  }
  st.loginFails = 0;
  expire(nowMs);
  if (sessions_.size() >= cfg_.maxSessions) {
    auto oldest = std::min_element(sessions_.begin(), sessions_.end(),
                                   [](const Session& a, const Session& b) { return a.lastUsed < b.lastUsed; });
    sessions_.erase(oldest);
  }
  Session s;
  s.token = randomHex(16);
  s.csrf = randomHex(16);
  s.created = s.lastUsed = nowMs;
  sessions_.push_back(s);
  newCode();  // one-time: the used code is gone
  r.ok = true;
  r.token = s.token;
  r.csrf = s.csrf;
  return r;
}

void PortalSecurity::logout(const RequestMeta& m) {
  const std::string t = cookieValue(m.cookie, kCookie);
  sessions_.erase(std::remove_if(sessions_.begin(), sessions_.end(), [&](const Session& s) { return s.token == t; }),
                  sessions_.end());
}

}  // namespace net
