/**
 * @file parser.h
 * @brief 报文解析：从原始帧中提取 L2/L3/L4 元信息
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_PROTOCOL_PARSER_H
#define L4LB_PROTOCOL_PARSER_H

#include "common/types.h"
#include <cstddef>
#include <cstdint>

namespace l4lb {

/// 解析结果
enum class ParseResult {
    OK,        ///< 解析成功，meta 中所有已解析层的偏移都在包长范围内
    MALFORMED, ///< 长度不足或头部字段非法，必须丢弃
    FRAGMENT,  ///< IPv4 分片（暂不支持重组），必须丢弃
};

/**
 * @brief ICMP 差错报文中内嵌的原始报文信息
 *
 * ICMP 差错（type 3/11/12）的载荷是触发差错的那个报文的 IP 头 + 至少 8 字节 L4 头，
 * 正好包含端口，可以据此找到对应的会话。
 */
struct IcmpErrorInfo {
    uint16_t inner_l3_offset;   ///< 内层 IP 头偏移（相对帧首）
    uint16_t inner_l4_offset;   ///< 内层 L4 头偏移
    FiveTuple inner;            ///< 内层报文的五元组（原始方向）
};

/// 协议解析器
class ProtocolParser {
public:
    /**
     * @brief 解析一个以太网帧
     *
     * 非 IPv4 帧只填充 L2 信息并返回 OK。
     *
     * IPv4 校验项：version == 4、IHL >= 5、total_length 与帧长一致；
     * TCP data offset >= 5 且头部完整；UDP length >= 8 且不超出 IP 载荷；
     * ICMP 头部完整。meta.total_len 为 L2 头 + IP total_length
     * （不含以太网尾部填充）。
     *
     * @param pkt 帧起始地址
     * @param len 帧长度
     * @param meta [out] 解析结果
     */
    static ParseResult parse(const uint8_t* pkt, size_t len, PacketMeta& meta);

    /**
     * @brief 解析 ICMP 差错报文的内层报文头
     *
     * meta 必须是 parse() 成功返回的 ICMP 报文。只接受内层为 TCP/UDP、
     * 且内层 IP 头 + 8 字节 L4 头完整的报文。
     */
    static bool parse_icmp_error(const uint8_t* pkt, const PacketMeta& meta,
                                 IcmpErrorInfo& info);
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_PARSER_H
