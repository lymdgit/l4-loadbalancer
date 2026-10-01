/**
 * @file context.cpp
 * @brief 数据面全局状态定义
 */

#include "dataplane/context.h"

#include "common/stats.h"
#include "core/loadbalancer.h"

namespace l4lb {

std::atomic<bool> g_running{true};
LoadBalancer g_lb;
uint16_t g_port_id = 0;
struct rte_mempool *g_mbuf_pool = nullptr;
uint64_t g_tx_offloads_enabled = 0;

uint16_t g_num_queues = 1;

std::array<PortLcoreStats, RTE_MAX_LCORE> g_port_stats;

PortStatsTotal port_stats_total() {
  PortStatsTotal t;
  for (const auto &s : g_port_stats) {
    t.rx += stat_get(s.rx);
    t.tx += stat_get(s.tx);
    t.dropped += stat_get(s.dropped);
  }
  return t;
}

} // namespace l4lb
