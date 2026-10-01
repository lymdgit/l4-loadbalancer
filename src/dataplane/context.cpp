/**
 * @file context.cpp
 * @brief 数据面全局状态定义
 */

#include "dataplane/context.h"

#include "core/loadbalancer.h"

namespace l4lb {

volatile bool g_running = true;
LoadBalancer g_lb;
uint16_t g_port_id = 0;
struct rte_mempool *g_mbuf_pool = nullptr;
uint64_t g_tx_offloads_enabled = 0;

uint16_t g_num_queues = 1;

std::atomic<uint64_t> g_stats_rx{0};
std::atomic<uint64_t> g_stats_tx{0};
std::atomic<uint64_t> g_stats_dropped{0};

} // namespace l4lb
