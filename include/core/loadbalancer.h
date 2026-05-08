/**
 * @file loadbalancer.h
 * @brief Core L4 load balancer packet path.
 */

#ifndef L4LB_CORE_LOADBALANCER_H
#define L4LB_CORE_LOADBALANCER_H

#include "common/config.h"
#include "common/logger.h"
#include "common/types.h"
#include "forward/dr_forwarder.h"
#include "forward/forwarder.h"
#include "forward/nat_forwarder.h"
#include "lb/real_server.h"
#include "lb/session.h"
#include "protocol/arp.h"
#include "protocol/ethernet.h"
#include "protocol/icmp.h"
#include "protocol/ip.h"

#include <array>
#include <atomic>
#include <memory>

#include <rte_common.h>
#include <rte_ethdev.h>
#include <rte_lcore.h>
#include <rte_mbuf.h>

namespace l4lb {

class LoadBalancer {
public:
  LoadBalancer() : running_(false) {}

  bool init(const std::string &config_file) {
    if (!Config::instance().load(config_file)) {
      LOG_ERROR("Failed to load config");
      return false;
    }

    auto &cfg = Config::instance();
    cfg.dump();

    local_ip_ = cfg.get_vip();
    local_mac_ = cfg.get_vip_mac();

    if (!RealServerManager::instance().load_from_config()) {
      LOG_ERROR("Failed to load real servers");
      return false;
    }

    auto all_servers = RealServerManager::instance().get_all_servers();
    LOG_INFO("Building RS IP array for return traffic detection:");
    for (const auto &rs : all_servers) {
      if (rs_ip_count_ < rs_ips_.size()) {
        rs_ips_[rs_ip_count_++] = rs.ip;
      }
      LOG_INFO("  RS IP: %s", ip_to_string(rs.ip).c_str());
    }
    LOG_INFO("Total %zu RS IPs in array", rs_ip_count_);

    SessionManager::instance().set_timeout(cfg.get_session_timeout());

    if (cfg.get_forward_mode() == ForwardMode::NAT) {
      forwarder_ = std::make_unique<NatForwarder>(tx_offload_caps_);
      is_nat_mode_ = true;
      LOG_INFO("Using NAT forwarding mode");
    } else {
      forwarder_ = std::make_unique<DrForwarder>();
      is_nat_mode_ = false;
      LOG_INFO("Using DR forwarding mode");
    }

    running_ = true;
    LOG_INFO("LoadBalancer initialized");
    return true;
  }

  bool process_packet(void *mbuf, uint8_t *data, size_t len, uint64_t now_tsc,
                      bool &should_send) {
    should_send = false;
    Statistics &stats = local_stats().stats;

    if (!running_) {
      return false;
    }

    stats.rx_packets++;

    auto *eth = Ethernet::parse_mutable(data, len);
    if (!eth) {
      stats.dropped_packets++;
      return false;
    }

    if (eth->is_arp()) {
      should_send = handle_arp(eth, data, len, now_tsc);
      return should_send;
    }

    if (eth->is_ipv4()) {
      return handle_ipv4(eth, data, len, mbuf, now_tsc, should_send);
    }

    stats.dropped_packets++;
    return false;
  }

  Statistics get_stats() const {
    Statistics total{};
    for (const auto &core : stats_) {
      total.rx_packets += core.stats.rx_packets;
      total.tx_packets += core.stats.tx_packets;
      total.dropped_packets += core.stats.dropped_packets;
      total.arp_packets += core.stats.arp_packets;
      total.icmp_packets += core.stats.icmp_packets;
      total.tcp_packets += core.stats.tcp_packets;
      total.udp_packets += core.stats.udp_packets;
      total.forwarded_packets += core.stats.forwarded_packets;
      total.nat_translations += core.stats.nat_translations;
      total.active_sessions += core.stats.active_sessions;
      total.total_sessions += core.stats.total_sessions;
    }
    return total;
  }

  void set_tx_offload_caps(uint64_t caps) { tx_offload_caps_ = caps; }

  void stop() { running_ = false; }

  void send_arp_probes(uint16_t port_id, struct rte_mempool *pool) {
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) {
      LOG_ERROR("Failed to allocate mbuf for ARP probe");
      return;
    }

    uint8_t *data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    auto all_servers = RealServerManager::instance().get_all_servers();
    for (const auto &rs : all_servers) {
      size_t pkt_len =
          ArpHandler::build_request(data, rs.ip, local_ip_, local_mac_);

      mbuf->data_len = pkt_len;
      mbuf->pkt_len = pkt_len;

      struct rte_mbuf *tx_mbuf = rte_pktmbuf_copy(mbuf, pool, 0, pkt_len);
      if (tx_mbuf) {
        uint16_t nb_tx = rte_eth_tx_burst(port_id, 0, &tx_mbuf, 1);
        if (nb_tx > 0) {
          LOG_INFO("Sent ARP probe to RS: %s", ip_to_string(rs.ip).c_str());
        } else {
          rte_pktmbuf_free(tx_mbuf);
        }
      }
    }
    rte_pktmbuf_free(mbuf);
  }

private:
  bool handle_arp(EthernetHeader *eth, uint8_t *data, size_t len,
                  uint64_t now_tsc) {
    Statistics &stats = local_stats().stats;
    if (len < Ethernet::HEADER_SIZE + sizeof(ArpHeader)) {
      return false;
    }

    auto *arp = reinterpret_cast<ArpHeader *>(data + Ethernet::HEADER_SIZE);
    stats.arp_packets++;

    if (ArpHandler::handle(eth, arp, local_ip_, local_mac_, now_tsc)) {
      stats.tx_packets++;
      return true;
    }
    return false;
  }

  bool handle_ipv4(EthernetHeader *eth, uint8_t *data, size_t len, void *mbuf,
                   uint64_t now_tsc, bool &should_send) {
    Statistics &stats = local_stats().stats;
    PacketMeta meta;
    if (!ProtocolParser::parse(data, len, meta)) {
      stats.dropped_packets++;
      should_send = false;
      return false;
    }

    if (Ethernet::mac_equal(meta.src_mac.data(), local_mac_.data())) {
      should_send = false;
      return true;
    }

    auto *ip = reinterpret_cast<IPv4Header *>(data + meta.l3_offset);

    if (ip->is_icmp() && meta.dst_ip == local_ip_) {
      should_send = handle_icmp(eth, ip, data, len, meta);
      return should_send;
    }

    bool is_return = is_nat_mode_ && is_from_realserver(meta.src_ip);
    bool is_inbound = (meta.dst_ip == local_ip_) && !is_return;

    if (is_inbound && (ip->is_tcp() || ip->is_udp())) {
      if (ip->is_tcp()) {
        stats.tcp_packets++;
      } else {
        stats.udp_packets++;
      }
      should_send = handle_inbound(eth, data, len, meta, mbuf, now_tsc);
      return should_send;
    }

    if (is_return && (ip->is_tcp() || ip->is_udp())) {
      should_send = handle_return(eth, data, len, meta, mbuf, now_tsc);
      return should_send;
    }

    LOG_DEBUG("Packet not for LB: src=%s dst=%s",
              ip_to_string(meta.src_ip).c_str(),
              ip_to_string(meta.dst_ip).c_str());
    should_send = false;
    return false;
  }

  bool handle_icmp(EthernetHeader *eth, IPv4Header *ip, uint8_t *data,
                   size_t len, const PacketMeta &meta) {
    Statistics &stats = local_stats().stats;
    auto *icmp = reinterpret_cast<IcmpHeader *>(data + meta.l4_offset);
    size_t icmp_len = len - meta.l4_offset;

    stats.icmp_packets++;

    if (IcmpHandler::handle_echo_request(icmp, icmp_len)) {
      eth->swap_mac();
      ip->swap_ip();
      IpChecksum::update(ip);

      stats.tx_packets++;
      return true;
    }
    return false;
  }

  bool handle_inbound(EthernetHeader *eth, uint8_t *data, size_t len,
                      const PacketMeta &meta, void *mbuf, uint64_t now_tsc) {
    (void)eth;
    Statistics &stats = local_stats().stats;
    FiveTuple tuple = meta.to_five_tuple();

    Session *session = SessionManager::instance().lookup_ptr(tuple, now_tsc);
    if (session) {
      auto *rs =
          RealServerManager::instance().get_server(session->real_server_id);
      if (rs && forwarder_->forward(data, len, meta, rs, session->nat_src_port,
                                    mbuf)) {
        ArpTable::instance().update(meta.src_ip, meta.src_mac, now_tsc);
        SessionManager::instance().touch_session(session, len, now_tsc);
        stats.forwarded_packets++;
        stats.tx_packets++;
        return true;
      }
    }

    auto *rs = RealServerManager::instance().select_server(tuple);
    if (!rs) {
      LOG_WARN("No available backend server");
      stats.dropped_packets++;
      return false;
    }

    LOG_DEBUG("New connection: %s:%u -> VIP:%u => RS %s:%u",
              ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
              ntohs(meta.dst_port), ip_to_string(rs->ip).c_str(), rs->port);

    Port nat_src_port = SessionManager::instance().create(
        tuple, rs->id, now_tsc, rs->ip, htons(rs->port));

    if (forwarder_->forward(data, len, meta, rs, nat_src_port, mbuf)) {
      ArpTable::instance().update(meta.src_ip, meta.src_mac, now_tsc);
      stats.forwarded_packets++;
      stats.nat_translations++;
      stats.tx_packets++;
      return true;
    }

    stats.dropped_packets++;
    return false;
  }

  bool handle_return(EthernetHeader *eth, uint8_t *data, size_t len,
                     const PacketMeta &meta, void *mbuf, uint64_t now_tsc) {
    (void)eth;
    (void)len;
    Statistics &stats = local_stats().stats;
    FiveTuple reverse_tuple = meta.to_five_tuple();

    LOG_DEBUG("[RETURN] Looking up: src=%s:%u dst=%s:%u",
              ip_to_string(reverse_tuple.src_ip).c_str(),
              ntohs(reverse_tuple.src_port),
              ip_to_string(reverse_tuple.dst_ip).c_str(),
              ntohs(reverse_tuple.dst_port));

    Session session;
    if (!SessionManager::instance().lookup_reverse(reverse_tuple, session)) {
      LOG_DEBUG("[RETURN] NO SESSION FOUND for: %s:%u -> %s:%u",
                ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
                ip_to_string(meta.dst_ip).c_str(), ntohs(meta.dst_port));
      return false;
    }

    LOG_DEBUG("Return traffic: RS %s:%u -> %s:%u (SNAT to VIP)",
              ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
              ip_to_string(meta.dst_ip).c_str(), ntohs(meta.dst_port));

    if (forwarder_->forward_reply(data, len, meta, session, mbuf)) {
      ArpTable::instance().update(meta.src_ip, meta.src_mac, now_tsc);
      stats.forwarded_packets++;
      stats.tx_packets++;
      return true;
    }

    stats.dropped_packets++;
    return false;
  }

  bool is_from_realserver(IPv4Addr ip) const {
    for (size_t i = 0; i < rs_ip_count_; ++i) {
      if (rs_ips_[i] == ip) {
        return true;
      }
    }
    return false;
  }

  bool is_realserver_ip(IPv4Addr ip) const { return is_from_realserver(ip); }

  struct CoreStats {
    Statistics stats{};
  } __rte_cache_aligned;

  CoreStats &local_stats() {
    unsigned lcore = rte_lcore_id();
    if (unlikely(lcore >= RTE_MAX_LCORE)) {
      lcore = rte_get_main_lcore();
    }
    return stats_[lcore];
  }

  std::atomic<bool> running_;
  IPv4Addr local_ip_{0};
  MacAddr local_mac_{};
  std::unique_ptr<Forwarder> forwarder_;
  std::array<IPv4Addr, RealServerManager::kMaxServers> rs_ips_{};
  size_t rs_ip_count_ = 0;
  bool is_nat_mode_ = false;
  uint64_t tx_offload_caps_ = 0;
  std::array<CoreStats, RTE_MAX_LCORE> stats_{};
};

} // namespace l4lb

#endif // L4LB_CORE_LOADBALANCER_H
