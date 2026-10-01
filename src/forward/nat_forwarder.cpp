/**
 * @file nat_forwarder.cpp
 * @brief NAT 转发模式实现
 */

#include "forward/nat_forwarder.h"

#include "common/config.h"
#include "common/logger.h"
#include "protocol/arp.h"
#include "protocol/checksum.h"
#include "protocol/ethernet.h"
#include "protocol/ip.h"

#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

namespace l4lb {

NatForwarder::NatForwarder(uint64_t tx_offload_caps) {
  local_mac_ = Config::instance().get_vip_mac();
  local_ip_ = Config::instance().get_vip();
  tx_offload_caps_ = tx_offload_caps;
}

bool NatForwarder::forward(uint8_t *pkt, size_t len, const PacketMeta &meta,
                          RealServer *rs, Port nat_src_port, void *mbuf) {
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

bool NatForwarder::forward_reply(uint8_t *pkt, size_t len,
                                const PacketMeta &meta, const Session &session,
                                void *mbuf) {
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

} // namespace l4lb
