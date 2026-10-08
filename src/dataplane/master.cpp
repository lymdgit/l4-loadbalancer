/**
 * @file master.cpp
 * @brief master 线程：ARP / 邻居表、健康检查、周期统计
 *
 * 普通线程（rte_thread_create_control，不占 -l 指定的 lcore），每 1ms 一轮：
 *   处理 worker 上报的事件（master_ring）-> 周期任务 -> 从独占 TX 队列发包
 * master 不收包：ARP 应答、健康检查回包由 worker 收到后通过 master_ring 转来。
 */

#include "dataplane/master.h"

#include "common/logger.h"
#include "ctrl/healthcheck.h"
#include "dataplane/context.h"
#include "dataplane/worker.h"
#include "protocol/arp.h"

#include <algorithm>
#include <sstream>
#include <vector>

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_rcu_qsbr.h>
#include <rte_thread.h>
#include "common/dpdk_ring.h"

namespace l4lb {

namespace {

HealthChecker g_hc;
rte_thread_t g_thread;
bool g_started = false;

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
  unsigned n;
  do {
    n = rte_ring_sc_dequeue_burst_elem(g_dp.master_ring, evs,
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
  } while (n == 64);
}

struct MasterTimers {
  uint64_t hc_ms = 0, neigh_ms = 0, garp_ms = 0, stats_ms = 0, nic_ms = 0;
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
  if (now - t.nic_ms >= 1000) {
    nic_stats_update(); // 让 stats 命令看到的网卡计数最多晚 1 秒
    t.nic_ms = now;
  }
  if (now - t.neigh_ms >= 1000) {
    std::vector<IPv4Addr> refresh;
    g_dp.neigh.age(master.now_tick, refresh);
    for (auto ip : refresh)
      send_arp(master, ip, false);
    preresolve(master, snap);
    t.neigh_ms = now;
  }
  // 周期统计：累计计数 + 本周期的速率和各线程忙碌率（压测时看这里）
  if (now - t.stats_ms >= g_dp.cfg.stats_interval * 1000ull) {
    std::string perf = perf_report_tick(rte_get_tsc_cycles());
    if (t.stats_ms) {
      std::istringstream is(format_stats(false) + perf);
      for (std::string line; std::getline(is, line);)
        LOG_INFO("%s", line.c_str());
    }
    t.stats_ms = now ? now : 1;
  }
}

uint32_t master_loop(void *arg) {
  WorkerCtx &m = *static_cast<WorkerCtx *>(arg);
  MasterTimers timers;

  g_hc.init(g_dp.cfg.health, g_dp.cfg.hc_src);
  // RCU：master 读快照和邻居表；thread id 用 kMasterRcuId（不与 lcore 冲突）
  rte_rcu_qsbr_thread_register(g_dp.qsbr, kMasterRcuId);
  rte_rcu_qsbr_thread_online(g_dp.qsbr, kMasterRcuId);
  LOG_INFO("Master thread started (TX queue %u)", m.idx);

  while (g_running.load(std::memory_order_relaxed)) {
    m.now_ms = (rte_get_tsc_cycles() - g_dp.start_tsc) * 1000 / g_dp.tsc_hz;
    m.now_tick = 1 + m.now_ms / 1000;

    drain_master_events(m);
    master_periodic(m, *g_dp.snapshots.current(), timers);
    tx_flush(m);

    rte_rcu_qsbr_quiescent(g_dp.qsbr, kMasterRcuId);
    rte_delay_us_sleep(1000);
  }

  tx_flush(m);
  rte_rcu_qsbr_thread_offline(g_dp.qsbr, kMasterRcuId);
  rte_rcu_qsbr_thread_unregister(g_dp.qsbr, kMasterRcuId);
  LOG_INFO("Master thread exiting");
  return 0;
}

} // namespace

void master_request_neigh(WorkerCtx &master, IPv4Addr ip) {
  if (ip && g_dp.neigh.want_request(ip, master.now_tick))
    send_arp(master, ip, false);
}

bool master_start(WorkerCtx *master) {
  int ret = rte_thread_create_control(&g_thread, "l4lb-master", master_loop,
                                      master);
  if (ret != 0) {
    LOG_ERROR("Failed to create master thread: %s", rte_strerror(-ret));
    return false;
  }
  g_started = true;
  return true;
}

void master_join() {
  if (g_started)
    rte_thread_join(g_thread, nullptr);
  g_started = false;
}

} // namespace l4lb
