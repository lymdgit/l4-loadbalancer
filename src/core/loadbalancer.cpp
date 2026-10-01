/**
 * @file loadbalancer.cpp
 * @brief 负载均衡器核心逻辑实现
 */

#include "core/loadbalancer.h"

#include "common/config.h"
#include "common/logger.h"
#include "common/stats.h"
#include "forward/dr_forwarder.h"
#include "forward/nat_forwarder.h"
#include "lb/real_server.h"
#include "lb/session.h"
#include "protocol/arp.h"
#include "protocol/checksum.h"
#include "protocol/ethernet.h"
#include "protocol/icmp.h"
#include "protocol/ip.h"
#include "protocol/parser.h"

#include <rte_ethdev.h>
#include <rte_mbuf.h>

// 在查询上确实不如maglev好，因为那是o1查询的
// 但在我这个项目中，只有第一次握手建联的首包时，才会去差一次
// 查询完后，后续所有的这个连接的包，都会直接查正向会话表，仍然是O1

namespace l4lb {

LoadBalancer::LoadBalancer() : running_(false) {}

// 定义在 .cpp 中：unique_ptr<Forwarder> 析构需要完整类型
LoadBalancer::~LoadBalancer() = default;

bool LoadBalancer::init(const std::string &config_file) {
  // 加载配置
  if (!Config::instance().load(config_file)) {
    LOG_ERROR("Failed to load config");
    return false;
  }
  // 生成一个config实例并打印相关信息
  auto &cfg = Config::instance();
  cfg.dump();

  // 初始化本机信息
  local_ip_ = cfg.get_vip();
  local_mac_ = cfg.get_vip_mac();

  // VIP 服务端口：只有这些端口的入站流量会被转发
  service_ports_.reset();
  for (uint16_t port : cfg.get_listen_ports()) {
    service_ports_.set(port);
    LOG_INFO("Service port: %u", port);
  }
  if (service_ports_.none()) {
    LOG_ERROR("No valid service port in [vip] ports");
    return false;
  }

  // 初始化 Real Server
  if (!RealServerManager::instance().load_from_config()) {
    LOG_ERROR("Failed to load real servers");
    return false;
  }

  // 构建 RS IP 集合，用于快速判断返回流量
  auto &rs_mgr = RealServerManager::instance();
  auto all_servers = rs_mgr.get_all_servers();
  LOG_INFO("Building RS IP set for return traffic detection:");
  for (const auto &rs : all_servers) {
    rs_ips_.insert(rs.ip);
    LOG_INFO("  RS IP: %s", ip_to_string(rs.ip).c_str());
  }
  LOG_INFO("Total %zu RS IPs in set", rs_ips_.size());

  // 设置会话超时
  SessionManager::instance().set_timeout(cfg.get_session_timeout());

  // 创建转发引擎      工厂模式：将实例化的过程延迟到子类中进行
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

LoadBalancer::LcoreStats &LoadBalancer::local_stats() {
  return stats_[stat_lcore()];
}

Statistics LoadBalancer::get_stats() const {
  Statistics s{};
  for (const auto &c : stats_) {
    s.rx_packets += stat_get(c.rx_packets);
    s.tx_packets += stat_get(c.tx_packets);
    s.dropped_packets += stat_get(c.dropped_packets);
    s.arp_packets += stat_get(c.arp_packets);
    s.icmp_packets += stat_get(c.icmp_packets);
    s.tcp_packets += stat_get(c.tcp_packets);
    s.udp_packets += stat_get(c.udp_packets);
    s.forwarded_packets += stat_get(c.forwarded_packets);
    s.nat_translations += stat_get(c.nat_translations);
  }
  return s;
}

bool LoadBalancer::is_service_port(Port port_be) const {
  return service_ports_.test(ntohs(port_be));
}

bool LoadBalancer::process_packet(void *mbuf, uint8_t *data, size_t len,
                                  bool &should_send) {
  should_send = false; // 默认不发送

  if (!running_)
    return false;

  auto &st = local_stats();
  stat_add(st.rx_packets);

  // 解析以太网头
  auto *eth = Ethernet::parse_mutable(data, len);
  if (!eth) {
    stat_add(st.dropped_packets);
    return false;
  }

  // ARP 处理
  if (eth->is_arp()) {
    should_send = handle_arp(eth, data, len);
    return should_send;
  }

  // IPv4 处理
  if (eth->is_ipv4()) {
    return handle_ipv4(eth, data, len, mbuf, should_send);
  }

  stat_add(st.dropped_packets);
  return false;
}

void LoadBalancer::send_arp_probes(uint16_t port_id, uint16_t queue_id,
                                   struct rte_mempool *pool) {
  // 遍历所有 RS 发送 ARP 请求，每个请求单独分配 mbuf
  auto all_servers = RealServerManager::instance().get_all_servers();
  for (const auto &rs : all_servers) {
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) {
      LOG_ERROR("Failed to allocate mbuf for ARP probe");
      return;
    }

    uint8_t *data = rte_pktmbuf_mtod(mbuf, uint8_t *);
    size_t pkt_len =
        ArpHandler::build_request(data, rs.ip, local_ip_, local_mac_);
    mbuf->data_len = pkt_len;
    mbuf->pkt_len = pkt_len;

    // 使用调用方 lcore 自己的 TX 队列，不能和其他 worker 共用
    if (rte_eth_tx_burst(port_id, queue_id, &mbuf, 1) > 0) {
      LOG_INFO("Sent ARP probe to RS: %s", ip_to_string(rs.ip).c_str());
    } else {
      rte_pktmbuf_free(mbuf);
    }
  }
}

bool LoadBalancer::handle_arp(EthernetHeader *eth, uint8_t *data, size_t len) {
  if (len < Ethernet::HEADER_SIZE + sizeof(ArpHeader)) {
    return false;
  }

  auto &st = local_stats();
  auto *arp = reinterpret_cast<ArpHeader *>(data + Ethernet::HEADER_SIZE);
  stat_add(st.arp_packets);

  if (ArpHandler::handle(eth, arp, local_ip_, local_mac_)) {
    stat_add(st.tx_packets);
    return true;
  }
  return false;
}

bool LoadBalancer::handle_ipv4(EthernetHeader *eth, uint8_t *data, size_t len,
                               void *mbuf, bool &should_send) {
  auto &st = local_stats();
  should_send = false;

  // 畸形包和分片一律丢弃：后续改写都依赖解析出的偏移在包长范围内
  PacketMeta meta;
  if (ProtocolParser::parse(data, len, meta) != ParseResult::OK) {
    stat_add(st.dropped_packets);
    return false;
  }

  // 关键修复：检查源 MAC 是否为本机 MAC
  // 如果是本机发送的包（Loopback/Reflection），直接忽略
  // 否则会造成 ARP 表被本机 MAC 污染 (Client IP -> LB MAC)
  if (Ethernet::mac_equal(meta.src_mac.data(), local_mac_.data())) {
    return true; // 视为已处理（忽略）
  }

  auto *ip = reinterpret_cast<IPv4Header *>(data + meta.l3_offset);

  // ICMP 处理 (Ping)
  if (ip->is_icmp() && meta.dst_ip == local_ip_) {
    should_send = handle_icmp(eth, ip, data, meta);
    return should_send;
  }

  // 判断流量方向
  // Full NAT 模式：Return traffic 也是 dst_ip=VIP，但 dst_port=ClientPort
  // 所以必须先判断 is_return，避免误判为 inbound

  // 判断是否是回程流量：看一下unordered_set中是否有这个ip
  bool is_return = is_nat_mode_ && is_from_realserver(meta.src_ip);
  // 判断是否是需要DNAT处理的包：查完源IP，再查一下目的IP是不是我
  bool is_inbound = (meta.dst_ip == local_ip_) && !is_return;

  // 入站流量：Client -> VIP:服务端口
  if (is_inbound && (ip->is_tcp() || ip->is_udp())) {
    if (!is_service_port(meta.dst_port)) {
      stat_add(st.dropped_packets);
      return false;
    }
    stat_add(ip->is_tcp() ? st.tcp_packets : st.udp_packets);
    should_send = handle_inbound(data, len, meta, mbuf);
    return should_send;
  }

  // 返回流量：RealServer -> Client (NAT 模式)
  if (is_return && (ip->is_tcp() || ip->is_udp())) {
    should_send = handle_return(data, len, meta, mbuf);
    return should_send;
  }

  LOG_DEBUG("Packet not for LB: src=%s dst=%s",
            ip_to_string(meta.src_ip).c_str(),
            ip_to_string(meta.dst_ip).c_str());
  return false;
}

bool LoadBalancer::handle_icmp(EthernetHeader *eth, IPv4Header *ip,
                               uint8_t *data, const PacketMeta &meta) {
  auto *icmp = reinterpret_cast<IcmpHeader *>(data + meta.l4_offset);
  // 以 IP total_length 为准，不把以太网尾部填充算进 ICMP 校验和
  size_t icmp_len = meta.total_len - meta.l4_offset;

  auto &st = local_stats();
  stat_add(st.icmp_packets);

  if (IcmpHandler::handle_echo_request(icmp, icmp_len)) {
    eth->swap_mac();
    ip->swap_ip();
    IpChecksum::update(ip);

    stat_add(st.tx_packets);
    return true;
  }
  return false;
}

bool LoadBalancer::handle_inbound(uint8_t *data, size_t len,
                                  const PacketMeta &meta, void *mbuf) {
  auto &st = local_stats();
  auto &sessions = SessionManager::instance();
  FiveTuple tuple = meta.to_five_tuple();

  // 1. 查找已有会话；会话对应的 RS 不可用时重新选择
  Session session;
  RealServer *rs = nullptr;
  Port nat_src_port = 0;
  bool is_new = false;
  if (sessions.lookup(tuple, session)) {
    rs = RealServerManager::instance().get_server(session.real_server_id);
    nat_src_port = session.nat_src_port;
  }

  if (!rs) {
    // 2. 新连接，选择后端服务器
    rs = RealServerManager::instance().select_server(tuple);
    if (!rs) {
      LOG_RATELIMIT(l4lb::LogLevel::WARN, 1, "No available backend server");
      stat_add(st.dropped_packets);
      return false;
    }

    // 性能优化：改为 DEBUG 级别
    LOG_DEBUG("New connection: %s:%u -> VIP:%u => RS %s:%u",
              ip_to_string(meta.src_ip).c_str(), ntohs(meta.src_port),
              ntohs(meta.dst_port), ip_to_string(rs->ip).c_str(), rs->port);

    // 3. 创建会话（同一五元组的旧会话会被替换，不会泄漏反向表条目）
    // NAT 模式需要 SNAT 端口和反向表；DR 模式回程不经过 LB，不占反向表
    // 注意：rs->port 是主机字节序，需要转换为网络字节序
    if (!sessions.create(tuple, rs->id, is_nat_mode_ ? rs->ip : 0,
                         htons(rs->port), nat_src_port)) {
      stat_add(st.dropped_packets);
      return false;
    }
    is_new = true;
  }

  // 4. 转发
  if (!forwarder_->forward(data, len, meta, rs, nat_src_port, mbuf)) {
    // 新建的会话没能转发出去（TTL 耗尽、查不到 RS MAC 等），回滚，避免残留
    if (is_new)
      sessions.remove(tuple);
    stat_add(st.dropped_packets);
    return false;
  }

  ArpTable::instance().update(meta.src_ip, meta.src_mac);
  if (is_new)
    stat_add(st.nat_translations);
  else
    sessions.update_stats(tuple, len);
  stat_add(st.forwarded_packets);
  stat_add(st.tx_packets);
  return true;
}

bool LoadBalancer::handle_return(uint8_t *data, size_t len,
                                 const PacketMeta &meta, void *mbuf) {
  auto &st = local_stats();
  // 构建反向五元组进行查找
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

  // SNAT：修改源 IP 为 VIP，源端口为原始目的端口
  if (forwarder_->forward_reply(data, len, meta, session, mbuf)) {
    ArpTable::instance().update(meta.src_ip, meta.src_mac);
    SessionManager::instance().update_stats(session.client_tuple, len);
    stat_add(st.forwarded_packets);
    stat_add(st.tx_packets);
    return true;
  }

  stat_add(st.dropped_packets);
  return false;
}

} // namespace l4lb
