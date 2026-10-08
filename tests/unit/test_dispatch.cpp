// receiver 分发规则单元测试：目标 worker 与会话 owner（fwd_owner / ret_owner）一致
#include "dataplane/receiver.h"
#include "dataplane/steering.h"
#include "protocol/ethernet.h"
#include "protocol/ip.h"
#include "test.h"

#include <arpa/inet.h>
#include <cstring>
#include <set>
#include <vector>

using namespace l4lb;

static IPv4Addr ip(const char *s) { return inet_addr(s); }

static const IPv4Addr kLip = ip("10.0.0.2");

/// 以太网 + IPv4 + 端口（只构造 receiver 需要看的字段）
static std::vector<uint8_t> frame(uint8_t proto, IPv4Addr src, IPv4Addr dst,
                                  uint16_t sport, uint16_t dport,
                                  uint16_t frag = 0) {
  std::vector<uint8_t> f(sizeof(EthernetHeader) + sizeof(IPv4Header) + 20, 0);
  auto *eth = reinterpret_cast<EthernetHeader *>(f.data());
  eth->set_ether_type(0x0800);
  auto *iph = reinterpret_cast<IPv4Header *>(f.data() + sizeof(EthernetHeader));
  iph->version_ihl = 0x45;
  iph->protocol = proto;
  iph->src_ip = src;
  iph->dst_ip = dst;
  iph->flags_fragment = htons(frag);
  uint8_t *l4 = f.data() + sizeof(EthernetHeader) + sizeof(IPv4Header);
  uint16_t sp = htons(sport), dp = htons(dport);
  memcpy(l4, &sp, 2);
  memcpy(l4 + 2, &dp, 2);
  return f;
}

static uint16_t target(const Dispatcher &d, const std::vector<uint8_t> &f) {
  return d.target(f.data(), f.size());
}

TEST(inbound_uses_fwd_owner) {
  Steering st;
  st.init_sw(2);
  Dispatcher d;
  d.init(&st, ForwardMode::NAT, {kLip});
  std::set<uint16_t> seen;
  for (uint16_t p = 1000; p < 1200; ++p) {
    for (uint8_t proto : {6, 17}) {
      FiveTuple t(ip("1.2.3.4"), ip("10.0.0.1"), htons(p), htons(80), proto);
      uint16_t w = target(d, frame(proto, t.src_ip, t.dst_ip, p, 80));
      CHECK_EQ(w, st.fwd_owner(t));
      seen.insert(w);
    }
  }
  CHECK_EQ(seen.size(), 2u); // 两个 worker 都分到了
}

TEST(fullnat_return_uses_ret_owner) {
  Steering st;
  st.init_sw(2);
  Dispatcher d;
  d.init(&st, ForwardMode::NAT, {kLip});
  for (uint16_t nat = kNatPortMin; nat < kNatPortMin + 100; ++nat) {
    FiveTuple t(ip("10.0.0.11"), kLip, htons(8080), htons(nat), 6);
    CHECK_EQ(target(d, frame(6, t.src_ip, t.dst_ip, 8080, nat)), nat % 2);
    CHECK_EQ(target(d, frame(6, t.src_ip, t.dst_ip, 8080, nat)),
             st.ret_owner(t));
  }
  // DR 模式没有回程：LIP 地址也按入站处理
  Dispatcher dr;
  dr.init(&st, ForwardMode::DR, {kLip});
  FiveTuple t(ip("10.0.0.11"), kLip, htons(8080), htons(10001), 6);
  CHECK_EQ(target(dr, frame(6, t.src_ip, t.dst_ip, 8080, 10001)),
           st.fwd_owner(t));
}

TEST(non_l4_spread_and_in_range) {
  Steering st;
  st.init_sw(2);
  Dispatcher d;
  d.init(&st, ForwardMode::NAT, {kLip});
  std::set<uint16_t> seen;
  for (int i = 0; i < 64; ++i) {
    IPv4Addr src = htonl(0x01020300 + i);
    seen.insert(target(d, frame(1, src, ip("10.0.0.1"), 0, 0)));    // ICMP
    seen.insert(target(d, frame(6, src, ip("10.0.0.1"), 1, 80, 0x2000))); // 分片
  }
  CHECK_EQ(seen.size(), 2u);
  for (uint16_t w : seen)
    CHECK(w < 2);
  // ARP / 太短的帧：worker 0
  std::vector<uint8_t> arp(60, 0);
  reinterpret_cast<EthernetHeader *>(arp.data())->set_ether_type(0x0806);
  CHECK_EQ(target(d, arp), 0);
  std::vector<uint8_t> runt(10, 0);
  CHECK_EQ(target(d, runt), 0);
}

TEST(single_worker) {
  Steering st;
  st.init_sw(1);
  Dispatcher d;
  d.init(&st, ForwardMode::NAT, {kLip});
  CHECK_EQ(target(d, frame(6, ip("1.2.3.4"), ip("10.0.0.1"), 1, 80)), 0);
}

int main() { return ut::run_all(); }
