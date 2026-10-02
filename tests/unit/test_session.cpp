// 会话表单元测试：增删查、两个 key、超时、时间轮重挂、超时变短、批量上限
#include "eal_fixture.h"
#include "lb/session.h"
#include "test.h"

#include <arpa/inet.h>
#include <vector>

using namespace l4lb;

static FiveTuple cli(uint32_t i) {
  return FiveTuple(htonl(0x0a000000 + i), htonl(0x0a000001), htons(1000 + i % 60000),
                   htons(80), 6);
}
static FiveTuple srv(uint32_t i) {
  return FiveTuple(htonl(0x0a00000b), htonl(0x0a000001), htons(80),
                   htons(10000 + i % 50000), 6);
}

TEST(lookup_both_keys) {
  SessionTable t;
  CHECK(t.init("ut_s1", 1024, SOCKET_ID_ANY, 1));
  FiveTuple c = cli(1), s = srv(1);
  Session *x = t.create(c, &s, 1, 10);
  CHECK(x);
  CHECK(t.lookup(c) == x);
  CHECK(t.lookup(s) == x);
  CHECK_EQ(t.active(), 1u);
  t.remove(x);
  CHECK(!t.lookup(c) && !t.lookup(s));
  CHECK_EQ(t.active(), 0u);
}

TEST(full_table) {
  SessionTable t;
  CHECK(t.init("ut_s2", 1024, SOCKET_ID_ANY, 1));
  for (uint32_t i = 0; i < 1024; ++i)
    CHECK(t.create(cli(i), nullptr, 1, 10));
  CHECK(!t.create(cli(5000), nullptr, 1, 10));
}

TEST(expire_basic) {
  SessionTable t;
  CHECK(t.init("ut_s3", 1024, SOCKET_ID_ANY, 1));
  // 存活至少 timeout 秒：tick 1 创建、超时 5 秒，tick 7 删除
  t.create(cli(1), nullptr, 1, 5);
  t.create(cli(2), nullptr, 1, 10);
  CHECK_EQ(t.expire(6), 0u);
  CHECK_EQ(t.expire(7), 1u);
  CHECK(!t.lookup(cli(1)) && t.lookup(cli(2)));
  CHECK_EQ(t.expire(11), 0u);
  CHECK_EQ(t.expire(12), 1u);
  CHECK_EQ(t.active(), 0u);
}

TEST(sessions_created_after_idle_period) {
  // 回归：一段时间没有会话后新建的会话不能被立即删除
  SessionTable t;
  CHECK(t.init("ut_s4", 4096, SOCKET_ID_ANY, 1));
  for (uint32_t i = 0; i < 100; ++i)
    t.create(cli(i), nullptr, 1, 1);
  CHECK_EQ(t.expire(3), 100u);
  for (uint64_t now = 3; now < 10; ++now)
    t.expire(now);
  for (uint32_t i = 100; i < 200; ++i)
    CHECK(t.create(cli(i), nullptr, 10, 1)); // expire at 11
  CHECK_EQ(t.expire(10), 0u);
  CHECK_EQ(t.active(), 100u);
}

TEST(touch_extends_and_shortens) {
  SessionTable t;
  CHECK(t.init("ut_s5", 1024, SOCKET_ID_ANY, 1));
  Session *s = t.create(cli(1), nullptr, 1, 5); // 7
  t.touch(s, 4, 100);                          // 105：变长，懒更新
  CHECK_EQ(t.expire(50), 0u);
  CHECK(t.lookup(cli(1)));
  t.touch(s, 50, 2); // 53：变短，立即移动
  CHECK_EQ(t.expire(52), 0u);
  CHECK_EQ(t.expire(53), 1u);
}

TEST(timeout_longer_than_wheel) {
  SessionTable t;
  CHECK(t.init("ut_s6", 1024, SOCKET_ID_ANY, 1));
  t.create(cli(1), nullptr, 1, SessionTable::kWheelSlots * 2 + 3);
  uint64_t exp = 2 + SessionTable::kWheelSlots * 2 + 3;
  size_t n = 0;
  for (uint64_t now = 1; now < exp; now += 7)
    n += t.expire(now);
  CHECK_EQ(n, 0u);
  CHECK_EQ(t.expire(exp), 1u);
}

TEST(expire_budget) {
  SessionTable t;
  const uint32_t N = SessionTable::kExpireBudget * 3;
  CHECK(t.init("ut_s7", N, SOCKET_ID_ANY, 1));
  for (uint32_t i = 0; i < N; ++i)
    CHECK(t.create(cli(i), nullptr, 1, 1));
  size_t first = t.expire(3);
  CHECK(first <= SessionTable::kExpireBudget);
  size_t total = first;
  for (int i = 0; i < 10; ++i)
    total += t.expire(3);
  CHECK_EQ(total, N);
}

TEST(remove_while_pending) {
  SessionTable t;
  const uint32_t N = SessionTable::kExpireBudget * 2;
  CHECK(t.init("ut_s8", N, SOCKET_ID_ANY, 1));
  std::vector<Session *> v;
  for (uint32_t i = 0; i < N; ++i)
    v.push_back(t.create(cli(i), nullptr, 1, 1));
  t.expire(3); // 处理一半，另一半留在 pending
  for (auto *s : v)
    if (t.lookup(s->client) == s)
      t.remove(s);
  CHECK_EQ(t.active(), 0u);
  CHECK_EQ(t.expire(4), 0u);
  CHECK(t.create(cli(1), nullptr, 4, 1));
}

int main(int argc, char **argv) { return run_with_eal(argc, argv); }
