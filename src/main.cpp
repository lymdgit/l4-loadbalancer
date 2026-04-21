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
 * 优势：
 * - 没有 F-Stack TCP 栈干扰，不会发送不必要的 RST
 * - 完全控制数据包流程
 * - 更简单、更高效
 *
 * @author L4 Load Balancer Project
 */

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>

#include "common/config.h"
#include "common/logger.h"
#include "common/types.h"
#include "core/loadbalancer.h"
#include "lb/real_server.h"
#include "lb/session.h"

using namespace l4lb;

// ============================================================================
// DPDK 配置
// ============================================================================
#define RX_RING_SIZE 2048
#define TX_RING_SIZE 4096   // 增大 TX 缓冲区，减少 DR 模式丢包
#define NUM_MBUFS 65535     // 增大内存池 (prev: 16383)
#define MBUF_CACHE_SIZE 512 // 每一个lcore缓存512个mbuf
#define BURST_SIZE 32       // 增大批处理，提升吞吐

// ============================================================================
// 全局变量
// ============================================================================
static volatile bool g_running = true;
static LoadBalancer g_lb; // 负载均衡相关配置：一致性hash
static uint64_t g_loop_count = 0;
static uint16_t g_port_id = 0; // 默认使用端口 0
static struct rte_mempool *g_mbuf_pool = nullptr;
static uint64_t g_tx_offloads_enabled = 0;

// ============================================================================
// Per-lcore 统计（替代全局 atomic，消除 cache line 竞争）
//
// 原来 g_stats_rx/tx/dropped 是全局 atomic，每次 rte_eth_rx_burst 后
// 所有 lcore 争抢同一 cache line（即使用 relaxed，仍需总线协议）。
//
// 新方案：每个 lcore 只写自己的槽，统计线程聚合时才读所有槽。
// alignas(RTE_CACHE_LINE_SIZE) 保证相邻槽不共享 cache line（false sharing）。
// ============================================================================
struct alignas(RTE_CACHE_LINE_SIZE) LcoreNicStats {
  uint64_t rx = 0;
  uint64_t tx = 0;
  uint64_t dropped = 0;
  // 3 × 8 = 24 字节，填充到 64（一个 cache line）
  uint8_t _pad[RTE_CACHE_LINE_SIZE - 3 * sizeof(uint64_t)];
};
static_assert(sizeof(LcoreNicStats) == RTE_CACHE_LINE_SIZE,
              "LcoreNicStats must be exactly one cache line");

static LcoreNicStats g_nic_stats[RTE_MAX_LCORE];

// 多队列配置
static uint16_t g_num_queues = 1;

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
                                   uint16_t queue) {
  if (buf->count == 0)
    return;

  uint16_t nb_tx = rte_eth_tx_burst(port, queue, buf->pkts, buf->count);

  // Per-lcore 统计，无需原子操作
  unsigned lid = rte_lcore_id();
  if (lid >= RTE_MAX_LCORE)
    lid = 0;
  g_nic_stats[lid].tx += nb_tx;

  if (unlikely(nb_tx < buf->count)) {
    g_nic_stats[lid].dropped += (buf->count - nb_tx);
    for (uint16_t i = nb_tx; i < buf->count; ++i) {
      rte_pktmbuf_free(buf->pkts[i]);
    }
  }
  buf->count = 0;
}

// 添加包到 TX buffer
static inline void tx_buffer_add(TxBuffer *buf, struct rte_mbuf *mbuf,
                                 uint16_t port, uint16_t queue) {
  buf->pkts[buf->count++] = mbuf; // 把当前mbuf指针存到数组里面，等待批量发送

  // Buffer 满了就发送
  if (buf->count >= TX_BATCH_SIZE) {
    tx_buffer_flush(buf, port, queue);
  }
}

// ============================================================================
// 信号处理
// ============================================================================
static void signal_handler(int sig) {
  (void)sig;
  LOG_INFO("Received signal %d, shutting down...", sig);
  g_running = false;
}

// ============================================================================
// DPDK 端口初始化 (支持 RSS 多队列)
// ============================================================================
static int port_init(uint16_t port, struct rte_mempool *mbuf_pool,
                     uint16_t num_queues) {
  struct rte_eth_conf port_conf;
  memset(&port_conf, 0, sizeof(port_conf));

  struct rte_eth_dev_info dev_info;
  int ret = rte_eth_dev_info_get(port, &dev_info);
  if (ret != 0) {
    LOG_ERROR("Error getting device info for port %u: %s", port,
              rte_strerror(-ret));
    return ret;
  }

  // 限制队列数量不超过硬件支持
  if (num_queues > dev_info.max_rx_queues) {
    LOG_WARN("Requested %u queues, but NIC supports max %u. Using %u.",
             num_queues, dev_info.max_rx_queues, dev_info.max_rx_queues);
    num_queues = dev_info.max_rx_queues;
  }
  if (num_queues > dev_info.max_tx_queues) {
    num_queues = dev_info.max_tx_queues;
  }
  g_num_queues = num_queues;

  // Enable checksum offloads if supported
  uint64_t wanted_tx_offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                                RTE_ETH_TX_OFFLOAD_UDP_CKSUM |
                                RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
  port_conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
  port_conf.txmode.offloads = wanted_tx_offloads & dev_info.tx_offload_capa;
  g_tx_offloads_enabled = port_conf.txmode.offloads;
  if (port_conf.txmode.offloads != wanted_tx_offloads) {
    LOG_WARN("TX offloads limited by NIC, wanted 0x%lx, using 0x%lx",
             wanted_tx_offloads, port_conf.txmode.offloads);
  }
  port_conf.rxmode.offloads =
      dev_info.rx_offload_capa & (RTE_ETH_RX_OFFLOAD_CHECKSUM);

  // 配置 RSS (如果多队列)
  if (num_queues > 1) {
    // 开启RSS多队列模式，让网卡按哈希把包分到多个 RX 队列
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    port_conf.rx_adv_conf.rss_conf.rss_key = NULL; // 使用默认 key
    // 让网卡使用IP + TCP/UDP 五元组去做hash
    // 同一个 TCP 连接的 5 元组不变 → 哈希值不变 → 会进同一个 RX 队列。
    port_conf.rx_adv_conf.rss_conf.rss_hf =
        RTE_ETH_RSS_IP | RTE_ETH_RSS_TCP | RTE_ETH_RSS_UDP;
    // 过滤掉网卡不支持的 RSS 类型
    port_conf.rx_adv_conf.rss_conf.rss_hf &= dev_info.flow_type_rss_offloads;
    LOG_INFO("Enabling RSS with %u queues, hash types: 0x%lx", num_queues,
             port_conf.rx_adv_conf.rss_conf.rss_hf);
  } else {
    port_conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
    LOG_INFO("Single queue mode (no RSS)");
  }

  // 配置端口
  ret = rte_eth_dev_configure(port, num_queues, num_queues, &port_conf);
  if (ret != 0) {
    LOG_ERROR("Error configuring port %u: %s", port, rte_strerror(-ret));
    return ret;
  }

#if defined(RTE_ETH_HASH_FUNCTION_SYMMETRIC_TOEPLITZ)
  if (num_queues > 1) {
    struct rte_eth_rss_conf rss_conf = port_conf.rx_adv_conf.rss_conf;
    rss_conf.hash_func = RTE_ETH_HASH_FUNCTION_SYMMETRIC_TOEPLITZ;
    ret = rte_eth_dev_rss_hash_update(port, &rss_conf);
    if (ret == 0) {
      LOG_INFO("RSS hash function: symmetric Toeplitz enabled");
    } else {
      LOG_WARN("Failed to enable symmetric RSS hash: %s", rte_strerror(-ret));
    }
  }
#endif

  // 调整 RX/TX 队列大小
  // 队列大小设置为2048个mbuf
  uint16_t nb_rx_desc = RX_RING_SIZE;
  uint16_t nb_tx_desc = TX_RING_SIZE;
  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rx_desc, &nb_tx_desc);
  if (ret != 0) {
    LOG_ERROR("Error adjusting descriptors: %s", rte_strerror(-ret));
    return ret;
  }

  // 设置每个 RX/TX 队列
  for (uint16_t q = 0; q < num_queues; q++) {
    ret = rte_eth_rx_queue_setup(
        port, q, nb_rx_desc, rte_eth_dev_socket_id(port), nullptr, mbuf_pool);
    if (ret < 0) {
      LOG_ERROR("Error setting up RX queue %u: %s", q, rte_strerror(-ret));
      return ret;
    }

    ret = rte_eth_tx_queue_setup(port, q, nb_tx_desc,
                                 rte_eth_dev_socket_id(port), nullptr);
    if (ret < 0) {
      LOG_ERROR("Error setting up TX queue %u: %s", q, rte_strerror(-ret));
      return ret;
    }
  }

  // 启动端口
  ret = rte_eth_dev_start(port);
  if (ret < 0) {
    LOG_ERROR("Error starting port: %s", rte_strerror(-ret));
    return ret;
  }

  // 启用混杂模式 (已禁用以提升性能)
  // ret = rte_eth_promiscuous_enable(port);
  // if (ret != 0) {
  //   LOG_WARN("Failed to enable promiscuous mode: %s", rte_strerror(-ret));
  // }

  // 获取 MAC 地址
  struct rte_ether_addr addr;
  ret = rte_eth_macaddr_get(port, &addr);
  if (ret == 0) {
    LOG_INFO("Port %u MAC: %02x:%02x:%02x:%02x:%02x:%02x", port,
             addr.addr_bytes[0], addr.addr_bytes[1], addr.addr_bytes[2],
             addr.addr_bytes[3], addr.addr_bytes[4], addr.addr_bytes[5]);
  }

  LOG_INFO("Port %u configured with %u RX/TX queues", port, num_queues);
  return 0;
}

// ============================================================================
// 处理单个数据包 (返回是否需要发送)
// ============================================================================
static inline struct rte_mbuf *process_packet_batch(struct rte_mbuf *mbuf) {
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
    return nullptr; // 不需要发送
  }
}

// ============================================================================
// Worker 循环 (每个 lcore 运行一个) - 批量发送优化版
// ============================================================================
static int worker_loop(void *arg) {
  uint16_t queue_id = *static_cast<uint16_t *>(arg);
  unsigned lcore_id = rte_lcore_id();

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

  while (g_running) {
    // -----------------------------------------------------------------------
    // 【热路径】核心业务：收包 + 转发，保持最高频执行，不在此处读时钟
    // -----------------------------------------------------------------------
    uint16_t nb_rx = rte_eth_rx_burst(g_port_id, queue_id, bufs, BURST_SIZE);

    if (nb_rx > 0) {
      // Per-lcore 统计，无原子操作
      g_nic_stats[lcore_id].rx += nb_rx;

      // 批量处理每个数据包
      for (uint16_t i = 0; i < nb_rx; ++i) {
        struct rte_mbuf *to_send = process_packet_batch(bufs[i]);
        if (to_send) {
          // 内联函数，只在调用处展开，没有函数调用开销
          tx_buffer_add(&tx_buf, to_send, g_port_id, queue_id);
        }
      }
      if (tx_buf.count > 0) {
        tx_buffer_flush(&tx_buf, g_port_id, queue_id);
        tx_buf.last_drain_tsc = cur_tsc; // 用缓存的 cur_tsc，避免再读时钟
      }
    }

    ++local_loop_count;

    // -----------------------------------------------------------------------
    // 【降频读表】每 1024 次循环才读一次硬件时钟（位运算，零额外开销）
    // 彻底消灭 __rdtsc 霸屏火焰图的问题
    // -----------------------------------------------------------------------
    if (unlikely((local_loop_count & 1023) == 0)) {
      cur_tsc = rte_get_tsc_cycles();

      // 定期刷新 TX buffer（超时未满也发送，避免延迟积压）
      if (tx_buf.count > 0 && (cur_tsc - tx_buf.last_drain_tsc) > drain_tsc) {
        tx_buffer_flush(&tx_buf, g_port_id, queue_id);
        tx_buf.last_drain_tsc = cur_tsc;
      }

      // 定期清理过期会话（每 500000 次循环 ≈ 每 512*1024 次循环检查一次）
      if ((local_loop_count & 524287) == 0) { // 524287 = 512*1024 - 1
        size_t cleaned = SessionManager::instance().cleanup_local(cur_tsc);
        if (cleaned > 0 && is_master) {
          LOG_INFO("Cleaned %zu expired sessions (local)", cleaned);
        }
      }

      // 定期打印统计信息 & 发送 ARP 探测（只有 master 执行）
      if (is_master && cur_tsc - last_stats_time >= stats_interval) {
        g_lb.send_arp_probes(g_port_id, g_mbuf_pool);

        auto stats = g_lb.get_stats();
        auto sess_stats = SessionManager::instance().get_stats();
        auto sess_dbg = SessionManager::instance().get_debug_stats();
        // 聚合所有 lcore 的 NIC 统计（仅控制面，不在热路径）
        uint64_t total_rx = 0, total_tx = 0, total_dropped = 0;
        for (unsigned i = 0; i < RTE_MAX_LCORE; ++i) {
          total_rx += g_nic_stats[i].rx;
          total_tx += g_nic_stats[i].tx;
          total_dropped += g_nic_stats[i].dropped;
        }

        LOG_INFO("=== L4 LB Statistics (RSS: %u queues, Batch TX) ===",
                 g_num_queues);
        LOG_INFO("DPDK RX: %lu, TX: %lu, Dropped: %lu", total_rx, total_tx,
                 total_dropped);
        LOG_INFO("LB RX: %lu, TX: %lu, Dropped: %lu", stats.rx_packets,
                 stats.tx_packets, stats.dropped_packets);
        LOG_INFO("ARP: %lu, ICMP: %lu, TCP: %lu, UDP: %lu", stats.arp_packets,
                 stats.icmp_packets, stats.tcp_packets, stats.udp_packets);
        LOG_INFO("Forwarded: %lu, NAT: %lu, Sessions: %lu",
                 stats.forwarded_packets, stats.nat_translations,
                 sess_stats.active_sessions);
        LOG_INFO("Sess dbg: lk hit %lu miss %lu | rev hit %lu miss %lu | "
                 "create %lu | upd miss %lu | cleanup %lu",
                 sess_dbg.lookup_hit, sess_dbg.lookup_miss,
                 sess_dbg.reverse_hit, sess_dbg.reverse_miss, sess_dbg.create,
                 sess_dbg.update_miss, sess_dbg.cleanup_removed);
        LOG_INFO("========================");

        last_stats_time = cur_tsc;
      }
    }
  }

  // 退出前刷新剩余的 TX buffer
  tx_buffer_flush(&tx_buf, g_port_id, queue_id);

  LOG_INFO("Worker on lcore %u exiting", lcore_id);
  return 0;
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
  uint64_t final_rx = 0, final_tx = 0, final_dropped = 0;
  for (unsigned i = 0; i < RTE_MAX_LCORE; ++i) {
    final_rx += g_nic_stats[i].rx;
    final_tx += g_nic_stats[i].tx;
    final_dropped += g_nic_stats[i].dropped;
  }
  LOG_INFO("  DPDK RX: %lu, TX: %lu, Dropped: %lu", final_rx, final_tx,
           final_dropped);
  LOG_INFO("  LB Forwarded: %lu", final_stats.forwarded_packets);
  LOG_INFO("  Total Sessions: %lu", final_sess.total_sessions);
  LOG_INFO("========================================================");

  rte_eal_cleanup();

  return 0;
}
