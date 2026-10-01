/**
 * @file dr_forwarder.h
 * @brief DR (Direct Routing) 转发模式
 *
 * DR 模式：只修改目的 MAC 地址，IP 层不变
 *
 * 入站: Client -> LB -> RealServer
 *   - 目的 MAC: LB MAC -> RS MAC
 *   - IP 地址不变
 *
 * 出站: RealServer -> Client (直接返回，不经过 LB)
 *
 * 要求：Real Server 需要在 loopback 接口配置 VIP
 *
 * 实现见 src/forward/dr_forwarder.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_FORWARD_DR_FORWARDER_H
#define L4LB_FORWARD_DR_FORWARDER_H

#include "forward/forwarder.h"

namespace l4lb {

class DrForwarder : public Forwarder {
public:
  DrForwarder();

  ForwardMode mode() const override { return ForwardMode::DR; }

  /// DR 模式转发 - 只修改目的 MAC
  bool forward(uint8_t *pkt, size_t len, const PacketMeta &meta,
               RealServer *rs, Port nat_src_port, void *mbuf) override;

  /// DR 模式不处理返回流量，始终返回 false
  bool forward_reply(uint8_t *pkt, size_t len, const PacketMeta &meta,
                     const Session &session, void *mbuf) override;

private:
  MacAddr local_mac_;
};

} // namespace l4lb

#endif // L4LB_FORWARD_DR_FORWARDER_H
