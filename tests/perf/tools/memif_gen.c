/*
 * memif_gen：l4lb 单核转发能力测试用的发包器（docs/压测方案.md 第十一节）
 *
 * 通过 DPDK net_memif（共享内存）直接和 l4lb 交换报文，绕开网卡、虚拟交换机和
 * 内核协议栈，测的是 l4lb 代码本身的处理能力：
 *
 *   发包核：构造 Client -> VIP 的 UDP 包（N 条流，源 IP/端口不同），按速率发出
 *   收包核：l4lb 转给 RS 的包（目的 MAC 是 RS）原路回射：交换 MAC/IP/端口，
 *           相当于 RS 回包，l4lb 再做 FULLNAT 回程转发给 Client；
 *           发给 Client 的包（目的 MAC 是本机）计数后丢弃
 *
 * 每个 Client 包在 l4lb 上产生 2 次转发（入站 + 回程）。
 *
 * memif 的 MAC 不指定时每次启动随机生成；l4lb 的会话会记住首包的源 MAC，
 * 所以每次运行都要用同一个 mac= 参数，否则上一轮留下的会话会把包发给旧 MAC。
 *
 * 用法（EAL 参数之后，-- 之后为本程序参数）：
 *   memif_gen -l 8-9 --file-prefix gen --no-pci \
 *     --vdev=net_memif0,role=client,socket=/tmp/x.sock,mac=02:00:00:00:00:99 -- \
 *     --vip 10.0.0.1 --port 9 --flows 10000 --rate 0 --duration 10
 *
 *   --vip/--port    VIP 和服务端口（UDP）
 *   --lb-mac        l4lb 的 MAC（memif server 的 mac 参数），默认 02:00:00:00:00:01
 *   --flows N       并发流（会话）数，默认 10000
 *   --rate PPS      发包速率，0 表示尽力发送，默认 0
 *   --size N        帧长（不含 FCS），默认 60（加 FCS 即 64 字节小包）
 *   --duration S    发送时长（秒），默认 10
 *   --no-reflect    不回射（只测入站方向）
 *   --rxq / --txq   队列数，必须等于 l4lb 的 TX / RX 队列数（memif_bench.sh 自动计算）
 */
#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>

#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>
#include <rte_udp.h>

#define BURST 32
#define NB_DESC 1024

static volatile int g_stop;
static uint16_t g_port;
static struct rte_mempool *g_pool;
static struct rte_ether_addr g_lb_mac = {{0x02, 0, 0, 0, 0, 0x01}};
static struct rte_ether_addr g_my_mac;
static uint32_t g_vip;      /* 网络字节序 */
static uint16_t g_vport;    /* 网络字节序 */
static uint32_t g_flows = 10000;
static uint64_t g_rate;     /* 0 = 尽力 */
static uint32_t g_size = 60;
static uint32_t g_duration = 10;
static int g_reflect = 1;
static uint16_t g_nb_rxq = 2, g_nb_txq = 2;

/* 每个线程只写自己的计数，main 读取 */
static struct {
  uint64_t tx, tx_fail;
} __rte_cache_aligned g_txs;
static struct {
  uint64_t rx, reflected, reflect_fail, to_client, other;
} __rte_cache_aligned g_rxs;

static void on_signal(int sig) {
  (void)sig;
  g_stop = 1;
}

/* 流 i -> 客户端 10.1.x.y:port（与 VIP 同在 10.0.0.0/8，l4lb 直接用首包源 MAC 回包） */
static inline void flow_addr(uint32_t i, uint32_t *ip, uint16_t *port) {
  *ip = rte_cpu_to_be_32((10u << 24) | (1u << 16) | ((i / 50000) & 0xffff));
  *port = rte_cpu_to_be_16((uint16_t)(1024 + i % 50000));
}

static void build(struct rte_mbuf *m, uint32_t flow) {
  uint8_t *p = (uint8_t *)rte_pktmbuf_append(m, g_size);
  struct rte_ether_hdr *eth = (struct rte_ether_hdr *)p;
  struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
  struct rte_udp_hdr *udp = (struct rte_udp_hdr *)(ip + 1);
  uint32_t sip;
  uint16_t sport;

  flow_addr(flow, &sip, &sport);
  rte_ether_addr_copy(&g_lb_mac, &eth->dst_addr);
  rte_ether_addr_copy(&g_my_mac, &eth->src_addr);
  eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
  memset(ip, 0, sizeof(*ip));
  ip->version_ihl = RTE_IPV4_VHL_DEF;
  ip->time_to_live = 64;
  ip->next_proto_id = IPPROTO_UDP;
  ip->total_length = rte_cpu_to_be_16((uint16_t)(g_size - sizeof(*eth)));
  ip->src_addr = sip;
  ip->dst_addr = g_vip;
  ip->hdr_checksum = rte_ipv4_cksum(ip);
  udp->src_port = sport;
  udp->dst_port = g_vport;
  udp->dgram_len =
      rte_cpu_to_be_16((uint16_t)(g_size - sizeof(*eth) - sizeof(*ip)));
  udp->dgram_cksum = 0; /* UDP 校验和 0 = 不校验，l4lb 改写后保持为 0 */
}

static int tx_loop(void *arg) {
  (void)arg;
  struct rte_mbuf *pkts[BURST];
  uint32_t flow = 0;
  const uint64_t hz = rte_get_tsc_hz();
  const uint64_t start = rte_rdtsc();
  const uint64_t end = start + (uint64_t)g_duration * hz;
  uint64_t sent = 0;

  while (!g_stop) {
    uint64_t now = rte_rdtsc();
    if (now >= end)
      break;
    /* 限速：按目标速率，到点才发下一批 */
    if (g_rate && sent * hz > (now - start) * g_rate)
      continue;
    if (rte_pktmbuf_alloc_bulk(g_pool, pkts, BURST) != 0)
      continue;
    for (int i = 0; i < BURST; ++i) {
      build(pkts[i], flow);
      if (++flow >= g_flows)
        flow = 0;
    }
    uint16_t n = rte_eth_tx_burst(g_port, 0, pkts, BURST);
    if (n < BURST) {
      rte_pktmbuf_free_bulk(pkts + n, BURST - n);
      g_txs.tx_fail += BURST - n;
    }
    g_txs.tx += n;
    sent += BURST;
  }
  return 0;
}

static inline int is_rs_mac(const struct rte_ether_addr *a) {
  static const uint8_t prefix[5] = {0x02, 0, 0, 0, 0};
  return memcmp(a->addr_bytes, prefix, 5) == 0 && (a->addr_bytes[5] & 0xf0) == 0x10;
}

/* 收包：发给 RS 的包回射，发给 Client 的包计数丢弃 */
static int rx_loop(void *arg) {
  (void)arg;
  struct rte_mbuf *pkts[BURST], *back[BURST];
  const uint16_t txq = 1; /* 回射包走队列 1，发包核独占队列 0 */

  while (!g_stop) {
    for (uint16_t q = 0; q < g_nb_rxq; ++q) {
      uint16_t n = rte_eth_rx_burst(g_port, q, pkts, BURST);
      uint16_t nb = 0;
      g_rxs.rx += n;
      for (uint16_t i = 0; i < n; ++i) {
        struct rte_mbuf *m = pkts[i];
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
        if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) ||
            rte_pktmbuf_data_len(m) < sizeof(*eth) + sizeof(struct rte_ipv4_hdr) +
                                          sizeof(struct rte_udp_hdr)) {
          g_rxs.other++; /* ARP、免费 ARP 等 */
          rte_pktmbuf_free(m);
          continue;
        }
        if (rte_is_same_ether_addr(&eth->dst_addr, &g_my_mac)) {
          g_rxs.to_client++; /* 完成一次往返 */
          rte_pktmbuf_free(m);
          continue;
        }
        /* 只回射发给 RS 的包（RS 的 MAC 是 02:00:00:00:00:1x，见 memif_bench.sh 的配置） */
        if (!g_reflect || !is_rs_mac(&eth->dst_addr)) {
          g_rxs.other++;
          rte_pktmbuf_free(m);
          continue;
        }
        /* 模拟 RS 回包：交换 MAC / IP / 端口（交换不改变 IP 校验和） */
        struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
        struct rte_udp_hdr *udp =
            (struct rte_udp_hdr *)((uint8_t *)ip + rte_ipv4_hdr_len(ip));
        struct rte_ether_addr tmac = eth->dst_addr;
        eth->dst_addr = eth->src_addr;
        eth->src_addr = tmac;
        uint32_t tip = ip->src_addr;
        ip->src_addr = ip->dst_addr;
        ip->dst_addr = tip;
        ip->time_to_live = 64;
        ip->hdr_checksum = 0;
        ip->hdr_checksum = rte_ipv4_cksum(ip);
        uint16_t tp = udp->src_port;
        udp->src_port = udp->dst_port;
        udp->dst_port = tp;
        m->ol_flags = 0;
        back[nb++] = m;
      }
      if (nb) {
        uint16_t s = rte_eth_tx_burst(g_port, txq, back, nb);
        g_rxs.reflected += s;
        if (s < nb) {
          g_rxs.reflect_fail += nb - s;
          rte_pktmbuf_free_bulk(back + s, nb - s);
        }
      }
    }
  }
  return 0;
}

static int parse_args(int argc, char **argv) {
  static const struct option opts[] = {
      {"vip", 1, 0, 'v'},      {"port", 1, 0, 'p'},     {"lb-mac", 1, 0, 'm'},
      {"flows", 1, 0, 'f'},    {"rate", 1, 0, 'r'},     {"size", 1, 0, 's'},
      {"duration", 1, 0, 'd'}, {"no-reflect", 0, 0, 'n'}, {"rxq", 1, 0, 'R'},
      {"txq", 1, 0, 'T'},      {0, 0, 0, 0}};
  int c;
  struct in_addr a;
  g_vport = rte_cpu_to_be_16(9);
  inet_pton(AF_INET, "10.0.0.1", &a);
  g_vip = a.s_addr;
  while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
    switch (c) {
    case 'v':
      if (inet_pton(AF_INET, optarg, &a) != 1)
        return -1;
      g_vip = a.s_addr;
      break;
    case 'p': g_vport = rte_cpu_to_be_16((uint16_t)atoi(optarg)); break;
    case 'm':
      if (rte_ether_unformat_addr(optarg, &g_lb_mac) != 0)
        return -1;
      break;
    case 'f': g_flows = (uint32_t)strtoul(optarg, NULL, 10); break;
    case 'r': g_rate = strtoull(optarg, NULL, 10); break;
    case 's': g_size = (uint32_t)strtoul(optarg, NULL, 10); break;
    case 'd': g_duration = (uint32_t)strtoul(optarg, NULL, 10); break;
    case 'n': g_reflect = 0; break;
    case 'R': g_nb_rxq = (uint16_t)atoi(optarg); break;
    case 'T': g_nb_txq = (uint16_t)atoi(optarg); break;
    default: return -1;
    }
  }
  if (g_flows == 0 || g_size < 60 || g_size > 1514 || g_nb_rxq < 1 || g_nb_txq < 2)
    return -1;
  return 0;
}

int main(int argc, char **argv) {
  int ret = rte_eal_init(argc, argv);
  if (ret < 0)
    rte_exit(EXIT_FAILURE, "EAL init failed\n");
  argc -= ret;
  argv += ret;
  if (parse_args(argc, argv) != 0)
    rte_exit(EXIT_FAILURE, "bad arguments, see the header of memif_gen.c\n");
  if (rte_lcore_count() < 3)
    rte_exit(EXIT_FAILURE, "need 3 lcores: main + tx + rx (e.g. -l 7-9)\n");
  if (rte_eth_dev_count_avail() < 1)
    rte_exit(EXIT_FAILURE, "no port (add --vdev=net_memif0,role=client,...)\n");
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);

  g_port = 0;
  g_pool = rte_pktmbuf_pool_create("gen_pool", 65535, 512, 0,
                                   RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
  if (!g_pool)
    rte_exit(EXIT_FAILURE, "mbuf pool failed\n");

  /*
   * 队列数必须与 l4lb 对应：本端 RX 数 = l4lb TX 数（worker 数 + 1），
   * 本端 TX 数 = l4lb RX 数。memif 两端的 ring 数不一致时，多出来的队列会访问
   * 不存在的 ring。发包核用 TX 队列 0，回射用 TX 队列 1（tx_burst 不能多线程共用一个队列）。
   */
  struct rte_eth_conf conf;
  memset(&conf, 0, sizeof(conf));
  if (rte_eth_dev_configure(g_port, g_nb_rxq, g_nb_txq, &conf) != 0)
    rte_exit(EXIT_FAILURE, "configure failed\n");
  for (uint16_t q = 0; q < g_nb_rxq; ++q)
    if (rte_eth_rx_queue_setup(g_port, q, NB_DESC, rte_socket_id(), NULL, g_pool))
      rte_exit(EXIT_FAILURE, "rx queue setup failed\n");
  for (uint16_t q = 0; q < g_nb_txq; ++q)
    if (rte_eth_tx_queue_setup(g_port, q, NB_DESC, rte_socket_id(), NULL))
      rte_exit(EXIT_FAILURE, "tx queue setup failed\n");
  if (rte_eth_dev_start(g_port) != 0)
    rte_exit(EXIT_FAILURE, "start failed\n");
  rte_eth_macaddr_get(g_port, &g_my_mac);

  /* 等 memif 连上 l4lb */
  struct rte_eth_link link;
  for (int i = 0; i < 100; ++i) {
    if (rte_eth_link_get_nowait(g_port, &link) == 0 && link.link_status)
      break;
    rte_delay_ms(100);
  }
  if (!link.link_status)
    rte_exit(EXIT_FAILURE, "memif link down: is l4lb running as memif server?\n");

  printf("memif_gen: flows %u, rate %s, frame %u B (+FCS), %u s, reflect %s\n",
         g_flows, g_rate ? "limited" : "max", g_size, g_duration,
         g_reflect ? "on" : "off");
  if (g_rate)
    printf("memif_gen: target %" PRIu64 " pps\n", g_rate);
  fflush(stdout);

  unsigned lc_tx = rte_get_next_lcore(-1, 1, 0);
  unsigned lc_rx = rte_get_next_lcore(lc_tx, 1, 0);
  rte_eal_remote_launch(tx_loop, NULL, lc_tx);
  rte_eal_remote_launch(rx_loop, NULL, lc_rx);

  const uint64_t hz = rte_get_tsc_hz();
  const uint64_t t0 = rte_rdtsc();
  rte_eal_wait_lcore(lc_tx); /* 发包结束 */
  const double sec = (double)(rte_rdtsc() - t0) / hz;
  rte_delay_ms(300);         /* 等在途的包回来 */
  g_stop = 1;
  rte_eal_wait_lcore(lc_rx);
  /* 机器可读的结果行，tests/perf/memif_bench.sh 解析 */
  printf("RESULT tx=%" PRIu64 " tx_fail=%" PRIu64 " rx=%" PRIu64
         " reflected=%" PRIu64 " reflect_fail=%" PRIu64 " to_client=%" PRIu64
         " other=%" PRIu64 " sec=%.2f tx_pps=%.0f client_pps=%.0f\n",
         g_txs.tx, g_txs.tx_fail, g_rxs.rx, g_rxs.reflected,
         g_rxs.reflect_fail, g_rxs.to_client, g_rxs.other, sec,
         g_txs.tx / sec, g_rxs.to_client / sec);

  rte_eth_dev_stop(g_port);
  rte_eth_dev_close(g_port);
  rte_eal_cleanup();
  return 0;
}
