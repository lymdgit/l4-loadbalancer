// 配置解析单元测试：新格式、旧格式兼容、错误输入
#include "common/config.h"
#include "common/logger.h"
#include "test.h"

#include <arpa/inet.h>
#include <cstdio>
#include <fstream>

using namespace l4lb;

static bool load(const std::string &text) {
  const char *path = "/tmp/l4lb_ut.conf";
  std::ofstream(path) << text;
  bool ok = Config::instance().load(path);
  remove(path);
  return ok;
}

TEST(new_format) {
  CHECK(load(R"(
[global]
mode = nat
max_sessions = 100000
toa = on
tcp_established_timeout = 600
[network]
netmask = 255.255.255.0
gateway = 10.0.0.254
local_ips = 10.0.0.2, 10.0.0.3
[healthcheck]
enabled = yes
interval = 2
timeout = 500
[service.web]
vip = 10.0.0.1
port = 80
scheduler = maglev
server1 = 10.0.0.11:8080:10
server2 = 10.0.0.12:8080:20:02:00:00:00:00:12
[service.dns]
vip = 10.0.0.1
port = 53
proto = udp
server1 = 10.0.0.13:53
)"));
  const auto &c = Config::instance().lb();
  CHECK(c.mode == ForwardMode::NAT);
  CHECK(c.toa);
  CHECK_EQ(c.max_sessions, 100000u);
  CHECK_EQ(c.timeouts.tcp_established, 600u);
  CHECK_EQ(c.local_ips.size(), size_t(2));
  CHECK_EQ(c.hc_src, inet_addr("10.0.0.2"));
  CHECK_EQ(c.health.interval_ms, 2000u);
  CHECK_EQ(c.services.size(), size_t(2));
  const auto &dns = c.services[0].name == "dns" ? c.services[0] : c.services[1];
  const auto &web = c.services[0].name == "web" ? c.services[0] : c.services[1];
  CHECK_EQ(int(dns.proto), 17);
  CHECK(web.sched == SchedulerType::MAGLEV);
  CHECK_EQ(web.rs.size(), size_t(2));
  CHECK_EQ(web.rs[1].port, 8080);
  CHECK_EQ(web.rs[1].weight, 20u);
  CHECK_EQ(int(web.rs[1].mac[5]), 0x12);
}

TEST(legacy_format) {
  CHECK(load(R"(
[global]
mode = dr
session_timeout = 30
[vip]
ip = 192.168.154.130
ports = 80,8080
mac = 00:0c:29:3e:38:9c
[realserver]
count = 2
server1 = 192.168.154.133:80:50:00:0c:29:bd:b3:a4
server2 = 192.168.154.132:80:50
[network]
hc_src = 192.168.154.140
)"));
  const auto &c = Config::instance().lb();
  CHECK(c.mode == ForwardMode::DR);
  CHECK_EQ(c.services.size(), size_t(4)); // 2 端口 x TCP/UDP
  CHECK_EQ(c.timeouts.tcp_established, 30u);
  CHECK_EQ(c.timeouts.tcp_syn, 10u);
  CHECK_EQ(int(c.vip_mac[0]), 0x00);
  CHECK_EQ(int(c.vip_mac[5]), 0x9c);
}

TEST(rejects_bad_input) {
  Logger::instance().set_level(LogLevel::OFF);
  CHECK(!load("[global]\nmode = foo\n[vip]\nip=1.1.1.1\n"));
  CHECK(!load("[vip]\nip = 999.1.1.1\n[realserver]\ncount=1\nserver1=1.1.1.1:80\n"));
  CHECK(!load("[vip]\nip = 1.1.1.1\n[realserver]\ncount=1\nserver1=1.1.1.1:70000\n"));
  CHECK(!load("[vip]\nip = 1.1.1.1\nports = 80\n[realserver]\ncount=1\nserver1=1.1.1.1:abc\n"));
  CHECK(!load("[global]\nmax_sessions = lots\n[vip]\nip=1.1.1.1\n"));
  CHECK(!load("[service.a]\nvip = 1.1.1.1\nport = 80\n")); // 没有 RS
  CHECK(!load("[service.a]\nvip = 1.1.1.1\nport = 80\nserver1 = 2.2.2.2:80\n"
              "[service.b]\nvip = 1.1.1.1\nport = 80\nserver1 = 2.2.2.3:80\n"));
  // FULLNAT 下 VIP 兼做 LIP，服务端口不能落在 SNAT 端口段
  CHECK(!load("[service.a]\nvip = 1.1.1.1\nport = 20000\nserver1 = 2.2.2.2:80\n"));
  Logger::instance().set_level(LogLevel::INFO);
}

int main() { return ut::run_all(); }
