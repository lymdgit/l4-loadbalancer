/**
 * @file nat_forwarder.cpp
 * @brief 报文改写实现
 */

#include "forward/nat_forwarder.h"

#include "protocol/checksum.h"
#include "protocol/ethernet.h"
#include "protocol/icmp.h"
#include "protocol/ip.h"

#include <cstring>

#include <rte_ethdev.h>
#include <rte_ip.h>
#include <rte_mbuf.h>

namespace l4lb {

namespace {

inline EthernetHeader *eth_of(struct rte_mbuf *m) {
  return rte_pktmbuf_mtod(m, EthernetHeader *);
}

inline void ip_checksum(IPv4Header *ip) {
  ip->checksum = 0;
  ip->checksum = IpChecksum::calculate(reinterpret_cast<uint8_t *>(ip),
                                       ip->get_header_len());
}

/// 把 SYN 中的 TCP timestamp 选项替换为 NOP，返回是否改动
bool strip_timestamp(TcpHeader *tcp) {
  uint8_t *opt = reinterpret_cast<uint8_t *>(tcp) + sizeof(TcpHeader);
  uint8_t *end = reinterpret_cast<uint8_t *>(tcp) + tcp->get_header_len();
  bool changed = false;
  while (opt < end) {
    uint8_t kind = opt[0];
    if (kind == TCPOPT_EOL)
      break;
    if (kind == TCPOPT_NOP) {
      ++opt;
      continue;
    }
    if (opt + 1 >= end || opt[1] < 2 || opt + opt[1] > end)
      break; // 选项长度非法，不再处理
    if (kind == TCPOPT_TIMESTAMP) {
      memset(opt, TCPOPT_NOP, opt[1]);
      changed = true;
    }
    opt += opt[1];
  }
  return changed;
}

/**
 * @brief 在 TCP 基本头之后插入 8 字节 TOA 选项
 *
 * 要求：TCP 头加 8 字节后不超过 60 字节；IP 包加 8 字节后不超过 MTU；
 * mbuf 是单段且有足够尾部空间。成功后更新 IP total_length、TCP doff 和 meta。
 */
bool insert_toa(struct rte_mbuf *m, PacketMeta &meta, IPv4Addr ip, Port port,
                uint16_t mtu) {
  auto *base = rte_pktmbuf_mtod(m, uint8_t *);
  auto *iph = reinterpret_cast<IPv4Header *>(base + meta.l3_offset);
  auto *tcp = reinterpret_cast<TcpHeader *>(base + meta.l4_offset);
  size_t doff = tcp->get_header_len();
  size_t ip_len = iph->get_total_length();
  if (doff + TCPOLEN_TOA > 60 || ip_len + TCPOLEN_TOA > mtu ||
      !rte_pktmbuf_is_contiguous(m))
    return false;

  // 去掉以太网尾部填充，再在尾部追加 8 字节
  size_t frame_len = meta.l3_offset + ip_len;
  if (rte_pktmbuf_data_len(m) > frame_len)
    rte_pktmbuf_trim(m, rte_pktmbuf_data_len(m) - frame_len);
  if (!rte_pktmbuf_append(m, TCPOLEN_TOA))
    return false;

  uint8_t *opt = base + meta.l4_offset + sizeof(TcpHeader);
  memmove(opt + TCPOLEN_TOA, opt, frame_len - (opt - base));
  opt[0] = TCPOPT_TOA;
  opt[1] = TCPOLEN_TOA;
  memcpy(opt + 2, &port, 2); // 网络字节序
  memcpy(opt + 4, &ip, 4);

  tcp->set_header_len(doff + TCPOLEN_TOA);
  iph->set_total_length(static_cast<uint16_t>(ip_len + TCPOLEN_TOA));
  meta.total_len += TCPOLEN_TOA;
  meta.payload_offset += TCPOLEN_TOA;
  return true;
}

/// L4 校验和：网卡支持时只写伪首部并设置 offload 标志，否则软件全量计算
void l4_checksum_full(struct rte_mbuf *m, IPv4Header *ip, uint8_t *l4,
                      const PacketMeta &meta, uint64_t offloads) {
  bool tcp = ip->is_tcp();
#ifdef L4LB_HW_CKSUM
  uint64_t want = tcp ? RTE_ETH_TX_OFFLOAD_TCP_CKSUM : RTE_ETH_TX_OFFLOAD_UDP_CKSUM;
  if (offloads & want) {
    m->l2_len = meta.l3_offset;
    m->l3_len = ip->get_header_len();
    m->ol_flags = RTE_MBUF_F_TX_IPV4 |
                  (tcp ? RTE_MBUF_F_TX_TCP_CKSUM : RTE_MBUF_F_TX_UDP_CKSUM);
    uint16_t ph = rte_ipv4_phdr_cksum(reinterpret_cast<const rte_ipv4_hdr *>(ip),
                                      m->ol_flags);
    if (tcp) {
      m->l4_len = reinterpret_cast<TcpHeader *>(l4)->get_header_len();
      reinterpret_cast<TcpHeader *>(l4)->checksum = ph;
    } else {
      m->l4_len = sizeof(UdpHeader);
      reinterpret_cast<UdpHeader *>(l4)->checksum = ph;
    }
    return;
  }
#else
  (void)offloads;
#endif
  m->ol_flags = 0;
  if (tcp)
    L4Checksum::recalculate_tcp_checksum(
        ip, reinterpret_cast<TcpHeader *>(l4),
        ip->get_total_length() - ip->get_header_len());
  else
    L4Checksum::recalculate_udp_checksum(ip, reinterpret_cast<UdpHeader *>(l4));
}

} // namespace

void clear_tx_offload(struct rte_mbuf *m) { m->ol_flags = 0; }

void dr_rewrite(struct rte_mbuf *m, const MacAddr &src_mac,
                const MacAddr &dst_mac) {
  auto *eth = eth_of(m);
  eth->set_dst_mac(dst_mac);
  eth->set_src_mac(src_mac);
  m->ol_flags = 0;
}

RewriteResult nat_rewrite(struct rte_mbuf *m, PacketMeta &meta,
                          const NatRewrite &rw, const NatOptions &opt,
                          RewriteOutcome &out) {
  auto *base = rte_pktmbuf_mtod(m, uint8_t *);
  auto *ip = reinterpret_cast<IPv4Header *>(base + meta.l3_offset);
  if (ip->ttl <= 1)
    return RewriteResult::TTL_EXCEEDED;

  // 1. TCP 选项改写（改变 TCP 头内容，之后需要全量计算 L4 校验和）
  bool l4_full = false;
  if (ip->is_tcp()) {
    auto *tcp = reinterpret_cast<TcpHeader *>(base + meta.l4_offset);
    if (opt.strip_ts && (tcp->flags & TCP_SYN) && strip_timestamp(tcp)) {
      out.ts_stripped = true;
      l4_full = true;
    }
    if (opt.add_toa) {
      if (insert_toa(m, meta, opt.toa_ip, opt.toa_port, opt.mtu)) {
        out.toa_added = true;
        l4_full = true;
        base = rte_pktmbuf_mtod(m, uint8_t *);
        ip = reinterpret_cast<IPv4Header *>(base + meta.l3_offset);
      } else {
        out.toa_no_room = true;
      }
    }
  }

  // 2. IP 头：地址、TTL；IP 校验和全量计算（20 字节，比多次增量更新更简单）
  IPv4Addr old_src = ip->src_ip, old_dst = ip->dst_ip;
  ip->src_ip = rw.src_ip;
  ip->dst_ip = rw.dst_ip;
  --ip->ttl;
  ip_checksum(ip);

  // 3. L4 端口和校验和
  uint8_t *l4 = base + meta.l4_offset;
  bool hw = false;
#ifdef L4LB_HW_CKSUM
  hw = opt.tx_offloads & (ip->is_tcp() ? RTE_ETH_TX_OFFLOAD_TCP_CKSUM
                                       : RTE_ETH_TX_OFFLOAD_UDP_CKSUM);
#endif
  if (ip->is_tcp()) {
    auto *tcp = reinterpret_cast<TcpHeader *>(l4);
    Port old_sp = tcp->src_port, old_dp = tcp->dst_port;
    tcp->src_port = rw.src_port;
    tcp->dst_port = rw.dst_port;
    if (l4_full || hw) {
      l4_checksum_full(m, ip, l4, meta, opt.tx_offloads);
    } else {
      L4Checksum::update_tcp_checksum_ip(tcp, old_src, rw.src_ip);
      L4Checksum::update_tcp_checksum_ip(tcp, old_dst, rw.dst_ip);
      L4Checksum::update_tcp_checksum_port(tcp, old_sp, rw.src_port);
      L4Checksum::update_tcp_checksum_port(tcp, old_dp, rw.dst_port);
      m->ol_flags = 0;
    }
  } else if (ip->is_udp()) {
    auto *udp = reinterpret_cast<UdpHeader *>(l4);
    Port old_sp = udp->src_port, old_dp = udp->dst_port;
    bool had_cksum = udp->checksum != 0;
    udp->src_port = rw.src_port;
    udp->dst_port = rw.dst_port;
    if (hw) {
      l4_checksum_full(m, ip, l4, meta, opt.tx_offloads);
    } else if (had_cksum) {
      L4Checksum::update_udp_checksum_ip(udp, old_src, rw.src_ip);
      L4Checksum::update_udp_checksum_ip(udp, old_dst, rw.dst_ip);
      L4Checksum::update_udp_checksum_port(udp, old_sp, rw.src_port);
      L4Checksum::update_udp_checksum_port(udp, old_dp, rw.dst_port);
      m->ol_flags = 0;
    } else {
      m->ol_flags = 0; // 校验和为 0 表示不校验，保持为 0
    }
  }

  // 4. MAC
  auto *eth = reinterpret_cast<EthernetHeader *>(base);
  eth->set_dst_mac(rw.dst_mac);
  eth->set_src_mac(rw.src_mac);
  return RewriteResult::OK;
}

RewriteResult nat_rewrite_icmp_error(struct rte_mbuf *m, const PacketMeta &meta,
                                     const IcmpErrorInfo &info,
                                     IPv4Addr outer_src, IPv4Addr outer_dst,
                                     const FiveTuple &inner,
                                     const MacAddr &src_mac,
                                     const MacAddr &dst_mac) {
  auto *base = rte_pktmbuf_mtod(m, uint8_t *);
  auto *ip = reinterpret_cast<IPv4Header *>(base + meta.l3_offset);
  if (ip->ttl <= 1)
    return RewriteResult::TTL_EXCEEDED;

  // 内层：被引用的原始报文头（L4 校验和覆盖整个原始报文，这里只有前 8 字节，
  // 无法重算；接收方协议栈不校验内层 L4 校验和）
  auto *iip = reinterpret_cast<IPv4Header *>(base + info.inner_l3_offset);
  iip->src_ip = inner.src_ip;
  iip->dst_ip = inner.dst_ip;
  ip_checksum(iip);
  auto *ports = reinterpret_cast<uint16_t *>(base + info.inner_l4_offset);
  ports[0] = inner.src_port;
  ports[1] = inner.dst_port;

  // 外层
  ip->src_ip = outer_src;
  ip->dst_ip = outer_dst;
  --ip->ttl;
  ip_checksum(ip);

  auto *icmp = reinterpret_cast<IcmpHeader *>(base + meta.l4_offset);
  icmp->checksum = 0;
  icmp->checksum = IcmpHandler::calculate_checksum(
      reinterpret_cast<uint8_t *>(icmp), meta.total_len - meta.l4_offset);

  auto *eth = reinterpret_cast<EthernetHeader *>(base);
  eth->set_dst_mac(dst_mac);
  eth->set_src_mac(src_mac);
  m->ol_flags = 0;
  return RewriteResult::OK;
}

} // namespace l4lb
