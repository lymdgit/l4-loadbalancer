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
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
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
    bool is_reply() const { return get_operation() == ArpOperation::REPLY; }
};

static_assert(sizeof(ArpHeader) == 28, "ArpHeader size must be 28 bytes");

/// ARP 表条目
struct ArpEntry {
    MacAddr mac;
    uint64_t timestamp;
    bool complete;

    ArpEntry() : mac{}, timestamp(0), complete(false) {}
    explicit ArpEntry(const MacAddr& m);
};

/// ARP 表管理类 - 分片减少锁竞争（实现见 src/protocol/arp.cpp）
class ArpTable {
public:
    static constexpr size_t kNumShards = 256;
    static constexpr uint64_t ENTRY_TIMEOUT = 300;

    static ArpTable& instance() { static ArpTable t; return t; }

    /// 学习或刷新一条 IP -> MAC 映射
    void update(IPv4Addr ip, const MacAddr& mac);

    /// 查找 IP 对应的 MAC，找到返回 true
    bool lookup(IPv4Addr ip, MacAddr& mac) const;

private:
    ArpTable() = default;
    static size_t hash_ip(IPv4Addr ip);

    struct Shard {
        std::mutex mutex;
        std::unordered_map<IPv4Addr, ArpEntry> table;
    };
    mutable std::array<Shard, kNumShards> shards_;
};

/// ARP 协议处理类（实现见 src/protocol/arp.cpp）
class ArpHandler {
public:
    /**
     * @brief 处理一个 ARP 报文
     * @return true 报文已被原地改写为 ARP Reply，需要发送
     */
    static bool handle(EthernetHeader* eth, ArpHeader* arp,
                       IPv4Addr local_ip, const MacAddr& local_mac);

    /// 处理 ARP Request：目标是本机时原地改写为 Reply
    static bool handle_request(EthernetHeader* eth, ArpHeader* arp,
                               IPv4Addr local_ip, const MacAddr& local_mac);

    /**
     * @brief 在 buf 中构造一个广播 ARP Request
     * @return 帧长度
     */
    static size_t build_request(uint8_t* buf, IPv4Addr target_ip,
                                IPv4Addr local_ip, const MacAddr& local_mac);
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_ARP_H
