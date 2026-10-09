/**
 * @file worker.h
 * @brief 数据面 worker 循环（每个 worker lcore 一个，所有 worker 代码相同）
 *
 * 每轮循环：取包（rtc：网卡 RX 队列；pipeline：receiver 的 rx_ring）-> 处理
 *           -> 处理其他 worker 转交来的包 -> 批量发包 -> 报告 RCU 静默期；
 *           每 1024 轮更新时钟、推进会话时间轮。
 * 邻居表、健康检查、周期统计在 master 线程（master.h），不在 worker 上。
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

/// 加入 TX 缓冲（满时立即发送），从 w.idx 号 TX 队列发出；worker 和 master 共用
void tx_buffer_add(WorkerCtx &w, struct rte_mbuf *m);

/// 发送 TX 缓冲中的包
void tx_flush(WorkerCtx &w);

/// 统计信息的文本形式（周期日志和控制命令 stats 共用）
std::string format_stats(bool verbose);

/**
 * @brief 周期性能报告：与上一次调用之间的速率和各线程忙碌率
 *
 * 由 master 线程每 stats_interval 秒调用一次并写日志；控制命令 rate 返回最近
 * 一次的结果。第一次调用只记录基准。
 */
std::string perf_report_tick(uint64_t now_tsc);

/// 最近一次 perf_report_tick 的结果（任意线程）
std::string perf_report_last();

/// 刷新网卡统计（g_dp.nic）；master 线程每秒一次，统计命令执行时也会调用
void nic_stats_update();

/**
 * @brief 全部原始计数，一行一个 "key value"（控制命令 counters）
 *
 * key 名字固定、值单调递增，带 time_ns 时间戳；压测前后各取一次相减即可
 * 得到 PPS / bps（scripts/l4lbctl.py delta）。见 docs/pps方案.md。
 */
std::string format_counters();

/// 按服务 / RS 的连接、包、字节计数（控制命令 stats -r，类似 ipvsadm -ln --stats）
std::string format_rs_stats();

} // namespace l4lb

#endif // L4LB_DATAPLANE_WORKER_H
