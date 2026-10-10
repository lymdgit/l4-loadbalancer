/**
 * @file receiver.cpp
 * @brief pipeline 模式的收包分发实现
 */

#include "dataplane/receiver.h"

#include "common/logger.h"
#include "dataplane/context.h"
#include "dataplane/steering.h"
#include "protocol/ethernet.h"
#include "protocol/ip.h"

#include <algorithm>
#include <cstring>

#include <rte_branch_prediction.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_jhash.h>
#include <rte_mbuf.h>
#include "common/dpdk_ring.h"

namespace l4lb {

void Dispatcher::init(const Steering *steering, ForwardMode mode,
                      const std::vector<IPv4Addr> &local_ips) {
  st_ = steering;
  fullnat_ = mode == ForwardMode::NAT;
  lips_ = local_ips;
  std::sort(lips_.begin(), lips_.end());
}

bool Dispatcher::is_lip(IPv4Addr ip) const {
  // LIP 通常只有几个，线性查找比二分更快
  for (IPv4Addr l : lips_)
    if (l == ip)
      return true;
  return false;
}

uint16_t Dispatcher::target(const uint8_t *data, size_t len) const {
  const uint16_t n = st_->workers();
  if (n == 1)
    return 0;
  if (unlikely(len < sizeof(EthernetHeader) + sizeof(IPv4Header)))
    return 0;
  auto *eth = reinterpret_cast<const EthernetHeader *>(data);
  if (unlikely(!eth->is_ipv4()))
    return 0; // ARP 等：哪个 worker 处理都可以
  auto *ip =
      reinterpret_cast<const IPv4Header *>(data + sizeof(EthernetHeader));
  size_t ihl = ip->get_header_len();
  uint16_t frag = rte_be_to_cpu_16(ip->flags_fragment) & 0x3FFF; // MF + 偏移
  bool l4 = (ip->protocol == 6 || ip->protocol == 17) && !frag &&
            ihl >= sizeof(IPv4Header) &&
            len >= sizeof(EthernetHeader) + ihl + 4;
  if (!l4) // ICMP / 分片 / 其他协议：按地址对分散，worker 内部再找 owner
    return static_cast<uint16_t>(rte_jhash_2words(ip->src_ip, ip->dst_ip, 0) %
                                 n);

  const uint8_t *l4h = data + sizeof(EthernetHeader) + ihl;
  Port sport, dport;
  memcpy(&sport, l4h, 2);
  memcpy(&dport, l4h + 2, 2);
  FiveTuple t(ip->src_ip, ip->dst_ip, sport, dport, ip->protocol);
  uint16_t p = rte_be_to_cpu_16(dport);
  if (fullnat_ && p >= kNatPortMin && p <= kNatPortMax && is_lip(ip->dst_ip))
    return st_->ret_owner(t);
  return st_->fwd_owner(t);
}

uint16_t dispatch_rx_queue(const Dispatcher &d, uint16_t queue,
                           WorkerStats &stats) {
  struct rte_mbuf *bufs[BURST_SIZE];
  uint16_t nb = rte_eth_rx_burst(g_dp.port_id, queue, bufs, BURST_SIZE);
  if (nb == 0)
    return 0;
  stats.add(ST_RX, nb);

  // 按目标 worker 分组，每组一次入队
  const uint16_t nw = g_dp.num_workers;
  struct rte_mbuf *groups[kMaxPipelineWorkers][BURST_SIZE];
  uint16_t counts[kMaxPipelineWorkers] = {};
  uint64_t bytes = 0;
  for (uint16_t i = 0; i < nb; ++i) {
    struct rte_mbuf *m = bufs[i];
    bytes += rte_pktmbuf_pkt_len(m);
    uint16_t to = d.target(rte_pktmbuf_mtod(m, const uint8_t *),
                           rte_pktmbuf_data_len(m));
    groups[to][counts[to]++] = m;
  }
  stats.add_bytes(BS_RX, bytes);
  for (uint16_t w = 0; w < nw; ++w) {
    if (counts[w] == 0)
      continue;
    unsigned done = rte_ring_sp_enqueue_burst(
        g_dp.workers[w]->rx_ring, reinterpret_cast<void **>(groups[w]),
        counts[w], nullptr);
    if (unlikely(done < counts[w])) {
      stats.add(ST_DROP_RX_RING, counts[w] - done);
      for (unsigned i = done; i < counts[w]; ++i)
        rte_pktmbuf_free(groups[w][i]);
    }
  }
  return nb;
}

int receiver_loop(void *) {
  Dispatcher d;
  d.init(&g_dp.steering, g_dp.cfg.mode, g_dp.cfg.local_ips);
  WorkerStats &stats = *g_dp.receiver_stats;
  LOG_INFO("Receiver started on lcore %u, polling %u RX queue(s) for %u "
           "workers",
           rte_lcore_id(), g_dp.num_rx_queues, g_dp.num_workers);
  BusyMeter busy;
  while (g_running.load(std::memory_order_relaxed)) {
    unsigned work = 0;
    for (uint16_t q = 0; q < g_dp.num_rx_queues; ++q)
      work += dispatch_rx_queue(d, q, stats);
    busy.loop(work != 0, stats);
  }
  LOG_INFO("Receiver on lcore %u exiting", rte_lcore_id());
  return 0;
}

} // namespace l4lb
