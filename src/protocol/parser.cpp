/**
 * @file parser.cpp
 * @brief 报文解析实现
 */

#include "protocol/parser.h"

#include "protocol/ethernet.h"
#include "protocol/ip.h"
#include <cstring>

namespace l4lb {

bool ProtocolParser::parse(const uint8_t *pkt, size_t len, PacketMeta &meta) {
  if (len < Ethernet::HEADER_SIZE)
    return false;

  auto *eth = reinterpret_cast<const EthernetHeader *>(pkt);
  memcpy(meta.dst_mac.data(), eth->dst_mac, 6);
  memcpy(meta.src_mac.data(), eth->src_mac, 6);
  meta.ether_type = eth->get_ether_type();
  meta.l2_offset = 0;
  meta.l3_offset = Ethernet::HEADER_SIZE;

  if (!eth->is_ipv4())
    return true;
  if (len < meta.l3_offset + sizeof(IPv4Header))
    return false;

  auto *ip = reinterpret_cast<const IPv4Header *>(pkt + meta.l3_offset);
  meta.src_ip = ip->src_ip;
  meta.dst_ip = ip->dst_ip;
  meta.ip_protocol = ip->protocol;
  meta.ip_ttl = ip->ttl;
  meta.l4_offset = meta.l3_offset + ip->get_header_len();
  meta.total_len = len;

  if (ip->is_tcp() && len >= meta.l4_offset + sizeof(TcpHeader)) {
    auto *tcp = reinterpret_cast<const TcpHeader *>(pkt + meta.l4_offset);
    meta.src_port = tcp->src_port;
    meta.dst_port = tcp->dst_port;
    meta.payload_offset = meta.l4_offset + tcp->get_header_len();
  } else if (ip->is_udp() && len >= meta.l4_offset + sizeof(UdpHeader)) {
    auto *udp = reinterpret_cast<const UdpHeader *>(pkt + meta.l4_offset);
    meta.src_port = udp->src_port;
    meta.dst_port = udp->dst_port;
    meta.payload_offset = meta.l4_offset + sizeof(UdpHeader);
  } else {
    meta.src_port = 0;
    meta.dst_port = 0;
    meta.payload_offset = meta.l4_offset;
  }

  meta.payload_len = len - meta.payload_offset;
  return true;
}

} // namespace l4lb
