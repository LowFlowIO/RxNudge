// Host preview of the web portal: the real PortalApi + AdminService on demo
// data behind a tiny blocking HTTP/1.1 server. For screenshots and manual UI
// work only (single-threaded, no keep-alive). Build: tools/host_portal.sh
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>

#include "../test/common/fake_net.h"
#include "app_core/admin_service.h"
#include "core/demo_data.h"
#include "core/civil.h"
#include "core/tz.h"
#include "net_core/ota_verify.h"
#include "net_core/portal_api.h"
#include "storage/log_store.h"
#include "storage/mem_fs.h"
#include "storage/repository.h"

namespace {

struct NullOta : net::IOtaTarget {
  size_t capacity() const override { return 0x640000; }
  bool begin(size_t, std::string*) override { return true; }
  bool write(const uint8_t*, size_t, std::string*) override { return true; }
  bool commit(std::string*) override { return true; }
  void abort() override {}
};

uint32_t nowMs() {
  using namespace std::chrono;
  return static_cast<uint32_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

std::string lower(std::string s) {
  for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  const int port = argc > 1 ? atoi(argv[1]) : 8080;
  core::PosixTz tz;
  core::parsePosixTz("CET-1CEST,M3.5.0,M10.5.0/3", tz);
  int64_t now = 0;
  now = core::localToUtc(tz, core::daysFromCivil(2026, 10, 18), 9 * 3600 + 30 * 60);
  core::FakeClock clk(now);
  core::Database db = core::makeDemoDatabase(core::utcToLocal(tz, now).days - 20);
  db.settings.setupDone = true;
  storage::MemFs internal, sd;
  storage::FsRepository repo(internal);
  storage::FsDoseLog log(internal);
  app::AppModel model(clk, db, &log);
  model.setRecentLog(core::makeDemoLog(db, model.tz(), now - core::kEarlyTakeWindowSec, 14));
  model.startEngine(now);
  model.process();
  FakeNetService netSvc;
  app::AdminHooks h;
  h.entropy = []() { return 0x5eedull; };
  app::AdminService admin(model, &repo, &internal, &sd, netSvc, nullptr, h, "0.6.0");
  uint64_t seed = 0x1234567;
  net::PortalSecurity sec([&seed]() { return seed = seed * 6364136223846793005ull + 1442695040888963407ull; });
  NullOta ota;
  net::PortalApi api(admin, sec, &ota, "rxnudge");
  sec.setDeviceIp("127.0.0.1");
  sec.start(nowMs());
  printf("code %s\n", sec.code().c_str());
  fflush(stdout);

  const int srv = socket(AF_INET, SOCK_STREAM, 0);
  const int one = 1;
  setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(srv, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0 || listen(srv, 8) != 0) {
    perror("bind");
    return 1;
  }
  for (;;) {
    const int c = accept(srv, nullptr, nullptr);
    if (c < 0) continue;
    std::string in;
    char buf[8192];
    size_t headerEnd = std::string::npos, contentLen = 0;
    for (;;) {
      const ssize_t n = recv(c, buf, sizeof buf, 0);
      if (n <= 0) break;
      in.append(buf, static_cast<size_t>(n));
      if (headerEnd == std::string::npos && (headerEnd = in.find("\r\n\r\n")) != std::string::npos) {
        const std::string hl = lower(in.substr(0, headerEnd));
        const size_t cl = hl.find("content-length:");
        if (cl != std::string::npos) contentLen = static_cast<size_t>(atol(hl.c_str() + cl + 15));
      }
      if (headerEnd != std::string::npos && in.size() >= headerEnd + 4 + contentLen) break;
    }
    if (headerEnd == std::string::npos) {
      close(c);
      continue;
    }
    net::HttpRequest r;
    const std::string head = in.substr(0, headerEnd);
    const size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
    r.meta.method = head.substr(0, sp1);
    const std::string target = head.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string::npos) r.query = target.substr(q + 1);
    r.meta.ip = "127.0.0.1";
    size_t pos = head.find("\r\n");
    while (pos != std::string::npos && pos < head.size()) {
      const size_t next = head.find("\r\n", pos + 2);
      const std::string line = head.substr(pos + 2, next == std::string::npos ? std::string::npos : next - pos - 2);
      const size_t colon = line.find(':');
      if (colon != std::string::npos) {
        const std::string k = lower(line.substr(0, colon));
        std::string v = line.substr(colon + 1);
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        if (k == "host") r.meta.host = v;
        else if (k == "cookie") r.meta.cookie = v;
        else if (k == "x-csrf-token") r.meta.csrf = v;
        else if (k == "origin") r.meta.origin = v;
        else if (k == "x-firmware-sha256") r.sha256 = v;
      }
      pos = next;
    }
    r.body = in.substr(headerEnd + 4, contentLen);
    const net::HttpResponse resp = api.handle(r, nowMs());
    std::string out = "HTTP/1.1 " + std::to_string(resp.status) + " " + net::statusText(resp.status) + "\r\n";
    out += "Content-Type: " + resp.contentType + "\r\nContent-Length: " + std::to_string(resp.body.size()) + "\r\nConnection: close\r\n";
    if (resp.gzip) out += "Content-Encoding: gzip\r\n";
    for (const auto& hd : resp.headers) out += hd.first + ": " + hd.second + "\r\n";
    out += "\r\n" + resp.body;
    size_t sent = 0;
    while (sent < out.size()) {
      const ssize_t n = send(c, out.data() + sent, out.size() - sent, 0);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    printf("%s %s -> %d\n", r.meta.method.c_str(), target.c_str(), resp.status);
    fflush(stdout);
    close(c);
  }
}
