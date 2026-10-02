// 需要 DPDK 内存（rte_hash / rte_malloc）的单元测试：不占用大页和网卡启动 EAL
#ifndef L4LB_TESTS_UNIT_EAL_FIXTURE_H
#define L4LB_TESTS_UNIT_EAL_FIXTURE_H

#include "test.h"
#include <string>
#include <unistd.h>

#include <rte_eal.h>
#include <rte_memory.h>

inline int run_with_eal(int argc, char **argv) {
  (void)argc;
  std::string prefix = "l4ut" + std::to_string(getpid());
  const char *eal[] = {argv[0],     "--no-huge",  "-m",          "256",
                       "--no-pci",  "--no-shconf", "--file-prefix", prefix.c_str(),
                       "-l",        "0",           "--log-level", "lib.eal:error"};
  if (rte_eal_init(sizeof(eal) / sizeof(eal[0]), const_cast<char **>(eal)) < 0) {
    printf("rte_eal_init failed\n");
    return 1;
  }
  int rc = ut::run_all();
  rte_eal_cleanup();
  return rc;
}

#endif
