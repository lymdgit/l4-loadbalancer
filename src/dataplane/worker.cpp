/**
 * @file worker.cpp
 * @brief 数据面 worker 循环、TX 缓冲与统计文本
 */

#include "dataplane/worker.h"

#include "common/logger.h"
#include "core/processor.h"
#include "dataplane/context.h"

#include <algorithm>
#include <cstdio>
#include <mutex>
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

namespace {

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

} // namespace

void tx_buffer_add(WorkerCtx &w, struct rte_mbuf *m) {
  w.tx.pkts[w.tx.count++] = m;
  if (w.tx.count >= BURST_SIZE)
    tx_flush(w);
}

std::string format_stats(bool verbose) {
  StatsTotal t = stats_total();
  std::ostringstream os;
  os << "=== L4 LB Statistics (" << (g_dp.pipeline ? "pipeline" : "rtc")
     << ", workers: " << g_dp.num_workers << ", steering: "
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
  if (t[ST_DROP_RX_RING])
    os << " rx_ring_full=" << t[ST_DROP_RX_RING];
  os << (any || t[ST_TX_FULL] || t[ST_DROP_RX_RING] ? "" : " none") << "\n";
  os << "NIC: ipackets " << g_dp.nic.ipackets.load(std::memory_order_relaxed)
     << ", opackets " << g_dp.nic.opackets.load(std::memory_order_relaxed)
     << ", imissed " << g_dp.nic.imissed.load(std::memory_order_relaxed)
     << ", ierrors " << g_dp.nic.ierrors.load(std::memory_order_relaxed)
     << ", oerrors " << g_dp.nic.oerrors.load(std::memory_order_relaxed)
     << ", rx_nombuf " << g_dp.nic.rx_nombuf.load(std::memory_order_relaxed)
     << "\n";
  if (verbose) {
    for (unsigned i = 0; i < ST_COUNT; ++i)
      os << stat_name(static_cast<Stat>(i)) << " " << t.c[i] << "\n";
    // busy_ms：累计忙碌时间，两次 stats -v 相减除以间隔即忙碌率（也见 rate 命令）
    if (const WorkerStats *r = g_dp.receiver_stats)
      os << "receiver lcore " << rte_get_main_lcore() << " rx " << r->get(ST_RX)
         << " rx_ring_full " << r->get(ST_DROP_RX_RING) << " busy_ms "
         << r->busy() * 1000 / g_dp.tsc_hz << "\n";
    for (uint16_t i = 0; i < g_dp.num_workers; ++i) {
      const WorkerCtx *w = g_dp.workers[i];
      os << "worker " << i << " lcore " << w->lcore_id << " rx "
         << w->stats.get(ST_RX) << " ring_in " << w->stats.get(ST_RING_IN)
         << " tx " << w->stats.get(ST_TX) << " redirect_out "
         << w->stats.get(ST_REDIRECT_OUT);
      if (w->rx_ring)
        os << " ring_used " << rte_ring_count(w->rx_ring);
      os << " busy_ms " << w->stats.busy() * 1000 / g_dp.tsc_hz << "\n";
    }
    if (const WorkerCtx *m = g_dp.master)
      os << "master tx " << m->stats.get(ST_TX) << " tx_full "
         << m->stats.get(ST_TX_FULL) << "\n";
  }
  return os.str();
}

// ============================================================================
// 周期性能报告（速率 + 忙碌率）
// ============================================================================

namespace {

/// 上一次报告时的计数，只在 master 线程访问
struct PerfBase {
  bool valid = false;
  uint64_t tsc = 0;
  StatsTotal total;
  uint64_t imissed = 0, ierrors = 0, nombuf = 0, ipackets = 0, opackets = 0;
  std::vector<uint64_t> busy; ///< receiver（pipeline）+ 各 worker
  std::vector<uint64_t> pkts;
};
PerfBase g_perf;
std::mutex g_perf_mu;
std::string g_perf_last = "no report yet (first one after stats_interval)\n";

std::string pct(uint64_t busy, uint64_t elapsed) {
  char b[16];
  snprintf(b, sizeof(b), "%.1f%%", elapsed ? 100.0 * busy / elapsed : 0.0);
  return b;
}

std::string rate(uint64_t n, double sec) {
  char b[32];
  double r = sec > 0 ? n / sec : 0;
  if (r >= 1e6)
    snprintf(b, sizeof(b), "%.2fM", r / 1e6);
  else if (r >= 1e3)
    snprintf(b, sizeof(b), "%.1fk", r / 1e3);
  else
    snprintf(b, sizeof(b), "%.0f", r);
  return b;
}

/// 线程列表：pipeline 的 receiver 在前，然后是各 worker
void collect(std::vector<uint64_t> &busy, std::vector<uint64_t> &pkts) {
  busy.clear();
  pkts.clear();
  if (const WorkerStats *r = g_dp.receiver_stats) {
    busy.push_back(r->busy());
    pkts.push_back(r->get(ST_RX));
  }
  for (uint16_t i = 0; i < g_dp.num_workers; ++i) {
    const WorkerStats &s = g_dp.workers[i]->stats;
    busy.push_back(s.busy());
    pkts.push_back(s.get(g_dp.pipeline ? ST_RING_IN : ST_RX) +
                   s.get(ST_REDIRECT_IN));
  }
}

} // namespace

void nic_stats_update() {
  struct rte_eth_stats st;
  if (rte_eth_stats_get(g_dp.port_id, &st) != 0)
    return;
  g_dp.nic.ipackets.store(st.ipackets, std::memory_order_relaxed);
  g_dp.nic.opackets.store(st.opackets, std::memory_order_relaxed);
  g_dp.nic.imissed.store(st.imissed, std::memory_order_relaxed);
  g_dp.nic.ierrors.store(st.ierrors, std::memory_order_relaxed);
  g_dp.nic.oerrors.store(st.oerrors, std::memory_order_relaxed);
  g_dp.nic.rx_nombuf.store(st.rx_nombuf, std::memory_order_relaxed);
}

std::string perf_report_tick(uint64_t now_tsc) {
  nic_stats_update();
  PerfBase cur;
  cur.valid = true;
  cur.tsc = now_tsc;
  cur.total = stats_total();
  cur.ipackets = g_dp.nic.ipackets.load(std::memory_order_relaxed);
  cur.opackets = g_dp.nic.opackets.load(std::memory_order_relaxed);
  cur.imissed = g_dp.nic.imissed.load(std::memory_order_relaxed);
  cur.ierrors = g_dp.nic.ierrors.load(std::memory_order_relaxed);
  cur.nombuf = g_dp.nic.rx_nombuf.load(std::memory_order_relaxed);
  collect(cur.busy, cur.pkts);

  PerfBase prev = g_perf;
  g_perf = cur;
  if (!prev.valid || cur.busy.size() != prev.busy.size())
    return "";

  const uint64_t el = cur.tsc - prev.tsc;
  const double sec = static_cast<double>(el) / g_dp.tsc_hz;
  auto d = [&](Stat s) { return cur.total[s] - prev.total[s]; };
  uint64_t drops = cur.total.drops() - prev.total.drops();

  std::ostringstream os;
  char head[64];
  snprintf(head, sizeof(head), "=== Perf (last %.1fs) ===\n", sec);
  os << head;
  os << "Rate: rx " << rate(d(ST_RX), sec) << " pps, tx " << rate(d(ST_TX), sec)
     << " pps, fwd in " << rate(d(ST_FWD_IN), sec) << " pps, fwd out "
     << rate(d(ST_FWD_OUT), sec) << " pps, new sess "
     << rate(d(ST_SESS_NEW), sec) << " /s, drops " << rate(drops, sec)
     << " /s\n";
  os << "Busy:";
  size_t i = 0;
  if (g_dp.receiver_stats) {
    os << " receiver " << pct(cur.busy[0] - prev.busy[0], el);
    ++i;
  }
  for (uint16_t w = 0; w < g_dp.num_workers; ++w, ++i)
    os << " | worker" << w << " " << pct(cur.busy[i] - prev.busy[i], el) << " "
       << rate(cur.pkts[i] - prev.pkts[i], sec) << "pps";
  os << "\n";
  os << "NIC: rx " << rate(cur.ipackets - prev.ipackets, sec) << " pps, tx "
     << rate(cur.opackets - prev.opackets, sec) << " pps, imissed +"
     << cur.imissed - prev.imissed << ", ierrors +"
     << cur.ierrors - prev.ierrors << ", rx_nombuf +" << cur.nombuf - prev.nombuf
     << " (total imissed " << cur.imissed << ")\n";
  std::string out = os.str();
  std::lock_guard<std::mutex> lk(g_perf_mu);
  g_perf_last = out;
  return out;
}

std::string perf_report_last() {
  std::lock_guard<std::mutex> lk(g_perf_mu);
  return g_perf_last;
}

// ============================================================================
// Worker 循环
// ============================================================================

int worker_loop(void *arg) {
  WorkerCtx &w = *static_cast<WorkerCtx *>(arg);
  struct rte_mbuf *bufs[BURST_SIZE];
  uint64_t loops = 0;
  // rtc：本 worker 轮询的 RX 队列 idx, idx + N, ...（RX 队列数可能多于 worker 数）
  std::vector<uint16_t> rxqs;
  if (!g_dp.pipeline)
    for (uint16_t q = w.idx; q < g_dp.num_rx_queues; q += g_dp.num_workers)
      rxqs.push_back(q);

  if (g_dp.pipeline)
    LOG_INFO("Worker %u started on lcore %u, rx ring, TX queue %u", w.idx,
             w.lcore_id, w.idx);
  else
    LOG_INFO("Worker %u started on lcore %u, queue %u%s", w.idx, w.lcore_id,
             w.idx, rxqs.size() > 1 ? " (+ extra RX queues)" : "");

  // 注册到 RCU：之后每轮循环报告一次静默期
  rte_rcu_qsbr_thread_register(g_dp.qsbr, w.lcore_id);
  rte_rcu_qsbr_thread_online(g_dp.qsbr, w.lcore_id);

  while (g_running.load(std::memory_order_relaxed)) {
    const Snapshot &snap = *g_dp.snapshots.current();
    const uint64_t t0 = rte_rdtsc();
    unsigned work = 0;

    // 【热路径】收包 + 处理：rtc 从网卡收，pipeline 从 receiver 的 ring 取
    if (g_dp.pipeline) {
      unsigned nb = rte_ring_sc_dequeue_burst(
          w.rx_ring, reinterpret_cast<void **>(bufs), BURST_SIZE, nullptr);
      if (nb) {
        w.stats.add(ST_RING_IN, nb);
        for (unsigned i = 0; i < nb; ++i)
          handle(w, snap, bufs[i], false);
      }
      work += nb;
    } else {
      for (uint16_t q : rxqs) {
        uint16_t nb = rte_eth_rx_burst(g_dp.port_id, q, bufs, BURST_SIZE);
        if (nb) {
          w.stats.add(ST_RX, nb);
          for (uint16_t i = 0; i < nb; ++i)
            handle(w, snap, bufs[i], false);
        }
        work += nb;
      }
    }
    // 其他 worker 转交来的包（会话属于本核）
    unsigned nr = rte_ring_sc_dequeue_burst(
        w.redirect_ring, reinterpret_cast<void **>(bufs), BURST_SIZE, nullptr);
    if (nr) {
      w.stats.add(ST_REDIRECT_IN, nr);
      for (unsigned i = 0; i < nr; ++i)
        handle(w, snap, bufs[i], true);
    }
    work += nr;
    tx_flush(w);
    // 忙碌率：只累计有包处理的轮次（空轮询不算）
    if (work)
      w.stats.add_busy(rte_rdtsc() - t0);

    // 本轮处理完毕，不再持有快照、邻居表的引用
    rte_rcu_qsbr_quiescent(g_dp.qsbr, w.lcore_id);

    // 【降频】每 1024 轮读一次时钟，推进时间轮
    if (unlikely((++loops & 1023) == 0)) {
      w.now_ms = (rte_get_tsc_cycles() - g_dp.start_tsc) * 1000 / g_dp.tsc_hz;
      w.now_tick = 1 + w.now_ms / 1000;
      size_t n = w.sessions.expire(w.now_tick);
      if (n)
        w.stats.add(ST_SESS_EXPIRED, n);
    }
  }

  tx_flush(w);
  rte_rcu_qsbr_thread_offline(g_dp.qsbr, w.lcore_id);
  rte_rcu_qsbr_thread_unregister(g_dp.qsbr, w.lcore_id);
  LOG_INFO("Worker %u on lcore %u exiting", w.idx, w.lcore_id);
  return 0;
}

} // namespace l4lb
