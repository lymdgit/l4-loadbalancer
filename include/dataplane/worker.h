/**
 * @file worker.h
 * @brief 数据面 worker 循环（Run-to-Completion，每个 lcore 一个）
 *
 * 实现见 src/dataplane/worker.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_WORKER_H
#define L4LB_DATAPLANE_WORKER_H

namespace l4lb {

/**
 * @brief worker 主循环：收包 -> 处理 -> 批量发包
 *
 * 签名符合 rte_eal_remote_launch 的要求。
 *
 * @param arg 指向本 lcore 使用的 queue_id (uint16_t)
 * @return 0
 */
int worker_loop(void *arg);

} // namespace l4lb

#endif // L4LB_DATAPLANE_WORKER_H
