/**
 * @file arp.h
 * @brief ARP 报文格式与构造
 *
 * 邻居表（IP -> MAC）见 net/neigh.h。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_PROTOCOL_ARP_H
#define L4LB_PROTOCOL_ARP_H

#include <cstddef>
#include <cstdint>
#include "common/types.h"
#include "protocol/ethernet.h"

namespace l4lb {

/// ARP 操作类型
enum class ArpOperation : uint16_t {
    REQUEST = 1,
    REPLY   = 2,
};

/// ARP 报文头结构（以太网 + IPv4）
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

    /// 是否是以太网 + IPv4 的 ARP（其他类型一律忽略）
    bool is_eth_ipv4() const {
        return ntohs(hw_type) == 1 && ntohs(proto_type) == 0x0800 &&
               hw_len == 6 && proto_len == 4;
    }
};

static_assert(sizeof(ArpHeader) == 28, "ArpHeader size must be 28 bytes");

/// ARP 报文构造（实现见 src/protocol/arp.cpp）
class ArpHandler {
public:
    /**
     * @brief 把收到的 ARP Request 原地改写为 Reply
     *
     * 调用方负责判断 target_ip 是否是本机地址。
     */
    static void make_reply(EthernetHeader* eth, ArpHeader* arp,
                           const MacAddr& local_mac);

    /**
     * @brief 在 buf 中构造一个广播 ARP Request
     * @return 帧长度
     */
    static size_t build_request(uint8_t* buf, IPv4Addr target_ip,
                                IPv4Addr local_ip, const MacAddr& local_mac);

    /**
     * @brief 构造免费 ARP（gratuitous ARP request，sender = target = local_ip）
     *
     * 启动和地址变更时广播，刷新交换机和邻居的 ARP 缓存。
     */
    static size_t build_gratuitous(uint8_t* buf, IPv4Addr local_ip,
                                   const MacAddr& local_mac);
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_ARP_H
