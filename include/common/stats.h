/**
 * @file stats.h
 * @brief per-lcore 统计计数器
 *
 * 数据面计数器的写法约定：
 * - 每个 lcore 只写自己的那一份，按 cache line 对齐，避免伪共享
 * - 计数器是 std::atomic，但只用 relaxed 的 load + store（不用 fetch_add），
 *   在 x86 上编译成普通的 mov，没有 lock 前缀
 * - 读取方（统计打印）跨核汇总，relaxed load 保证读到的是完整的 64 位值
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_COMMON_STATS_H
#define L4LB_COMMON_STATS_H

#include <atomic>
#include <cstdint>

#include <rte_common.h> // RTE_CACHE_LINE_SIZE
#include <rte_lcore.h>

namespace l4lb {

/// per-lcore 计数器：只能由所属 lcore 调用
inline void stat_add(std::atomic<uint64_t> &c, uint64_t n = 1) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline uint64_t stat_get(const std::atomic<uint64_t> &c) {
  return c.load(std::memory_order_relaxed);
}

/// 当前线程对应的 per-lcore 下标；非 EAL 线程统一落到 0
inline unsigned stat_lcore() {
  unsigned lcore = rte_lcore_id();
  return lcore < RTE_MAX_LCORE ? lcore : 0;
}

} // namespace l4lb

#endif // L4LB_COMMON_STATS_H
