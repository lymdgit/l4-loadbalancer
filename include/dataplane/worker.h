/**
 * @file worker.h
 * @brief 数据面 worker 循环（Run-to-Completion，每个 lcore 一个）
 *
 * 每轮循环：收包 -> 处理 -> 处理其他 worker 转交来的包 -> 批量发包
 *           -> 报告 RCU 静默期；每 1024 轮更新时钟、推进会话时间轮。
 * master（worker 0）额外负责：邻居表（ARP 请求/学习/老化/免费 ARP）、
 * 健康检查探测、周期打印统计。这些报文都从 master 自己的 TX 队列发出。
 *
 * 实现见 src/dataplane/worker.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_WORKER_H
#define L4LB_DATAPLANE_WORKER_H

#include "common/types.h"
#include <string>

struct rte_mbuf;

namespace l4lb {

struct WorkerCtx;

/// worker 主循环，签名符合 rte_eal_remote_launch；arg 为 WorkerCtx*
int worker_loop(void *arg);

/// 加入本 worker 的 TX 缓冲（满时立即发送）
void tx_buffer_add(WorkerCtx &w, struct rte_mbuf *m);

/// master 发 ARP 请求解析 ip（带频率限制）
void master_request_neigh(WorkerCtx &master, IPv4Addr ip);

/// 统计信息的文本形式（周期日志和控制命令 stats 共用）
std::string format_stats(bool verbose);

} // namespace l4lb

#endif // L4LB_DATAPLANE_WORKER_H
