// steering 单元测试：软件 Toeplitz 与标准测试向量一致；SNAT 端口选择让回程 owner 正确
#include "dataplane/steering.h"
#include "test.h"

#include <arpa/inet.h>
#include <set>

using namespace l4lb;

static IPv4Addr ip(const char *s) { return inet_addr(s); }

// Microsoft RSS 验证向量（默认 key）：
// https://learn.microsoft.com/windows-hardware/drivers/network/verifying-the-rss-hash-calculation
struct Vec {
  const char *src, *dst;
  uint16_t sport, dport;
  uint32_t h_ip, h_tcp;
};
static const Vec kVecs[] = {
    {"66.9.149.187", "161.142.100.80", 2794, 1766, 0x323e8fc2, 0x51ccc178},
    {"199.92.111.2", "65.69.140.83", 14230, 4739, 0xd718262a, 0xc626b0ea},
    {"24.19.198.95", "12.22.207.184", 12898, 38024, 0xd2d0a5de, 0x5c2b394a},
    {"38.27.205.30", "209.142.163.6", 48228, 2217, 0x82989176, 0xafc7327f},
    {"153.39.163.191", "202.188.127.2", 44251, 1303, 0x5d1809c5, 0x10e828a2},
};

TEST(toeplitz_vectors) {
  uint16_t reta[4] = {0, 1, 2, 3};
  Steering st;
  st.init_hw(4, Steering::kDefaultRssKey, reta, 4, true, false);
  for (const auto &v : kVecs) {
    FiveTuple t(ip(v.src), ip(v.dst), htons(v.sport), htons(v.dport), 6);
    CHECK_EQ(st.rss_hash(t, false), v.h_ip);
    CHECK_EQ(st.rss_hash(t, true), v.h_tcp);
  }
}

TEST(sw_mode_owner) {
  Steering st;
  st.init_sw(4);
  FiveTuple ret(ip("10.0.0.11"), ip("10.0.0.1"), htons(80), htons(10006), 6);
  CHECK_EQ(st.ret_owner(ret), 10006 % 4);
  CHECK_EQ(st.port_step(6), 4);
  // 正向 owner 稳定且在范围内
  FiveTuple fwd(ip("1.2.3.4"), ip("10.0.0.1"), htons(5555), htons(80), 6);
  CHECK(st.fwd_owner(fwd) < 4);
  CHECK_EQ(st.fwd_owner(fwd), st.fwd_owner(fwd));
}

TEST(hw_mode_port_selection) {
  // 模拟 create_fullnat：每个 worker 只选回程 owner 是自己的端口，
  // 检查选出的端口互不冲突、回程 owner 正确，且每个 worker 都有足够多端口
  uint16_t reta[128];
  for (int i = 0; i < 128; ++i)
    reta[i] = i % 4;
  Steering st;
  st.init_hw(4, Steering::kDefaultRssKey, reta, 128, true, false);
  std::set<uint16_t> used;
  for (uint16_t w = 0; w < 4; ++w) {
    int mine = 0;
    for (uint32_t p = 10000; p <= 60000; ++p) {
      FiveTuple ret(ip("10.0.0.11"), ip("10.0.0.1"), htons(80),
                    htons(static_cast<uint16_t>(p)), 6);
      if (st.ret_owner(ret) != w)
        continue;
      CHECK(used.insert(static_cast<uint16_t>(p)).second);
      ++mine;
    }
    CHECK(mine > 50001 / 4 * 0.9);
  }
  CHECK_EQ(used.size(), 50001u);
}

TEST(udp_without_l4_rss) {
  // vmxnet3：UDP 只按 IP 做 RSS，回程 owner 改由端口决定
  uint16_t reta[8] = {0, 1, 0, 1, 0, 1, 0, 1};
  Steering st;
  st.init_hw(2, Steering::kDefaultRssKey, reta, 8, true, false);
  CHECK(st.ret_by_rss(6));
  CHECK(!st.ret_by_rss(17));
  CHECK_EQ(st.port_step(17), 2);
  FiveTuple ret(ip("10.0.0.11"), ip("10.0.0.1"), htons(53), htons(10001), 17);
  CHECK_EQ(st.ret_owner(ret), 1);
}

int main() { return ut::run_all(); }
