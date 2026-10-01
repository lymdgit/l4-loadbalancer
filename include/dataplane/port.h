/**
 * @file port.h
 * @brief DPDK 网卡端口初始化（RSS 多队列、校验和 offload）
 *
 * 实现见 src/dataplane/port.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_PORT_H
#define L4LB_DATAPLANE_PORT_H

#include <cstdint>

struct rte_mempool;

namespace l4lb {

/**
 * @brief 配置并启动一个 DPDK 端口
 *
 * 队列数会被限制在网卡支持的范围内，实际值写入 g_num_queues；
 * 启用的 TX offload 写入 g_tx_offloads_enabled。
 *
 * @param port 端口号
 * @param mbuf_pool RX 队列使用的 mbuf 内存池
 * @param num_queues 期望的 RX/TX 队列数
 * @return 0 成功，负值为 DPDK 错误码
 */
int port_init(uint16_t port, struct rte_mempool *mbuf_pool,
              uint16_t num_queues);

} // namespace l4lb

#endif // L4LB_DATAPLANE_PORT_H
