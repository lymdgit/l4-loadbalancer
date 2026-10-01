/**
 * @file icmp.h
 * @brief ICMP 协议处理（实现 Ping 功能）
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_PROTOCOL_ICMP_H
#define L4LB_PROTOCOL_ICMP_H

#include <cstddef>
#include <cstdint>
#include "common/types.h"

namespace l4lb {

/// ICMP 类型
enum class IcmpType : uint8_t {
    ECHO_REPLY   = 0,
    DEST_UNREACH = 3,
    ECHO_REQUEST = 8,
    TIME_EXCEEDED = 11,
};

/// ICMP 头结构
struct __attribute__((packed)) IcmpHeader {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
    
    bool is_echo_request() const { return type == static_cast<uint8_t>(IcmpType::ECHO_REQUEST); }
    bool is_echo_reply() const { return type == static_cast<uint8_t>(IcmpType::ECHO_REPLY); }
};

static_assert(sizeof(IcmpHeader) == 8, "IcmpHeader size must be 8 bytes");

/// ICMP 处理类（实现见 src/protocol/icmp.cpp）
class IcmpHandler {
public:
    /// 计算校验和
    static uint16_t calculate_checksum(const uint8_t* data, size_t len);

    /**
     * @brief 处理 ICMP Echo Request，原地改写为 Echo Reply
     * 
     * @param icmp ICMP 头指针
     * @param icmp_len ICMP 数据长度（含数据）
     * @return true 需要发送响应
     */
    static bool handle_echo_request(IcmpHeader* icmp, size_t icmp_len);
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_ICMP_H
