/**
 * @file loadbalancer.h
 * @brief 负载均衡器核心类 - 支持双向流量处理
 *
 * 无锁化优化：
 * 1. stats_ 原来是单个共享变量，所有 lcore 并发 ++，是数据竞争（UB）。
 *    改为 per-lcore 数组（cache line 对齐），每个 lcore 只写自己的槽，
 *    get_stats() 聚合时仅在控制面调用。
 *
 * 2. is_from_realserver() 原来用 std::unordered_set（堆上哈希表），
 *    每个 IP 包都调用一次。改为小型平坦数组线性扫描：
 *    典型 2-3 个后端，2-3 次比较比哈希表更快（无堆访问，全在寄存器）。
 */

#ifndef L4LB_CORE_LOADBALANCER_H
#define L4LB_CORE_LOADBALANCER_H

#include <atomic>
#include <cstring>
#include <memory>

#include <rte_lcore.h>

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

namespace l4lb {

/**
 * @brief Per-lcore 统计槽（cache line 对齐，消除 false sharing）
 *
 * sizeof(Statistics) = 11 × 8 = 88 字节，填充到 128 字节（2 个 cache line）。
 * 这样 per_lcore_stats_[i] 和 per_lcore_stats_[i+1] 不共享任何 cache line。
 */
struct alignas(RTE_CACHE_LINE_SIZE) PerLcoreStats {
    Statistics s{};
    // 88 字节 → 填充到 128（下一个 2×cache_line 边界）
    uint8_t _pad[128 - sizeof(Statistics)];
};
static_assert(sizeof(Statistics) <= 128, "Statistics exceeds 2 cache lines");

/**
 * @brief 负载均衡器核心类
 */
class LoadBalancer {
public:
    LoadBalancer() : running_(false), is_nat_mode_(false),
                     tx_offload_caps_(0), rs_ip_count_(0) {
        rs_ips_flat_.fill(0);
    }

    bool init(const std::string& config_file) {
        if (!Config::instance().load(config_file)) {
            LOG_ERROR("Failed to load config");
            return false;
        }
        auto& cfg = Config::instance();
        cfg.dump();

        local_ip_  = cfg.get_vip();
        local_mac_ = cfg.get_vip_mac();

        if (!RealServerManager::instance().load_from_config()) {
            LOG_ERROR("Failed to load real servers");
            return false;
        }

        // 构建扁平 RS IP 数组（替代 unordered_set，线性扫描更快）
        auto all_servers = RealServerManager::instance().get_all_servers();
        rs_ip_count_ = 0;
        LOG_INFO("Building RS IP flat array for return traffic detection:");
        for (const auto& rs : all_servers) {
            if (rs_ip_count_ < kMaxRsIps) {
                rs_ips_flat_[rs_ip_count_++] = rs.ip;
                LOG_INFO("  RS IP[%u]: %s", rs_ip_count_ - 1,
                         ip_to_string(rs.ip).c_str());
            }
        }
        LOG_INFO("Total %u RS IPs in flat array", rs_ip_count_);

        SessionManager::instance().set_timeout(cfg.get_session_timeout());

        if (cfg.get_forward_mode() == ForwardMode::NAT) {
            forwarder_   = std::make_unique<NatForwarder>(tx_offload_caps_);
            is_nat_mode_ = true;
            LOG_INFO("Using NAT forwarding mode");
        } else {
            forwarder_   = std::make_unique<DrForwarder>();
            is_nat_mode_ = false;
            LOG_INFO("Using DR forwarding mode");
        }

        running_ = true;
        LOG_INFO("LoadBalancer initialized");
        return true;
    }

    /**
     * @brief 处理数据包（每个 lcore 调用自己 lcore_id 对应的统计槽）
     */
    bool process_packet(void* mbuf, uint8_t* data, size_t len,
                        bool& should_send) {
        should_send = false;
        if (!running_) return false;

        // 获取当前 lcore 的统计槽，无需任何同步
        unsigned lid = safe_lcore_id();
        Statistics& stats = per_lcore_stats_[lid].s;

        ++stats.rx_packets;

        auto* eth = Ethernet::parse_mutable(data, len);
        if (!eth) { ++stats.dropped_packets; return false; }

        if (eth->is_arp()) {
            should_send = handle_arp(eth, data, len, stats);
            return should_send;
        }

        if (eth->is_ipv4()) {
            return handle_ipv4(eth, data, len, mbuf, should_send, stats);
        }

        ++stats.dropped_packets;
        return false;
    }

    /**
     * @brief 聚合所有 lcore 的统计信息（仅控制面调用）
     */
    Statistics get_stats() const {
        Statistics total{};
        for (unsigned i = 0; i < RTE_MAX_LCORE; ++i) {
            const Statistics& s = per_lcore_stats_[i].s;
            total.rx_packets        += s.rx_packets;
            total.tx_packets        += s.tx_packets;
            total.dropped_packets   += s.dropped_packets;
            total.arp_packets       += s.arp_packets;
            total.icmp_packets      += s.icmp_packets;
            total.tcp_packets       += s.tcp_packets;
            total.udp_packets       += s.udp_packets;
            total.forwarded_packets += s.forwarded_packets;
            total.nat_translations  += s.nat_translations;
            total.active_sessions   += s.active_sessions;
            total.total_sessions    += s.total_sessions;
        }
        return total;
    }

    void set_tx_offload_caps(uint64_t caps) { tx_offload_caps_ = caps; }
    void stop() { running_ = false; }

    void send_arp_probes(uint16_t port_id, struct rte_mempool* pool) {
        struct rte_mbuf* mbuf = rte_pktmbuf_alloc(pool);
        if (!mbuf) { LOG_ERROR("Failed to allocate mbuf for ARP probe"); return; }

        uint8_t* data = rte_pktmbuf_mtod(mbuf, uint8_t*);
        auto all_servers = RealServerManager::instance().get_all_servers();

        for (const auto& rs : all_servers) {
            size_t pkt_len = ArpHandler::build_request(data, rs.ip, local_ip_, local_mac_);
            mbuf->data_len = pkt_len;
            mbuf->pkt_len  = pkt_len;

            struct rte_mbuf* tx_mbuf = rte_pktmbuf_copy(mbuf, pool, 0, pkt_len);
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
    bool handle_arp(EthernetHeader* eth, uint8_t* data, size_t len,
                    Statistics& stats) {
        if (len < Ethernet::HEADER_SIZE + sizeof(ArpHeader)) return false;
        auto* arp = reinterpret_cast<ArpHeader*>(data + Ethernet::HEADER_SIZE);
        ++stats.arp_packets;
        if (ArpHandler::handle(eth, arp, local_ip_, local_mac_)) {
            ++stats.tx_packets;
            return true;
        }
        return false;
    }

    bool handle_ipv4(EthernetHeader* eth, uint8_t* data, size_t len,
                     void* mbuf, bool& should_send, Statistics& stats) {
        PacketMeta meta;
        if (!ProtocolParser::parse(data, len, meta)) {
            ++stats.dropped_packets;
            should_send = false;
            return false;
        }

        if (Ethernet::mac_equal(meta.src_mac.data(), local_mac_.data())) {
            should_send = false;
            return true;
        }

        auto* ip = reinterpret_cast<IPv4Header*>(data + meta.l3_offset);

        if (ip->is_icmp() && meta.dst_ip == local_ip_) {
            should_send = handle_icmp(eth, ip, data, len, meta, stats);
            return should_send;
        }

        // 线性扫描判断 RS 回程流量（2-3 个后端，比 unordered_set 快）
        bool is_return   = is_nat_mode_ && is_from_realserver(meta.src_ip);
        bool is_inbound  = (meta.dst_ip == local_ip_) && !is_return;

        if (is_inbound && (ip->is_tcp() || ip->is_udp())) {
            if (ip->is_tcp()) ++stats.tcp_packets;
            else              ++stats.udp_packets;
            should_send = handle_inbound(eth, data, len, meta, mbuf, stats);
            return should_send;
        }

        if (is_return && (ip->is_tcp() || ip->is_udp())) {
            should_send = handle_return(eth, data, len, meta, mbuf, stats);
            return should_send;
        }

        LOG_DEBUG("Packet not for LB: src=%s dst=%s",
                  ip_to_string(meta.src_ip).c_str(),
                  ip_to_string(meta.dst_ip).c_str());
        should_send = false;
        return false;
    }

    bool handle_icmp(EthernetHeader* eth, IPv4Header* ip, uint8_t* data,
                     size_t len, const PacketMeta& meta, Statistics& stats) {
        auto* icmp = reinterpret_cast<IcmpHeader*>(data + meta.l4_offset);
        size_t icmp_len = len - meta.l4_offset;
        ++stats.icmp_packets;
        if (IcmpHandler::handle_echo_request(icmp, icmp_len)) {
            eth->swap_mac();
            ip->swap_ip();
            IpChecksum::update(ip);
            ++stats.tx_packets;
            return true;
        }
        return false;
    }

    bool handle_inbound(EthernetHeader* eth, uint8_t* data, size_t len,
                        const PacketMeta& meta, void* mbuf, Statistics& stats) {
        FiveTuple tuple = meta.to_five_tuple();
        Session session;

        if (SessionManager::instance().lookup(tuple, session)) {
            auto* rs = RealServerManager::instance().get_server(session.real_server_id);
            if (rs && forwarder_->forward(data, len, meta, rs, session.nat_src_port, mbuf)) {
                ArpTable::instance().update(meta.src_ip, meta.src_mac);
                SessionManager::instance().update_stats(tuple, len);
                ++stats.forwarded_packets;
                ++stats.tx_packets;
                return true;
            }
        }

        auto* rs = RealServerManager::instance().select_server(tuple);
        if (!rs) {
            LOG_WARN("No available backend server");
            ++stats.dropped_packets;
            return false;
        }

        LOG_DEBUG("New connection: %s:%u -> VIP:%u => RS %s:%u",
                  ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
                  ntohs(meta.dst_port), ip_to_string(rs->ip).c_str(), rs->port);

        Port nat_src_port = SessionManager::instance().create(
            tuple, rs->id, rs->ip, htons(rs->port));

        if (forwarder_->forward(data, len, meta, rs, nat_src_port, mbuf)) {
            ArpTable::instance().update(meta.src_ip, meta.src_mac);
            ++stats.forwarded_packets;
            ++stats.nat_translations;
            ++stats.tx_packets;
            return true;
        }

        ++stats.dropped_packets;
        return false;
    }

    bool handle_return(EthernetHeader* eth, uint8_t* data, size_t len,
                       const PacketMeta& meta, void* mbuf, Statistics& stats) {
        FiveTuple reverse_tuple = meta.to_five_tuple();

        LOG_DEBUG("[RETURN] Looking up: src=%s:%u dst=%s:%u",
                  ip_to_string(reverse_tuple.src_ip).c_str(), ntohs(reverse_tuple.src_port),
                  ip_to_string(reverse_tuple.dst_ip).c_str(), ntohs(reverse_tuple.dst_port));

        Session session;
        if (!SessionManager::instance().lookup_reverse(reverse_tuple, session)) {
            LOG_DEBUG("[RETURN] NO SESSION FOUND for: %s:%u -> %s:%u",
                      ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
                      ip_to_string(meta.dst_ip).c_str(), ntohs(meta.dst_port));
            return false;
        }

        if (forwarder_->forward_reply(data, len, meta, session, mbuf)) {
            ArpTable::instance().update(meta.src_ip, meta.src_mac);
            SessionManager::instance().update_stats(session.client_tuple, len);
            ++stats.forwarded_packets;
            ++stats.tx_packets;
            return true;
        }

        ++stats.dropped_packets;
        return false;
    }

    /**
     * @brief 判断是否是 RS 回程流量（线性扫描小数组，无堆访问）
     *
     * 2-3 个后端服务器时，2-3 次 uint32_t 比较 + 分支预测比 unordered_set
     * 的哈希计算 + 指针追逐快 3-5 倍。
     */
    bool is_from_realserver(IPv4Addr ip) const {
        for (uint32_t i = 0; i < rs_ip_count_; ++i) {
            if (rs_ips_flat_[i] == ip) return true;
        }
        return false;
    }

    static unsigned safe_lcore_id() {
        unsigned id = rte_lcore_id();
        return (id < RTE_MAX_LCORE) ? id : 0;
    }

    // ─── 配置（只读，初始化后不变）────────────────────────────────────────
    std::atomic<bool>        running_;
    IPv4Addr                 local_ip_{};
    MacAddr                  local_mac_{};
    std::unique_ptr<Forwarder> forwarder_;
    bool                     is_nat_mode_;
    uint64_t                 tx_offload_caps_;

    // RS IP 扁平数组（替代 unordered_set，线性扫描）
    static constexpr uint32_t kMaxRsIps = 64;
    std::array<IPv4Addr, kMaxRsIps> rs_ips_flat_;
    uint32_t rs_ip_count_;

    // ─── Per-lcore 统计（无锁，cache line 对齐）──────────────────────────
    PerLcoreStats per_lcore_stats_[RTE_MAX_LCORE]{};
};

} // namespace l4lb

#endif // L4LB_CORE_LOADBALANCER_H
