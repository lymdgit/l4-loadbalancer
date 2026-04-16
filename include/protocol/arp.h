/**
 * @file arp.h
 * @brief ARP 协议处理
 *
 * 无锁化优化：
 * ArpTable 原来有 256 个分片，每个分片一把 std::mutex，每个数据包都要加锁。
 *
 * 新设计：per-lcore 无锁 ARP 缓存
 *   - 利用 DPDK RSS 对称哈希：同一 TCP 连接的正向流量（Client→LB）和反向
 *     流量（RS→LB）被哈希到同一个 RX 队列，因此由同一个 lcore 处理。
 *   - 每个 lcore 只访问自己的缓存槽，完全无需加锁。
 *   - update() 只被当前 lcore 调用，lookup() 也只查当前 lcore 的表。
 */

#ifndef L4LB_PROTOCOL_ARP_H
#define L4LB_PROTOCOL_ARP_H

#include <cstdint>
#include <cstring>
#include <unordered_map>

#include <rte_cycles.h>
#include <rte_lcore.h>

#include "common/logger.h"
#include "common/types.h"
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
    bool is_reply()   const { return get_operation() == ArpOperation::REPLY;   }
};

static_assert(sizeof(ArpHeader) == 28, "ArpHeader size must be 28 bytes");

/**
 * @brief Per-lcore 无锁 ARP 缓存
 *
 * 并发模型：
 *   - 每个 lcore 只读写自己的 caches_[lcore_id] 槽，无跨核访问
 *   - 无 mutex，无 spinlock，无 atomic
 *
 * 为什么 per-lcore 是正确的：
 *   DPDK RSS + 对称 Toeplitz 哈希确保：
 *     Client:SrcPort → LB (forward) 与 RS:DstPort → LB (return)
 *   被哈希到同一个 RX 队列 → 同一个 lcore 处理。
 *   因此，插入 client MAC 的 lcore 就是后续查找该 MAC 的 lcore，天然隔离。
 */
class ArpTable {
public:
    static ArpTable& instance() { static ArpTable t; return t; }

    /**
     * @brief 更新 ARP 缓存（无锁，只访问当前 lcore 的槽）
     */
    void update(IPv4Addr ip, const MacAddr& mac) {
        caches_[safe_lcore_id()].table[ip] = mac;
    }

    /**
     * @brief 查找 MAC 地址（无锁，只访问当前 lcore 的槽）
     */
    bool lookup(IPv4Addr ip, MacAddr& mac) const {
        const auto& tbl = caches_[safe_lcore_id()].table;
        auto it = tbl.find(ip);
        if (it == tbl.end()) return false;
        mac = it->second;
        return true;
    }

private:
    ArpTable() = default;

    static unsigned safe_lcore_id() {
        unsigned id = rte_lcore_id();
        // LCORE_ID_ANY (0xFFFFFFFF) 表示非 DPDK lcore 上下文，回退到槽 0
        return (id < RTE_MAX_LCORE) ? id : 0;
    }

    /**
     * @brief Per-lcore 缓存槽
     *
     * alignas(RTE_CACHE_LINE_SIZE) 保证每个槽起始于不同 cache line，
     * 彻底消除 false sharing。
     * unordered_map 在堆上，槽本身只含指针，64 字节对齐足够。
     */
    struct alignas(RTE_CACHE_LINE_SIZE) LcoreCache {
        std::unordered_map<IPv4Addr, MacAddr> table;
    };

    LcoreCache caches_[RTE_MAX_LCORE];
};

/// ARP 协议处理类
class ArpHandler {
public:
    static bool handle(EthernetHeader* eth, ArpHeader* arp,
                       IPv4Addr local_ip, const MacAddr& local_mac) {
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

    static bool handle_request(EthernetHeader* eth, ArpHeader* arp,
                                IPv4Addr local_ip, const MacAddr& local_mac) {
        if (arp->target_ip != local_ip) return false;

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

    static size_t build_request(uint8_t* buf, IPv4Addr target_ip,
                                 IPv4Addr local_ip, const MacAddr& local_mac) {
        auto* eth = reinterpret_cast<EthernetHeader*>(buf);
        auto* arp = reinterpret_cast<ArpHeader*>(buf + sizeof(EthernetHeader));

        memset(eth->dst_mac, 0xFF, 6);
        memcpy(eth->src_mac, local_mac.data(), 6);
        eth->set_ether_type(static_cast<uint16_t>(EtherType::ARP));

        arp->hw_type    = htons(1);
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
