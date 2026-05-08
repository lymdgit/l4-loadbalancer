/**
 * @file ip.h
 * @brief IP/TCP/UDP 协议处理
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

/// IP 校验和计算
class IpChecksum {
public:
    static uint16_t calculate(const uint8_t* data, size_t len) {
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
    
    static void update(IPv4Header* ip) {
        ip->checksum = 0;
        ip->checksum = calculate(reinterpret_cast<uint8_t*>(ip), ip->get_header_len());
    }
    
    /// 增量更新（高效）
    static uint16_t incremental_update(uint16_t old_sum, uint16_t old_val, uint16_t new_val) {
        uint32_t sum = (~old_sum & 0xFFFF) + (~old_val & 0xFFFF) + new_val;
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        return ~sum;
    }
};

/// 协议解析器
class ProtocolParser {
public:
    static bool parse(const uint8_t* pkt, size_t len, PacketMeta& meta) {
        if (len < Ethernet::HEADER_SIZE) return false;
        
        auto* eth = reinterpret_cast<const EthernetHeader*>(pkt);
        memcpy(meta.dst_mac.data(), eth->dst_mac, 6);
        memcpy(meta.src_mac.data(), eth->src_mac, 6);
        meta.ether_type = eth->get_ether_type();
        meta.l2_offset = 0;
        meta.l3_offset = Ethernet::HEADER_SIZE;
        
        if (!eth->is_ipv4()) return true;
        if (len < meta.l3_offset + sizeof(IPv4Header)) return false;
        
        auto* ip = reinterpret_cast<const IPv4Header*>(pkt + meta.l3_offset);
        meta.src_ip = ip->src_ip;
        meta.dst_ip = ip->dst_ip;
        meta.ip_protocol = ip->protocol;
        meta.ip_ttl = ip->ttl;
        meta.l4_offset = meta.l3_offset + ip->get_header_len();
        meta.total_len = len;
        
        if (ip->is_tcp() && len >= meta.l4_offset + sizeof(TcpHeader)) {
            auto* tcp = reinterpret_cast<const TcpHeader*>(pkt + meta.l4_offset);
            meta.src_port = tcp->src_port;
            meta.dst_port = tcp->dst_port;
            meta.payload_offset = meta.l4_offset + tcp->get_header_len();
        } else if (ip->is_udp() && len >= meta.l4_offset + sizeof(UdpHeader)) {
            auto* udp = reinterpret_cast<const UdpHeader*>(pkt + meta.l4_offset);
            meta.src_port = udp->src_port;
            meta.dst_port = udp->dst_port;
            meta.payload_offset = meta.l4_offset + sizeof(UdpHeader);
        } else {
            meta.src_port = 0;
            meta.dst_port = 0;
            meta.payload_offset = meta.l4_offset;
        }
        
        meta.payload_len = len - meta.payload_offset;
        return true;
    }
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_IP_H
