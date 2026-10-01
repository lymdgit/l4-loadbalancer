/**
 * @file ip.h
 * @brief IPv4 / TCP / UDP 协议头结构定义
 *
 * 只包含协议头的内存布局和字段访问方法。
 * 校验和见 protocol/checksum.h，报文解析见 protocol/parser.h。
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_PROTOCOL_IP_H
#define L4LB_PROTOCOL_IP_H

#include <cstdint>
#include "common/types.h"
#include "protocol/ethernet.h"

namespace l4lb {

/// IPv4 头结构
struct __attribute__((packed)) IPv4Header {
    uint8_t  version_ihl;      // 版本(4) + 头长度(4)
    uint8_t  tos;              // 服务类型
    uint16_t total_length;     // 总长度
    uint16_t identification;   // 标识
    uint16_t flags_fragment;   // 标志(3) + 片偏移(13)
    uint8_t  ttl;              // 生存时间
    uint8_t  protocol;         // 协议
    uint16_t checksum;         // 头校验和
    uint32_t src_ip;           // 源IP
    uint32_t dst_ip;           // 目的IP
    
    uint8_t get_version() const { return (version_ihl >> 4) & 0x0F; }
    uint8_t get_ihl() const { return version_ihl & 0x0F; }
    size_t get_header_len() const { return get_ihl() * 4; }
    uint16_t get_total_length() const { return ntohs(total_length); }
    
    bool is_tcp() const { return protocol == static_cast<uint8_t>(IPProtocol::TCP); }
    bool is_udp() const { return protocol == static_cast<uint8_t>(IPProtocol::UDP); }
    bool is_icmp() const { return protocol == static_cast<uint8_t>(IPProtocol::ICMP); }
    
    void set_src_ip(uint32_t ip) { src_ip = ip; }
    void set_dst_ip(uint32_t ip) { dst_ip = ip; }
    
    void swap_ip() {
        uint32_t tmp = src_ip;
        src_ip = dst_ip;
        dst_ip = tmp;
    }
};

static_assert(sizeof(IPv4Header) == 20, "IPv4Header size must be 20 bytes");

/// TCP 头结构
struct __attribute__((packed)) TcpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;    // 数据偏移(4) + 保留(4)
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
    
    uint16_t get_src_port() const { return ntohs(src_port); }
    uint16_t get_dst_port() const { return ntohs(dst_port); }
    void set_src_port(uint16_t p) { src_port = htons(p); }
    void set_dst_port(uint16_t p) { dst_port = htons(p); }
    size_t get_header_len() const { return ((data_offset >> 4) & 0x0F) * 4; }
};

static_assert(sizeof(TcpHeader) == 20, "TcpHeader size must be 20 bytes");

/// UDP 头结构
struct __attribute__((packed)) UdpHeader {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
    
    uint16_t get_src_port() const { return ntohs(src_port); }
    uint16_t get_dst_port() const { return ntohs(dst_port); }
    void set_src_port(uint16_t p) { src_port = htons(p); }
    void set_dst_port(uint16_t p) { dst_port = htons(p); }
    uint16_t get_length() const { return ntohs(length); }
};

static_assert(sizeof(UdpHeader) == 8, "UdpHeader size must be 8 bytes");

} // namespace l4lb

#endif // L4LB_PROTOCOL_IP_H
