/**
 * @file stats.h
 * @brief per-lcore 统计计数器
 *
 * 数据面计数器的写法约定：
 * - 每个 worker 只写自己的那一份，按 cache line 对齐，避免伪共享
 * - 计数器是 std::atomic，但只用 relaxed 的 load + store（不用 fetch_add），
 *   在 x86 上编译成普通的 mov，没有 lock 前缀
 * - 读取方（统计打印、控制面）跨核汇总，relaxed load 保证读到完整的 64 位值
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_COMMON_STATS_H
#define L4LB_COMMON_STATS_H

#include <array>
#include <atomic>
#include <cstdint>

#include <rte_common.h> // RTE_CACHE_LINE_SIZE

namespace l4lb {

/// 计数器：只能由所属 lcore 调用
inline void stat_add(std::atomic<uint64_t> &c, uint64_t n = 1) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline uint64_t stat_get(const std::atomic<uint64_t> &c) {
  return c.load(std::memory_order_relaxed);
}

/// worker 计数器 ID；名字表见 stat_name()
enum Stat : uint32_t {
  // 网卡
  ST_RX,
  ST_TX,
  ST_TX_FULL,      ///< TX 队列满丢弃
  // 分类
  ST_ARP,
  ST_ICMP,
  ST_TCP,
  ST_UDP,
  // 转发
  ST_FWD_IN,       ///< Client -> RS
  ST_FWD_OUT,      ///< RS -> Client（FULLNAT）
  ST_ICMP_ERR_FWD, ///< 转发的 ICMP 差错
  // 丢弃原因
  ST_DROP_MALFORMED,
  ST_DROP_FRAGMENT,
  ST_DROP_CKSUM,      ///< 网卡报告校验和错误
  ST_DROP_NOT_LOCAL,  ///< 目的地址不是本机
  ST_DROP_NO_SERVICE, ///< VIP 上没有这个端口/协议
  ST_DROP_NO_SESSION, ///< 非 SYN 且无会话 / 回程无会话
  ST_DROP_NO_RS,      ///< 没有可用 RS
  ST_DROP_RS_DOWN,    ///< 会话的 RS 已下线
  ST_DROP_TABLE_FULL, ///< 会话表满
  ST_DROP_NO_PORT,    ///< SNAT 端口耗尽
  ST_DROP_NO_NEIGH,   ///< 下一跳 MAC 未解析
  ST_DROP_TTL,
  ST_DROP_REDIRECT,   ///< 转交其他 worker 时 ring 满
  ST_DROP_OTHER,
  // 会话
  ST_SESS_NEW,
  ST_SESS_EXPIRED,
  ST_SESS_CLOSED,     ///< RST/RS 下线等主动结束
  // 多核分发
  ST_REDIRECT_OUT,    ///< 本核收到、转交给 owner
  ST_REDIRECT_IN,     ///< 从其他核转交来
  ST_RING_IN,         ///< pipeline：从 receiver 的 ring 取到
  ST_DROP_RX_RING,    ///< pipeline：worker 的 rx_ring 满，receiver 丢弃
  ST_RSS_MISMATCH,    ///< 网卡 RSS hash 与软件计算不一致（自检）
  ST_RSS_NO_HASH,     ///< HW 模式下网卡没有给出 RSS hash（RSS 实际未生效）
  // FULLNAT 选项处理
  ST_TOA_ADDED,
  ST_TOA_NO_ROOM,
  ST_TS_STRIPPED,
  ST_HC_RESP,         ///< 收到健康检查回包
  ST_COUNT
};

const char *stat_name(Stat id);

/// 一个 worker 的全部计数器
struct alignas(RTE_CACHE_LINE_SIZE) WorkerStats {
  std::array<std::atomic<uint64_t>, ST_COUNT> c{};
  /// 忙碌时间：有包处理的那些轮次花掉的 TSC 周期（忙轮询下 CPU 永远 100%，
  /// 用它除以经过的 TSC 得到真实负载）
  std::atomic<uint64_t> busy_tsc{0};

  void add(Stat id, uint64_t n = 1) { stat_add(c[id], n); }
  uint64_t get(Stat id) const { return stat_get(c[id]); }
  void add_busy(uint64_t cycles) { stat_add(busy_tsc, cycles); }
  uint64_t busy() const { return stat_get(busy_tsc); }
};

/// 汇总后的快照
struct StatsTotal {
  std::array<uint64_t, ST_COUNT> c{};
  uint64_t operator[](Stat id) const { return c[id]; }
  uint64_t drops() const;
};

} // namespace l4lb

#endif // L4LB_COMMON_STATS_H
