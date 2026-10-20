#pragma once
// Web portal access control (pure, host-tested):
//  - one-time 6-digit code shown on the device (no default credentials);
//    it is consumed by a successful login and replaced by a fresh one
//  - session tokens (128-bit, HttpOnly SameSite=Strict cookie) with idle and
//    absolute expiry, at most 3 sessions
//  - CSRF: every state-changing request carries X-CSRF-Token == session token
//    and, if the browser sent one, an Origin that matches the Host
//  - per-IP request rate limit and login lock-out; too many failed logins in
//    total rotate the code
//  - LAN-only source addresses and an allow-list of Host headers (DNS rebinding)
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace net {

struct SecurityConfig {
  uint32_t sessionIdleMs = 15UL * 60UL * 1000UL;
  uint32_t sessionMaxMs = 2UL * 3600UL * 1000UL;
  size_t maxSessions = 3;
  int loginFailsPerIp = 5;          // then that address is locked out
  uint32_t loginLockMs = 5UL * 60UL * 1000UL;
  int loginFailsTotal = 15;         // then the code is replaced (shown again on the device)
  int requestsPerWindow = 120;      // per address
  uint32_t windowMs = 60UL * 1000UL;
  size_t maxTrackedIps = 16;
};

enum class Verdict : uint8_t { Ok, NotLan, BadHost, RateLimited, Unauthorized, CsrfFailed };
const char* verdictName(Verdict v);
int verdictStatus(Verdict v);  // HTTP status code

struct RequestMeta {
  std::string method;  // "GET", "POST", ...
  std::string ip;      // remote IPv4 address
  std::string host;    // Host header
  std::string cookie;  // Cookie header
  std::string csrf;    // X-CSRF-Token header
  std::string origin;  // Origin header ("" when absent)
};

struct LoginResult {
  bool ok = false;
  std::string token, csrf;
  std::string error;
  uint32_t retryAfterMs = 0;
};

class PortalSecurity {
 public:
  using Rng = std::function<uint64_t()>;
  static constexpr const char* kCookie = "mt_session";

  explicit PortalSecurity(Rng rng, SecurityConfig c = SecurityConfig());
  void start(uint32_t nowMs);  // new code, no sessions
  void stop();
  bool running() const { return running_; }
  const std::string& code() const { return code_; }
  uint32_t codeGeneration() const { return codeGen_; }  // changes whenever the code changes
  void setDeviceIp(const std::string& ip) { deviceIp_ = ip; }
  const std::string& deviceIp() const { return deviceIp_; }

  LoginResult login(const RequestMeta& m, const std::string& code, uint32_t nowMs);
  void logout(const RequestMeta& m);
  // Network/rate checks for every request; session + CSRF checks when needed.
  Verdict check(const RequestMeta& m, bool needsSession, uint32_t nowMs, std::string* csrfOut = nullptr);
  size_t sessionCount(uint32_t nowMs);
  uint32_t lastActivityMs() const { return lastActivity_; }
  void touch(uint32_t nowMs) { lastActivity_ = nowMs; }

 private:
  struct Session {
    std::string token, csrf;
    uint32_t created, lastUsed;
  };
  struct IpState {
    std::string ip;
    uint32_t windowStart = 0;
    int requests = 0;
    int loginFails = 0;
    uint32_t lockUntil = 0;
    uint32_t lastSeen = 0;
  };
  std::string randomHex(size_t bytes);
  void newCode();
  IpState& ipState(const std::string& ip, uint32_t nowMs);
  void expire(uint32_t nowMs);
  Session* find(const std::string& token, uint32_t nowMs);

  Rng rng_;
  SecurityConfig cfg_;
  bool running_ = false;
  std::string code_;
  uint32_t codeGen_ = 0;
  int totalFails_ = 0;
  std::vector<Session> sessions_;
  std::vector<IpState> ips_;
  std::string deviceIp_;
  uint32_t lastActivity_ = 0;
};

bool isLanAddress(const std::string& ipv4);  // 10/8, 172.16/12, 192.168/16, 169.254/16, 127/8
bool hostAllowed(const std::string& host, const std::string& deviceIp);
std::string cookieValue(const std::string& cookieHeader, const std::string& name);

}  // namespace net
