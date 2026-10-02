/**
 * @file nat_forwarder.h
 * @brief 报文改写：FULLNAT 地址/端口转换、DR MAC 改写、TOA、ICMP 差错转换
 *
 * FULLNAT（两个方向都改源和目的）：
 *   入站 Client:cport -> VIP:vport   =>   LIP:nat_port -> RS:rport
 *   回程 RS:rport -> LIP:nat_port    =>   VIP:vport -> Client:cport
 *
 * DR：只改以太网头，IP 层不变（RS 的 lo 上配置 VIP，直接回包给客户端）。
 *
 * 这一层只做改写和校验和，不查会话、不查邻居表；下一跳 MAC 由调用方给出。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_FORWARD_NAT_FORWARDER_H
#define L4LB_FORWARD_NAT_FORWARDER_H

#include "common/types.h"
#include "protocol/parser.h"
#include <cstdint>

struct rte_mbuf;

namespace l4lb {

/// 一次 FULLNAT 改写的目标值（网络字节序）
struct NatRewrite {
  IPv4Addr src_ip, dst_ip;
  Port src_port, dst_port;
  MacAddr src_mac, dst_mac;
};

/// 改写选项
struct NatOptions {
  bool strip_ts = false;  ///< 去掉 SYN 中的 TCP timestamp 选项
  bool add_toa = false;   ///< 插入 TOA（客户端地址）
  IPv4Addr toa_ip = 0;    ///< TOA 中的客户端 IP / 端口（网络字节序）
  Port toa_port = 0;
  uint64_t tx_offloads = 0;
  uint16_t mtu = 1500;
};

enum class RewriteResult {
  OK,
  TTL_EXCEEDED,
};

/// 改写过程中实际发生的事情（用于统计）
struct RewriteOutcome {
  bool ts_stripped = false;
  bool toa_added = false;
  bool toa_no_room = false;
};

/**
 * @brief FULLNAT 改写（两个方向通用）
 *
 * TTL 减 1；增量更新校验和，TCP 选项改变时全量重算；
 * 网卡支持时 L4 校验和交给硬件。meta 中的长度/偏移会随 TOA 插入更新。
 */
RewriteResult nat_rewrite(struct rte_mbuf *m, PacketMeta &meta,
                          const NatRewrite &rw, const NatOptions &opt,
                          RewriteOutcome &out);

/// DR：只改 MAC
void dr_rewrite(struct rte_mbuf *m, const MacAddr &src_mac,
                const MacAddr &dst_mac);

/**
 * @brief ICMP 差错报文的 FULLNAT 转换
 *
 * outer：外层 IP 头的新源/目的地址；inner：内层（被引用的原始报文）
 * 的新五元组。内层 IP 校验和与整个 ICMP 校验和重新计算。
 */
RewriteResult nat_rewrite_icmp_error(struct rte_mbuf *m, const PacketMeta &meta,
                                     const IcmpErrorInfo &info,
                                     IPv4Addr outer_src, IPv4Addr outer_dst,
                                     const FiveTuple &inner,
                                     const MacAddr &src_mac,
                                     const MacAddr &dst_mac);

/// 本机发出的回包（ARP/ICMP 应答）清掉接收时的 offload 标志
void clear_tx_offload(struct rte_mbuf *m);

} // namespace l4lb

#endif // L4LB_FORWARD_NAT_FORWARDER_H
