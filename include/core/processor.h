/**
 * @file processor.h
 * @brief 报文处理主流程（每个 worker 调用，无全局锁）
 *
 *   收包 -> 校验 -> 分类 -> [不是本核的会话 -> 转交 owner]
 *        -> 查/建会话 -> TCP 状态机 -> 改写 -> 发包
 *
 * 分类规则（dst = 外层目的地址）：
 *   ARP                                  -> 应答本机地址的请求 / 学习邻居
 *   ICMP 到本机地址：echo                -> 应答
 *                    差错（type 3/11/12）-> 按内层报文找会话，转换后转发
 *   TCP/UDP 到 LIP，端口在 SNAT 段       -> FULLNAT 回程
 *   TCP/UDP 到 hc_src，端口在健康检查段  -> 健康检查回包（交给 master）
 *   TCP/UDP 到 VIP:服务端口              -> 入站
 *   其他                                 -> 丢弃
 *
 * 实现见 src/core/processor.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CORE_PROCESSOR_H
#define L4LB_CORE_PROCESSOR_H

#include "common/stats.h"
#include <cstdint>

struct rte_mbuf;

namespace l4lb {

struct WorkerCtx;
struct Snapshot;

enum class Verdict : uint8_t {
  SEND,     ///< 报文已改写，由本 worker 发送
  DROP,     ///< 丢弃（reason 为丢弃原因）
  REDIRECT, ///< 交给 target 号 worker 处理
  CONSUMED, ///< 报文已被处理掉（如健康检查回包），调用方只需释放
};

struct Result {
  Verdict verdict;
  Stat reason;     ///< DROP 时的计数器
  uint16_t target; ///< REDIRECT 时的目标 worker
};

/**
 * @brief 处理一个报文
 * @param redirected 是否是其他 worker 转交来的（转交来的不会再次转交）
 */
Result process_packet(WorkerCtx &w, const Snapshot &snap, struct rte_mbuf *m,
                      bool redirected);

} // namespace l4lb

#endif // L4LB_CORE_PROCESSOR_H
