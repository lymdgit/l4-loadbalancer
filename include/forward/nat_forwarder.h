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
 * 校验和工具见 protocol/checksum.h，实现见 src/forward/nat_forwarder.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_FORWARD_NAT_FORWARDER_H
#define L4LB_FORWARD_NAT_FORWARDER_H

#include "forward/forwarder.h"

namespace l4lb {

class NatForwarder : public Forwarder {
public:
  explicit NatForwarder(uint64_t tx_offload_caps);

  ForwardMode mode() const override { return ForwardMode::NAT; }

  /**
   * @brief 转发入站流量（DNAT + SNAT 到 VIP）
   *
   * Client -> VIP:port  =>  VIP:nat_src_port -> RS:rs_port
   */
  bool forward(uint8_t *pkt, size_t len, const PacketMeta &meta,
               RealServer *rs, Port nat_src_port, void *mbuf) override;

  /**
   * @brief 转发返回流量
   *
   * RS:rs_port -> VIP:nat_src_port  =>  VIP:port -> Client
   */
  bool forward_reply(uint8_t *pkt, size_t len, const PacketMeta &meta,
                     const Session &session, void *mbuf) override;

private:
  MacAddr local_mac_;
  IPv4Addr local_ip_;
  uint64_t tx_offload_caps_ = 0;
};

} // namespace l4lb

#endif // L4LB_FORWARD_NAT_FORWARDER_H
