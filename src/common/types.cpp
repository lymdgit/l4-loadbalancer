/**
 * @file types.cpp
 * @brief 公共工具函数实现：IP/MAC 地址与字符串互转
 */

#include "common/types.h"

#include <cstdio>

namespace l4lb {

IPv4Addr ip_from_string(const std::string &ip_str) {
  uint32_t a, b, c, d;
  if (sscanf(ip_str.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
    return 0;
  }
  // 存储为网络字节序（大端）：第一个字节在最低地址
  // 在小端机器上，(d << 24) | (c << 16) | (b << 8) | a 会产生网络字节序
  return (d << 24) | (c << 16) | (b << 8) | a;
}

std::string ip_to_string(IPv4Addr ip) {
  char buf[16];
  // ip 是网络字节序，最低字节是第一段
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF,
           (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
  return std::string(buf);
}

MacAddr mac_from_string(const std::string &mac_str) {
  MacAddr mac{};
  unsigned int a[6];
  if (sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3],
             &a[4], &a[5]) == 6) {
    for (int i = 0; i < 6; ++i) {
      mac[i] = static_cast<uint8_t>(a[i]);
    }
  }
  return mac;
}

std::string mac_to_string(const MacAddr &mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

} // namespace l4lb
