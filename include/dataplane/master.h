/**
 * @file master.h
 * @brief master 线程（普通线程，不占 lcore）：邻居表、健康检查、周期统计
 *
 * 原来由 worker 0 兼任，现在单独成线程，所有 worker 运行完全相同的代码
 * （docs/pipeline改造.md）。master 独占最后一个 TX 队列发 ARP / 健康检查报文；
 * 它需要的输入（ARP 学习、回包）由 worker 通过 master_ring 上报。
 *
 * 实现见 src/dataplane/master.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_MASTER_H
#define L4LB_DATAPLANE_MASTER_H

#include "common/types.h"

namespace l4lb {

struct WorkerCtx;

/// 启动 master 线程；master->idx 为它独占的 TX 队列
bool master_start(WorkerCtx *master);

/// 等待 master 线程退出（g_running 置 false 之后调用）
void master_join();

/// master 发 ARP 请求解析 ip（带频率限制）；只能在 master 线程调用
void master_request_neigh(WorkerCtx &master, IPv4Addr ip);

} // namespace l4lb

#endif // L4LB_DATAPLANE_MASTER_H
