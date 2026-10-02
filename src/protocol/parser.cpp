/**
 * @file parser.cpp
 * @brief 报文解析实现
 */

#include "protocol/parser.h"

#include "protocol/ethernet.h"
#include "protocol/icmp.h"
#include "protocol/ip.h"
#include <cstring>

namespace l4lb {

namespace {
constexpr uint16_t kIpFlagMF = 0x2000;       ///< More Fragments
constexpr uint16_t kIpFragOffMask = 0x1FFF; ///< 片偏移
} // namespace

ParseResult ProtocolParser::parse(const uint8_t *pkt, size_t len,
                                  PacketMeta &meta) {
  if (len < Ethernet::HEADER_SIZE)
    return ParseResult::MALFORMED;

  auto *eth = reinterpret_cast<const EthernetHeader *>(pkt);
  memcpy(meta.dst_mac.data(), eth->dst_mac, 6);
  memcpy(meta.src_mac.data(), eth->src_mac, 6);
  meta.ether_type = eth->get_ether_type();
  meta.l2_offset = 0;
  meta.l3_offset = Ethernet::HEADER_SIZE;
  meta.src_port = 0;
  meta.dst_port = 0;
  meta.tcp_flags = 0;

  if (!eth->is_ipv4())
    return ParseResult::OK;

  // ---- IPv4 头 ----
  if (len < meta.l3_offset + sizeof(IPv4Header))
    return ParseResult::MALFORMED;

  auto *ip = reinterpret_cast<const IPv4Header *>(pkt + meta.l3_offset);
  size_t ihl = ip->get_header_len();
  size_t ip_total = ip->get_total_length();
  if (ip->get_version() != 4 || ihl < sizeof(IPv4Header) || ip_total < ihl ||
      meta.l3_offset + ip_total > len) {
    return ParseResult::MALFORMED;
  }

  // 帧可能带以太网尾部填充，后续一律以 IP total_length 为准
  const size_t end = meta.l3_offset + ip_total;

  meta.src_ip = ip->src_ip;
  meta.dst_ip = ip->dst_ip;
  meta.ip_protocol = ip->protocol;
  meta.ip_ttl = ip->ttl;
  meta.l4_offset = meta.l3_offset + ihl;
  meta.total_len = end;
  meta.payload_offset = meta.l4_offset;

  // 分片：非首片没有 L4 头，首片的 L4 校验和覆盖整个原始报文，都不能按包改写
  uint16_t frag = ntohs(ip->flags_fragment);
  if ((frag & kIpFlagMF) || (frag & kIpFragOffMask))
    return ParseResult::FRAGMENT;

  // ---- L4 头 ----
  if (ip->is_tcp()) {
    if (meta.l4_offset + sizeof(TcpHeader) > end)
      return ParseResult::MALFORMED;
    auto *tcp = reinterpret_cast<const TcpHeader *>(pkt + meta.l4_offset);
    size_t doff = tcp->get_header_len();
    if (doff < sizeof(TcpHeader) || meta.l4_offset + doff > end)
      return ParseResult::MALFORMED;
    meta.src_port = tcp->src_port;
    meta.dst_port = tcp->dst_port;
    meta.tcp_flags = tcp->flags;
    meta.payload_offset = meta.l4_offset + doff;
  } else if (ip->is_udp()) {
    if (meta.l4_offset + sizeof(UdpHeader) > end)
      return ParseResult::MALFORMED;
    auto *udp = reinterpret_cast<const UdpHeader *>(pkt + meta.l4_offset);
    size_t udp_len = udp->get_length();
    if (udp_len < sizeof(UdpHeader) || meta.l4_offset + udp_len > end)
      return ParseResult::MALFORMED;
    meta.src_port = udp->src_port;
    meta.dst_port = udp->dst_port;
    meta.payload_offset = meta.l4_offset + sizeof(UdpHeader);
  } else if (ip->is_icmp()) {
    if (meta.l4_offset + sizeof(IcmpHeader) > end)
      return ParseResult::MALFORMED;
  }

  meta.payload_len = end - meta.payload_offset;
  return ParseResult::OK;
}

bool ProtocolParser::parse_icmp_error(const uint8_t *pkt,
                                      const PacketMeta &meta,
                                      IcmpErrorInfo &info) {
  auto *icmp = reinterpret_cast<const IcmpHeader *>(pkt + meta.l4_offset);
  if (!icmp_is_error(icmp->type))
    return false;

  size_t inner_l3 = meta.l4_offset + sizeof(IcmpHeader);
  if (inner_l3 + sizeof(IPv4Header) > meta.total_len)
    return false;
  auto *ip = reinterpret_cast<const IPv4Header *>(pkt + inner_l3);
  size_t ihl = ip->get_header_len();
  if (ip->get_version() != 4 || ihl < sizeof(IPv4Header))
    return false;
  if (!ip->is_tcp() && !ip->is_udp())
    return false;
  // 只看首片（非首片没有端口）
  if (ntohs(ip->flags_fragment) & kIpFragOffMask)
    return false;
  size_t inner_l4 = inner_l3 + ihl;
  if (inner_l4 + 8 > meta.total_len) // RFC 792：至少带 8 字节 L4 头
    return false;

  auto *ports = reinterpret_cast<const uint16_t *>(pkt + inner_l4);
  info.inner_l3_offset = static_cast<uint16_t>(inner_l3);
  info.inner_l4_offset = static_cast<uint16_t>(inner_l4);
  info.inner = FiveTuple(ip->src_ip, ip->dst_ip, ports[0], ports[1],
                         ip->protocol);
  return true;
}

} // namespace l4lb
