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
