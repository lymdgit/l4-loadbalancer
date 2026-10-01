/**
 * @file main.cpp
 * @brief L4 负载均衡器主程序 - 纯 DPDK 实现
 *
 * 直接使用 DPDK 进行数据包处理，不依赖 F-Stack：
 * 1. 使用 DPDK 收发数据包
 * 2. 解析 IP/TCP/UDP 头部
 * 3. 使用一致性哈希选择后端
 * 4. NAT 模式修改数据包头部
 * 5. 直接转发
 *
 * 本文件只负责：参数解析 -> EAL/端口/模块初始化 -> 启动 worker -> 退出清理。
 * - 端口初始化：src/dataplane/port.cpp
 * - worker 循环：src/dataplane/worker.cpp
 * - 报文处理：  src/core/loadbalancer.cpp
 *
 * @author L4 Load Balancer Project
 */

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rte_eal.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "common/config.h"
#include "common/logger.h"
#include "common/types.h"
#include "core/loadbalancer.h"
#include "dataplane/context.h"
#include "dataplane/port.h"
#include "dataplane/worker.h"
#include "lb/real_server.h"
#include "lb/session.h"

using namespace l4lb;

// ============================================================================
// 信号处理
// ============================================================================
static void signal_handler(int sig) {
  (void)sig;
  LOG_INFO("Received signal %d, shutting down...", sig);
  g_running = false;
}

// ============================================================================
// 主函数
// ============================================================================
int main(int argc, char *argv[]) {
  std::string config_file = "config/lb.conf";
  std::string log_level = "info";

  // 查找并提取 LB 特定参数
  std::vector<char *> dpdk_argv;
  dpdk_argv.push_back(argv[0]);

  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--lb-config") == 0 && i + 1 < argc) {
      config_file = argv[i + 1];
      ++i; // 跳过下一个参数
    } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
      log_level = argv[i + 1];
      ++i;
    } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      g_port_id = static_cast<uint16_t>(atoi(argv[i + 1]));
      ++i;
    } else if (strcmp(argv[i], "--help-lb") == 0) {
      printf("L4 Load Balancer (Pure DPDK) - High Performance L4 LB\n");
      printf("=====================================================\n\n");
      printf("Usage: %s [DPDK EAL options] -- [LB options]\n\n", argv[0]);
      printf("LB Options (after --):\n");
      printf("  --lb-config <file>   Load balancer config file (default: "
             "config/lb.conf)\n");
      printf("  --log <level>        Log level: debug/info/warn/error "
             "(default: info)\n");
      printf("  --port <id>          DPDK port ID to use (default: 0)\n");
      printf("  --help-lb            Show this help\n\n");
      printf("Example:\n");
      printf("  %s -l 0-1 -n 4 -- --lb-config config/lb.conf --log info\n\n",
             argv[0]);
      return 0;
    } else {
      // DPDK 参数
      dpdk_argv.push_back(argv[i]);
    }
  }

  // 设置日志级别
  Logger::instance().set_level(log_level);

  // 注册信号处理
  signal(SIGINT, signal_handler);
  signal(SIGTERM, signal_handler);

  LOG_INFO("========================================================");
  LOG_INFO("   L4 Load Balancer (Pure DPDK) Starting...");
  LOG_INFO("========================================================");
  LOG_INFO("Config: %s", config_file.c_str());
  LOG_INFO("Mode: TRUE L4 (NAT packet forwarding, no TCP stack)");
  LOG_INFO("========================================================");

  // 初始化 DPDK EAL
  LOG_INFO("Initializing DPDK EAL...");
  int ret = rte_eal_init(static_cast<int>(dpdk_argv.size()), dpdk_argv.data());
  if (ret < 0) {
    LOG_FATAL("Failed to initialize DPDK EAL: %s", rte_strerror(-ret));
    return 1;
  }
  LOG_INFO("DPDK EAL initialized");

  // 检查可用端口
  uint16_t nb_ports = rte_eth_dev_count_avail();
  if (nb_ports == 0) {
    LOG_FATAL("No Ethernet ports available");
    rte_eal_cleanup();
    return 1;
  }
  LOG_INFO("Found %u available ports", nb_ports);

  if (g_port_id >= nb_ports) {
    LOG_FATAL("Port %u not available (max: %u)", g_port_id, nb_ports - 1);
    rte_eal_cleanup();
    return 1;
  }

  // 创建 mbuf 内存池
  LOG_INFO("Creating mbuf pool...");
  // 对这个内存池创建函数进行详细说明
  // @param 1 : 内存池的名称
  // @param 2 : 池中mbuf的总数
  // @param 3 : 每个核心的本地缓存数量
  // @param 4 : 每个mbuf私有区的长度
  // @param 5 : 单个mbuf数据区的大小，默认大小为2048 + 预留头部
  // @param 6 : cpu和内存绑定同一个节点，防止NUMA
  g_mbuf_pool =
      rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (g_mbuf_pool == nullptr) {
    LOG_FATAL("Failed to create mbuf pool: %s", rte_strerror(rte_errno));
    rte_eal_cleanup();
    return 1;
  }

  // 获取可用的 lcore 数量，作为队列数量
  uint16_t num_lcores = rte_lcore_count();
  LOG_INFO("Detected %u lcores, will use RSS with %u queues", num_lcores,
           num_lcores);

  // 初始化端口 (使用 lcore 数量作为队列数)
  LOG_INFO("Initializing port %u with %u queues...", g_port_id, num_lcores);
  if (port_init(g_port_id, g_mbuf_pool, num_lcores) != 0) {
    LOG_FATAL("Failed to initialize port %u", g_port_id);
    rte_eal_cleanup();
    return 1;
  }
  LOG_INFO("Port %u initialized with %u queues", g_port_id, g_num_queues);

  // 初始化 SessionManager 反向哈希表（必须在 EAL 之后，LoadBalancer 之前）
  if (!SessionManager::instance().init()) {
    LOG_FATAL("Failed to initialize SessionManager reverse hash table");
    rte_eal_cleanup();
    return 1;
  }
  LOG_INFO(
      "TX offloads enabled: 0x%lx (HW IP=%s, HW TCP=%s, HW UDP=%s)",
      g_tx_offloads_enabled,
      (g_tx_offloads_enabled & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) ? "on" : "off",
      (g_tx_offloads_enabled & RTE_ETH_TX_OFFLOAD_TCP_CKSUM) ? "on" : "off",
      (g_tx_offloads_enabled & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) ? "on" : "off");

  // 初始化负载均衡器
  LOG_INFO("Initializing Load Balancer...");
  g_lb.set_tx_offload_caps(g_tx_offloads_enabled);
  if (!g_lb.init(config_file)) {
    LOG_FATAL("Failed to initialize Load Balancer");
    rte_eal_cleanup();
    return 1;
  }

  // 打印后端服务器信息
  auto &rs_mgr = RealServerManager::instance();
  LOG_INFO("Backend servers:");
  auto all_servers = rs_mgr.get_all_servers();
  for (const auto &rs : all_servers) {
    LOG_INFO("  [%u] %s:%u weight=%u mac=%s", rs.id,
             ip_to_string(rs.ip).c_str(), rs.port, rs.weight,
             mac_to_string(rs.mac).c_str());
  }

  LOG_INFO("========================================================");
  LOG_INFO("L4 Load Balancer is running!");
  LOG_INFO("VIP: %s", ip_to_string(Config::instance().get_vip()).c_str());
  LOG_INFO("Mode: %s", Config::instance().get_forward_mode() == ForwardMode::NAT
                           ? "NAT"
                           : "DR");
  LOG_INFO("Using DPDK port: %u", g_port_id);
  LOG_INFO("RSS Queues: %u (multi-core enabled)", g_num_queues);
  LOG_INFO("NO TCP STACK - Pure packet forwarding!");
  LOG_INFO("========================================================");
  LOG_INFO("Press Ctrl+C to stop");
  LOG_INFO("========================================================");

  // 启动多核 worker
  // 为每个 lcore 分配 queue_id
  static uint16_t queue_ids[RTE_MAX_LCORE];
  uint16_t queue_id = 0;
  unsigned lcore_id;

  // 在所有 worker lcore 上启动 worker_loop
  RTE_LCORE_FOREACH_WORKER(lcore_id) {
    if (queue_id < g_num_queues) {
      queue_ids[lcore_id] = queue_id;
      LOG_INFO("Launching worker on lcore %u, queue %u", lcore_id, queue_id);
      rte_eal_remote_launch(worker_loop, &queue_ids[lcore_id], lcore_id);
      ++queue_id;
    }
  }

  // master lcore 也运行一个 worker (使用剩余的队列，或者队列 0)
  uint16_t master_queue = (queue_id < g_num_queues) ? queue_id : 0;
  queue_ids[rte_get_main_lcore()] = master_queue;
  LOG_INFO("Master lcore %u running on queue %u", rte_get_main_lcore(),
           master_queue);
  worker_loop(&queue_ids[rte_get_main_lcore()]);

  // 等待所有 worker 结束
  rte_eal_mp_wait_lcore();

  // 清理
  g_lb.stop();
  SessionManager::instance().cleanup();

  LOG_INFO("Stopping port %u...", g_port_id);
  rte_eth_dev_stop(g_port_id);
  rte_eth_dev_close(g_port_id);

  LOG_INFO("========================================================");
  LOG_INFO("L4 Load Balancer stopped");
  auto final_stats = g_lb.get_stats();
  auto final_sess = SessionManager::instance().get_stats();
  LOG_INFO("Final Statistics:");
  LOG_INFO("  DPDK RX: %lu, TX: %lu", g_stats_rx.load(), g_stats_tx.load());
  LOG_INFO("  LB Forwarded: %lu", final_stats.forwarded_packets);
  LOG_INFO("  Total Sessions: %lu", final_sess.total_sessions);
  LOG_INFO("========================================================");

  rte_eal_cleanup();

  return 0;
}
