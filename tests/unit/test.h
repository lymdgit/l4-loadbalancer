// 极简单元测试框架（不依赖外部库）：TEST / CHECK / CHECK_EQ
#ifndef L4LB_TESTS_UNIT_TEST_H
#define L4LB_TESTS_UNIT_TEST_H

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ut {

struct Case {
  const char *name;
  std::function<void()> fn;
};

inline std::vector<Case> &registry() {
  static std::vector<Case> r;
  return r;
}

inline int &failures() {
  static int f = 0;
  return f;
}

struct Register {
  Register(const char *name, std::function<void()> fn) {
    registry().push_back({name, fn});
  }
};

inline int run_all() {
  int failed_cases = 0;
  for (auto &c : registry()) {
    int before = failures();
    c.fn();
    bool ok = failures() == before;
    printf("%s  %s\n", ok ? "PASS" : "FAIL", c.name);
    failed_cases += !ok;
  }
  printf("%zu cases, %d failed\n", registry().size(), failed_cases);
  return failed_cases ? 1 : 0;
}

} // namespace ut

#define TEST(name)                                                             \
  static void test_##name();                                                   \
  static ut::Register reg_##name(#name, test_##name);                          \
  static void test_##name()

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      printf("    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);      \
      ++ut::failures();                                                        \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    auto va_ = (a);                                                            \
    auto vb_ = (b);                                                            \
    if (!(va_ == vb_)) {                                                       \
      printf("    %s:%d: CHECK_EQ(%s, %s) failed: %s vs %s\n", __FILE__,       \
             __LINE__, #a, #b, std::to_string(va_).c_str(),                    \
             std::to_string(vb_).c_str());                                     \
      ++ut::failures();                                                        \
    }                                                                          \
  } while (0)

#endif
