/**
 * @file nat_forwarder.h
 * @brief NAT 转发模式 - 完整的 DNAT/SNAT 实现
 *
 * NAT 模式：修改数据包的源/目的 IP 地址和端口进行转发
 *
 * 入站流量 (DNAT): Client -> LB -> RealServer
 *   - 目的 IP: VIP -> RS IP
 *   - 目的端口: VIP Port -> RS Port
 *   - 更新 IP 校验和和 TCP/UDP 校验和
 *
 * 出站流量 (SNAT): RealServer -> LB -> Client
 *   - 源 IP: RS IP -> VIP
 *   - 源端口: RS Port -> 原始目的端口
 *   - 更新 IP 校验和和 TCP/UDP 校验和
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_FORWARD_NAT_FORWARDER_H
#define L4LB_FORWARD_NAT_FORWARDER_H

#include "common/config.h"
#include "common/logger.h"
#include "forward/forwarder.h"
#include "lb/session.h"
#include "protocol/arp.h"
#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>


namespace l4lb {

/**
 * @brief TCP/UDP 校验和更新工具
 *
 * TCP/UDP 校验和是基于伪头部（包含 IP 地址）计算的，
 * 所以当修改 IP 地址或端口时必须更新校验和。
 */
class L4Checksum {
public:
  /**
   * @brief 增量更新校验和
   *
   * 基于 RFC 1624 的增量校验和更新算法
   */
  static uint16_t incremental_update(uint16_t old_sum, uint16_t old_val,
                                     uint16_t new_val) {
    uint32_t sum = (~old_sum & 0xFFFF) + (~old_val & 0xFFFF) + new_val;
    while (sum >> 16) {
      sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return ~sum;
  }

  /**
   * @brief 更新 TCP 校验和（修改 IP 地址时）
   */
  static void update_tcp_checksum_ip(TcpHeader *tcp, uint32_t old_ip,
                                     uint32_t new_ip) {
    // TCP 校验和包含 IP 伪头部，需要更新
    tcp->checksum =
        incremental_update(tcp->checksum, old_ip >> 16, new_ip >> 16);
    tcp->checksum =
        incremental_update(tcp->checksum, old_ip & 0xFFFF, new_ip & 0xFFFF);
  }

  /**
   * @brief 更新 TCP 校验和（修改端口时）
   */
  static void update_tcp_checksum_port(TcpHeader *tcp, uint16_t old_port,
                                       uint16_t new_port) {
    tcp->checksum = incremental_update(tcp->checksum, old_port, new_port);
  }

  /**
   * @brief 更新 UDP 校验和（修改 IP 地址时）
   */
  static void update_udp_checksum_ip(UdpHeader *udp, uint32_t old_ip,
                                     uint32_t new_ip) {
    if (udp->checksum == 0)
      return; // UDP 校验和可选
    udp->checksum =
        incremental_update(udp->checksum, old_ip >> 16, new_ip >> 16);
    udp->checksum =
        incremental_update(udp->checksum, old_ip & 0xFFFF, new_ip & 0xFFFF);
    if (udp->checksum == 0)
      udp->checksum = 0xFFFF; // 避免校验和为 0
  }

  /**
   * @brief 更新 UDP 校验和（修改端口时）
   */
  static void update_udp_checksum_port(UdpHeader *udp, uint16_t old_port,
                                       uint16_t new_port) {
    if (udp->checksum == 0)
      return;
    udp->checksum = incremental_update(udp->checksum, old_port, new_port);
    if (udp->checksum == 0)
      udp->checksum = 0xFFFF;
  }
};

class NatForwarder : public Forwarder {
public:
  explicit NatForwarder(uint64_t tx_offload_caps) {
    local_mac_ = Config::instance().get_vip_mac();
    local_ip_ = Config::instance().get_vip();
    tx_offload_caps_ = tx_offload_caps;
  }

  ForwardMode mode() const override { return ForwardMode::NAT; }

  /**
   * @brief 转发入站流量（DNAT）
   *
   * Client -> VIP:port  =>  Client -> RS:rs_port
   */
  /**
   * @brief 全量重新计算 IP 校验和
   */
  static void recalculate_ip_checksum(IPv4Header *ip) {
    ip->checksum = 0;
    ip->checksum = IpChecksum::calculate(reinterpret_cast<uint8_t *>(ip),
                                         ip->get_header_len());
  }

  /**
   * @brief 计算伪头部校验和（部分和）
   */
  static uint32_t calculate_pseudo_header_sum(uint32_t src_ip, uint32_t dst_ip,
                                              uint8_t protocol,
                                              uint16_t length) {
    uint32_t sum = 0;
    sum += (src_ip >> 16) + (src_ip & 0xFFFF);
    sum += (dst_ip >> 16) + (dst_ip & 0xFFFF);
    sum += htons(protocol);
    sum += htons(length);
    return sum;
  }

  /**
   * @brief 全量重新计算 TCP 校验和
   */
  static void recalculate_tcp_checksum(IPv4Header *ip, TcpHeader *tcp,
                                       size_t tcp_len) {
    tcp->checksum = 0;
    uint32_t sum = calculate_pseudo_header_sum(
        ip->src_ip, ip->dst_ip, ip->protocol, static_cast<uint16_t>(tcp_len));

    // 计算 TCP payload 校验和
    uint16_t *ptr = reinterpret_cast<uint16_t *>(tcp);
    while (tcp_len > 1) {
      sum += *ptr++;
      tcp_len -= 2;
    }
    if (tcp_len > 0) {
      sum += *reinterpret_cast<const uint8_t *>(ptr);
    }

    while (sum >> 16) {
      sum = (sum & 0xFFFF) + (sum >> 16);
    }
    tcp->checksum = static_cast<uint16_t>(~sum);
  }

  /**
   * @brief 全量重新计算 UDP 校验和
   */
  static void recalculate_udp_checksum(IPv4Header *ip, UdpHeader *udp) {
    udp->checksum = 0;
    // UDP 长度包含 header
    uint16_t udp_len = udp->get_length();
    uint32_t sum = calculate_pseudo_header_sum(ip->src_ip, ip->dst_ip,
                                               ip->protocol, udp_len);

    uint16_t *ptr = reinterpret_cast<uint16_t *>(udp);
    size_t len = udp_len;
    while (len > 1) {
      sum += *ptr++;
      len -= 2;
    }
    if (len > 0) {
      sum += *reinterpret_cast<const uint8_t *>(ptr);
    }

    while (sum >> 16) {
      sum = (sum & 0xFFFF) + (sum >> 16);
    }
    udp->checksum = static_cast<uint16_t>(~sum);
    if (udp->checksum == 0)
      udp->checksum = 0xFFFF;
  }

  bool forward(uint8_t *pkt, size_t len, const PacketMeta &meta,
               RealServer *rs, Port nat_src_port, void *mbuf) override {
    if (!rs) {
      LOG_ERROR("NAT forward: rs is null");
      return false;
    }

    auto *eth = reinterpret_cast<EthernetHeader *>(pkt);
    auto *ip = reinterpret_cast<IPv4Header *>(pkt + meta.l3_offset);

    // 1. 修改 IP 头部 & 更新 IP 校验和
    // Full NAT: dst_ip (VIP->RS), src_ip (Client->VIP)
    uint32_t old_src_ip = ip->src_ip;
    uint32_t old_dst_ip = ip->dst_ip;
    uint32_t new_src_ip = local_ip_;
    uint32_t new_dst_ip = rs->ip;

    ip->dst_ip = new_dst_ip;
    ip->src_ip = new_src_ip;

    // 更新 IP 校验和 (增量)
    // 由于我们修改了两个IP，需要调用两次 update，或者合并计算
    // IP Checksum 只覆盖 IP Header
    ip->checksum = L4Checksum::incremental_update(
        ip->checksum, old_src_ip >> 16, new_src_ip >> 16);
    ip->checksum = L4Checksum::incremental_update(
        ip->checksum, old_src_ip & 0xFFFF, new_src_ip & 0xFFFF);
    ip->checksum = L4Checksum::incremental_update(
        ip->checksum, old_dst_ip >> 16, new_dst_ip >> 16);
    ip->checksum = L4Checksum::incremental_update(
        ip->checksum, old_dst_ip & 0xFFFF, new_dst_ip & 0xFFFF);

    // TTL 递减
    if (ip->ttl <= 1) {
      return false;
    }
    uint16_t old_ttl = (uint16_t)ip->ttl | ((uint16_t)ip->protocol << 8);
    --ip->ttl;
    uint16_t new_ttl = (uint16_t)ip->ttl | ((uint16_t)ip->protocol << 8);
    ip->checksum =
        L4Checksum::incremental_update(ip->checksum, old_ttl, new_ttl);

    // 2. 修改端口 & 更新 L4 校验和
    if (ip->is_tcp()) {
      auto *tcp = reinterpret_cast<TcpHeader *>(pkt + meta.l4_offset);
      uint16_t old_src_port = tcp->src_port;
      uint16_t new_src_port =
          (nat_src_port != 0) ? nat_src_port : old_src_port;
      uint16_t old_port = tcp->dst_port;
      uint16_t new_port = htons(rs->port);
      tcp->dst_port = new_port;
      tcp->src_port = new_src_port;

      // 更新 TCP 校验和 (伪头部变动 + 端口变动)
      // 伪头部变动：SrcIP, DstIP
      L4Checksum::update_tcp_checksum_ip(tcp, old_src_ip, new_src_ip);
      L4Checksum::update_tcp_checksum_ip(tcp, old_dst_ip, new_dst_ip);
      // 端口变动
      if (new_src_port != old_src_port) {
        L4Checksum::update_tcp_checksum_port(tcp, old_src_port, new_src_port);
      }
      L4Checksum::update_tcp_checksum_port(tcp, old_port, new_port);

    } else if (ip->is_udp()) {
      auto *udp = reinterpret_cast<UdpHeader *>(pkt + meta.l4_offset);
      uint16_t old_src_port = udp->src_port;
      uint16_t new_src_port =
          (nat_src_port != 0) ? nat_src_port : old_src_port;
      uint16_t old_port = udp->dst_port;
      uint16_t new_port = htons(rs->port);
      udp->dst_port = new_port;
      udp->src_port = new_src_port;

      // UDP 校验和 (如果启用)
      if (udp->checksum != 0) {
        L4Checksum::update_udp_checksum_ip(udp, old_src_ip, new_src_ip);
        L4Checksum::update_udp_checksum_ip(udp, old_dst_ip, new_dst_ip);
        if (new_src_port != old_src_port) {
          L4Checksum::update_udp_checksum_port(udp, old_src_port,
                                               new_src_port);
        }
        L4Checksum::update_udp_checksum_port(udp, old_port, new_port);
      }
    }

    // 3. 修改 MAC 地址
    MacAddr dst_mac;
    bool mac_is_zero = (rs->mac[0] == 0 && rs->mac[1] == 0 && rs->mac[2] == 0 &&
                        rs->mac[3] == 0);
    if (!mac_is_zero) {
      dst_mac = rs->mac;
    } else if (ArpTable::instance().lookup(rs->ip, dst_mac)) {
    } else {
      dst_mac = Ethernet::broadcast_mac();
    }
    eth->set_dst_mac(dst_mac);
    eth->set_src_mac(local_mac_);

#ifdef L4LB_HW_CKSUM
    const bool hw_tcp =
        (tx_offload_caps_ & RTE_ETH_TX_OFFLOAD_TCP_CKSUM) != 0;
    const bool hw_udp =
        (tx_offload_caps_ & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) != 0;
    const bool hw_ip =
#ifdef L4LB_HW_CKSUM_IP
        (tx_offload_caps_ & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) != 0;
#else
        false;
#endif
    // HW offload for L4 checksums (keep IP checksum software by default)
    auto *m = reinterpret_cast<rte_mbuf *>(mbuf);
    m->ol_flags |= RTE_MBUF_F_TX_IPV4;
    if (hw_ip) {
      ip->checksum = 0;
      m->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
    }
    if (ip->is_tcp()) {
      if (hw_tcp) {
        auto *tcp = reinterpret_cast<TcpHeader *>(pkt + meta.l4_offset);
        tcp->checksum = rte_ipv4_phdr_cksum(
            reinterpret_cast<const rte_ipv4_hdr *>(ip),
            RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_TCP_CKSUM);
        m->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
        m->l4_len = tcp->get_header_len();
      }
    } else if (ip->is_udp()) {
      if (hw_udp) {
        auto *udp = reinterpret_cast<UdpHeader *>(pkt + meta.l4_offset);
        udp->checksum = rte_ipv4_phdr_cksum(
            reinterpret_cast<const rte_ipv4_hdr *>(ip),
            RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM);
        m->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
        m->l4_len = sizeof(UdpHeader);
      }
    }
    m->l2_len = meta.l3_offset;
    m->l3_len = ip->get_header_len();
#endif

    return true;
  }

  bool forward_reply(uint8_t *pkt, size_t len, const PacketMeta &meta,
                     const Session &session, void *mbuf) override {
    auto *eth = reinterpret_cast<EthernetHeader *>(pkt);
    auto *ip = reinterpret_cast<IPv4Header *>(pkt + meta.l3_offset);

    // SNAT: src_ip (RS->VIP), dst_ip (VIP->Client)
    uint32_t old_src_ip = ip->src_ip;
    uint32_t old_dst_ip = ip->dst_ip;
    uint32_t new_src_ip = local_ip_;
    uint32_t new_dst_ip = session.client_tuple.src_ip;

    ip->src_ip = new_src_ip;
    ip->dst_ip = new_dst_ip;

    // 更新 IP 校验和 (增量)
    ip->checksum = L4Checksum::incremental_update(
        ip->checksum, old_src_ip >> 16, new_src_ip >> 16);
    ip->checksum = L4Checksum::incremental_update(
        ip->checksum, old_src_ip & 0xFFFF, new_src_ip & 0xFFFF);
    ip->checksum = L4Checksum::incremental_update(
        ip->checksum, old_dst_ip >> 16, new_dst_ip >> 16);
    ip->checksum = L4Checksum::incremental_update(
        ip->checksum, old_dst_ip & 0xFFFF, new_dst_ip & 0xFFFF);

    // TTL 递减
    if (ip->ttl > 1) {
      uint16_t old_ttl = (uint16_t)ip->ttl | ((uint16_t)ip->protocol << 8);
      --ip->ttl;
      uint16_t new_ttl = (uint16_t)ip->ttl | ((uint16_t)ip->protocol << 8);
      ip->checksum =
          L4Checksum::incremental_update(ip->checksum, old_ttl, new_ttl);
    }

    // 修改端口 & 更新 L4 校验和
    if (ip->is_tcp()) {
      auto *tcp = reinterpret_cast<TcpHeader *>(pkt + meta.l4_offset);
      uint16_t old_src_port = tcp->src_port;
      uint16_t new_src_port = session.client_tuple.dst_port;
      uint16_t old_dst_port = tcp->dst_port;
      uint16_t new_dst_port = session.client_tuple.src_port;
      tcp->src_port = new_src_port;
      tcp->dst_port = new_dst_port;

      L4Checksum::update_tcp_checksum_ip(tcp, old_src_ip, new_src_ip);
      L4Checksum::update_tcp_checksum_ip(tcp, old_dst_ip, new_dst_ip);
      L4Checksum::update_tcp_checksum_port(tcp, old_src_port, new_src_port);
      L4Checksum::update_tcp_checksum_port(tcp, old_dst_port, new_dst_port);

    } else if (ip->is_udp()) {
      auto *udp = reinterpret_cast<UdpHeader *>(pkt + meta.l4_offset);
      uint16_t old_src_port = udp->src_port;
      uint16_t new_src_port = session.client_tuple.dst_port;
      uint16_t old_dst_port = udp->dst_port;
      uint16_t new_dst_port = session.client_tuple.src_port;
      udp->src_port = new_src_port;
      udp->dst_port = new_dst_port;

      if (udp->checksum != 0) {
        L4Checksum::update_udp_checksum_ip(udp, old_src_ip, new_src_ip);
        L4Checksum::update_udp_checksum_ip(udp, old_dst_ip, new_dst_ip);
        L4Checksum::update_udp_checksum_port(udp, old_src_port, new_src_port);
        L4Checksum::update_udp_checksum_port(udp, old_dst_port, new_dst_port);
      }
    }

    // 修改 MAC (查 ARP)
    MacAddr dst_mac;
    if (ArpTable::instance().lookup(ip->dst_ip, dst_mac)) {
      eth->set_dst_mac(dst_mac);
    } else {
      LOG_WARN("SNAT: No MAC for Client %s, using broadcast",
               ip_to_string(ip->dst_ip).c_str());
      dst_mac = Ethernet::broadcast_mac();
      eth->set_dst_mac(dst_mac);
    }
    eth->set_src_mac(local_mac_);

#ifdef L4LB_HW_CKSUM
    const bool hw_tcp =
        (tx_offload_caps_ & RTE_ETH_TX_OFFLOAD_TCP_CKSUM) != 0;
    const bool hw_udp =
        (tx_offload_caps_ & RTE_ETH_TX_OFFLOAD_UDP_CKSUM) != 0;
    const bool hw_ip =
#ifdef L4LB_HW_CKSUM_IP
        (tx_offload_caps_ & RTE_ETH_TX_OFFLOAD_IPV4_CKSUM) != 0;
#else
        false;
#endif
    // HW offload for L4 checksums (keep IP checksum software by default)
    auto *m = reinterpret_cast<rte_mbuf *>(mbuf);
    m->ol_flags |= RTE_MBUF_F_TX_IPV4;
    if (hw_ip) {
      ip->checksum = 0;
      m->ol_flags |= RTE_MBUF_F_TX_IP_CKSUM;
    }
    if (ip->is_tcp()) {
      if (hw_tcp) {
        auto *tcp = reinterpret_cast<TcpHeader *>(pkt + meta.l4_offset);
        tcp->checksum = rte_ipv4_phdr_cksum(
            reinterpret_cast<const rte_ipv4_hdr *>(ip),
            RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_TCP_CKSUM);
        m->ol_flags |= RTE_MBUF_F_TX_TCP_CKSUM;
        m->l4_len = tcp->get_header_len();
      }
    } else if (ip->is_udp()) {
      if (hw_udp) {
        auto *udp = reinterpret_cast<UdpHeader *>(pkt + meta.l4_offset);
        udp->checksum = rte_ipv4_phdr_cksum(
            reinterpret_cast<const rte_ipv4_hdr *>(ip),
            RTE_MBUF_F_TX_IPV4 | RTE_MBUF_F_TX_UDP_CKSUM);
        m->ol_flags |= RTE_MBUF_F_TX_UDP_CKSUM;
        m->l4_len = sizeof(UdpHeader);
      }
    }
    m->l2_len = meta.l3_offset;
    m->l3_len = ip->get_header_len();
#endif

    return true;
  }

private:
  MacAddr local_mac_;
  IPv4Addr local_ip_;
  uint64_t tx_offload_caps_ = 0;
};

} // namespace l4lb

#endif // L4LB_FORWARD_NAT_FORWARDER_H
