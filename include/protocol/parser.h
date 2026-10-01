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
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_PARSER_H
