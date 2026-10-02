/**
 * @file types.h
 * @brief 公共类型定义
 *
 * 本文件定义了负载均衡器中使用的所有核心数据结构，包括：
 * - 五元组 (FiveTuple): 用于标识一个网络连接
 * - 数据包元信息 (PacketMeta): 解析后的数据包信息
 *
 * 后端服务器见 ctrl/snapshot.h，会话见 lb/session.h，统计见 common/stats.h
 *
 * @author L4 Load Balancer Project
 * @date 2024
 */

#ifndef L4LB_COMMON_TYPES_H
#define L4LB_COMMON_TYPES_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace l4lb {

// ============================================================================
// 基础类型定义
// ============================================================================

/// MAC 地址长度
constexpr size_t MAC_ADDR_LEN = 6;

/// IPv4 地址类型
using IPv4Addr = uint32_t;

/// 端口类型
using Port = uint16_t;

/// MAC 地址类型
using MacAddr = std::array<uint8_t, MAC_ADDR_LEN>;

// ============================================================================
// 协议类型枚举
// ============================================================================

/**
 * @brief 以太网类型枚举
 *
 * 定义以太网帧中 EtherType 字段的常用值
 */
enum class EtherType : uint16_t {
  IPv4 = 0x0800, ///< IPv4 协议
  IPv6 = 0x86DD, ///< IPv6 协议
  ARP = 0x0806,  ///< ARP 协议
  VLAN = 0x8100, ///< VLAN 标签
};

/**
 * @brief IP 协议类型枚举
 */
enum class IPProtocol : uint8_t {
  ICMP = 1, ///< ICMP 协议
  TCP = 6,  ///< TCP 协议
  UDP = 17, ///< UDP 协议
};

/**
 * @brief 转发模式枚举
 */
enum class ForwardMode {
  NAT, ///< 网络地址转换模式
  DR,  ///< 直接路由模式
};

/**
 * @brief 服务器状态枚举
 */
enum class ServerStatus {
  UP,       ///< 服务器正常
  DOWN,     ///< 服务器宕机
  CHECKING, ///< 健康检查中
};

// ============================================================================
// 错误码定义
// ============================================================================

/**
 * @brief 错误码枚举
 *
 * 统一的错误码定义，便于错误处理和日志记录
 */
enum class ErrorCode : int {
  SUCCESS = 0,           ///< 成功
  ERR_INVALID_PACKET,    ///< 无效数据包
  ERR_CHECKSUM_FAILED,   ///< 校验和错误
  ERR_NO_BACKEND,        ///< 无可用后端
  ERR_SESSION_NOT_FOUND, ///< 会话未找到
  ERR_MEMORY_ALLOC,      ///< 内存分配失败
  ERR_CONFIG_INVALID,    ///< 配置无效
  ERR_INIT_FAILED,       ///< 初始化失败
};

// ============================================================================
// 五元组定义
// ============================================================================

/**
 * @brief 五元组结构 (packed, 用于 rte_hash memcmp key)
 *
 * 五元组是标识一个 TCP/UDP 连接的关键信息，用于：
 * 1. 一致性哈希选择后端服务器
 * 2. 会话表查找
 * 3. 连接跟踪
 *
 * 使用 __attribute__((packed)) 消除 padding 字节，
 * 构造时用 memset 保证所有字节为 0，
 * 确保 rte_hash 的 memcmp 比较完全正确。
 *
 * 热路径上每包都会构造和比较，因此保留为头文件内联实现。
 */
struct __attribute__((packed)) FiveTuple {
  IPv4Addr src_ip;  ///< 源 IP 地址 (网络字节序)
  IPv4Addr dst_ip;  ///< 目的 IP 地址 (网络字节序)
  Port src_port;    ///< 源端口 (网络字节序)
  Port dst_port;    ///< 目的端口 (网络字节序)
  uint8_t protocol; ///< 协议类型 (TCP=6, UDP=17)

  FiveTuple() { memset(this, 0, sizeof(*this)); }

  FiveTuple(IPv4Addr sip, IPv4Addr dip, Port sp, Port dp, uint8_t proto) {
    memset(this, 0, sizeof(*this));
    src_ip = sip;
    dst_ip = dip;
    src_port = sp;
    dst_port = dp;
    protocol = proto;
  }

  /// 相等比较运算符，用于会话表的查找操作
  bool operator==(const FiveTuple &other) const {
    return memcmp(this, &other, sizeof(FiveTuple)) == 0;
  }

  /// 打包成 3 个 32 位字（用于哈希，避免按 13 字节读取时越界）
  void words(uint32_t w[3]) const {
    w[0] = src_ip;
    w[1] = dst_ip;
    w[2] = (static_cast<uint32_t>(src_port) << 16) ^ dst_port ^
           (static_cast<uint32_t>(protocol) << 8);
  }

  /// 生成反向五元组（源和目的交换）
  FiveTuple reverse() const {
    return FiveTuple(dst_ip, src_ip, dst_port, src_port, protocol);
  }
};

/**
 * @brief 五元组的哈希函数 - 快速 FNV-1a 风格混合
 *
 * 异或然后乘以一个大的素数，速度快，碰撞率低。
 * unordered_map 查找时会内联这个函数，避免函数跳转开销。
 */
struct FiveTupleHash {
  size_t operator()(const FiveTuple &tuple) const {
    size_t h = 14695981039346656037ULL;
    h = (h ^ tuple.src_ip) * 1099511628211ULL;
    h = (h ^ tuple.dst_ip) * 1099511628211ULL;
    h = (h ^ tuple.src_port) * 1099511628211ULL;
    h = (h ^ tuple.dst_port) * 1099511628211ULL;
    h = (h ^ tuple.protocol) * 1099511628211ULL;
    return h;
  }
};

// ============================================================================
// 数据包元信息
// ============================================================================

/**
 * @brief 数据包元信息结构
 *
 * 解析数据包后提取的关键信息，避免重复解析
 *
 * 设计思想：
 * - 一次解析，多次使用
 * - 零拷贝设计：保存偏移而非拷贝数据
 */
struct PacketMeta {
  // 以太网层信息
  MacAddr src_mac;     ///< 源 MAC 地址
  MacAddr dst_mac;     ///< 目的 MAC 地址
  uint16_t ether_type; ///< 以太网类型

  // IP 层信息
  IPv4Addr src_ip;     ///< 源 IP 地址
  IPv4Addr dst_ip;     ///< 目的 IP 地址
  uint8_t ip_protocol; ///< IP 协议类型
  uint8_t ip_ttl;      ///< TTL

  // 传输层信息
  Port src_port;     ///< 源端口
  Port dst_port;     ///< 目的端口
  uint8_t tcp_flags; ///< TCP 标志位（非 TCP 为 0）

  // 各层头部偏移量（用于零拷贝修改）
  uint16_t l2_offset;      ///< 以太网头偏移
  uint16_t l3_offset;      ///< IP 头偏移
  uint16_t l4_offset;      ///< TCP/UDP 头偏移
  uint16_t payload_offset; ///< 载荷偏移

  // 长度信息
  uint16_t total_len;   ///< 数据包总长度
  uint16_t payload_len; ///< 载荷长度

  /// 提取五元组
  FiveTuple to_five_tuple() const {
    return FiveTuple(src_ip, dst_ip, src_port, dst_port, ip_protocol);
  }
};

// ============================================================================
// 工具函数（实现见 src/common/types.cpp）
// ============================================================================

/**
 * @brief IP 地址字符串转网络字节序
 *
 * 注意：返回的是网络字节序（大端），与数据包中的格式一致
 *
 * @param ip_str IP 地址字符串 (如 "192.168.1.1")
 * @return 网络字节序的 IP 地址，解析失败返回 0
 */
IPv4Addr ip_from_string(const std::string &ip_str);

/// 严格解析 IPv4 地址，失败返回 false
bool parse_ipv4(const std::string &ip_str, IPv4Addr &out);

/**
 * @brief 网络字节序 IP 转字符串
 */
std::string ip_to_string(IPv4Addr ip);

/**
 * @brief MAC 地址字符串转字节数组
 *
 * @param mac_str MAC 地址字符串 (如 "00:0C:29:3E:38:92")
 * @return MAC 地址数组，解析失败返回全 0
 */
MacAddr mac_from_string(const std::string &mac_str);

/// 严格解析 MAC 地址，失败返回 false
bool parse_mac(const std::string &mac_str, MacAddr &out);

/// MAC 是否全 0
inline bool mac_is_zero(const MacAddr &mac) {
  for (auto b : mac)
    if (b)
      return false;
  return true;
}

/**
 * @brief MAC 地址转字符串
 */
std::string mac_to_string(const MacAddr &mac);

} // namespace l4lb

#endif // L4LB_COMMON_TYPES_H
