/**
 * @file icmp.h
 * @brief ICMP 协议处理（实现 Ping 功能）
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_PROTOCOL_ICMP_H
#define L4LB_PROTOCOL_ICMP_H

#include <cstdint>
#include "common/types.h"
#include "protocol/ethernet.h"

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

/// ICMP 处理类
class IcmpHandler {
public:
    /**
     * @brief 计算校验和
     */
    static uint16_t calculate_checksum(const uint8_t* data, size_t len) {
        uint32_t sum = 0;
        const uint16_t* ptr = reinterpret_cast<const uint16_t*>(data);
        
        while (len > 1) {
            sum += *ptr++;
            len -= 2;
        }
        if (len == 1) {
            sum += *reinterpret_cast<const uint8_t*>(ptr);
        }
        
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        return static_cast<uint16_t>(~sum);
    }
    
    /**
     * @brief 处理 ICMP Echo Request，生成 Echo Reply
     * 
     * @param icmp ICMP 头指针
     * @param icmp_len ICMP 数据长度（含数据）
     * @return true 需要发送响应
     */
    static bool handle_echo_request(IcmpHeader* icmp, size_t icmp_len) {
        if (!icmp->is_echo_request()) return false;
        
        // 修改类型为 Echo Reply
        icmp->type = static_cast<uint8_t>(IcmpType::ECHO_REPLY);
        icmp->code = 0;
        
        // 重新计算校验和
        icmp->checksum = 0;
        icmp->checksum = calculate_checksum(
            reinterpret_cast<uint8_t*>(icmp), icmp_len);
        
        return true;
    }
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_ICMP_H
