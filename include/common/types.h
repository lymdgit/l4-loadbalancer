/**
 * @file types.h
 * @brief 公共类型定义
 *
 * 本文件定义了负载均衡器中使用的所有核心数据结构，包括：
 * - 五元组 (FiveTuple): 用于标识一个网络连接
 * - 数据包元信息 (PacketMeta): 解析后的数据包信息
 * - 后端服务器 (RealServer): 真实服务器信息
 * - 会话信息 (Session): 连接会话状态
 *
 * @author L4 Load Balancer Project
 * @date 2024
 */

#ifndef L4LB_COMMON_TYPES_H
#define L4LB_COMMON_TYPES_H

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
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
 * @brief 五元组结构
 *
 * 五元组是标识一个 TCP/UDP 连接的关键信息：
 * - 源 IP 地址
 * - 源端口
 * - 目的 IP 地址
 * - 目的端口
 * - 协议类型 (TCP/UDP)
 *
 * 用于：
 * 1. 一致性哈希选择后端服务器
 * 2. 会话表查找
 * 3. 连接跟踪
 */
/**
 * @brief 五元组结构 (packed, 用于 rte_hash memcmp key)
 *
 * 使用 __attribute__((packed)) 消除 padding 字节，
 * 构造时用 memset 保证所有字节为 0，
 * 确保 rte_hash 的 memcmp 比较完全正确。
 */
struct __attribute__((packed)) FiveTuple {
  IPv4Addr src_ip;  ///< 源 IP 地址 (网络字节序)
  IPv4Addr dst_ip;  ///< 目的 IP 地址 (网络字节序)
  Port src_port;    ///< 源端口 (网络字节序)
  Port dst_port;    ///< 目的端口 (网络字节序)
  uint8_t protocol; ///< 协议类型 (TCP=6, UDP=17)

  /**
   * @brief 默认构造函数
   */
  FiveTuple() {
    memset(this, 0, sizeof(*this));
  }

  /**
   * @brief 参数化构造函数
   */
  FiveTuple(IPv4Addr sip, IPv4Addr dip, Port sp, Port dp, uint8_t proto) {
    memset(this, 0, sizeof(*this));
    src_ip = sip;
    dst_ip = dip;
    src_port = sp;
    dst_port = dp;
    protocol = proto;
  }

  /**
   * @brief 相等比较运算符
   *
   * 用于会话表的查找操作
   */
  bool operator==(const FiveTuple &other) const {
    return memcmp(this, &other, sizeof(FiveTuple)) == 0;
  }

  /**
   * @brief 生成反向五元组
   *
   * 用于构建返回方向的会话条目
   *
   * @return 反向的五元组（源和目的交换）
   */
  FiveTuple reverse() const {
    return FiveTuple(dst_ip, src_ip, dst_port, src_port, protocol);
  }
};

/**
 * @brief 五元组的哈希函数 - 快速 FNV-1a 风格混合
 */
// 快速哈希函数，用于计算五元组的哈希值
// 编译器回在unordered_map编译的时候，内联这个函数，硬塞进去find源码里
// 避免了函数跳转的开销
// FNV-1a风格的hash算法，速度快，碰撞率低   异或然后乘以一个大的素数
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
 * - 零拷贝设计：保存指针而非拷贝数据
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
  Port src_port; ///< 源端口
  Port dst_port; ///< 目的端口

  // 各层头部偏移量（用于零拷贝修改）
  uint16_t l2_offset;      ///< 以太网头偏移
  uint16_t l3_offset;      ///< IP 头偏移
  uint16_t l4_offset;      ///< TCP/UDP 头偏移
  uint16_t payload_offset; ///< 载荷偏移

  // 长度信息
  uint16_t total_len;   ///< 数据包总长度
  uint16_t payload_len; ///< 载荷长度

  /**
   * @brief 提取五元组
   *
   * @return 从元信息构建的五元组
   */
  FiveTuple to_five_tuple() const {
    return FiveTuple(src_ip, dst_ip, src_port, dst_port, ip_protocol);
  }
};

// ============================================================================
// 后端服务器定义
// ============================================================================

/**
 * @brief Real Server 结构
 *
 * 表示一个后端真实服务器的信息
 */
struct RealServer {
  uint32_t id;         ///< 服务器唯一 ID
  IPv4Addr ip;         ///< 服务器 IP 地址
  Port port;           ///< 服务端口
  MacAddr mac;         ///< MAC 地址（用于 DR 模式）
  uint32_t weight;     ///< 权重（影响流量分配比例）
  ServerStatus status; ///< 服务器状态

  // 统计信息
  uint64_t conn_count; ///< 当前连接数
  uint64_t total_conn; ///< 总连接数
  uint64_t bytes_in;   ///< 入站字节数
  uint64_t bytes_out;  ///< 出站字节数

  /**
   * @brief 默认构造函数
   */
  RealServer()
      : id(0), ip(0), port(0), mac{}, weight(100),
        status(ServerStatus::CHECKING), conn_count(0), total_conn(0),
        bytes_in(0), bytes_out(0) {}

  /**
   * @brief 检查服务器是否可用
   */
  bool is_available() const { return status == ServerStatus::UP; }
};

// ============================================================================
// 会话信息定义
// ============================================================================

/**
 * @brief 会话结构
 *
 * 记录一个连接的状态信息，用于：
 * 1. 会话保持：同一连接的所有包发往同一后端
 * 2. NAT 连接跟踪：记录地址转换信息
 */
struct Session {
  FiveTuple client_tuple;  ///< 客户端五元组（原始）
  FiveTuple server_tuple;  ///< 后端五元组（转换后）
  uint32_t real_server_id; ///< 分配的后端服务器 ID
  Port nat_src_port;       ///< SNAT 源端口(网络字节序, 0 表示未做端口转换)
  uint64_t create_time;    ///< 创建时间（TSC）
  uint64_t last_active;    ///< 最后活跃时间（TSC）
  uint64_t packets;        ///< 数据包计数
  uint64_t bytes;          ///< 字节计数

  /**
   * @brief 更新活跃时间（使用 TSC，避免 chrono 系统调用）
   */
  void touch(uint64_t tsc) { last_active = tsc; }

  /**
   * @brief 检查会话是否过期
   * @param now_tsc 当前 TSC
   * @param timeout_tsc 超时 TSC 间隔 (rte_get_tsc_hz() * timeout_sec)
   */
  bool is_expired(uint64_t now_tsc, uint64_t timeout_tsc) const {
    return (now_tsc - last_active) > timeout_tsc;
  }
};

// ============================================================================
// 统计信息
// ============================================================================

/**
 * @brief 全局统计信息
 *
 * 用于监控和调优
 */
struct Statistics {
  // 数据包计数
  uint64_t rx_packets;      ///< 接收数据包数
  uint64_t tx_packets;      ///< 发送数据包数
  uint64_t dropped_packets; ///< 丢弃数据包数

  // 协议分类计数
  uint64_t arp_packets;  ///< ARP 数据包数
  uint64_t icmp_packets; ///< ICMP 数据包数
  uint64_t tcp_packets;  ///< TCP 数据包数
  uint64_t udp_packets;  ///< UDP 数据包数

  // 转发统计
  uint64_t forwarded_packets; ///< 成功转发数
  uint64_t nat_translations;  ///< NAT 转换次数

  // 会话统计
  uint64_t active_sessions; ///< 当前活跃会话
  uint64_t total_sessions;  ///< 总会话数

  /**
   * @brief 重置所有计数器
   */
  void reset() { std::memset(this, 0, sizeof(Statistics)); }
};

// ============================================================================
// 工具函数
// ============================================================================

/**
 * @brief IP 地址字符串转网络字节序
 *
 * 注意：返回的是网络字节序（大端），与数据包中的格式一致
 *
 * @param ip_str IP 地址字符串 (如 "192.168.1.1")
 * @return 网络字节序的 IP 地址
 */
inline IPv4Addr ip_from_string(const std::string &ip_str) {
  uint32_t a, b, c, d;
  if (sscanf(ip_str.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
    return 0;
  }
  // 存储为网络字节序（大端）：第一个字节在最低地址
  // 在小端机器上，(d << 24) | (c << 16) | (b << 8) | a 会产生网络字节序
  return (d << 24) | (c << 16) | (b << 8) | a;
}

/**
 * @brief 网络字节序 IP 转字符串
 *
 * @param ip 网络字节序的 IP 地址
 * @return IP 地址字符串
 */
inline std::string ip_to_string(IPv4Addr ip) {
  char buf[16];
  // ip 是网络字节序，最低字节是第一段
  snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip & 0xFF, (ip >> 8) & 0xFF,
           (ip >> 16) & 0xFF, (ip >> 24) & 0xFF);
  return std::string(buf);
}

/**
 * @brief MAC 地址字符串转字节数组
 *
 * @param mac_str MAC 地址字符串 (如 "00:0C:29:3E:38:92")
 * @return MAC 地址数组
 */
inline MacAddr mac_from_string(const std::string &mac_str) {
  MacAddr mac{};
  unsigned int a[6];
  if (sscanf(mac_str.c_str(), "%x:%x:%x:%x:%x:%x", &a[0], &a[1], &a[2], &a[3],
             &a[4], &a[5]) == 6) {
    for (int i = 0; i < 6; ++i) {
      mac[i] = static_cast<uint8_t>(a[i]);
    }
  }
  return mac;
}

/**
 * @brief MAC 地址转字符串
 *
 * @param mac MAC 地址数组
 * @return MAC 地址字符串
 */
inline std::string mac_to_string(const MacAddr &mac) {
  char buf[18];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return std::string(buf);
}

} // namespace l4lb

#endif // L4LB_COMMON_TYPES_H
