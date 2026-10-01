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
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_set>

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

  /// 获取统计信息
  Statistics get_stats() const { return stats_; }

  void set_tx_offload_caps(uint64_t caps) { tx_offload_caps_ = caps; }

  /// 停止
  void stop() { running_ = false; }

  /// 主动发送 ARP 请求探测所有后端服务器
  void send_arp_probes(uint16_t port_id, struct rte_mempool *pool);

private:
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
                   size_t len, const PacketMeta &meta);

  /**
   * @brief 处理入站流量（DNAT）
   *
   * Client -> VIP:port  =>  Client -> RS:port
   */
  bool handle_inbound(EthernetHeader *eth, uint8_t *data, size_t len,
                      const PacketMeta &meta, void *mbuf);

  /**
   * @brief 处理返回流量（SNAT）
   *
   * RS:port -> Client  =>  VIP:port -> Client
   */
  bool handle_return(EthernetHeader *eth, uint8_t *data, size_t len,
                     const PacketMeta &meta, void *mbuf);

  /// 判断 IP 是否属于 Real Server
  bool is_from_realserver(IPv4Addr ip) const {
    return rs_ips_.find(ip) != rs_ips_.end();
  }

  /// 判断 IP 是否是 Real Server 的 IP（别名，语义更清晰）
  bool is_realserver_ip(IPv4Addr ip) const {
    return rs_ips_.find(ip) != rs_ips_.end();
  }

  std::atomic<bool> running_;
  IPv4Addr local_ip_;
  MacAddr local_mac_;
  std::unique_ptr<Forwarder> forwarder_; // 转发引擎（NAT / DR）
  // 方便判断是否是回程流量
  std::unordered_set<IPv4Addr> rs_ips_; // Real Server IP 集合
  bool is_nat_mode_ = false;
  uint64_t tx_offload_caps_ = 0;
  Statistics stats_{};
};

} // namespace l4lb

#endif // L4LB_CORE_LOADBALANCER_H
