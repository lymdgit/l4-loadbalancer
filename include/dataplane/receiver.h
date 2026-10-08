/**
 * @file receiver.h
 * @brief pipeline 模式的收包分发（docs/pipeline改造.md）
 *
 * receiver 只做轻量解析（以太网 / IPv4 / 端口），按会话 owner 把包放进
 * 对应 worker 的 rx_ring，不查会话、不改包、不发包：
 *
 *   FULLNAT 回程（dst 是 LIP，端口在 SNAT 段）  ret_owner(五元组)
 *   其他 TCP / UDP                              fwd_owner(五元组)
 *   ARP / ICMP / 分片 / 非 IPv4 / 畸形           hash(源, 目的地址) % N
 *
 * 分不准的情况（如 ICMP 差错要按内层五元组找 owner）由 worker 再转交一次。
 * rtc 模式下 master 线程也用它处理 worker 之外的 RX 队列（见 master.cpp）。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_RECEIVER_H
#define L4LB_DATAPLANE_RECEIVER_H

#include "common/config.h"
#include "common/stats.h"
#include "common/types.h"
#include <cstddef>
#include <cstdint>
#include <vector>

namespace l4lb {

class Steering;

/// pipeline 模式的 worker 数上限（receiver 按 worker 分组的栈上缓冲大小）
constexpr uint16_t kMaxPipelineWorkers = 16;

/// 计算一个包应该交给哪个 worker（只读，可多线程使用）
class Dispatcher {
public:
  void init(const Steering *steering, ForwardMode mode,
            const std::vector<IPv4Addr> &local_ips);

  /// @param data 以太网帧起始  @param len 帧长度
  uint16_t target(const uint8_t *data, size_t len) const;

private:
  bool is_lip(IPv4Addr ip) const;

  const Steering *st_ = nullptr;
  bool fullnat_ = false;
  std::vector<IPv4Addr> lips_;
};

/**
 * @brief 收一个 RX 队列的一批包并分发到 worker 的 rx_ring
 *
 * ring 满的包直接丢弃（计入 ST_DROP_RX_RING）。调用者必须是这些 rx_ring
 * 唯一的生产者。
 * @return 收到的包数
 */
uint16_t dispatch_rx_queue(const Dispatcher &d, uint16_t queue,
                           WorkerStats &stats);

/// receiver 主循环（pipeline 模式的 main lcore），签名符合 rte_eal_remote_launch
int receiver_loop(void *arg);

} // namespace l4lb

#endif // L4LB_DATAPLANE_RECEIVER_H
