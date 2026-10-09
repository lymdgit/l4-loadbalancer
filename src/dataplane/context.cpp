/**
 * @file context.cpp
 * @brief 数据面全局状态定义
 */

#include "dataplane/context.h"

#include "common/dpdk_ring.h"

namespace l4lb {

Dataplane g_dp;
std::atomic<bool> g_running{true};

StatsTotal stats_total() {
  StatsTotal t;
  for (uint16_t i = 0; i < g_dp.num_workers; ++i) {
    const WorkerCtx *w = g_dp.workers[i];
    if (!w)
      continue;
    for (unsigned s = 0; s < ST_COUNT; ++s)
      t.c[s] += w->stats.get(static_cast<Stat>(s));
    for (unsigned s = 0; s < BS_COUNT; ++s)
      t.bytes[s] += w->stats.get_bytes(static_cast<ByteStat>(s));
  }
  for (const WorkerStats *a :
       {g_dp.receiver_stats, g_dp.master ? &g_dp.master->stats : nullptr})
    if (a) {
      for (unsigned s = 0; s < ST_COUNT; ++s)
        t.c[s] += a->get(static_cast<Stat>(s));
      for (unsigned s = 0; s < BS_COUNT; ++s)
        t.bytes[s] += a->get_bytes(static_cast<ByteStat>(s));
    }
  return t;
}

RsTotal rs_total(uint32_t rs_id) {
  RsTotal t;
  if (rs_id >= kMaxRsCounters)
    return t;
  for (uint16_t i = 0; i < g_dp.num_workers; ++i) {
    const WorkerCtx *w = g_dp.workers[i];
    if (!w || !w->rs_stats)
      continue;
    const RsCounters &c = w->rs_stats[rs_id];
    t.conns += stat_get(c.conns);
    t.pkts_in += stat_get(c.pkts_in);
    t.bytes_in += stat_get(c.bytes_in);
    t.pkts_out += stat_get(c.pkts_out);
    t.bytes_out += stat_get(c.bytes_out);
  }
  return t;
}

uint64_t sessions_active(const StatsTotal &t) {
  uint64_t gone = t[ST_SESS_EXPIRED] + t[ST_SESS_CLOSED];
  return t[ST_SESS_NEW] > gone ? t[ST_SESS_NEW] - gone : 0;
}

void post_master_event(const MasterEvent &ev) {
  MasterEvent copy = ev;
  rte_ring_mp_enqueue_elem(g_dp.master_ring, &copy, sizeof(copy));
}

} // namespace l4lb
