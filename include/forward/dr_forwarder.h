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
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_FORWARD_DR_FORWARDER_H
#define L4LB_FORWARD_DR_FORWARDER_H

#include "common/config.h"
#include "common/logger.h"
#include "forward/forwarder.h"
#include "protocol/arp.h"

namespace l4lb {

class DrForwarder : public Forwarder {
public:
  DrForwarder() { local_mac_ = Config::instance().get_vip_mac(); }

  ForwardMode mode() const override { return ForwardMode::DR; }

  /**
   * @brief DR 模式转发 - 只修改目的 MAC
   */
  bool forward(uint8_t *pkt, size_t len, const PacketMeta &meta,
               RealServer *rs, Port nat_src_port, void *mbuf) override {
    (void)nat_src_port;
    (void)mbuf;
    if (!rs)
      return false;

    auto *eth = reinterpret_cast<EthernetHeader *>(pkt);

    // 优先使用配置文件中的 MAC (更可靠)
    // 检查 MAC 是否有效 (任意字节非零)
    bool has_config_mac = false;
    for (int i = 0; i < 6; ++i) {
      if (rs->mac[i] != 0) {
        has_config_mac = true;
        break;
      }
    }

    if (has_config_mac) {
      eth->set_dst_mac(rs->mac);
    } else {
      // 回退到 ARP 表查找
      MacAddr dst_mac;
      if (ArpTable::instance().lookup(rs->ip, dst_mac)) {
        eth->set_dst_mac(dst_mac);
      } else {
        LOG_WARN("No MAC for RS %s (config and ARP both empty)",
                 ip_to_string(rs->ip).c_str());
        return false;
      }
    }

    // 修改源 MAC 为本机
    eth->set_src_mac(local_mac_);

    LOG_DEBUG("DR forward to %s", mac_to_string(eth->get_dst_mac()).c_str());

    return true;
  }

  /**
   * @brief DR 模式不处理返回流量
   */
  bool forward_reply(uint8_t *pkt, size_t len, const PacketMeta &meta,
                     const Session &session, void *mbuf) override {
    (void)mbuf;
    // DR 模式下，返回流量直接从 RS 到客户端，不经过 LB
    return false;
  }

private:
  MacAddr local_mac_;
};

} // namespace l4lb

#endif // L4LB_FORWARD_DR_FORWARDER_H
