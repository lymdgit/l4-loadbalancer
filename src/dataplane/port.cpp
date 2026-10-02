/**
 * @file port.cpp
 * @brief DPDK 端口初始化实现
 */

#include "dataplane/port.h"

#include "common/logger.h"
#include "dataplane/context.h"
#include "dataplane/steering.h"

#include <cstring>
#include <vector>

#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>

namespace l4lb {

namespace {

/// 写入并读回 RETA：第 i 项指向队列 i % nq
bool setup_reta(uint16_t port, uint16_t reta_size, uint16_t nq,
                std::vector<uint16_t> &reta) {
  if (reta_size == 0 || reta_size > Steering::kMaxReta)
    return false;
  std::vector<struct rte_eth_rss_reta_entry64> conf(
      (reta_size + RTE_ETH_RETA_GROUP_SIZE - 1) / RTE_ETH_RETA_GROUP_SIZE);
  for (uint16_t i = 0; i < reta_size; ++i) {
    auto &e = conf[i / RTE_ETH_RETA_GROUP_SIZE];
    e.mask |= 1ULL << (i % RTE_ETH_RETA_GROUP_SIZE);
    e.reta[i % RTE_ETH_RETA_GROUP_SIZE] = i % nq;
  }
  int ret = rte_eth_dev_rss_reta_update(port, conf.data(), reta_size);
  if (ret != 0)
    LOG_WARN("RETA update failed (%s), reading the driver's table",
             rte_strerror(-ret));
  for (auto &e : conf) {
    e.mask = ~0ULL;
    memset(e.reta, 0, sizeof(e.reta));
  }
  ret = rte_eth_dev_rss_reta_query(port, conf.data(), reta_size);
  reta.resize(reta_size);
  for (uint16_t i = 0; i < reta_size; ++i)
    reta[i] = ret == 0 ? conf[i / RTE_ETH_RETA_GROUP_SIZE]
                             .reta[i % RTE_ETH_RETA_GROUP_SIZE]
                       : i % nq;
  if (ret != 0)
    LOG_WARN("RETA query failed (%s), assuming entry i -> queue i %% %u",
             rte_strerror(-ret), nq);
  return true;
}

void wait_link(uint16_t port) {
  struct rte_eth_link link;
  for (int i = 0; i < 50; ++i) {
    memset(&link, 0, sizeof(link));
    if (rte_eth_link_get_nowait(port, &link) == 0 &&
        link.link_status == RTE_ETH_LINK_UP) {
      LOG_INFO("Port %u link up, %u Mbps %s", port, link.link_speed,
               link.link_duplex == RTE_ETH_LINK_FULL_DUPLEX ? "full-duplex"
                                                            : "half-duplex");
      return;
    }
    rte_delay_ms(100);
  }
  LOG_WARN("Port %u link is still down after 5s, continuing", port);
}

} // namespace

int port_init(uint16_t port, struct rte_mempool *pool, uint16_t want_queues,
              bool force_sw, Steering &steering, PortSetup &out) {
  struct rte_eth_dev_info info;
  int ret = rte_eth_dev_info_get(port, &info);
  if (ret != 0) {
    LOG_ERROR("Error getting device info for port %u: %s", port,
              rte_strerror(-ret));
    return ret;
  }

  uint16_t nq = want_queues;
  if (nq > info.max_rx_queues)
    nq = info.max_rx_queues;
  if (nq > info.max_tx_queues)
    nq = info.max_tx_queues;
  out.num_queues = nq;

  struct rte_eth_conf conf;
  memset(&conf, 0, sizeof(conf));

  // TX：硬件校验和
  uint64_t want_tx = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                     RTE_ETH_TX_OFFLOAD_UDP_CKSUM | RTE_ETH_TX_OFFLOAD_TCP_CKSUM;
  conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
  conf.txmode.offloads = want_tx & info.tx_offload_capa;
  out.tx_offloads = conf.txmode.offloads;
  // RX：校验和检查（坏包在 processor 中丢弃）+ RSS hash 写入 mbuf
  conf.rxmode.offloads = info.rx_offload_capa & (RTE_ETH_RX_OFFLOAD_CHECKSUM |
                                                 RTE_ETH_RX_OFFLOAD_RSS_HASH);

  // RSS：显式设置 key，便于软件复现网卡的 hash
  static uint8_t rss_key[Steering::kRssKeyLen];
  memcpy(rss_key, Steering::kDefaultRssKey, sizeof(rss_key));
  uint64_t rss_hf = (RTE_ETH_RSS_IPV4 | RTE_ETH_RSS_NONFRAG_IPV4_TCP |
                     RTE_ETH_RSS_NONFRAG_IPV4_UDP) &
                    info.flow_type_rss_offloads;
  bool key_ok = info.hash_key_size == 0 ||
                info.hash_key_size == Steering::kRssKeyLen;
  bool use_rss = nq > 1 && !force_sw && (rss_hf & RTE_ETH_RSS_IPV4) && key_ok;
  if (use_rss) {
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_RSS;
    conf.rx_adv_conf.rss_conf.rss_key = rss_key;
    conf.rx_adv_conf.rss_conf.rss_key_len = Steering::kRssKeyLen;
    conf.rx_adv_conf.rss_conf.rss_hf = rss_hf;
  } else {
    conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
  }

  ret = rte_eth_dev_configure(port, nq, nq, &conf);
  if (ret != 0) {
    LOG_ERROR("Error configuring port %u: %s", port, rte_strerror(-ret));
    return ret;
  }

  uint16_t nb_rxd = RX_RING_SIZE, nb_txd = TX_RING_SIZE;
  ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &nb_rxd, &nb_txd);
  if (ret != 0) {
    LOG_ERROR("Error adjusting descriptors: %s", rte_strerror(-ret));
    return ret;
  }
  int socket = rte_eth_dev_socket_id(port);
  for (uint16_t q = 0; q < nq; ++q) {
    ret = rte_eth_rx_queue_setup(port, q, nb_rxd, socket, nullptr, pool);
    if (ret < 0) {
      LOG_ERROR("Error setting up RX queue %u: %s", q, rte_strerror(-ret));
      return ret;
    }
    ret = rte_eth_tx_queue_setup(port, q, nb_txd, socket, nullptr);
    if (ret < 0) {
      LOG_ERROR("Error setting up TX queue %u: %s", q, rte_strerror(-ret));
      return ret;
    }
  }

  ret = rte_eth_dev_start(port);
  if (ret < 0) {
    LOG_ERROR("Error starting port: %s", rte_strerror(-ret));
    return ret;
  }

  // steering：RSS 生效时 owner 与网卡分队列一致，否则软件分发
  if (use_rss) {
    // reta_size 在 configure 之后才有效
    if (rte_eth_dev_info_get(port, &info) != 0)
      info.reta_size = 0;
    std::vector<uint16_t> reta;
    if (!setup_reta(port, info.reta_size, nq, reta)) {
      reta.resize(nq);
      for (uint16_t i = 0; i < nq; ++i)
        reta[i] = i;
    }
    bool tcp_l4 = rss_hf & RTE_ETH_RSS_NONFRAG_IPV4_TCP;
    bool udp_l4 = rss_hf & RTE_ETH_RSS_NONFRAG_IPV4_UDP;
    steering.init_hw(nq, rss_key, reta.data(),
                     static_cast<uint16_t>(reta.size()), tcp_l4, udp_l4);
    LOG_INFO("RSS: %u queues, reta_size %zu, hash 0x%lx (tcp ports %s, udp "
             "ports %s)",
             nq, reta.size(), rss_hf, tcp_l4 ? "yes" : "no",
             udp_l4 ? "yes" : "no");
  } else {
    steering.init_sw(nq);
    LOG_INFO("%u queue(s), software steering%s", nq,
             nq > 1 ? " (NIC has no usable RSS)" : "");
  }

  wait_link(port);

  struct rte_ether_addr addr;
  if (rte_eth_macaddr_get(port, &addr) == 0)
    memcpy(out.mac.data(), addr.addr_bytes, 6);
  uint16_t mtu = 0;
  if (rte_eth_dev_get_mtu(port, &mtu) == 0 && mtu)
    out.mtu = mtu;
  LOG_INFO("Port %u MAC %s, MTU %u, TX offloads 0x%lx", port,
           mac_to_string(out.mac).c_str(), out.mtu, out.tx_offloads);
  return 0;
}

} // namespace l4lb
