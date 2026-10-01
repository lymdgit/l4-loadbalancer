/**
 * @file dr_forwarder.cpp
 * @brief DR 转发模式实现
 */

#include "forward/dr_forwarder.h"

#include "common/config.h"
#include "common/logger.h"
#include "protocol/arp.h"
#include "protocol/ethernet.h"

namespace l4lb {

DrForwarder::DrForwarder() { local_mac_ = Config::instance().get_vip_mac(); }

bool DrForwarder::forward(uint8_t *pkt, size_t len, const PacketMeta &meta,
                          RealServer *rs, Port nat_src_port, void *mbuf) {
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

bool DrForwarder::forward_reply(uint8_t *pkt, size_t len,
                                const PacketMeta &meta, const Session &session,
                                void *mbuf) {
  (void)mbuf;
  // DR 模式下，返回流量直接从 RS 到客户端，不经过 LB
  return false;
}

} // namespace l4lb
