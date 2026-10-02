/**
 * @file port.h
 * @brief DPDK 网卡端口初始化（RSS 多队列、RETA、校验和 offload、链路等待）
 *
 * 实现见 src/dataplane/port.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_PORT_H
#define L4LB_DATAPLANE_PORT_H

#include "common/types.h"
#include <cstdint>

struct rte_mempool;

namespace l4lb {

class Steering;

struct PortSetup {
  uint16_t num_queues = 0;  ///< 实际 RX/TX 队列数
  uint64_t tx_offloads = 0; ///< 启用的 TX offload
  MacAddr mac{};            ///< 网卡 MAC
  uint16_t mtu = 1500;
};

/**
 * @brief 配置并启动端口，并根据网卡 RSS 能力初始化 steering
 *
 * @param want_queues 期望的队列数（= lcore 数）
 * @param force_sw    强制使用软件分发（调试用）
 * @return 0 成功，负值为 DPDK 错误码
 */
int port_init(uint16_t port, struct rte_mempool *pool, uint16_t want_queues,
              bool force_sw, Steering &steering, PortSetup &out);

} // namespace l4lb

#endif // L4LB_DATAPLANE_PORT_H
