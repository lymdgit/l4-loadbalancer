/**
 * @file checksum.cpp
 * @brief 校验和全量计算实现
 */

#include "protocol/checksum.h"

#include <arpa/inet.h>

namespace l4lb {

// ============================================================================
// IpChecksum
// ============================================================================

uint16_t IpChecksum::calculate(const uint8_t *data, size_t len) {
  uint32_t sum = 0;
  const uint16_t *ptr = reinterpret_cast<const uint16_t *>(data);

  while (len > 1) {
    sum += *ptr++;
    len -= 2;
  }
  if (len == 1) {
    sum += *reinterpret_cast<const uint8_t *>(ptr);
  }
  while (sum >> 16) {
    sum = (sum & 0xFFFF) + (sum >> 16);
  }
  return static_cast<uint16_t>(~sum);
}

void IpChecksum::update(IPv4Header *ip) {
  ip->checksum = 0;
  ip->checksum =
      calculate(reinterpret_cast<uint8_t *>(ip), ip->get_header_len());
}

// ============================================================================
// L4Checksum
// ============================================================================

uint32_t L4Checksum::calculate_pseudo_header_sum(uint32_t src_ip,
                                                 uint32_t dst_ip,
                                                 uint8_t protocol,
                                                 uint16_t length) {
  uint32_t sum = 0;
  sum += (src_ip >> 16) + (src_ip & 0xFFFF);
  sum += (dst_ip >> 16) + (dst_ip & 0xFFFF);
  sum += htons(protocol);
  sum += htons(length);
  return sum;
}

void L4Checksum::recalculate_ip_checksum(IPv4Header *ip) {
  ip->checksum = 0;
  ip->checksum = IpChecksum::calculate(reinterpret_cast<uint8_t *>(ip),
                                       ip->get_header_len());
}

void L4Checksum::recalculate_tcp_checksum(IPv4Header *ip, TcpHeader *tcp,
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

void L4Checksum::recalculate_udp_checksum(IPv4Header *ip, UdpHeader *udp) {
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

} // namespace l4lb
