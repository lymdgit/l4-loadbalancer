/**
 * @file processor.cpp
 * @brief 报文处理主流程实现
 */

#include "core/processor.h"

#include "common/logger.h"
#include "ctrl/snapshot.h"
#include "dataplane/context.h"
#include "forward/nat_forwarder.h"
#include "lb/tcp_state.h"
#include "protocol/arp.h"
#include "protocol/checksum.h"
#include "protocol/ethernet.h"
#include "protocol/icmp.h"
#include "protocol/ip.h"
#include "protocol/parser.h"

#include <rte_byteorder.h>
#include <rte_mbuf.h>

namespace l4lb {

namespace {

// ============================================================================
// 小工具
// ============================================================================

inline Result drop(Stat reason) { return {Verdict::DROP, reason, 0}; }
inline Result send() { return {Verdict::SEND, ST_COUNT, 0}; }
inline Result consumed() { return {Verdict::CONSUMED, ST_COUNT, 0}; }
inline Result redirect(uint16_t to) { return {Verdict::REDIRECT, ST_COUNT, to}; }

inline bool in_nat_range(Port port_be) {
  uint16_t p = rte_be_to_cpu_16(port_be);
  return p >= kNatPortMin && p <= kNatPortMax;
}

inline bool in_hc_range(Port port_be) {
  uint16_t p = rte_be_to_cpu_16(port_be);
  return p >= kHcPortMin && p <= kHcPortMax;
}

inline bool cksum_bad(const struct rte_mbuf *m) {
  return (m->ol_flags & RTE_MBUF_F_RX_IP_CKSUM_MASK) ==
             RTE_MBUF_F_RX_IP_CKSUM_BAD ||
         (m->ol_flags & RTE_MBUF_F_RX_L4_CKSUM_MASK) ==
             RTE_MBUF_F_RX_L4_CKSUM_BAD;
}

void request_neigh(IPv4Addr ip) {
  MasterEvent ev{};
  ev.type = EV_NEIGH_MISS;
  ev.ip = ip;
  post_master_event(ev);
}

/**
 * @brief 解析下一跳 MAC，结果缓存在会话里（邻居表 generation 不变就不再查）
 *
 * @param fallback 邻居表里没有时使用的 MAC（全 0 表示没有兜底）
 */
bool resolve_mac(IPv4Addr next_hop, MacAddr &cache, uint32_t &cache_gen,
                 const MacAddr &fallback, MacAddr &out) {
  uint32_t gen = g_dp.neigh.generation();
  if (cache_gen == gen) {
    out = cache;
    return true;
  }
  if (next_hop && g_dp.neigh.lookup(next_hop, out)) {
    cache = out;
    cache_gen = gen;
    return true;
  }
  if (next_hop)
    request_neigh(next_hop);
  if (!mac_is_zero(fallback)) {
    out = fallback; // 不缓存：邻居表学到后改用 ARP 结果
    return true;
  }
  return false;
}

/// RS 的 MAC：配置了静态 MAC 直接用，否则查邻居表（RS 不在直连网段时走网关）
bool resolve_rs_mac(Session &s, const RsState &rs, MacAddr &out) {
  if (!mac_is_zero(rs.mac)) {
    out = rs.mac;
    return true;
  }
  return resolve_mac(g_dp.route.next_hop(rs.ip), s.rs_mac, s.rs_mac_gen,
                     MacAddr{}, out);
}

/// 回程下一跳（客户端或网关）的 MAC；直连客户端可以用首包的源 MAC 兜底
bool resolve_client_mac(Session &s, MacAddr &out) {
  IPv4Addr cip = s.client.src_ip;
  bool on_link = g_dp.route.on_link(cip);
  return resolve_mac(g_dp.route.next_hop(cip), s.cli_mac, s.cli_mac_gen,
                     on_link ? s.cli_src_mac : MacAddr{}, out);
}

void close_session(WorkerCtx &w, Session *s) {
  w.sessions.remove(s);
  w.stats.add(ST_SESS_CLOSED);
}

void update_state(WorkerCtx &w, Session *s, uint8_t flags, Dir dir) {
  if (s->state != TcpState::UDP)
    s->state = tcp_next_state(s->state, flags, dir, s->fin_seen);
  w.sessions.touch(s, w.now_tick, state_timeout(s->state, g_dp.cfg.timeouts));
}

// ============================================================================
// 新建会话
// ============================================================================

/// FULLNAT：选 LIP 和 SNAT 端口，保证回程包的 owner 是本 worker
Session *create_fullnat(WorkerCtx &w, const Snapshot &snap,
                        const FiveTuple &client, const RsState &rs,
                        uint32_t timeout, Stat &fail) {
  const auto &lips = snap.local_ips;
  if (lips.empty()) {
    fail = ST_DROP_NO_PORT;
    return nullptr;
  }
  if (w.lip_cursor.size() < lips.size())
    w.lip_cursor.resize(lips.size(), 0);

  const Steering &st = g_dp.steering;
  const uint8_t proto = client.protocol;
  const uint16_t step = st.port_step(proto);
  const uint32_t range = kNatPortMax - kNatPortMin + 1;
  const uint32_t slots = range / step; // 本 worker 在一个 LIP 上可用的候选数
  // RSS 决定 owner 时约 1/N 的候选属于本核，多试一些
  const uint32_t probes = st.ret_by_rss(proto) ? 64u * st.workers() : 64u;

  for (size_t n = 0; n < lips.size(); ++n) {
    size_t li = (w.lip_rr + n) % lips.size();
    uint32_t &cur = w.lip_cursor[li];
    for (uint32_t i = 0; i < probes; ++i) {
      uint32_t k = cur++ % slots;
      uint16_t port = static_cast<uint16_t>(
          kNatPortMin + k * step +
          (step > 1 ? (w.idx + step - kNatPortMin % step) % step : 0));
      if (port > kNatPortMax)
        continue;
      FiveTuple server(rs.ip, lips[li], rs.port_be, rte_cpu_to_be_16(port),
                       proto);
      if (st.ret_by_rss(proto) && st.ret_owner(server) != w.idx)
        continue; // 回程包会被网卡送到别的核
      if (w.sessions.key_in_use(server))
        continue;
      Session *s = w.sessions.create(client, &server, w.now_tick, timeout);
      if (!s) {
        fail = ST_DROP_TABLE_FULL;
        return nullptr;
      }
      w.lip_rr = static_cast<uint32_t>(li + 1);
      return s;
    }
  }
  fail = ST_DROP_NO_PORT;
  return nullptr;
}

Session *new_session(WorkerCtx &w, const Snapshot &snap, const Service &svc,
                     const FiveTuple &tuple, const EthernetHeader *eth,
                     Stat &fail) {
  int pick = svc.sched.pick(tuple, w.idx);
  if (pick < 0) {
    LOG_RATELIMIT(l4lb::LogLevel::WARN, 1, "No available backend for %s",
                  svc.name.c_str());
    fail = ST_DROP_NO_RS;
    return nullptr;
  }
  const RsState &rs = *svc.rs[pick];
  const auto &cfg = g_dp.cfg;
  bool tcp = tuple.protocol == 6;
  uint32_t timeout = tcp ? cfg.timeouts.tcp_syn : cfg.timeouts.udp;

  Session *s;
  if (snap.mode == ForwardMode::NAT) {
    s = create_fullnat(w, snap, tuple, rs, timeout, fail);
  } else {
    s = w.sessions.create(tuple, nullptr, w.now_tick, timeout);
    if (!s)
      fail = ST_DROP_TABLE_FULL;
  }
  if (!s)
    return nullptr;

  s->rs_id = rs.id;
  s->svc_idx = svc.idx;
  s->state = tcp ? TcpState::NONE : TcpState::UDP;
  memcpy(s->cli_src_mac.data(), eth->src_mac, 6);
  if (snap.mode == ForwardMode::NAT && tcp && cfg.toa)
    s->flags |= SF_TOA_PENDING;
  w.stats.add(ST_SESS_NEW);

  // FULLNAT 回程要发给客户端：直连客户端的 MAC 提示给邻居表，避免首个回包等 ARP
  if (snap.mode == ForwardMode::NAT && g_dp.route.on_link(tuple.src_ip)) {
    MasterEvent ev{};
    ev.type = EV_NEIGH_HINT;
    ev.ip = tuple.src_ip;
    ev.mac = s->cli_src_mac;
    post_master_event(ev);
  }
  return s;
}

// ============================================================================
// 入站：Client -> VIP
// ============================================================================

Result handle_inbound(WorkerCtx &w, const Snapshot &snap, const Service &svc,
                      struct rte_mbuf *m, PacketMeta &meta, bool redirected) {
  FiveTuple tuple = meta.to_five_tuple();
  const Steering &st = g_dp.steering;

  uint16_t owner = st.fwd_owner(tuple);
  if (owner != w.idx)
    return redirected ? drop(ST_DROP_OTHER) : redirect(owner);

  const bool tcp = meta.ip_protocol == 6;
  const bool syn = tcp && (meta.tcp_flags & (TCP_SYN | TCP_ACK | TCP_RST)) ==
                              TCP_SYN;

  // 自检：网卡给出的 RSS hash 应与软件计算一致（只抽查 SYN）
  if (syn && st.hw() && (m->ol_flags & RTE_MBUF_F_RX_RSS_HASH) &&
      m->hash.rss != st.rss_hash(tuple, st.l4_hashed(6)))
    w.stats.add(ST_RSS_MISMATCH);

  auto *eth = rte_pktmbuf_mtod(m, EthernetHeader *);
  Session *s = w.sessions.lookup(tuple);
  const RsState *rs = nullptr;
  if (s) {
    rs = snap.rs(s->rs_id);
    bool reuse = syn && tcp_state_reusable(s->state);
    if (!rs || !rs->usable() || reuse) {
      // RS 已删除/下线，或旧连接已结束后端口被复用：结束旧会话
      close_session(w, s);
      s = nullptr;
      if (!syn && tcp)
        return drop(rs && !reuse ? ST_DROP_RS_DOWN : ST_DROP_NO_SESSION);
    }
  }

  bool is_new = false;
  if (!s) {
    if (tcp && !syn)
      return drop(ST_DROP_NO_SESSION); // 只有 SYN 能建立新连接
    Stat fail = ST_DROP_OTHER;
    s = new_session(w, snap, svc, tuple, eth, fail);
    if (!s)
      return drop(fail);
    rs = snap.rs(s->rs_id);
    is_new = true;
  }

  update_state(w, s, meta.tcp_flags, Dir::CLIENT_TO_RS);
  ++s->pkts_in;
  s->bytes_in += rte_pktmbuf_pkt_len(m);

  MacAddr rs_mac;
  if (!resolve_rs_mac(*s, *rs, rs_mac)) {
    if (is_new)
      close_session(w, s);
    return drop(ST_DROP_NO_NEIGH);
  }

  if (snap.mode == ForwardMode::DR) {
    dr_rewrite(m, g_dp.local_mac, rs_mac);
    w.stats.add(ST_FWD_IN);
    return send();
  }

  // FULLNAT：Client:cport -> VIP:vport  =>  LIP:nat_port -> RS:rport
  NatRewrite rw;
  rw.src_ip = s->server.dst_ip;
  rw.src_port = s->server.dst_port;
  rw.dst_ip = s->server.src_ip;
  rw.dst_port = s->server.src_port;
  rw.src_mac = g_dp.local_mac;
  rw.dst_mac = rs_mac;

  NatOptions opt;
  opt.tx_offloads = g_dp.tx_offloads;
  opt.mtu = g_dp.mtu;
  opt.strip_ts = tcp && g_dp.cfg.strip_tcp_timestamp;
  // TOA：握手完成前，客户端方向的非 SYN 包都带上（第三个 ACK 可能重传）
  opt.add_toa = (s->flags & SF_TOA_PENDING) &&
                !(meta.tcp_flags & (TCP_SYN | TCP_RST));
  opt.toa_ip = s->client.src_ip;
  opt.toa_port = s->client.src_port;

  RewriteOutcome out;
  if (nat_rewrite(m, meta, rw, opt, out) != RewriteResult::OK) {
    if (is_new)
      close_session(w, s);
    return drop(ST_DROP_TTL);
  }
  if (out.ts_stripped)
    w.stats.add(ST_TS_STRIPPED);
  if (out.toa_added)
    w.stats.add(ST_TOA_ADDED);
  if (out.toa_no_room)
    w.stats.add(ST_TOA_NO_ROOM);
  w.stats.add(ST_FWD_IN);
  return send();
}

// ============================================================================
// FULLNAT 回程：RS -> LIP
// ============================================================================

Result handle_return(WorkerCtx &w, const Snapshot &snap, struct rte_mbuf *m,
                     PacketMeta &meta, bool redirected) {
  FiveTuple tuple = meta.to_five_tuple();
  uint16_t owner = g_dp.steering.ret_owner(tuple);
  if (owner != w.idx)
    return redirected ? drop(ST_DROP_OTHER) : redirect(owner);

  Session *s = w.sessions.lookup(tuple);
  if (!s || !(s->flags & SF_FULLNAT) || !(s->server == tuple))
    return drop(ST_DROP_NO_SESSION);

  const RsState *rs = snap.rs(s->rs_id);
  if (!rs || !rs->usable()) {
    close_session(w, s);
    return drop(ST_DROP_RS_DOWN);
  }

  update_state(w, s, meta.tcp_flags, Dir::RS_TO_CLIENT);
  // RS 回了非 SYN 包，说明它已经收到握手第三个 ACK（以及其中的 TOA）
  if ((s->flags & SF_TOA_PENDING) && !(meta.tcp_flags & TCP_SYN))
    s->flags &= ~SF_TOA_PENDING;
  ++s->pkts_out;
  s->bytes_out += rte_pktmbuf_pkt_len(m);

  MacAddr cli_mac;
  if (!resolve_client_mac(*s, cli_mac))
    return drop(ST_DROP_NO_NEIGH);

  // RS:rport -> LIP:nat_port  =>  VIP:vport -> Client:cport
  NatRewrite rw;
  rw.src_ip = s->client.dst_ip;
  rw.src_port = s->client.dst_port;
  rw.dst_ip = s->client.src_ip;
  rw.dst_port = s->client.src_port;
  rw.src_mac = g_dp.local_mac;
  rw.dst_mac = cli_mac;

  NatOptions opt;
  opt.tx_offloads = g_dp.tx_offloads;
  opt.mtu = g_dp.mtu;
  RewriteOutcome out;
  if (nat_rewrite(m, meta, rw, opt, out) != RewriteResult::OK)
    return drop(ST_DROP_TTL);
  w.stats.add(ST_FWD_OUT);
  return send();
}

// ============================================================================
// ICMP
// ============================================================================

Result handle_icmp_error(WorkerCtx &w, const Snapshot &snap, struct rte_mbuf *m,
                         const PacketMeta &meta, bool redirected) {
  IcmpErrorInfo info;
  if (!ProtocolParser::parse_icmp_error(rte_pktmbuf_mtod(m, uint8_t *), meta,
                                        info))
    return drop(ST_DROP_MALFORMED);
  const FiveTuple &in = info.inner;
  const Steering &st = g_dp.steering;

  // 情况 1（FULLNAT）：差错针对 LB 发给 RS 的包（LIP:nat_port -> RS:rport）
  if (snap.mode == ForwardMode::NAT && snap.is_lip(meta.dst_ip) &&
      snap.is_lip(in.src_ip) && in_nat_range(in.src_port)) {
    FiveTuple key = in.reverse(); // RS -> LIP，即会话的回程 key
    uint16_t owner = st.ret_owner(key);
    if (owner != w.idx)
      return redirected ? drop(ST_DROP_OTHER) : redirect(owner);
    Session *s = w.sessions.lookup(key);
    if (!s || !(s->server == key))
      return drop(ST_DROP_NO_SESSION);
    MacAddr mac;
    if (!resolve_client_mac(*s, mac))
      return drop(ST_DROP_NO_NEIGH);
    // 转换成客户端视角：外层 VIP -> Client，内层 Client:cport -> VIP:vport
    if (nat_rewrite_icmp_error(m, meta, info, s->client.dst_ip,
                               s->client.src_ip, s->client, g_dp.local_mac,
                               mac) != RewriteResult::OK)
      return drop(ST_DROP_TTL);
    w.stats.add(ST_ICMP_ERR_FWD);
    return send();
  }

  // 情况 2：差错针对发给客户端的包（VIP:vport -> Client:cport）
  if (snap.is_vip(meta.dst_ip) && snap.is_vip(in.src_ip)) {
    FiveTuple key = in.reverse(); // Client -> VIP，即会话的正向 key
    uint16_t owner = st.fwd_owner(key);
    if (owner != w.idx)
      return redirected ? drop(ST_DROP_OTHER) : redirect(owner);
    Session *s = w.sessions.lookup(key);
    if (!s || !(s->client == key))
      return drop(ST_DROP_NO_SESSION);
    const RsState *rs = snap.rs(s->rs_id);
    MacAddr mac;
    if (!rs || !resolve_rs_mac(*s, *rs, mac))
      return drop(ST_DROP_NO_NEIGH);
    if (snap.mode == ForwardMode::DR) {
      // RS 的 lo 上有 VIP，原样转发即可
      dr_rewrite(m, g_dp.local_mac, mac);
    } else {
      // 转换成 RS 视角：外层 LIP -> RS，内层 RS:rport -> LIP:nat_port
      if (nat_rewrite_icmp_error(m, meta, info, s->server.dst_ip,
                                 s->server.src_ip, s->server, g_dp.local_mac,
                                 mac) != RewriteResult::OK)
        return drop(ST_DROP_TTL);
    }
    w.stats.add(ST_ICMP_ERR_FWD);
    return send();
  }
  return drop(ST_DROP_NO_SESSION);
}

Result handle_icmp(WorkerCtx &w, const Snapshot &snap, struct rte_mbuf *m,
                   const PacketMeta &meta, bool redirected) {
  auto *base = rte_pktmbuf_mtod(m, uint8_t *);
  auto *icmp = reinterpret_cast<IcmpHeader *>(base + meta.l4_offset);
  if (icmp_is_error(icmp->type))
    return handle_icmp_error(w, snap, m, meta, redirected);

  // 以 IP total_length 为准，不把以太网尾部填充算进 ICMP 校验和
  size_t icmp_len = meta.total_len - meta.l4_offset;
  if (!IcmpHandler::handle_echo_request(icmp, icmp_len))
    return drop(ST_DROP_OTHER);
  auto *eth = reinterpret_cast<EthernetHeader *>(base);
  auto *ip = reinterpret_cast<IPv4Header *>(base + meta.l3_offset);
  eth->swap_mac();
  eth->set_src_mac(g_dp.local_mac);
  ip->swap_ip();
  ip->ttl = 64;
  IpChecksum::update(ip);
  clear_tx_offload(m);
  return send();
}

// ============================================================================
// 健康检查回包：交给 master
// ============================================================================

Result handle_hc_response(WorkerCtx &w, struct rte_mbuf *m,
                          const PacketMeta &meta) {
  if (meta.ip_protocol != 6)
    return drop(ST_DROP_OTHER);
  auto *tcp = rte_pktmbuf_mtod_offset(m, TcpHeader *, meta.l4_offset);
  MasterEvent ev{};
  ev.type = EV_HC_RESP;
  ev.ip = meta.src_ip;
  ev.port = rte_be_to_cpu_16(meta.dst_port);
  ev.aux = rte_be_to_cpu_16(meta.src_port);
  ev.tcp_flags = meta.tcp_flags;
  ev.seq = rte_be_to_cpu_32(tcp->seq_num);
  ev.ack = rte_be_to_cpu_32(tcp->ack_num);
  post_master_event(ev);
  w.stats.add(ST_HC_RESP);
  return consumed();
}

// ============================================================================
// ARP
// ============================================================================

Result handle_arp(WorkerCtx &w, const Snapshot &snap, struct rte_mbuf *m) {
  auto *base = rte_pktmbuf_mtod(m, uint8_t *);
  if (rte_pktmbuf_data_len(m) < Ethernet::HEADER_SIZE + sizeof(ArpHeader))
    return drop(ST_DROP_MALFORMED);
  auto *eth = reinterpret_cast<EthernetHeader *>(base);
  auto *arp = reinterpret_cast<ArpHeader *>(base + Ethernet::HEADER_SIZE);
  w.stats.add(ST_ARP);
  if (!arp->is_eth_ipv4())
    return consumed();

  // 别人用了本机地址（或本机的免费 ARP 回环）：不学习
  if (snap.is_local(arp->sender_ip)) {
    if (memcmp(arp->sender_mac, g_dp.local_mac.data(), 6) != 0)
      LOG_RATELIMIT(l4lb::LogLevel::WARN, 10,
                    "ARP: address conflict on %s from %s",
                    ip_to_string(arp->sender_ip).c_str(),
                    mac_to_string(eth->get_src_mac()).c_str());
    return consumed();
  }
  if (!snap.is_local(arp->target_ip))
    return consumed(); // 别人的 ARP，不处理

  MasterEvent ev{};
  ev.ip = arp->sender_ip;
  memcpy(ev.mac.data(), arp->sender_mac, 6);
  if (arp->is_request()) {
    if (arp->sender_ip && g_dp.route.on_link(arp->sender_ip)) {
      ev.type = EV_ARP_FOR_US;
      post_master_event(ev);
    }
    ArpHandler::make_reply(eth, arp, g_dp.local_mac);
    clear_tx_offload(m);
    return send();
  }
  if (arp->is_reply()) {
    ev.type = EV_ARP_REPLY;
    post_master_event(ev);
  }
  return consumed();
}

} // namespace

// ============================================================================
// 入口
// ============================================================================

Result process_packet(WorkerCtx &w, const Snapshot &snap, struct rte_mbuf *m,
                      bool redirected) {
  auto *data = rte_pktmbuf_mtod(m, uint8_t *);
  size_t len = rte_pktmbuf_data_len(m);
  if (len < Ethernet::HEADER_SIZE)
    return drop(ST_DROP_MALFORMED);
  auto *eth = reinterpret_cast<EthernetHeader *>(data);

  if (eth->is_arp())
    return handle_arp(w, snap, m);
  if (!eth->is_ipv4())
    return drop(ST_DROP_OTHER);

  PacketMeta meta;
  switch (ProtocolParser::parse(data, len, meta)) {
  case ParseResult::OK:
    break;
  case ParseResult::FRAGMENT:
    return drop(ST_DROP_FRAGMENT);
  default:
    return drop(ST_DROP_MALFORMED);
  }
  if (cksum_bad(m))
    return drop(ST_DROP_CKSUM);
  // 本机发出又被回环收到的帧（例如交换机泛洪）：忽略
  if (Ethernet::mac_equal(meta.src_mac.data(), g_dp.local_mac.data()))
    return consumed();

  const uint32_t dst = meta.dst_ip;
  if (meta.ip_protocol == 1) {
    if (!snap.is_local(dst))
      return drop(ST_DROP_NOT_LOCAL);
    if (!redirected)
      w.stats.add(ST_ICMP);
    return handle_icmp(w, snap, m, meta, redirected);
  }
  if (meta.ip_protocol != 6 && meta.ip_protocol != 17)
    return drop(snap.is_local(dst) ? ST_DROP_NO_SERVICE : ST_DROP_NOT_LOCAL);
  if (!redirected)
    w.stats.add(meta.ip_protocol == 6 ? ST_TCP : ST_UDP);

  // FULLNAT 回程（LIP 上的 SNAT 端口段）
  if (snap.mode == ForwardMode::NAT && snap.is_lip(dst) &&
      in_nat_range(meta.dst_port))
    return handle_return(w, snap, m, meta, redirected);
  // 健康检查回包
  if (dst == snap.hc_src && in_hc_range(meta.dst_port))
    return handle_hc_response(w, m, meta);
  // 入站
  if (const Service *svc =
          snap.find_service(dst, meta.dst_port, meta.ip_protocol))
    return handle_inbound(w, snap, *svc, m, meta, redirected);
  return drop(snap.is_local(dst) ? ST_DROP_NO_SERVICE : ST_DROP_NOT_LOCAL);
}

} // namespace l4lb
