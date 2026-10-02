/**
 * @file main.cpp
 * @brief L4 负载均衡器主程序 - 纯 DPDK 实现
 *
 * 本文件只负责启动流程：参数解析 -> 加载配置 -> EAL / 端口 / 各模块初始化
 * -> 启动 worker 和控制线程 -> 退出清理。
 *
 *   数据面：     src/dataplane/{port,worker,steering}.cpp, src/core/processor.cpp
 *   会话与调度： src/lb/{session,scheduler,tcp_state}.cpp
 *   控制面：     src/ctrl/{snapshot,healthcheck,control}.cpp
 *   邻居/路由：  src/net/neigh.cpp, include/net/route.h
 *
 * @author L4 Load Balancer Project
 */

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_rcu_qsbr.h>
#include "common/dpdk_ring.h"

#include "common/config.h"
#include "common/logger.h"
#include "ctrl/control.h"
#include "dataplane/context.h"
#include "dataplane/port.h"
#include "dataplane/worker.h"

using namespace l4lb;

// ============================================================================
// 信号处理：只做 async-signal-safe 的事（写 lock-free atomic 标志），日志在主流程打印
// ============================================================================
static volatile sig_atomic_t g_signal_received = 0;

static void signal_handler(int sig) {
  g_signal_received = sig;
  g_running.store(false, std::memory_order_relaxed);
}

static_assert(std::atomic<bool>::is_always_lock_free,
              "g_running must be lock-free to be written from a signal handler");

static void usage(const char *prog) {
  printf("L4 Load Balancer (Pure DPDK)\n\n"
         "Usage: %s [DPDK EAL options] -- [LB options]\n\n"
         "LB Options (after --):\n"
         "  --lb-config <file>   config file (default: config/lb.conf)\n"
         "  --log <level>        debug/info/warn/error (overrides config)\n"
         "  --port <id>          DPDK port ID (default: 0)\n"
         "  --check-config       validate the config file and exit\n"
         "  --help-lb            show this help\n\n"
         "Example:\n"
         "  %s -l 1-4 -- --lb-config config/lb.conf\n",
         prog, prog);
}

static int fail(const char *msg) {
  LOG_FATAL("%s", msg);
  rte_eal_cleanup();
  return 1;
}

int main(int argc, char *argv[]) {
  std::string config_file = "config/lb.conf";
  std::string log_level;
  long port_arg = 0;
  bool check_only = false;

  std::vector<char *> eal_argv{argv[0]};
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--lb-config") == 0 && i + 1 < argc) {
      config_file = argv[++i];
    } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
      log_level = argv[++i];
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      char *end = nullptr;
      port_arg = strtol(argv[++i], &end, 10);
      if (*end || port_arg < 0 || port_arg >= RTE_MAX_ETHPORTS) {
        fprintf(stderr, "invalid --port %s\n", argv[i]);
        return 1;
      }
    } else if (strcmp(argv[i], "--check-config") == 0) {
      check_only = true;
    } else if (strcmp(argv[i], "--help-lb") == 0) {
      usage(argv[0]);
      return 0;
    } else {
      eal_argv.push_back(argv[i]);
    }
  }

  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  // ---- 配置（在 EAL 之前加载，配置错误时不必占用网卡和大页）----
  auto &config = Config::instance();
  if (!config.load(config_file)) {
    LOG_FATAL("Invalid config %s", config_file.c_str());
    return 1;
  }
  g_dp.cfg = config.lb();
  Logger::instance().set_level(log_level.empty() ? g_dp.cfg.log_level
                                                 : log_level);
  config.dump();
  if (check_only) {
    printf("config %s OK\n", config_file.c_str());
    return 0;
  }

  // ---- EAL ----
  int ret = rte_eal_init(static_cast<int>(eal_argv.size()), eal_argv.data());
  if (ret < 0) {
    LOG_FATAL("Failed to initialize DPDK EAL: %s", rte_strerror(rte_errno));
    return 1;
  }
  g_dp.tsc_hz = rte_get_tsc_hz();
  g_dp.start_tsc = rte_get_tsc_cycles();
  g_dp.port_id = static_cast<uint16_t>(port_arg);
  if (!rte_eth_dev_is_valid_port(g_dp.port_id))
    return fail("DPDK port not available (bind a NIC first, see "
                "scripts/setup_dpdk_env.sh)");
  g_dp.socket_id = rte_eth_dev_socket_id(g_dp.port_id);
  if (g_dp.socket_id < 0)
    g_dp.socket_id = static_cast<int>(rte_socket_id());

  const uint16_t num_lcores = static_cast<uint16_t>(rte_lcore_count());

  // ---- mbuf 池：按队列描述符、转交 ring 和 lcore 缓存计算 ----
  unsigned nb_mbufs = num_lcores * (RX_RING_SIZE + TX_RING_SIZE +
                                    REDIRECT_RING_SIZE + BURST_SIZE * 2) +
                      num_lcores * MBUF_CACHE_SIZE * 2 + 4096;
  g_dp.pool = rte_pktmbuf_pool_create("MBUF_POOL", nb_mbufs, MBUF_CACHE_SIZE, 0,
                                      RTE_MBUF_DEFAULT_BUF_SIZE, g_dp.socket_id);
  if (!g_dp.pool)
    return fail("Failed to create mbuf pool");
  LOG_INFO("mbuf pool: %u mbufs on socket %d", nb_mbufs, g_dp.socket_id);

  // ---- 端口 + steering ----
  PortSetup ps;
  if (port_init(g_dp.port_id, g_dp.pool, num_lcores,
                g_dp.cfg.force_sw_steering, g_dp.steering, ps) != 0)
    return fail("Failed to initialize port");
  // 每个 lcore 必须独占一组 RX/TX 队列：rx_burst / tx_burst 对同一队列不是线程安全的
  if (ps.num_queues < num_lcores) {
    LOG_FATAL("NIC supports only %u queues but %u lcores were given; "
              "use at most %u lcores (EAL -l option)",
              ps.num_queues, num_lcores, ps.num_queues);
    rte_eth_dev_stop(g_dp.port_id);
    rte_eth_dev_close(g_dp.port_id);
    rte_eal_cleanup();
    return 1;
  }
  g_dp.tx_offloads = ps.tx_offloads;
  g_dp.mtu = ps.mtu;
  g_dp.local_mac = ps.mac;
  if (!mac_is_zero(g_dp.cfg.vip_mac) && g_dp.cfg.vip_mac != ps.mac) {
    LOG_WARN("configured vip_mac %s differs from NIC MAC %s; using the "
             "configured one",
             mac_to_string(g_dp.cfg.vip_mac).c_str(),
             mac_to_string(ps.mac).c_str());
    g_dp.local_mac = g_dp.cfg.vip_mac;
  }

  // ---- RCU / 路由 / 邻居表 / 快照 ----
  size_t qsz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
  g_dp.qsbr = static_cast<struct rte_rcu_qsbr *>(
      rte_zmalloc("qsbr", qsz, RTE_CACHE_LINE_SIZE));
  if (!g_dp.qsbr || rte_rcu_qsbr_init(g_dp.qsbr, RTE_MAX_LCORE) != 0)
    return fail("Failed to init RCU");

  IPv4Addr route_src = !g_dp.cfg.local_ips.empty() ? g_dp.cfg.local_ips[0]
                                                   : g_dp.cfg.services[0].vip;
  g_dp.route.init(route_src, g_dp.cfg.netmask, g_dp.cfg.gateway);

  if (!g_dp.neigh.init(g_dp.qsbr, g_dp.socket_id))
    return fail("Failed to init neighbor table");
  for (const auto &svc : g_dp.cfg.services)
    for (const auto &rs : svc.rs)
      if (!mac_is_zero(rs.mac))
        g_dp.neigh.add_static(rs.ip, rs.mac);

  g_dp.master_ring = l4lb_ring_create_elem(
      "master_ev", sizeof(MasterEvent), MASTER_RING_SIZE, g_dp.socket_id,
      RING_F_SC_DEQ);
  g_dp.health_ring = l4lb_ring_create_elem(
      "health_ev", sizeof(HealthEvent), 1024, g_dp.socket_id,
      RING_F_SP_ENQ | RING_F_SC_DEQ);
  if (!g_dp.master_ring || !g_dp.health_ring)
    return fail("Failed to create rings");

  g_dp.snapshots.init(g_dp.cfg, g_dp.qsbr);

  // ---- worker 上下文：worker 下标 = 队列号，main lcore 为 worker 0（master）----
  g_dp.num_workers = num_lcores;
  uint32_t per_worker = g_dp.cfg.max_sessions / num_lcores;
  if (per_worker < 1024)
    per_worker = 1024;
  std::vector<unsigned> lcores{rte_get_main_lcore()};
  unsigned lc;
  RTE_LCORE_FOREACH_WORKER(lc) { lcores.push_back(lc); }
  for (uint16_t i = 0; i < num_lcores; ++i) {
    void *mem = rte_zmalloc_socket("worker", sizeof(WorkerCtx),
                                   RTE_CACHE_LINE_SIZE, g_dp.socket_id);
    if (!mem)
      return fail("Failed to allocate worker context");
    auto *w = new (mem) WorkerCtx();
    w->idx = i;
    w->lcore_id = lcores[i];
    w->lip_cursor.assign(g_dp.cfg.local_ips.size(), 0);
    char name[32];
    snprintf(name, sizeof(name), "sess_%u", i);
    if (!w->sessions.init(name, per_worker, g_dp.socket_id, 1))
      return fail("Failed to create session table");
    snprintf(name, sizeof(name), "redirect_%u", i);
    w->redirect_ring = rte_ring_create(name, REDIRECT_RING_SIZE,
                                       g_dp.socket_id, RING_F_SC_DEQ);
    if (!w->redirect_ring)
      return fail("Failed to create redirect ring");
    g_dp.workers[i] = w;
  }
  LOG_INFO("%u workers, %u sessions per worker", num_lcores, per_worker);

  ControlServer ctl;
  if (!g_dp.cfg.control_socket.empty() && !ctl.start(g_dp.cfg.control_socket))
    LOG_WARN("control socket disabled");

  LOG_INFO("========================================================");
  LOG_INFO("L4 Load Balancer is running! mode=%s",
           g_dp.cfg.mode == ForwardMode::NAT ? "FULLNAT" : "DR");
  LOG_INFO("========================================================");

  for (uint16_t i = 1; i < num_lcores; ++i) {
    LOG_INFO("Launching worker on lcore %u, queue %u", g_dp.workers[i]->lcore_id,
             i);
    rte_eal_remote_launch(worker_loop, g_dp.workers[i],
                          g_dp.workers[i]->lcore_id);
  }
  LOG_INFO("Master lcore %u running on queue 0", g_dp.workers[0]->lcore_id);
  worker_loop(g_dp.workers[0]);
  rte_eal_mp_wait_lcore();

  if (g_signal_received)
    LOG_INFO("Received signal %d, shutting down...",
             static_cast<int>(g_signal_received));
  ctl.stop();

  // ---- 退出统计 ----
  StatsTotal t = stats_total();
  LOG_INFO("========================================================");
  LOG_INFO("L4 Load Balancer stopped");
  LOG_INFO("Final Statistics:");
  LOG_INFO("  DPDK RX: %lu, TX: %lu", t[ST_RX], t[ST_TX]);
  LOG_INFO("  DPDK Dropped: %lu", t.drops());
  LOG_INFO("  LB Forwarded: %lu, Dropped: %lu", t[ST_FWD_IN] + t[ST_FWD_OUT],
           t.drops());
  LOG_INFO("  Total Sessions: %lu, Active: %lu", t[ST_SESS_NEW],
           sessions_active(t));
  LOG_INFO("  Session create fail: %lu, replaced: 0, cleaned: %lu",
           t[ST_DROP_TABLE_FULL] + t[ST_DROP_NO_PORT],
           t[ST_SESS_EXPIRED] + t[ST_SESS_CLOSED]);
  LOG_INFO("  Redirect out: %lu, RSS mismatch: %lu", t[ST_REDIRECT_OUT],
           t[ST_RSS_MISMATCH]);
  for (unsigned i = ST_DROP_MALFORMED; i <= ST_DROP_OTHER; ++i)
    if (t.c[i])
      LOG_INFO("  %s: %lu", stat_name(static_cast<Stat>(i)), t.c[i]);
  LOG_INFO("========================================================");

  // ---- 清理 ----
  for (uint16_t i = 0; i < num_lcores; ++i) {
    WorkerCtx *w = g_dp.workers[i];
    w->sessions.destroy();
    rte_ring_free(w->redirect_ring);
    w->~WorkerCtx();
    rte_free(w);
  }
  g_dp.snapshots.destroy();
  g_dp.neigh.destroy();
  rte_eth_dev_stop(g_dp.port_id);
  rte_eth_dev_close(g_dp.port_id);
  rte_ring_free(g_dp.master_ring);
  rte_ring_free(g_dp.health_ring);
  rte_free(g_dp.qsbr);
  rte_eal_cleanup();
  return 0;
}
