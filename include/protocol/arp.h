/**
 * @file arp.h
 * @brief ARP 协议处理
 * 
 * ARP 用于将 IP 地址解析为 MAC 地址。
 * 
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_PROTOCOL_ARP_H
#define L4LB_PROTOCOL_ARP_H

#include <array>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "common/types.h"
#include "common/logger.h"
#include "protocol/ethernet.h"

namespace l4lb {

/// ARP 操作类型
enum class ArpOperation : uint16_t {
    REQUEST = 1,
    REPLY   = 2,
};

/// ARP 报文头结构
struct __attribute__((packed)) ArpHeader {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t  hw_len;
    uint8_t  proto_len;
    uint16_t operation;
    uint8_t  sender_mac[6];
    uint32_t sender_ip;
    uint8_t  target_mac[6];
    uint32_t target_ip;
    
    ArpOperation get_operation() const { return static_cast<ArpOperation>(ntohs(operation)); }
    void set_operation(ArpOperation op) { operation = htons(static_cast<uint16_t>(op)); }
    bool is_request() const { return get_operation() == ArpOperation::REQUEST; }
    bool is_reply() const { return get_operation() == ArpOperation::REPLY; }
};

static_assert(sizeof(ArpHeader) == 28, "ArpHeader size must be 28 bytes");

/// ARP 表条目
struct ArpEntry {
    MacAddr mac;
    uint64_t timestamp;
    bool complete;
    
    ArpEntry() : mac{}, timestamp(0), complete(false) {}
    ArpEntry(const MacAddr& m, uint64_t now_tsc)
        : mac(m), timestamp(now_tsc), complete(true) {}
};

/// ARP 表管理类 - 分片减少锁竞争
class ArpTable {
public:
    static constexpr size_t kNumShards = 256;
    static constexpr uint64_t ENTRY_TIMEOUT = 300;
    
    static ArpTable& instance() { static ArpTable t; return t; }
    
    void update(IPv4Addr ip, const MacAddr& mac, uint64_t now_tsc) {
        auto &shard = shards_[hash_ip(ip) % kNumShards];
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.table.find(ip);
        if (it != shard.table.end() && it->second.complete &&
            it->second.mac == mac) {
            it->second.timestamp = now_tsc;
            return;
        }
        shard.table[ip] = ArpEntry(mac, now_tsc);
    }
    
    bool lookup(IPv4Addr ip, MacAddr& mac) const {
        auto &shard = shards_[hash_ip(ip) % kNumShards];
        std::lock_guard<std::mutex> lock(shard.mutex);
        auto it = shard.table.find(ip);
        if (it != shard.table.end() && it->second.complete) {
            mac = it->second.mac;
            return true;
        }
        return false;
    }
    
private:
    ArpTable() = default;
    static size_t hash_ip(IPv4Addr ip) {
        return std::hash<uint32_t>{}(ip);
    }
    struct Shard {
        std::mutex mutex;
        std::unordered_map<IPv4Addr, ArpEntry> table;
    };
    mutable std::array<Shard, kNumShards> shards_;
};

/// ARP 协议处理类
class ArpHandler {
public:
    static bool handle(EthernetHeader* eth, ArpHeader* arp,
                       IPv4Addr local_ip, const MacAddr& local_mac,
                       uint64_t now_tsc) {
        if (arp->is_request()) {
            return handle_request(eth, arp, local_ip, local_mac, now_tsc);
        }
        if (arp->is_reply()) {
            MacAddr mac; memcpy(mac.data(), arp->sender_mac, 6);
            ArpTable::instance().update(arp->sender_ip, mac, now_tsc);
        }
        return false;
    }
    
    static bool handle_request(EthernetHeader* eth, ArpHeader* arp,
                                IPv4Addr local_ip, const MacAddr& local_mac,
                                uint64_t now_tsc) {
        if (arp->target_ip != local_ip) return false;
        
        MacAddr sender_mac; memcpy(sender_mac.data(), arp->sender_mac, 6);
        ArpTable::instance().update(arp->sender_ip, sender_mac, now_tsc);
        
        eth->swap_mac();
        eth->set_src_mac(local_mac);
        
        arp->set_operation(ArpOperation::REPLY);
        memcpy(arp->target_mac, arp->sender_mac, 6);
        arp->target_ip = arp->sender_ip;
        memcpy(arp->sender_mac, local_mac.data(), 6);
        arp->sender_ip = local_ip;
        
        return true;
    }
    
    static size_t build_request(uint8_t* buf, IPv4Addr target_ip,
                                 IPv4Addr local_ip, const MacAddr& local_mac) {
        auto* eth = reinterpret_cast<EthernetHeader*>(buf);
        auto* arp = reinterpret_cast<ArpHeader*>(buf + sizeof(EthernetHeader));
        
        memset(eth->dst_mac, 0xFF, 6);
        memcpy(eth->src_mac, local_mac.data(), 6);
        eth->set_ether_type(static_cast<uint16_t>(EtherType::ARP));
        
        arp->hw_type = htons(1);
        arp->proto_type = htons(0x0800);
        arp->hw_len = 6; arp->proto_len = 4;
        arp->set_operation(ArpOperation::REQUEST);
        memcpy(arp->sender_mac, local_mac.data(), 6);
        arp->sender_ip = local_ip;
        memset(arp->target_mac, 0, 6);
        arp->target_ip = target_ip;
        
        return sizeof(EthernetHeader) + sizeof(ArpHeader);
    }
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_ARP_H
