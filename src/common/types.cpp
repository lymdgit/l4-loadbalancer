/**
 * @file types.cpp
 * @brief 公共工具函数实现：IP/MAC 地址与字符串互转
 */

#include "common/types.h"

#include <arpa/inet.h>
#include <cstdio>

namespace l4lb {

bool parse_ipv4(const std::string &ip_str, IPv4Addr &out) {
  struct in_addr addr;
  // inet_pton 只接受严格的点分十进制（拒绝 999.1.1.1、前导空格等）
  if (inet_pton(AF_INET, ip_str.c_str(), &addr) != 1)
    return false;
  out = addr.s_addr; // 网络字节序
  return true;
}

IPv4Addr ip_from_string(const std::string &ip_str) {
  IPv4Addr ip = 0;
  return parse_ipv4(ip_str, ip) ? ip : 0;
}

std::string ip_to_string(IPv4Addr ip) {
  char buf[INET_ADDRSTRLEN];
  struct in_addr addr;
  addr.s_addr = ip;
  inet_ntop(AF_INET, &addr, buf, sizeof(buf));
  return std::string(buf);
}

bool parse_mac(const std::string &mac_str, MacAddr &out) {
  unsigned int a[6];
  char extra;
  if (sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x%c", &a[0], &a[1], &a[2],
             &a[3], &a[4], &a[5], &extra) != 6)
    return false;
  for (int i = 0; i < 6; ++i) {
    if (a[i] > 0xFF)
      return false;
    out[i] = static_cast<uint8_t>(a[i]);
  }
  return true;
}

MacAddr mac_from_string(const std::string &mac_str) {
  MacAddr mac{};
  return parse_mac(mac_str, mac) ? mac : MacAddr{};
}

std::string mac_to_string(const MacAddr &mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

} // namespace l4lb
