/**
 * @file arp.cpp
 * @brief ARP 表与 ARP 协议处理实现
 */

#include "protocol/arp.h"

#include <chrono>
#include <cstring>
#include <functional>

namespace l4lb {

// ============================================================================
// ArpEntry
// ============================================================================

ArpEntry::ArpEntry(const MacAddr &m) : mac(m), complete(true) {
  timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
}

// ============================================================================
// ArpTable
// ============================================================================

void ArpTable::update(IPv4Addr ip, const MacAddr &mac) {
  auto &shard = shards_[hash_ip(ip) % kNumShards];
  std::lock_guard<std::mutex> lock(shard.mutex);
  shard.table[ip] = ArpEntry(mac);
}

bool ArpTable::lookup(IPv4Addr ip, MacAddr &mac) const {
  auto &shard = shards_[hash_ip(ip) % kNumShards];
  std::lock_guard<std::mutex> lock(shard.mutex);
  auto it = shard.table.find(ip);
  if (it != shard.table.end() && it->second.complete) {
    mac = it->second.mac;
    return true;
  }
  return false;
}

size_t ArpTable::hash_ip(IPv4Addr ip) { return std::hash<uint32_t>{}(ip); }

// ============================================================================
// ArpHandler
// ============================================================================

bool ArpHandler::handle(EthernetHeader *eth, ArpHeader *arp, IPv4Addr local_ip,
                        const MacAddr &local_mac) {
  if (arp->is_request()) {
    return handle_request(eth, arp, local_ip, local_mac);
  }
  if (arp->is_reply()) {
    MacAddr mac;
    memcpy(mac.data(), arp->sender_mac, 6);
    ArpTable::instance().update(arp->sender_ip, mac);
  }
  return false;
}

bool ArpHandler::handle_request(EthernetHeader *eth, ArpHeader *arp,
                                IPv4Addr local_ip, const MacAddr &local_mac) {
  if (arp->target_ip != local_ip)
    return false;

  MacAddr sender_mac;
  memcpy(sender_mac.data(), arp->sender_mac, 6);
  ArpTable::instance().update(arp->sender_ip, sender_mac);

  eth->swap_mac();
  eth->set_src_mac(local_mac);

  arp->set_operation(ArpOperation::REPLY);
  memcpy(arp->target_mac, arp->sender_mac, 6);
  arp->target_ip = arp->sender_ip;
  memcpy(arp->sender_mac, local_mac.data(), 6);
  arp->sender_ip = local_ip;

  return true;
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

} // namespace l4lb
