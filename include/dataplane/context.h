/**
 * @file context.h
 * @brief 数据面全局状态与 DPDK 参数
 *
 * 原 main.cpp 中的全局变量集中到这里，由 port / worker / main 共享。
 * 定义见 src/dataplane/context.cpp。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_CONTEXT_H
#define L4LB_DATAPLANE_CONTEXT_H

#include <array>
#include <atomic>
#include <cstdint>

#include <rte_common.h> // RTE_CACHE_LINE_SIZE
#include <rte_config.h> // RTE_MAX_LCORE

struct rte_mempool;

namespace l4lb {

class LoadBalancer;

// ============================================================================
// DPDK 配置
// ============================================================================
constexpr uint16_t RX_RING_SIZE = 2048;
constexpr uint16_t TX_RING_SIZE = 4096;   // 增大 TX 缓冲区，减少 DR 模式丢包
constexpr unsigned NUM_MBUFS = 65535;     // 增大内存池 (prev: 16383)
constexpr unsigned MBUF_CACHE_SIZE = 512; // 每一个lcore缓存512个mbuf
constexpr uint16_t BURST_SIZE = 64;       // 增大批处理，提升吞吐

// ============================================================================
// 全局状态
// ============================================================================

/// 运行标志：信号处理函数只写这个变量（lock-free atomic，async-signal-safe）
extern std::atomic<bool> g_running;
extern LoadBalancer g_lb; // 负载均衡相关配置：一致性hash
extern uint16_t g_port_id; // 默认使用端口 0
extern struct rte_mempool *g_mbuf_pool;
extern uint64_t g_tx_offloads_enabled;

// 多队列配置（port_init 根据网卡能力写入实际队列数）
extern uint16_t g_num_queues;

/// 网卡收发统计（per-lcore，只由所属 lcore 写，见 common/stats.h）
struct alignas(RTE_CACHE_LINE_SIZE) PortLcoreStats {
  std::atomic<uint64_t> rx{0};
  std::atomic<uint64_t> tx{0};
  std::atomic<uint64_t> dropped{0};
};
extern std::array<PortLcoreStats, RTE_MAX_LCORE> g_port_stats;

/// 汇总所有 lcore 的网卡收发统计
struct PortStatsTotal {
  uint64_t rx = 0;
  uint64_t tx = 0;
  uint64_t dropped = 0;
};
PortStatsTotal port_stats_total();

} // namespace l4lb

#endif // L4LB_DATAPLANE_CONTEXT_H
