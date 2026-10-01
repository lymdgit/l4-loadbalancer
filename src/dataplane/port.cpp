/**
 * @file port.cpp
 * @brief DPDK 端口初始化实现 (支持 RSS 多队列)
 */

#include "dataplane/port.h"

#include "common/logger.h"
#include "dataplane/context.h"

#include <cstring>

#include <rte_errno.h>
#include <rte_ethdev.h>
#include <rte_mempool.h>

namespace l4lb {

int port_init(uint16_t port, struct rte_mempool *mbuf_pool,
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

} // namespace l4lb
