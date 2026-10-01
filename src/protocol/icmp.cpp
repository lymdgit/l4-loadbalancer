/**
 * @file icmp.cpp
 * @brief ICMP 协议处理实现
 */

#include "protocol/icmp.h"

namespace l4lb {

uint16_t IcmpHandler::calculate_checksum(const uint8_t *data, size_t len) {
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

bool IcmpHandler::handle_echo_request(IcmpHeader *icmp, size_t icmp_len) {
  if (!icmp->is_echo_request())
    return false;

  // 修改类型为 Echo Reply
  icmp->type = static_cast<uint8_t>(IcmpType::ECHO_REPLY);
  icmp->code = 0;

  // 重新计算校验和
  icmp->checksum = 0;
  icmp->checksum =
      calculate_checksum(reinterpret_cast<uint8_t *>(icmp), icmp_len);

  return true;
}

} // namespace l4lb
