/**
 * @file ethernet.h
 * @brief 以太网帧处理
 * 
 * 定义以太网帧结构和相关操作函数。
 * 
 * 以太网帧格式：
 * +---------------+---------------+------------+------+
 * | Dst MAC (6B)  | Src MAC (6B)  | Type (2B)  | Data |
 * +---------------+---------------+------------+------+
 * 
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_PROTOCOL_ETHERNET_H
#define L4LB_PROTOCOL_ETHERNET_H

#include <cstdint>
#include <cstring>
#include <arpa/inet.h>  // 使用系统的 ntohs/htons/ntohl/htonl
#include "common/types.h"

namespace l4lb {

/**
 * @brief 以太网帧头结构
 * 
 * 使用 __attribute__((packed)) 确保结构体紧凑排列，
 * 与网络数据包格式完全对应。
 */
struct __attribute__((packed)) EthernetHeader {
    uint8_t  dst_mac[MAC_ADDR_LEN];   ///< 目标 MAC 地址
    uint8_t  src_mac[MAC_ADDR_LEN];   ///< 源 MAC 地址
    uint16_t ether_type;               ///< 以太网类型（网络字节序）
    
    uint16_t get_ether_type() const {
        return ntohs(ether_type);
    }
    
    void set_ether_type(uint16_t type) {
        ether_type = htons(type);
    }
    
    bool is_ipv4() const {
        return get_ether_type() == static_cast<uint16_t>(EtherType::IPv4);
    }
    
    bool is_arp() const {
        return get_ether_type() == static_cast<uint16_t>(EtherType::ARP);
    }
    
    bool is_broadcast() const {
        static const uint8_t broadcast[MAC_ADDR_LEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        return memcmp(dst_mac, broadcast, MAC_ADDR_LEN) == 0;
    }
    
    void swap_mac() {
        uint8_t tmp[MAC_ADDR_LEN];
        memcpy(tmp, dst_mac, MAC_ADDR_LEN);
        memcpy(dst_mac, src_mac, MAC_ADDR_LEN);
        memcpy(src_mac, tmp, MAC_ADDR_LEN);
    }
    
    void set_dst_mac(const MacAddr& mac) {
        memcpy(dst_mac, mac.data(), MAC_ADDR_LEN);
    }
    
    void set_src_mac(const MacAddr& mac) {
        memcpy(src_mac, mac.data(), MAC_ADDR_LEN);
    }
    
    MacAddr get_dst_mac() const {
        MacAddr mac;
        memcpy(mac.data(), dst_mac, MAC_ADDR_LEN);
        return mac;
    }
    
    MacAddr get_src_mac() const {
        MacAddr mac;
        memcpy(mac.data(), src_mac, MAC_ADDR_LEN);
        return mac;
    }
};

static_assert(sizeof(EthernetHeader) == 14, "EthernetHeader size must be 14 bytes");

/**
 * @brief 以太网帧处理工具类
 */
class Ethernet {
public:
    static constexpr size_t HEADER_SIZE = sizeof(EthernetHeader);
    static constexpr size_t MIN_FRAME_SIZE = 60;
    static constexpr size_t MAX_FRAME_SIZE = 1514;
    static constexpr size_t MTU = 1500;
    
    static const EthernetHeader* parse(const uint8_t* data, size_t len) {
        if (len < HEADER_SIZE) return nullptr;
        return reinterpret_cast<const EthernetHeader*>(data);
    }
    
    static EthernetHeader* parse_mutable(uint8_t* data, size_t len) {
        if (len < HEADER_SIZE) return nullptr;
        return reinterpret_cast<EthernetHeader*>(data);
    }
    
    static size_t payload_offset() {
        return HEADER_SIZE;
    }
    
    static bool mac_equal(const uint8_t* mac1, const uint8_t* mac2) {
        return memcmp(mac1, mac2, MAC_ADDR_LEN) == 0;
    }
    
    static bool is_multicast(const uint8_t* mac) {
        return (mac[0] & 0x01) != 0;
    }
    
    static const MacAddr& broadcast_mac() {
        static const MacAddr bcast = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        return bcast;
    }
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_ETHERNET_H

