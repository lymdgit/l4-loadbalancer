/**
 * @file worker.cpp
 * @brief 数据面 worker 循环与 master 周期任务
 */

#include "dataplane/worker.h"

#include "common/logger.h"
#include "core/processor.h"
#include "ctrl/healthcheck.h"
#include "dataplane/context.h"
#include "protocol/arp.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include <rte_branch_prediction.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_rcu_qsbr.h>
#include "common/dpdk_ring.h"

namespace l4lb {

namespace {

HealthChecker g_hc; ///< 只在 master 上使用

void tx_flush(WorkerCtx &w) {
  TxBuffer &b = w.tx;
  if (b.count == 0)
    return;
  uint16_t nb = rte_eth_tx_burst(g_dp.port_id, w.idx, b.pkts, b.count);
  w.stats.add(ST_TX, nb);
  if (unlikely(nb < b.count)) {
    w.stats.add(ST_TX_FULL, b.count - nb);
    for (uint16_t i = nb; i < b.count; ++i)
      rte_pktmbuf_free(b.pkts[i]);
  }
  b.count = 0;
}

void handle(WorkerCtx &w, const Snapshot &snap, struct rte_mbuf *m,
            bool redirected) {
  Result r = process_packet(w, snap, m, redirected);
  switch (r.verdict) {
  case Verdict::SEND:
    tx_buffer_add(w, m);
    return;
  case Verdict::REDIRECT: {
    WorkerCtx *to = g_dp.workers[r.target];
    if (to && rte_ring_mp_enqueue(to->redirect_ring, m) == 0) {
      w.stats.add(ST_REDIRECT_OUT);
      return;
    }
    w.stats.add(ST_DROP_REDIRECT);
    break;
  }
  case Verdict::DROP:
    w.stats.add(r.reason);
    break;
  case Verdict::CONSUMED:
    break;
  }
  rte_pktmbuf_free(m);
}

// ---------------------------------------------------------------------------
// master：ARP / 邻居表
// ---------------------------------------------------------------------------

IPv4Addr arp_src_ip() {
  const auto &c = g_dp.cfg;
  if (!c.local_ips.empty())
    return c.local_ips[0];
  if (c.hc_src)
    return c.hc_src;
  return c.services[0].vip;
}

void send_arp(WorkerCtx &master, IPv4Addr target, bool gratuitous) {
  struct rte_mbuf *m = rte_pktmbuf_alloc(g_dp.pool);
  if (!m)
    return;
  uint8_t *buf = reinterpret_cast<uint8_t *>(
      rte_pktmbuf_append(m, sizeof(EthernetHeader) + sizeof(ArpHeader)));
  if (!buf) {
    rte_pktmbuf_free(m);
    return;
  }
  if (gratuitous)
    ArpHandler::build_gratuitous(buf, target, g_dp.local_mac);
  else
    ArpHandler::build_request(buf, target, arp_src_ip(), g_dp.local_mac);
  tx_buffer_add(master, m);
}

void send_garp(WorkerCtx &master, const Snapshot &snap) {
  std::vector<IPv4Addr> addrs;
  for (const auto &s : snap.services)
    addrs.push_back(s.vip);
  for (auto ip : snap.local_ips)
    addrs.push_back(ip);
  if (snap.hc_src)
    addrs.push_back(snap.hc_src);
  std::sort(addrs.begin(), addrs.end());
  addrs.erase(std::unique(addrs.begin(), addrs.end()), addrs.end());
  for (auto ip : addrs)
    send_arp(master, ip, true);
}

/// 网关和没有静态 MAC 的 RS：提前解析，避免首包丢失
void preresolve(WorkerCtx &master, const Snapshot &snap) {
  MacAddr mac;
  if (g_dp.route.gateway && !g_dp.neigh.lookup(g_dp.route.gateway, mac))
    master_request_neigh(master, g_dp.route.gateway);
  for (const auto &rs : snap.rs_pool) {
    IPv4Addr nh = g_dp.route.next_hop(rs.ip);
    if (mac_is_zero(rs.mac) && nh && !g_dp.neigh.lookup(nh, mac))
      master_request_neigh(master, nh);
  }
}

void drain_master_events(WorkerCtx &master) {
  MasterEvent evs[64];
  unsigned n = rte_ring_sc_dequeue_burst_elem(g_dp.master_ring, evs,
                                              sizeof(MasterEvent), 64, nullptr);
  for (unsigned i = 0; i < n; ++i) {
    const MasterEvent &e = evs[i];
    switch (e.type) {
    case EV_ARP_FOR_US:
      g_dp.neigh.learn(e.ip, e.mac, false, master.now_tick);
      break;
    case EV_ARP_REPLY:
      g_dp.neigh.learn(e.ip, e.mac, true, master.now_tick);
      break;
    case EV_NEIGH_HINT:
      g_dp.neigh.hint(e.ip, e.mac, master.now_tick);
      break;
    case EV_NEIGH_MISS:
      master_request_neigh(master, e.ip);
      break;
    case EV_HC_RESP:
      g_hc.on_response(master, e, master.now_ms);
      break;
    default:
      break;
    }
  }
}

struct MasterTimers {
  uint64_t hc_ms = 0, neigh_ms = 0, garp_ms = 0, stats_ms = 0;
  int garp_count = 0;
};

void master_periodic(WorkerCtx &master, const Snapshot &snap,
                     MasterTimers &t) {
  uint64_t now = master.now_ms;
  // 免费 ARP：启动后前 3 秒每秒一次（防止第一个被丢），之后每 60 秒一次
  uint64_t garp_gap = t.garp_count < 3 ? 1000 : 60000;
  if (t.garp_count == 0 || now - t.garp_ms >= garp_gap) {
    send_garp(master, snap);
    t.garp_ms = now;
    ++t.garp_count;
  }
  if (now - t.hc_ms >= 100) {
    g_hc.tick(master, snap, now);
    t.hc_ms = now;
  }
  if (now - t.neigh_ms >= 1000) {
    std::vector<IPv4Addr> refresh;
    g_dp.neigh.age(master.now_tick, refresh);
    for (auto ip : refresh)
      send_arp(master, ip, false);
    preresolve(master, snap);
    t.neigh_ms = now;
  }
  if (now - t.stats_ms >= 10000) {
    std::istringstream is(format_stats(false));
    for (std::string line; std::getline(is, line);)
      LOG_INFO("%s", line.c_str());
    t.stats_ms = now;
  }
}

} // namespace

void tx_buffer_add(WorkerCtx &w, struct rte_mbuf *m) {
  w.tx.pkts[w.tx.count++] = m;
  if (w.tx.count >= BURST_SIZE)
    tx_flush(w);
}

void master_request_neigh(WorkerCtx &master, IPv4Addr ip) {
  if (ip && g_dp.neigh.want_request(ip, master.now_tick))
    send_arp(master, ip, false);
}

std::string format_stats(bool verbose) {
  StatsTotal t = stats_total();
  std::ostringstream os;
  os << "=== L4 LB Statistics (workers: " << g_dp.num_workers << ", steering: "
     << (g_dp.steering.hw() ? "hw-rss" : "sw") << ") ===\n";
  os << "DPDK RX: " << t[ST_RX] << ", TX: " << t[ST_TX]
     << ", Dropped: " << t.drops() << "\n";
  os << "Forwarded: in " << t[ST_FWD_IN] << ", out " << t[ST_FWD_OUT]
     << ", icmp err " << t[ST_ICMP_ERR_FWD] << " | TCP " << t[ST_TCP]
     << ", UDP " << t[ST_UDP] << ", ICMP " << t[ST_ICMP] << ", ARP "
     << t[ST_ARP] << "\n";
  os << "Sessions: active " << sessions_active(t) << ", new "
     << t[ST_SESS_NEW] << ", expired " << t[ST_SESS_EXPIRED] << ", closed "
     << t[ST_SESS_CLOSED] << "\n";
  os << "Redirect: out " << t[ST_REDIRECT_OUT] << ", in "
     << t[ST_REDIRECT_IN] << ", rss mismatch " << t[ST_RSS_MISMATCH]
     << ", rss no hash " << t[ST_RSS_NO_HASH]
     << " | neighbors " << g_dp.neigh.count() << "\n";
  os << "Drops:";
  bool any = false;
  for (unsigned i = ST_DROP_MALFORMED; i <= ST_DROP_OTHER; ++i)
    if (t.c[i]) {
      os << " " << stat_name(static_cast<Stat>(i)) << "=" << t.c[i];
      any = true;
    }
  if (t[ST_TX_FULL])
    os << " tx_full=" << t[ST_TX_FULL];
  os << (any || t[ST_TX_FULL] ? "" : " none") << "\n";
  if (verbose) {
    for (unsigned i = 0; i < ST_COUNT; ++i)
      os << stat_name(static_cast<Stat>(i)) << " " << t.c[i] << "\n";
    for (uint16_t i = 0; i < g_dp.num_workers; ++i) {
      const WorkerCtx *w = g_dp.workers[i];
      os << "worker " << i << " lcore " << w->lcore_id << " rx "
         << w->stats.get(ST_RX) << " tx " << w->stats.get(ST_TX)
         << " redirect_out " << w->stats.get(ST_REDIRECT_OUT) << "\n";
    }
  }
  return os.str();
}

// ============================================================================
// Worker 循环
// ============================================================================

int worker_loop(void *arg) {
  WorkerCtx &w = *static_cast<WorkerCtx *>(arg);
  const bool master = w.is_master();
  struct rte_mbuf *bufs[BURST_SIZE];
  MasterTimers timers;
  uint64_t loops = 0;

  if (master)
    g_hc.init(g_dp.cfg.health, g_dp.cfg.hc_src);

  LOG_INFO("Worker %u started on lcore %u, queue %u%s", w.idx, w.lcore_id,
           w.idx, master ? " (master)" : "");

  // 注册到 RCU：之后每轮循环报告一次静默期
  rte_rcu_qsbr_thread_register(g_dp.qsbr, w.lcore_id);
  rte_rcu_qsbr_thread_online(g_dp.qsbr, w.lcore_id);

  while (g_running.load(std::memory_order_relaxed)) {
    const Snapshot &snap = *g_dp.snapshots.current();

    // 【热路径】收包 + 处理
    uint16_t nb = rte_eth_rx_burst(g_dp.port_id, w.idx, bufs, BURST_SIZE);
    if (nb) {
      w.stats.add(ST_RX, nb);
      for (uint16_t i = 0; i < nb; ++i)
        handle(w, snap, bufs[i], false);
    }
    // 其他 worker 转交来的包（会话属于本核）
    unsigned nr = rte_ring_sc_dequeue_burst(
        w.redirect_ring, reinterpret_cast<void **>(bufs), BURST_SIZE, nullptr);
    if (nr) {
      w.stats.add(ST_REDIRECT_IN, nr);
      for (unsigned i = 0; i < nr; ++i)
        handle(w, snap, bufs[i], true);
    }
    if (master)
      drain_master_events(w);
    tx_flush(w);

    // 本轮处理完毕，不再持有快照、邻居表的引用
    rte_rcu_qsbr_quiescent(g_dp.qsbr, w.lcore_id);

    // 【降频】每 1024 轮读一次时钟，推进时间轮，执行 master 任务
    if (unlikely((++loops & 1023) == 0)) {
      w.now_ms = (rte_get_tsc_cycles() - g_dp.start_tsc) * 1000 / g_dp.tsc_hz;
      w.now_tick = 1 + w.now_ms / 1000;
      size_t n = w.sessions.expire(w.now_tick);
      if (n)
        w.stats.add(ST_SESS_EXPIRED, n);
      if (master) {
        master_periodic(w, *g_dp.snapshots.current(), timers);
        tx_flush(w);
      }
    }
  }

  tx_flush(w);
  rte_rcu_qsbr_thread_offline(g_dp.qsbr, w.lcore_id);
  rte_rcu_qsbr_thread_unregister(g_dp.qsbr, w.lcore_id);
  LOG_INFO("Worker %u on lcore %u exiting", w.idx, w.lcore_id);
  return 0;
}

} // namespace l4lb
