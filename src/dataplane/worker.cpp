/**
 * @file worker.cpp
 * @brief 数据面 worker 循环实现（批量发送优化版）
 */

#include "dataplane/worker.h"

#include "common/logger.h"
#include "common/stats.h"
#include "common/types.h"
#include "core/loadbalancer.h"
#include "dataplane/context.h"
#include "lb/session.h"

#include <rte_branch_prediction.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

namespace l4lb {

// ============================================================================
// 批量发送优化配置
// ============================================================================
#define TX_BATCH_SIZE 64 // 批量发送阈值，与 BURST_SIZE 匹配
#define TX_DRAIN_US 50   // 缩短刷新间隔，降低延迟
#define US_PER_S 1000000 // 每秒微秒数

// Per-core TX buffer 结构
struct TxBuffer {
  struct rte_mbuf *pkts
      [TX_BATCH_SIZE]; // 这个数组中，最多存64个mbuf的指针。这就是结构体指针数组
  uint16_t count;
  uint64_t last_drain_tsc;
};

// 刷新 TX buffer
// 把积攒了一批的包发送出去
static inline void tx_buffer_flush(TxBuffer *buf, uint16_t port,
                                   uint16_t queue, PortLcoreStats &st) {
  if (buf->count == 0)
    return;

  uint16_t nb_tx = rte_eth_tx_burst(port, queue, buf->pkts, buf->count);
  // per-lcore 计数，统计发出去多少包
  stat_add(st.tx, nb_tx);

  // 释放未发送的包：未能成功发送的包，直接释放掉
  if (unlikely(nb_tx < buf->count)) {
    stat_add(st.dropped, buf->count - nb_tx);
    for (uint16_t i = nb_tx; i < buf->count; ++i) {
      rte_pktmbuf_free(buf->pkts[i]);
    }
  }
  buf->count = 0;
}

// 添加包到 TX buffer
static inline void tx_buffer_add(TxBuffer *buf, struct rte_mbuf *mbuf,
                                 uint16_t port, uint16_t queue,
                                 PortLcoreStats &st) {
  buf->pkts[buf->count++] = mbuf; // 把当前mbuf指针存到数组里面，等待批量发送

  // Buffer 满了就发送
  if (buf->count >= TX_BATCH_SIZE) {
    tx_buffer_flush(buf, port, queue, st);
  }
}

// ============================================================================
// 处理单个数据包 (返回是否需要发送)
// ============================================================================
static inline struct rte_mbuf *process_packet_batch(struct rte_mbuf *mbuf,
                                                    PortLcoreStats &st) {
  uint8_t *data = rte_pktmbuf_mtod(mbuf, uint8_t *);
  size_t len = rte_pktmbuf_data_len(mbuf);

  // 调用 LoadBalancer 处理
  bool should_send = false;
  bool handled = g_lb.process_packet(mbuf, data, len, should_send);

  if (handled && should_send) {
    return mbuf; // 返回需要发送的包
  } else {
    // 不发送，释放 mbuf
    rte_pktmbuf_free(mbuf);
    if (!handled) {
      stat_add(st.dropped);
    }
    return nullptr; // 不需要发送
  }
}

// ============================================================================
// Worker 循环 (每个 lcore 运行一个) - 批量发送优化版
// ============================================================================
int worker_loop(void *arg) {
  uint16_t queue_id = *static_cast<uint16_t *>(arg);
  unsigned lcore_id = rte_lcore_id();
  PortLcoreStats &port_st = g_port_stats[stat_lcore()];
  auto &sessions = SessionManager::instance();

  struct rte_mbuf *bufs[BURST_SIZE];
  TxBuffer tx_buf = {.pkts = {}, .count = 0, .last_drain_tsc = 0};

  uint64_t cur_tsc = rte_get_tsc_cycles(); // 初始时读一次
  uint64_t last_stats_time = cur_tsc;
  uint64_t stats_interval = rte_get_tsc_hz() * 10; // 每 10 秒打印统计
  uint64_t drain_tsc =
      (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * TX_DRAIN_US;
  uint64_t local_loop_count = 0;
  bool is_master = (lcore_id == rte_get_main_lcore());

  LOG_INFO("Worker started on lcore %u, queue %u%s (batch TX enabled)",
           lcore_id, queue_id, is_master ? " (master)" : "");

  // 注册到反向表的 RCU：之后每轮循环报告一次静默期
  sessions.worker_online(lcore_id);

  while (g_running.load(std::memory_order_relaxed)) {
    // -----------------------------------------------------------------------
    // 【热路径】核心业务：收包 + 转发，保持最高频执行，不在此处读时钟
    // -----------------------------------------------------------------------
    uint16_t nb_rx = rte_eth_rx_burst(g_port_id, queue_id, bufs, BURST_SIZE);

    if (nb_rx > 0) {
      // per-lcore 计数：统计接收到的总包数
      stat_add(port_st.rx, nb_rx);

      // 批量处理每个数据包
      for (uint16_t i = 0; i < nb_rx; ++i) {
        struct rte_mbuf *to_send = process_packet_batch(bufs[i], port_st);
        if (to_send) {
          // 内联函数，只在调用处展开，没有函数调用开销
          tx_buffer_add(&tx_buf, to_send, g_port_id, queue_id, port_st);
        }
      }
      if (tx_buf.count > 0) {
        tx_buffer_flush(&tx_buf, g_port_id, queue_id, port_st);
        tx_buf.last_drain_tsc = cur_tsc; // 用缓存的 cur_tsc，避免再读时钟
      }
    }

    // 本轮处理完毕，不再持有任何反向表 value 指针
    sessions.quiescent(lcore_id);

    ++local_loop_count;

    // -----------------------------------------------------------------------
    // 【降频读表】每 1024 次循环才读一次硬件时钟（位运算，零额外开销）
    // 彻底消灭 __rdtsc 霸屏火焰图的问题
    // -----------------------------------------------------------------------
    if (unlikely((local_loop_count & 1023) == 0)) {
      cur_tsc = rte_get_tsc_cycles();

      // 定期刷新 TX buffer（超时未满也发送，避免延迟积压）
      if (tx_buf.count > 0 && (cur_tsc - tx_buf.last_drain_tsc) > drain_tsc) {
        tx_buffer_flush(&tx_buf, g_port_id, queue_id, port_st);
        tx_buf.last_drain_tsc = cur_tsc;
      }

      // 定期清理过期会话（每 500000 次循环 ≈ 每 512*1024 次循环检查一次）
      if ((local_loop_count & 524287) == 0) { // 524287 = 512*1024 - 1
        size_t cleaned = sessions.cleanup_local(cur_tsc);
        if (cleaned > 0 && is_master) {
          LOG_INFO("Cleaned %zu expired sessions (local)", cleaned);
        }
      }

      // 定期打印统计信息 & 发送 ARP 探测（只有 master 执行）
      if (is_master && cur_tsc - last_stats_time >= stats_interval) {
        g_lb.send_arp_probes(g_port_id, queue_id, g_mbuf_pool);

        auto port_total = port_stats_total();
        auto stats = g_lb.get_stats();
        auto sess_stats = sessions.get_stats();
        auto sess_dbg = sessions.get_debug_stats();
        LOG_INFO("=== L4 LB Statistics (RSS: %u queues, Batch TX) ===",
                 g_num_queues);
        LOG_INFO("DPDK RX: %lu, TX: %lu, Dropped: %lu", port_total.rx,
                 port_total.tx, port_total.dropped);
        LOG_INFO("LB RX: %lu, TX: %lu, Dropped: %lu", stats.rx_packets,
                 stats.tx_packets, stats.dropped_packets);
        LOG_INFO("ARP: %lu, ICMP: %lu, TCP: %lu, UDP: %lu", stats.arp_packets,
                 stats.icmp_packets, stats.tcp_packets, stats.udp_packets);
        LOG_INFO("Forwarded: %lu, NAT: %lu, Sessions: %lu",
                 stats.forwarded_packets, stats.nat_translations,
                 sess_stats.active_sessions);
        LOG_INFO("Sess dbg: lk hit %lu miss %lu | rev hit %lu miss %lu | "
                 "create %lu fail %lu replaced %lu | upd miss %lu | "
                 "cleanup %lu",
                 sess_dbg.lookup_hit, sess_dbg.lookup_miss,
                 sess_dbg.reverse_hit, sess_dbg.reverse_miss, sess_dbg.create,
                 sess_dbg.create_fail, sess_dbg.replaced,
                 sess_dbg.update_miss, sess_dbg.cleanup_removed);
        LOG_INFO("========================");

        last_stats_time = cur_tsc;
      }
    }
  }

  // 退出前刷新剩余的 TX buffer
  tx_buffer_flush(&tx_buf, g_port_id, queue_id, port_st);

  // 退出 RCU：之后反向表回收不再等待本 lcore
  sessions.worker_offline(lcore_id);

  LOG_INFO("Worker on lcore %u exiting", lcore_id);
  return 0;
}

} // namespace l4lb
