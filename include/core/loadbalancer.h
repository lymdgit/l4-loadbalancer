/**
 * @file loadbalancer.h
 * @brief 负载均衡器核心类 - 支持双向流量处理
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CORE_LOADBALANCER_H
#define L4LB_CORE_LOADBALANCER_H

#include "common/config.h"
#include "common/logger.h"
#include "common/types.h"
#include "forward/dr_forwarder.h"
#include "forward/forwarder.h"
#include "forward/nat_forwarder.h"
#include "lb/real_server.h"
#include "lb/session.h"
#include "protocol/arp.h"
#include "protocol/ethernet.h"
#include "protocol/icmp.h"
#include "protocol/ip.h"
#include <atomic>
#include <memory>
#include <unordered_set>

// 在查询上确实不如maglev好，因为那是o1查询的
// 但在我这个项目中，只有第一次握手建联的首包时，才会去差一次
// 查询完后，后续所有的这个连接的包，都会直接查正向会话表，仍然是O1

namespace l4lb {

/**
 * @brief 负载均衡器核心类
 *
 * 真正的 L4 负载均衡器，在数据包级别工作：
 * - 入站流量 (Client -> VIP): DNAT 到后端服务器
 * - 出站流量 (RS -> Client): SNAT 源地址为 VIP
 */
class LoadBalancer {
public:
  LoadBalancer() : running_(false) {}

  /**
   * @brief 初始化
   */
  bool init(const std::string &config_file) {
    // 加载配置
    if (!Config::instance().load(config_file)) {
      LOG_ERROR("Failed to load config");
      return false;
    }
    // 生成一个config实例并打印相关信息
    auto &cfg = Config::instance();
    cfg.dump();

    // 初始化本机信息
    local_ip_ = cfg.get_vip();
    local_mac_ = cfg.get_vip_mac();

    // 初始化 Real Server
    if (!RealServerManager::instance().load_from_config()) {
      LOG_ERROR("Failed to load real servers");
      return false;
    }

    // 构建 RS IP 集合，用于快速判断返回流量
    auto &rs_mgr = RealServerManager::instance();
    auto all_servers = rs_mgr.get_all_servers();
    LOG_INFO("Building RS IP set for return traffic detection:");
    for (const auto &rs : all_servers) {
      rs_ips_.insert(rs.ip);
      LOG_INFO("  RS IP: %s", ip_to_string(rs.ip).c_str());
    }
    LOG_INFO("Total %zu RS IPs in set", rs_ips_.size());

    // 设置会话超时
    SessionManager::instance().set_timeout(cfg.get_session_timeout());

    // 创建转发引擎      工厂模式：将实例化的过程延迟到子类中进行
    if (cfg.get_forward_mode() == ForwardMode::NAT) {
      forwarder_ = std::make_unique<NatForwarder>(tx_offload_caps_);
      is_nat_mode_ = true;
      LOG_INFO("Using NAT forwarding mode");
    } else {
      forwarder_ = std::make_unique<DrForwarder>();
      is_nat_mode_ = false;
      LOG_INFO("Using DR forwarding mode");
    }

    running_ = true;
    LOG_INFO("LoadBalancer initialized");
    return true;
  }

  /**
   * @brief 处理数据包（入口函数）
   *
   * @param mbuf DPDK mbuf 指针（可为空）
   * @param data 数据包内容
   * @param len 数据包长度
   * @param should_send [out] 是否需要发送数据包
   * @return true 我们处理了这个包（调用者应返回 FF_DISPATCH_ERROR）
   * @return false 我们不处理这个包（调用者应让 F-Stack 继续处理）
   */
  bool process_packet(void *mbuf, uint8_t *data, size_t len,
                      bool &should_send) {
    should_send = false; // 默认不发送

    if (!running_)
      return false;

    ++stats_.rx_packets;

    // 解析以太网头
    auto *eth = Ethernet::parse_mutable(data, len);
    if (!eth) {
      ++stats_.dropped_packets;
      return false;
    }

    // ARP 处理
    if (eth->is_arp()) {
      should_send = handle_arp(eth, data, len);
      return should_send;
    }

    // IPv4 处理
    if (eth->is_ipv4()) {
      return handle_ipv4(eth, data, len, mbuf, should_send);
    }

    ++stats_.dropped_packets;
    return false;
  }

  /**
   * @brief 获取统计信息
   */
  Statistics get_stats() const { return stats_; }

  void set_tx_offload_caps(uint64_t caps) { tx_offload_caps_ = caps; }

  /**
   * @brief 停止
   */
  void stop() { running_ = false; }

  /**
   * @brief 主动发送 ARP 请求探测所有后端服务器
   */
  /**
   * @brief 主动发送 ARP 请求探测所有后端服务器
   */
  void send_arp_probes(uint16_t port_id, struct rte_mempool *pool) {
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) {
      LOG_ERROR("Failed to allocate mbuf for ARP probe");
      return;
    }

    uint8_t *data = rte_pktmbuf_mtod(mbuf, uint8_t *);

    // 遍历所有 RS 发送 ARP 请求
    auto all_servers = RealServerManager::instance().get_all_servers();
    for (const auto &rs : all_servers) {
      size_t pkt_len =
          ArpHandler::build_request(data, rs.ip, local_ip_, local_mac_);

      // 设置 mbuf 长度
      mbuf->data_len = pkt_len;
      mbuf->pkt_len = pkt_len;

      // 复制一份 mbuf (因为 burst 会消耗 mbuf) - 简单起见，这里直接重用 alloc
      // 注意：实际上不能直接重用已发送的
      // mbuf，所以这里只发一个做演示，或者在这个循环里每次都 alloc
      // 为了正确性，我们在循环里重新 alloc

      struct rte_mbuf *tx_mbuf = rte_pktmbuf_copy(mbuf, pool, 0, pkt_len);
      if (tx_mbuf) {
        uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &tx_mbuf, 1);
        if (nb_tx > 0) {
          LOG_INFO("Sent ARP probe to RS: %s", ip_to_string(rs.ip).c_str());
        } else {
          rte_pktmbuf_free(tx_mbuf);
        }
      }
    }
    rte_pktmbuf_free(mbuf);
  }

private:
  /**
   * @brief 处理 ARP
   */
  bool handle_arp(EthernetHeader *eth, uint8_t *data, size_t len) {
    if (len < Ethernet::HEADER_SIZE + sizeof(ArpHeader)) {
      return false;
    }

    auto *arp = reinterpret_cast<ArpHeader *>(data + Ethernet::HEADER_SIZE);
    ++stats_.arp_packets;

    if (ArpHandler::handle(eth, arp, local_ip_, local_mac_)) {
      ++stats_.tx_packets;
      return true;
    }
    return false;
  }

  /**
   * @brief 处理 IPv4
   * @param should_send [out] 是否需要发送数据包
   * @return true 我们处理了（调用者返回 FF_DISPATCH_ERROR）
   */
  bool handle_ipv4(EthernetHeader *eth, uint8_t *data, size_t len, void *mbuf,
                   bool &should_send) {
    PacketMeta meta;
    if (!ProtocolParser::parse(data, len, meta)) {
      ++stats_.dropped_packets;
      should_send = false;
      return false;
    }

    // 关键修复：检查源 MAC 是否为本机 MAC
    // 如果是本机发送的包（Loopback/Reflection），直接忽略
    // 否则会造成 ARP 表被本机 MAC 污染 (Client IP -> LB MAC)
    if (Ethernet::mac_equal(meta.src_mac.data(), local_mac_.data())) {
      should_send = false;
      return true; // 视为已处理（忽略）
    }

    auto *ip = reinterpret_cast<IPv4Header *>(data + meta.l3_offset);

    // ICMP 处理 (Ping)
    if (ip->is_icmp() && meta.dst_ip == local_ip_) {
      should_send = handle_icmp(eth, ip, data, len, meta);
      return should_send;
    }

    // 判断流量方向
    // Full NAT 模式：Return traffic 也是 dst_ip=VIP，但 dst_port=ClientPort
    // 所以必须先判断 is_return，避免误判为 inbound

    // 判断是否是回程流量：看一下unordered_set中是否有这个ip
    bool is_return = is_nat_mode_ && is_from_realserver(meta.src_ip);
    // 判断是否是需要DNAT处理的包：查完源IP，再查一下目的IP是不是我
    bool is_inbound = (meta.dst_ip == local_ip_) && !is_return;
    // 判断是否是DR模式的直连流量
    bool is_to_realserver = is_realserver_ip(meta.dst_ip);

    // 性能优化：注释掉高频日志，避免影响性能测试
    // 如需调试，可以改为 LOG_DEBUG 或取消注释
    /*
    if (ip->is_tcp() || ip->is_udp()) {
        bool is_lb_traffic = is_inbound || is_return || is_to_realserver ||
                              ntohs(meta.src_port) == 8080 ||
    ntohs(meta.dst_port) == 8080; if (is_lb_traffic) { LOG_DEBUG("[PKT] %s:%u ->
    %s:%u (inbound=%d, return=%d, to_rs=%d)", ip_to_string(meta.src_ip).c_str(),
    ntohs(meta.src_port), ip_to_string(meta.dst_ip).c_str(),
    ntohs(meta.dst_port), is_inbound, is_return, is_to_realserver);
        }
    }
    */

    // 入站流量：Client -> VIP
    if (is_inbound && (ip->is_tcp() || ip->is_udp())) {
      if (ip->is_tcp())
        ++stats_.tcp_packets;
      else
        ++stats_.udp_packets;
      should_send = handle_inbound(eth, data, len, meta, mbuf);
      return should_send;
    }

    // 返回流量：RealServer -> Client (NAT 模式)
    if (is_return && (ip->is_tcp() || ip->is_udp())) {
      should_send = handle_return(eth, data, len, meta, mbuf);
      return should_send;
    }

    LOG_DEBUG("Packet not for LB: src=%s dst=%s",
              ip_to_string(meta.src_ip).c_str(),
              ip_to_string(meta.dst_ip).c_str());
    should_send = false;
    return false;
  }

  /**
   * @brief 处理 ICMP
   */
  bool handle_icmp(EthernetHeader *eth, IPv4Header *ip, uint8_t *data,
                   size_t len, const PacketMeta &meta) {
    auto *icmp = reinterpret_cast<IcmpHeader *>(data + meta.l4_offset);
    size_t icmp_len = len - meta.l4_offset;

    ++stats_.icmp_packets;

    if (IcmpHandler::handle_echo_request(icmp, icmp_len)) {
      eth->swap_mac();
      ip->swap_ip();
      IpChecksum::update(ip);

      ++stats_.tx_packets;
      return true;
    }
    return false;
  }

  /**
   * @brief 处理入站流量（DNAT）
   *
   * Client -> VIP:port  =>  Client -> RS:port
   */
  bool handle_inbound(EthernetHeader *eth, uint8_t *data, size_t len,
                      const PacketMeta &meta, void *mbuf) {
    FiveTuple tuple = meta.to_five_tuple();

    // 1. 查找已有会话
    Session session;
    if (SessionManager::instance().lookup(tuple, session)) {
      auto *rs =
          RealServerManager::instance().get_server(session.real_server_id);
      if (rs && forwarder_->forward(data, len, meta, rs, session.nat_src_port,
                                    mbuf)) {
        ArpTable::instance().update(meta.src_ip, meta.src_mac);
        SessionManager::instance().update_stats(tuple, len);
        ++stats_.forwarded_packets;
        ++stats_.tx_packets;
        return true;
      }
    }

    // 2. 新连接，选择后端服务器
    auto *rs = RealServerManager::instance().select_server(tuple);
    if (!rs) {
      LOG_WARN("No available backend server");
      ++stats_.dropped_packets;
      return false;
    }

    // 性能优化：改为 DEBUG 级别
    LOG_DEBUG("New connection: %s:%u -> VIP:%u => RS %s:%u",
              ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
              ntohs(meta.dst_port), ip_to_string(rs->ip).c_str(), rs->port);

    // 3. 创建会话（包含反向映射用于返回流量）
    // 注意：rs->port 是主机字节序，需要转换为网络字节序
    Port nat_src_port = SessionManager::instance().create(tuple, rs->id, rs->ip,
                                                          htons(rs->port));

    // 4. 转发
    if (forwarder_->forward(data, len, meta, rs, nat_src_port, mbuf)) {
      ArpTable::instance().update(meta.src_ip, meta.src_mac);
      ++stats_.forwarded_packets;
      ++stats_.nat_translations;
      ++stats_.tx_packets;
      return true;
    }

    ++stats_.dropped_packets;
    return false;
  }

  /**
   * @brief 处理返回流量（SNAT）
   *
   * RS:port -> Client  =>  VIP:port -> Client
   */
  bool handle_return(EthernetHeader *eth, uint8_t *data, size_t len,
                     const PacketMeta &meta, void *mbuf) {
    // 构建反向五元组进行查找
    FiveTuple reverse_tuple = meta.to_five_tuple();

    LOG_DEBUG("[RETURN] Looking up: src=%s:%u dst=%s:%u",
              ip_to_string(reverse_tuple.src_ip).c_str(),
              ntohs(reverse_tuple.src_port),
              ip_to_string(reverse_tuple.dst_ip).c_str(),
              ntohs(reverse_tuple.dst_port));

    Session session;
    if (!SessionManager::instance().lookup_reverse(reverse_tuple, session)) {
      LOG_DEBUG("[RETURN] NO SESSION FOUND for: %s:%u -> %s:%u",
                ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
                ip_to_string(meta.dst_ip).c_str(), ntohs(meta.dst_port));
      return false;
    }

    LOG_DEBUG("Return traffic: RS %s:%u -> %s:%u (SNAT to VIP)",
              ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
              ip_to_string(meta.dst_ip).c_str(), ntohs(meta.dst_port));

    // SNAT：修改源 IP 为 VIP，源端口为原始目的端口
    if (forwarder_->forward_reply(data, len, meta, session, mbuf)) {
      ArpTable::instance().update(meta.src_ip, meta.src_mac);
      SessionManager::instance().update_stats(session.client_tuple, len);
      ++stats_.forwarded_packets;
      ++stats_.tx_packets;
      return true;
    }

    ++stats_.dropped_packets;
    return false;
  }

  /**
   * @brief 判断 IP 是否属于 Real Server
   */
  bool is_from_realserver(IPv4Addr ip) const {
    return rs_ips_.find(ip) != rs_ips_.end();
  }

  /**
   * @brief 判断 IP 是否是 Real Server 的 IP（别名，语义更清晰）
   */
  bool is_realserver_ip(IPv4Addr ip) const {
    return rs_ips_.find(ip) != rs_ips_.end();
  }

  std::atomic<bool> running_;
  IPv4Addr local_ip_;
  MacAddr local_mac_;
  std::unique_ptr<Forwarder> forwarder_; // 存放key value
  // 方便判断是否是回程流量
  std::unordered_set<IPv4Addr> rs_ips_; // Real Server IP 集合
  bool is_nat_mode_ = false;
  uint64_t tx_offload_caps_ = 0;
  Statistics stats_{};
};

} // namespace l4lb

#endif // L4LB_CORE_LOADBALANCER_H
