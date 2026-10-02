// 调度器单元测试：WRR 比例与平滑性、Maglev 均衡性与最小扰动、无可用后端
#include "lb/scheduler.h"
#include "test.h"

#include <arpa/inet.h>
#include <cmath>
#include <map>

using namespace l4lb;

static std::vector<SchedBackend> backends(std::vector<uint32_t> weights) {
  std::vector<SchedBackend> b;
  for (size_t i = 0; i < weights.size(); ++i)
    b.push_back({htonl(0x0a000100 + i), static_cast<uint16_t>(80), weights[i]});
  return b;
}

static FiveTuple tup(uint32_t i) {
  return FiveTuple(htonl(0x0b000000 + i), htonl(0x0a000001),
                   htons(static_cast<uint16_t>(1024 + i)), htons(80), 6);
}

TEST(wrr_ratio) {
  Scheduler s;
  s.build(SchedulerType::WRR, backends({1, 2, 3}));
  std::map<int, int> n;
  for (int i = 0; i < 6000; ++i)
    ++n[s.pick(tup(i), 0)];
  CHECK_EQ(n[0], 1000);
  CHECK_EQ(n[1], 2000);
  CHECK_EQ(n[2], 3000);
}

TEST(wrr_smooth) {
  // 平滑 WRR（nginx 算法）：权重 5:1:1 一轮是 a a b a c a a，
  // 而不是普通加权轮询的 a a a a a b c
  Scheduler s;
  s.build(SchedulerType::WRR, backends({5, 1, 1}));
  std::vector<int> seq;
  for (int i = 0; i < 7; ++i)
    seq.push_back(s.pick(tup(i), 0));
  CHECK(seq == (std::vector<int>{0, 0, 1, 0, 2, 0, 0}));
}

TEST(wrr_zero_weight_and_empty) {
  Scheduler s;
  s.build(SchedulerType::WRR, backends({0, 4}));
  for (int i = 0; i < 100; ++i)
    CHECK_EQ(s.pick(tup(i), 0), 1);
  s.build(SchedulerType::WRR, backends({0, 0}));
  CHECK_EQ(s.pick(tup(1), 0), -1);
  s.build(SchedulerType::MAGLEV, backends({0, 0}));
  CHECK_EQ(s.pick(tup(1), 0), -1);
}

TEST(maglev_balance) {
  Scheduler s;
  s.build(SchedulerType::MAGLEV, backends({1, 1, 1, 1, 1}));
  auto d = s.distribution(5);
  for (auto c : d) {
    double share = static_cast<double>(c) / Scheduler::kMaglevSize;
    CHECK(std::fabs(share - 0.2) < 0.01);
  }
}

TEST(maglev_weighted) {
  Scheduler s;
  s.build(SchedulerType::MAGLEV, backends({1, 3}));
  auto d = s.distribution(2);
  double share = static_cast<double>(d[1]) / Scheduler::kMaglevSize;
  CHECK(std::fabs(share - 0.75) < 0.02);
}

TEST(maglev_minimal_disruption) {
  // 去掉一个后端：原本落在其他后端上的连接，绝大多数仍落在原后端
  Scheduler a, b;
  a.build(SchedulerType::MAGLEV, backends({1, 1, 1, 1, 1}));
  b.build(SchedulerType::MAGLEV, backends({1, 1, 0, 1, 1}));
  int kept = 0, total = 0;
  for (int i = 0; i < 20000; ++i) {
    int pa = a.pick(tup(i), 0);
    if (pa == 2)
      continue;
    ++total;
    kept += b.pick(tup(i), 0) == pa;
  }
  CHECK(kept > total * 0.95);
}

TEST(maglev_deterministic) {
  // 同样的输入构建出同样的表（多台 LB 无需同步即可一致调度）
  Scheduler a, b;
  a.build(SchedulerType::MAGLEV, backends({2, 1, 1}));
  b.build(SchedulerType::MAGLEV, backends({2, 1, 1}));
  for (int i = 0; i < 1000; ++i)
    CHECK_EQ(a.pick(tup(i), 0), b.pick(tup(i), 3));
}

int main() { return ut::run_all(); }
