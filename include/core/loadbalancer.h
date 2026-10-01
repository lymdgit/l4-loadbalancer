/**
 * @file loadbalancer.h
 * @brief 负载均衡器核心类 - 支持双向流量处理
 *
 * 实现见 src/core/loadbalancer.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CORE_LOADBALANCER_H
#define L4LB_CORE_LOADBALANCER_H

#include "common/types.h"
#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

#include <rte_common.h> // RTE_CACHE_LINE_SIZE
#include <rte_config.h> // RTE_MAX_LCORE

struct rte_mempool;

namespace l4lb {

class Forwarder;
struct EthernetHeader;
struct IPv4Header;

/**
 * @brief 负载均衡器核心类
 *
 * 真正的 L4 负载均衡器，在数据包级别工作：
 * - 入站流量 (Client -> VIP): DNAT 到后端服务器
 * - 出站流量 (RS -> Client): SNAT 源地址为 VIP
 */
class LoadBalancer {
public:
  LoadBalancer();
  ~LoadBalancer();

  /// 加载配置、初始化 RS 和转发引擎
  bool init(const std::string &config_file);

  /**
   * @brief 处理数据包（入口函数）
   *
   * @param mbuf DPDK mbuf 指针
   * @param data 数据包内容
   * @param len 数据包长度
   * @param should_send [out] 是否需要发送数据包（报文已被原地改写）
   * @return true 包被本模块处理
   * @return false 包不属于本模块，调用者应丢弃
   */
  bool process_packet(void *mbuf, uint8_t *data, size_t len,
                      bool &should_send);

  /// 汇总所有 lcore 的统计信息
  Statistics get_stats() const;

  void set_tx_offload_caps(uint64_t caps) { tx_offload_caps_ = caps; }

  /// 停止
  void stop() { running_ = false; }

  /**
   * @brief 主动发送 ARP 请求探测所有后端服务器
   * @param queue_id 调用方 lcore 自己的 TX 队列（TX 队列不能跨核共用）
   */
  void send_arp_probes(uint16_t port_id, uint16_t queue_id,
                       struct rte_mempool *pool);

private:
  /// per-lcore 统计，只由所属 lcore 写（见 common/stats.h）
  struct alignas(RTE_CACHE_LINE_SIZE) LcoreStats {
    std::atomic<uint64_t> rx_packets{0};
    std::atomic<uint64_t> tx_packets{0};
    std::atomic<uint64_t> dropped_packets{0};
    std::atomic<uint64_t> arp_packets{0};
    std::atomic<uint64_t> icmp_packets{0};
    std::atomic<uint64_t> tcp_packets{0};
    std::atomic<uint64_t> udp_packets{0};
    std::atomic<uint64_t> forwarded_packets{0};
    std::atomic<uint64_t> nat_translations{0};
  };

  LcoreStats &local_stats();

  /// 处理 ARP
  bool handle_arp(EthernetHeader *eth, uint8_t *data, size_t len);

  /**
   * @brief 处理 IPv4
   * @param should_send [out] 是否需要发送数据包
   */
  bool handle_ipv4(EthernetHeader *eth, uint8_t *data, size_t len, void *mbuf,
                   bool &should_send);

  /// 处理 ICMP（Ping 本机）
  bool handle_icmp(EthernetHeader *eth, IPv4Header *ip, uint8_t *data,
                   const PacketMeta &meta);

  /**
   * @brief 处理入站流量（DNAT）
   *
   * Client -> VIP:port  =>  Client -> RS:port
   */
  bool handle_inbound(uint8_t *data, size_t len, const PacketMeta &meta,
                      void *mbuf);

  /**
   * @brief 处理返回流量（SNAT）
   *
   * RS:port -> Client  =>  VIP:port -> Client
   */
  bool handle_return(uint8_t *data, size_t len, const PacketMeta &meta,
                     void *mbuf);

  /// 判断 IP 是否属于 Real Server
  bool is_from_realserver(IPv4Addr ip) const {
    return rs_ips_.find(ip) != rs_ips_.end();
  }

  /// 目的端口（网络字节序）是否是配置的 VIP 服务端口
  bool is_service_port(Port port_be) const;

  std::atomic<bool> running_;
  IPv4Addr local_ip_;
  MacAddr local_mac_;
  std::unique_ptr<Forwarder> forwarder_; // 转发引擎（NAT / DR）
  // 方便判断是否是回程流量
  std::unordered_set<IPv4Addr> rs_ips_; // Real Server IP 集合
  std::bitset<65536> service_ports_;    // VIP 服务端口（主机字节序下标）
  bool is_nat_mode_ = false;
  uint64_t tx_offload_caps_ = 0;
  std::array<LcoreStats, RTE_MAX_LCORE> stats_;
};

} // namespace l4lb

#endif // L4LB_CORE_LOADBALANCER_H
