/**
 * @file arp.cpp
 * @brief ARP 报文构造实现
 */

#include "protocol/arp.h"

#include <cstring>

namespace l4lb {

void ArpHandler::make_reply(EthernetHeader *eth, ArpHeader *arp,
                            const MacAddr &local_mac) {
  IPv4Addr local_ip = arp->target_ip;

  memcpy(eth->dst_mac, arp->sender_mac, 6);
  eth->set_src_mac(local_mac);

  arp->set_operation(ArpOperation::REPLY);
  memcpy(arp->target_mac, arp->sender_mac, 6);
  arp->target_ip = arp->sender_ip;
  memcpy(arp->sender_mac, local_mac.data(), 6);
  arp->sender_ip = local_ip;
}

size_t ArpHandler::build_request(uint8_t *buf, IPv4Addr target_ip,
                                 IPv4Addr local_ip, const MacAddr &local_mac) {
  auto *eth = reinterpret_cast<EthernetHeader *>(buf);
  auto *arp = reinterpret_cast<ArpHeader *>(buf + sizeof(EthernetHeader));

  memset(eth->dst_mac, 0xFF, 6);
  memcpy(eth->src_mac, local_mac.data(), 6);
  eth->set_ether_type(static_cast<uint16_t>(EtherType::ARP));

  arp->hw_type = htons(1);
  arp->proto_type = htons(0x0800);
  arp->hw_len = 6;
  arp->proto_len = 4;
  arp->set_operation(ArpOperation::REQUEST);
  memcpy(arp->sender_mac, local_mac.data(), 6);
  arp->sender_ip = local_ip;
  memset(arp->target_mac, 0, 6);
  arp->target_ip = target_ip;

  return sizeof(EthernetHeader) + sizeof(ArpHeader);
}

size_t ArpHandler::build_gratuitous(uint8_t *buf, IPv4Addr local_ip,
                                    const MacAddr &local_mac) {
  return build_request(buf, local_ip, local_ip, local_mac);
}

} // namespace l4lb
