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

/// 协议解析器
class ProtocolParser {
public:
    /**
     * @brief 解析一个以太网帧
     *
     * 非 IPv4 帧只填充 L2 信息并返回 true。
     *
     * @param pkt 帧起始地址
     * @param len 帧长度
     * @param meta [out] 解析结果
     * @return false 长度不足以容纳以太网头或 IPv4 头
     */
    static bool parse(const uint8_t* pkt, size_t len, PacketMeta& meta);
};

} // namespace l4lb

#endif // L4LB_PROTOCOL_PARSER_H
