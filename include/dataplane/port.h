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

/**
 * @brief 队列规划
 *
 * TX：每个 worker 一个，master 线程独占最后一个（= num_workers）。
 * RX：TX 队列数向上取 2 的幂（vmxnet3 要求），不超过网卡上限。af_packet 等
 *     按 fanout 往每个队列送包的设备，所有 RX 队列都必须有人轮询。
 */
struct PortPlan {
  uint16_t num_workers = 0;
  uint16_t rx_queues = 0;
  uint16_t tx_queues = 0;
};

struct PortSetup {
  uint64_t tx_offloads = 0; ///< 启用的 TX offload
  MacAddr mac{};            ///< 网卡 MAC
  uint16_t mtu = 1500;
};

/**
 * @brief 按 worker 数规划队列
 * @return false 网卡 TX 队列不够（已打印原因）
 */
bool port_plan(uint16_t port, uint16_t num_workers, PortPlan &plan);

/**
 * @brief 配置并启动端口，并根据网卡 RSS 能力初始化 steering
 *
 * RSS 只把流量分到 worker 的队列（RETA 第 i 项 -> 队列 i % num_workers）。
 * @param force_sw 不用 RSS，软件分发（pipeline 模式 / 调试）
 * @return 0 成功，负值为 DPDK 错误码
 */
int port_init(uint16_t port, struct rte_mempool *pool, const PortPlan &plan,
              bool force_sw, Steering &steering, PortSetup &out);

} // namespace l4lb

#endif // L4LB_DATAPLANE_PORT_H
