/**
 * @file snapshot.cpp
 * @brief 配置快照构建与发布
 */

#include "ctrl/snapshot.h"

#include "common/logger.h"
#include <arpa/inet.h>
#include <sstream>

#include <rte_hash_crc.h>
#include <rte_rcu_qsbr.h>

namespace l4lb {

// ============================================================================
// Snapshot 查找表
// ============================================================================

namespace {
inline uint32_t svc_hash(IPv4Addr vip, Port port_be, uint8_t proto) {
  uint64_t k = (static_cast<uint64_t>(vip) << 24) |
               (static_cast<uint64_t>(port_be) << 8) | proto;
  return rte_hash_crc_8byte(k, 0);
}
inline uint32_t addr_hash(IPv4Addr ip) { return rte_hash_crc_4byte(ip, 0); }
} // namespace

bool Snapshot::set_has(const std::vector<IPv4Addr> &set, IPv4Addr ip) {
  uint32_t mask = kAddrSlots - 1;
  for (uint32_t i = addr_hash(ip) & mask;; i = (i + 1) & mask) {
    if (set[i] == ip)
      return true;
    if (set[i] == 0)
      return false;
  }
}

void Snapshot::set_add(std::vector<IPv4Addr> &set, IPv4Addr ip) {
  if (ip == 0 || set_has(set, ip))
    return;
  uint32_t mask = kAddrSlots - 1;
  uint32_t i = addr_hash(ip) & mask;
  while (set[i] != 0)
    i = (i + 1) & mask;
  set[i] = ip;
}

const Service *Snapshot::find_service(IPv4Addr vip, Port port_be,
                                      uint8_t proto) const {
  uint32_t mask = kSvcSlots - 1;
  for (uint32_t i = svc_hash(vip, port_be, proto) & mask;; i = (i + 1) & mask) {
    int16_t idx = svc_slots_[i];
    if (idx < 0)
      return nullptr;
    const Service &s = services[idx];
    if (s.vip == vip && s.port_be == port_be && s.proto == proto)
      return &s;
  }
}

void Snapshot::index() {
  svc_slots_.assign(kSvcSlots, -1);
  uint32_t mask = kSvcSlots - 1;
  for (const auto &s : services) {
    uint32_t i = svc_hash(s.vip, s.port_be, s.proto) & mask;
    while (svc_slots_[i] >= 0)
      i = (i + 1) & mask;
    svc_slots_[i] = static_cast<int16_t>(s.idx);
  }
  vips_.assign(kAddrSlots, 0);
  lips_.assign(kAddrSlots, 0);
  locals_.assign(kAddrSlots, 0);
  for (const auto &s : services) {
    set_add(vips_, s.vip);
    set_add(locals_, s.vip);
  }
  for (auto ip : local_ips) {
    set_add(lips_, ip);
    set_add(locals_, ip);
  }
  set_add(locals_, hc_src);
}

// ============================================================================
// SnapshotManager
// ============================================================================

bool SnapshotManager::init(const LbConfig &cfg, struct rte_rcu_qsbr *qsbr) {
  qsbr_ = qsbr;
  mode_ = cfg.mode;
  local_ips_ = cfg.mode == ForwardMode::NAT ? cfg.local_ips
                                            : std::vector<IPv4Addr>{};
  hc_src_ = cfg.hc_src;
  for (const auto &sc : cfg.services) {
    DesiredService ds;
    ds.conf = sc;
    ds.conf.rs.clear();
    for (const auto &rc : sc.rs) {
      DesiredRs r;
      r.id = next_rs_id_++;
      r.conf = rc;
      ds.rs.push_back(r);
    }
    services_.push_back(std::move(ds));
  }
  publish();
  return true;
}

void SnapshotManager::destroy() {
  delete cur_.exchange(nullptr);
}

SnapshotManager::DesiredRs *SnapshotManager::find_rs(uint32_t rs_id,
                                                     uint16_t *svc_idx) {
  for (size_t i = 0; i < services_.size(); ++i)
    for (auto &r : services_[i].rs)
      if (r.id == rs_id) {
        if (svc_idx)
          *svc_idx = static_cast<uint16_t>(i);
        return &r;
      }
  return nullptr;
}

std::string SnapshotManager::set_weight(uint32_t rs_id, uint32_t weight) {
  std::lock_guard<std::mutex> lock(mu_);
  auto *r = find_rs(rs_id);
  if (!r)
    return "no such rs " + std::to_string(rs_id);
  if (weight > 65535)
    return "weight must be 0-65535";
  r->conf.weight = weight;
  return "";
}

std::string SnapshotManager::set_enabled(uint32_t rs_id, bool enabled) {
  std::lock_guard<std::mutex> lock(mu_);
  auto *r = find_rs(rs_id);
  if (!r)
    return "no such rs " + std::to_string(rs_id);
  r->enabled = enabled;
  return "";
}

std::string SnapshotManager::add_rs(uint16_t svc_idx, const RsConf &rs,
                                    uint32_t &new_id) {
  std::lock_guard<std::mutex> lock(mu_);
  if (svc_idx >= services_.size())
    return "no such service " + std::to_string(svc_idx);
  auto &svc = services_[svc_idx];
  if (svc.rs.size() >= kMaxRsPerService)
    return "too many real servers in service";
  for (const auto &r : svc.rs)
    if (r.conf.ip == rs.ip && r.conf.port == rs.port)
      return "rs already exists (id " + std::to_string(r.id) + ")";
  if (next_rs_id_ >= kMaxRsId)
    return "rs id space exhausted";
  DesiredRs r;
  r.id = next_rs_id_++;
  r.conf = rs;
  svc.rs.push_back(r);
  new_id = r.id;
  return "";
}

std::string SnapshotManager::del_rs(uint32_t rs_id) {
  std::lock_guard<std::mutex> lock(mu_);
  for (auto &svc : services_)
    for (auto it = svc.rs.begin(); it != svc.rs.end(); ++it)
      if (it->id == rs_id) {
        svc.rs.erase(it);
        return "";
      }
  return "no such rs " + std::to_string(rs_id);
}

void SnapshotManager::set_health(uint32_t rs_id, bool healthy) {
  std::lock_guard<std::mutex> lock(mu_);
  if (auto *r = find_rs(rs_id))
    r->healthy = healthy;
}

Snapshot *SnapshotManager::build() const {
  auto *snap = new Snapshot();
  snap->version = version_;
  snap->mode = mode_;
  snap->local_ips = local_ips_;
  snap->hc_src = hc_src_;

  size_t total = 0;
  for (const auto &ds : services_)
    total += ds.rs.size();
  snap->rs_pool.reserve(total); // 之后不能再扩容，Service::rs 保存的是指针
  snap->rs_by_id.assign(next_rs_id_, nullptr);

  for (size_t i = 0; i < services_.size(); ++i) {
    const auto &ds = services_[i];
    Service svc;
    svc.idx = static_cast<uint16_t>(i);
    svc.name = ds.conf.name;
    svc.vip = ds.conf.vip;
    svc.port = ds.conf.port;
    svc.port_be = htons(ds.conf.port);
    svc.proto = ds.conf.proto;

    std::vector<SchedBackend> backends;
    for (const auto &dr : ds.rs) {
      RsState rs;
      rs.id = dr.id;
      rs.svc_idx = svc.idx;
      rs.ip = dr.conf.ip;
      rs.port = dr.conf.port;
      rs.port_be = htons(dr.conf.port);
      rs.mac = dr.conf.mac;
      rs.weight = dr.conf.weight;
      rs.enabled = dr.enabled;
      rs.healthy = dr.healthy;
      snap->rs_pool.push_back(rs);
      RsState *p = &snap->rs_pool.back();
      svc.rs.push_back(p);
      snap->rs_by_id[rs.id] = p;
      backends.push_back(
          {rs.ip, rs.port, rs.schedulable() ? rs.weight : 0u});
    }
    svc.sched.build(ds.conf.sched, backends);
    snap->services.push_back(std::move(svc));
  }
  snap->index();
  return snap;
}

void SnapshotManager::publish() {
  Snapshot *old;
  {
    std::lock_guard<std::mutex> lock(mu_);
    ++version_;
    old = cur_.exchange(build(), std::memory_order_acq_rel);
  }
  // 等所有 worker 都经过一次静默期（不再持有旧快照指针）后释放
  if (old) {
    if (qsbr_)
      rte_rcu_qsbr_synchronize(qsbr_, RTE_QSBR_THRID_INVALID);
    delete old;
  }
}

std::string SnapshotManager::describe() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::ostringstream os;
  os << "mode " << (mode_ == ForwardMode::NAT ? "fullnat" : "dr")
     << "  version " << version_ << "\n";
  for (size_t i = 0; i < services_.size(); ++i) {
    const auto &ds = services_[i];
    os << "service " << i << " " << ds.conf.name << " "
       << ip_to_string(ds.conf.vip) << ":" << ds.conf.port << "/"
       << (ds.conf.proto == 17 ? "udp" : "tcp") << " sched "
       << (ds.conf.sched == SchedulerType::MAGLEV ? "maglev" : "wrr") << "\n";
    for (const auto &r : ds.rs) {
      os << "  rs " << r.id << " " << ip_to_string(r.conf.ip) << ":"
         << r.conf.port << " weight " << r.conf.weight << " "
         << (r.enabled ? "enabled" : "disabled") << " "
         << (r.healthy ? "up" : "down") << "\n";
    }
  }
  return os.str();
}

} // namespace l4lb
