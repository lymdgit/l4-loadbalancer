/**
 * @file healthcheck.cpp
 * @brief RS 健康检查实现
 */

#include "ctrl/healthcheck.h"

#include "common/logger.h"
#include "ctrl/snapshot.h"
#include "dataplane/context.h"
#include "dataplane/worker.h"
#include "protocol/checksum.h"
#include "protocol/ethernet.h"
#include "protocol/ip.h"

#include <algorithm>
#include <cstring>

#include <rte_byteorder.h>
#include <rte_mbuf.h>
#include "common/dpdk_ring.h"

namespace l4lb {

void HealthChecker::init(const HealthConf &conf, IPv4Addr src_ip) {
  conf_ = conf;
  src_ip_ = src_ip;
}

void HealthChecker::tick(WorkerCtx &master, const Snapshot &snap,
                         uint64_t now_ms) {
  if (!conf_.enabled)
    return;

  // 与快照同步 RS 列表（RS 可能被控制命令增删）
  std::vector<uint32_t> seen;
  seen.reserve(snap.rs_pool.size());
  for (const auto &rs : snap.rs_pool) {
    // TCP 探测只适用于 TCP 服务；UDP 服务的 RS 不做健康检查（视为一直健康）
    if (snap.services[rs.svc_idx].proto != 6)
      continue;
    seen.push_back(rs.id);
    auto it = probes_.find(rs.id);
    if (it == probes_.end()) {
      Probe p;
      p.rs_id = rs.id;
      p.healthy = rs.healthy;
      p.next_ms = now_ms + (rs.id * 37u) % conf_.interval_ms; // 错开探测时间
      it = probes_.emplace(rs.id, p).first;
    }
    Probe &p = it->second;
    p.ip = rs.ip;
    p.port = rs.port;
    p.mac = rs.mac;
    p.snap_healthy = rs.healthy;
  }
  for (auto it = probes_.begin(); it != probes_.end();) {
    if (std::find(seen.begin(), seen.end(), it->first) == seen.end()) {
      if (it->second.in_flight)
        by_port_.erase(it->second.src_port);
      it = probes_.erase(it);
    } else {
      ++it;
    }
  }

  for (auto &kv : probes_) {
    Probe &p = kv.second;
    if (p.in_flight && now_ms - p.sent_ms >= conf_.timeout_ms) {
      by_port_.erase(p.src_port);
      p.in_flight = false;
      result(p, false, now_ms);
    }
    if (!p.in_flight && now_ms >= p.next_ms) {
      send_probe(master, p, now_ms);
      p.next_ms = now_ms + conf_.interval_ms;
    }
    // 本地判定与快照不一致：上报给控制线程（ring 满时下次重试，最多每秒一次）
    if (p.healthy != p.snap_healthy && now_ms - p.reported_ms >= 1000) {
      HealthEvent ev{p.rs_id, p.healthy};
      if (rte_ring_sp_enqueue_elem(g_dp.health_ring, &ev, sizeof(ev)) == 0)
        p.reported_ms = now_ms;
    }
  }
}

void HealthChecker::result(Probe &p, bool ok, uint64_t now_ms) {
  (void)now_ms;
  if (ok) {
    p.fail = 0;
    if (++p.ok >= conf_.rise && !p.healthy) {
      p.healthy = true;
      LOG_INFO("healthcheck: RS %u %s:%u is UP", p.rs_id,
               ip_to_string(p.ip).c_str(), p.port);
    }
  } else {
    p.ok = 0;
    if (++p.fail >= conf_.fall && p.healthy) {
      p.healthy = false;
      LOG_WARN("healthcheck: RS %u %s:%u is DOWN", p.rs_id,
               ip_to_string(p.ip).c_str(), p.port);
    }
  }
}

bool HealthChecker::build_tcp(WorkerCtx &master, const Probe &p, uint8_t flags,
                              uint32_t seq, uint32_t ack) {
  MacAddr dst_mac = p.mac;
  if (mac_is_zero(dst_mac) &&
      !g_dp.neigh.lookup(g_dp.route.next_hop(p.ip), dst_mac)) {
    master_request_neigh(master, g_dp.route.next_hop(p.ip));
    return false;
  }
  struct rte_mbuf *m = rte_pktmbuf_alloc(g_dp.pool);
  if (!m)
    return false;
  const size_t len =
      sizeof(EthernetHeader) + sizeof(IPv4Header) + sizeof(TcpHeader);
  auto *base = reinterpret_cast<uint8_t *>(rte_pktmbuf_append(m, len));
  if (!base) {
    rte_pktmbuf_free(m);
    return false;
  }
  memset(base, 0, len);
  auto *eth = reinterpret_cast<EthernetHeader *>(base);
  auto *ip = reinterpret_cast<IPv4Header *>(base + sizeof(EthernetHeader));
  auto *tcp = reinterpret_cast<TcpHeader *>(reinterpret_cast<uint8_t *>(ip) +
                                            sizeof(IPv4Header));
  eth->set_dst_mac(dst_mac);
  eth->set_src_mac(g_dp.local_mac);
  eth->set_ether_type(0x0800);
  ip->version_ihl = 0x45;
  ip->total_length = htons(sizeof(IPv4Header) + sizeof(TcpHeader));
  ip->flags_fragment = htons(0x4000);
  ip->ttl = 64;
  ip->protocol = 6;
  ip->src_ip = src_ip_;
  ip->dst_ip = p.ip;
  IpChecksum::update(ip);
  tcp->src_port = htons(p.src_port);
  tcp->dst_port = htons(p.port);
  tcp->seq_num = htonl(seq);
  tcp->ack_num = htonl(ack);
  tcp->data_offset = 5 << 4;
  tcp->flags = flags;
  tcp->window = htons(flags & TCP_RST ? 0 : 1024);
  L4Checksum::recalculate_tcp_checksum(ip, tcp, sizeof(TcpHeader));
  tx_buffer_add(master, m);
  return true;
}

void HealthChecker::send_probe(WorkerCtx &master, Probe &p, uint64_t now_ms) {
  // 从端口段里选一个空闲端口
  for (int i = 0; i <= kHcPortMax - kHcPortMin; ++i) {
    uint16_t port = next_port_;
    next_port_ = port >= kHcPortMax ? kHcPortMin : port + 1;
    if (by_port_.find(port) == by_port_.end()) {
      p.src_port = port;
      break;
    }
  }
  isn_seed_ = isn_seed_ * 1103515245u + 12345u;
  p.isn = isn_seed_;
  p.sent_ms = now_ms;
  p.in_flight = true;
  by_port_[p.src_port] = p.rs_id;
  // 下一跳未解析时 build_tcp 失败，等待超时计为一次失败
  build_tcp(master, p, TCP_SYN, p.isn, 0);
}

void HealthChecker::send_rst(WorkerCtx &master, const Probe &p,
                             const MasterEvent &ev) {
  build_tcp(master, p, TCP_RST, ev.ack, 0);
}

void HealthChecker::on_response(WorkerCtx &master, const MasterEvent &ev,
                                uint64_t now_ms) {
  auto pit = by_port_.find(ev.port);
  if (pit == by_port_.end())
    return;
  auto it = probes_.find(pit->second);
  if (it == probes_.end())
    return;
  Probe &p = it->second;
  if (!p.in_flight || ev.ip != p.ip || ev.aux != p.port)
    return;

  bool synack = (ev.tcp_flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK);
  if (synack && ev.ack != p.isn + 1)
    return; // 不是这次探测的应答
  by_port_.erase(pit);
  p.in_flight = false;
  if (synack) {
    send_rst(master, p, ev);
    result(p, true, now_ms);
  } else if (ev.tcp_flags & TCP_RST) {
    result(p, false, now_ms);
  }
}

} // namespace l4lb
