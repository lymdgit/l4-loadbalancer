/**
 * @file route.h
 * @brief 最简路由：直连网段走邻居，其余走网关
 *
 * 没有配置 netmask 时所有地址都按直连处理（与旧版本行为一致，适合扁平二层网络）。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_NET_ROUTE_H
#define L4LB_NET_ROUTE_H

#include "common/types.h"

namespace l4lb {

struct Route {
  IPv4Addr netmask = 0; ///< 网络字节序；0 表示所有地址都直连
  IPv4Addr network = 0;
  IPv4Addr gateway = 0;

  void init(IPv4Addr local_ip, IPv4Addr mask, IPv4Addr gw) {
    netmask = mask;
    network = local_ip & mask;
    gateway = gw;
  }

  bool on_link(IPv4Addr ip) const {
    return netmask == 0 || (ip & netmask) == network;
  }

  /// 下一跳：直连返回目的地址本身，否则返回网关（无网关时返回 0，表示不可达）
  IPv4Addr next_hop(IPv4Addr dst) const {
    return on_link(dst) ? dst : gateway;
  }
};

} // namespace l4lb

#endif // L4LB_NET_ROUTE_H
