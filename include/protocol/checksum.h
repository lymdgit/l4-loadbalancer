/**
 * @file checksum.h
 * @brief IP / TCP / UDP 校验和计算
 *
 * - 增量更新（RFC 1624）在每个转发包上都会调用，保留为头文件内联实现
 * - 全量计算只在少数路径使用，实现在 src/protocol/checksum.cpp
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_PROTOCOL_CHECKSUM_H
#define L4LB_PROTOCOL_CHECKSUM_H

#include "protocol/ip.h"
#include <cstddef>
#include <cstdint>

namespace l4lb {

/// IP 校验和计算
class IpChecksum {
public:
    /// 全量计算一段数据的反码和校验和
    static uint16_t calculate(const uint8_t* data, size_t len);

    /// 全量重新计算 IP 头校验和
    static void update(IPv4Header* ip);

    /// 增量更新（高效）
    static inline uint16_t incremental_update(uint16_t old_sum, uint16_t old_val,
                                              uint16_t new_val) {
        uint32_t sum = (~old_sum & 0xFFFF) + (~old_val & 0xFFFF) + new_val;
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        return ~sum;
    }
};

/**
 * @brief TCP/UDP 校验和更新工具
 *
 * TCP/UDP 校验和是基于伪头部（包含 IP 地址）计算的，
 * 所以当修改 IP 地址或端口时必须更新校验和。
 */
class L4Checksum {
public:
    /**
     * @brief 增量更新校验和
     *
     * 基于 RFC 1624 的增量校验和更新算法
     */
    static inline uint16_t incremental_update(uint16_t old_sum, uint16_t old_val,
                                              uint16_t new_val) {
        uint32_t sum = (~old_sum & 0xFFFF) + (~old_val & 0xFFFF) + new_val;
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        return ~sum;
    }

    /// 更新 TCP 校验和（修改 IP 地址时）
    static inline void update_tcp_checksum_ip(TcpHeader* tcp, uint32_t old_ip,
                                              uint32_t new_ip) {
        // TCP 校验和包含 IP 伪头部，需要更新
        tcp->checksum =
            incremental_update(tcp->checksum, old_ip >> 16, new_ip >> 16);
        tcp->checksum =
            incremental_update(tcp->checksum, old_ip & 0xFFFF, new_ip & 0xFFFF);
    }

    /// 更新 TCP 校验和（修改端口时）
    static inline void update_tcp_checksum_port(TcpHeader* tcp, uint16_t old_port,
                                                uint16_t new_port) {
        tcp->checksum = incremental_update(tcp->checksum, old_port, new_port);
    }

    /// 更新 UDP 校验和（修改 IP 地址时）
    static inline void update_udp_checksum_ip(UdpHeader* udp, uint32_t old_ip,
                                              uint32_t new_ip) {
        if (udp->checksum == 0)
            return; // UDP 校验和可选
        udp->checksum =
            incremental_update(udp->checksum, old_ip >> 16, new_ip >> 16);
        udp->checksum =
            incremental_update(udp->checksum, old_ip & 0xFFFF, new_ip & 0xFFFF);
        if (udp->checksum == 0)
            udp->checksum = 0xFFFF; // 避免校验和为 0
    }

    /// 更新 UDP 校验和（修改端口时）
    static inline void update_udp_checksum_port(UdpHeader* udp, uint16_t old_port,
                                                uint16_t new_port) {
        if (udp->checksum == 0)
            return;
        udp->checksum = incremental_update(udp->checksum, old_port, new_port);
        if (udp->checksum == 0)
            udp->checksum = 0xFFFF;
    }

    /// 计算伪头部校验和（部分和）
    static uint32_t calculate_pseudo_header_sum(uint32_t src_ip, uint32_t dst_ip,
                                                uint8_t protocol, uint16_t length);

    /// 全量重新计算 IP 校验和
    static void recalculate_ip_checksum(IPv4Header* ip);

    /// 全量重新计算 TCP 校验和
    static void recalculate_tcp_checksum(IPv4Header* ip, TcpHeader* tcp,
                                         size_t tcp_len);

    /// 全量重新计算 UDP 校验和
    static void recalculate_udp_checksum(IPv4Header* ip, UdpHeader* udp);
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_CHECKSUM_H
