/**
 * @file neigh.cpp
 * @brief 邻居表实现
 */

#include "net/neigh.h"

#include "common/logger.h"

#include <cstring>
#include <vector>

#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_hash_crc.h>
#include <rte_rcu_qsbr.h>

namespace l4lb {

namespace {
constexpr uint64_t kValidBit = 1ULL << 63;

uint64_t pack(const MacAddr &mac) {
  uint64_t v = 0;
  memcpy(&v, mac.data(), 6);
  return v | kValidBit;
}

void unpack(uint64_t v, MacAddr &mac) { memcpy(mac.data(), &v, 6); }
} // namespace

bool NeighTable::init(struct rte_rcu_qsbr *qsbr, int socket_id) {
  struct rte_hash_parameters params = {};
  params.name = "neigh_table";
  params.entries = kCapacity;
  params.key_len = sizeof(IPv4Addr);
  params.hash_func = rte_hash_crc;
  params.socket_id = socket_id;
  // 单写者（master）+ 无锁读；删除的槽位由 RCU 回收
  params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;
  hash_ = rte_hash_create(&params);
  if (!hash_) {
    LOG_ERROR("neigh table: rte_hash_create failed: %s",
              rte_strerror(rte_errno));
    return false;
  }
  struct rte_hash_rcu_config rcu = {};
  rcu.v = qsbr;
  rcu.mode = RTE_HASH_QSBR_MODE_DQ;
  if (rte_hash_rcu_qsbr_add(hash_, &rcu) != 0) {
    LOG_ERROR("neigh table: attach RCU failed: %s", rte_strerror(rte_errno));
    destroy();
    return false;
  }
  meta_.reserve(1024);
  return true;
}

void NeighTable::destroy() {
  if (hash_)
    rte_hash_free(hash_);
  hash_ = nullptr;
  meta_.clear();
}

bool NeighTable::lookup(IPv4Addr ip, MacAddr &mac) const {
  void *data = nullptr;
  if (rte_hash_lookup_data(hash_, &ip, &data) < 0)
    return false;
  uint64_t v = reinterpret_cast<uintptr_t>(data);
  if (!(v & kValidBit))
    return false;
  unpack(v, mac);
  return true;
}

void NeighTable::store(IPv4Addr ip, const MacAddr &mac) {
  // 64 位 value 原子写入：读者要么看到旧 MAC，要么看到新 MAC
  if (rte_hash_add_key_data(hash_, &ip,
                            reinterpret_cast<void *>(pack(mac))) < 0) {
    LOG_RATELIMIT(l4lb::LogLevel::WARN, 10, "neigh table full");
    return;
  }
  auto &m = meta_[ip];
  if (!m.valid)
    count_.fetch_add(1, std::memory_order_relaxed);
  m.valid = true;
  gen_.fetch_add(1, std::memory_order_release);
}

void NeighTable::erase(IPv4Addr ip) {
  rte_hash_del_key(hash_, &ip);
  auto it = meta_.find(ip);
  if (it != meta_.end()) {
    if (it->second.valid)
      count_.fetch_sub(1, std::memory_order_relaxed);
    meta_.erase(it);
  }
  gen_.fetch_add(1, std::memory_order_release);
}

void NeighTable::add_static(IPv4Addr ip, const MacAddr &mac) {
  store(ip, mac);
  meta_[ip].is_static = true;
}

void NeighTable::learn(IPv4Addr ip, const MacAddr &mac, bool solicited,
                       uint64_t now) {
  auto it = meta_.find(ip);
  if (it != meta_.end() && it->second.is_static)
    return;
  bool known = it != meta_.end() && it->second.valid;
  // 未请求过、表里也没有的地址：表快满时不学习，防止被大量 ARP 撑满
  if (!solicited && !known && count() >= kCapacity / 2)
    return;
  MacAddr old{};
  if (!known || !lookup(ip, old) || old != mac) {
    if (known && old != mac)
      LOG_INFO("neigh %s: %s -> %s", ip_to_string(ip).c_str(),
               mac_to_string(old).c_str(), mac_to_string(mac).c_str());
    store(ip, mac);
  }
  meta_[ip].confirmed = now;
}

void NeighTable::hint(IPv4Addr ip, const MacAddr &mac, uint64_t now) {
  auto it = meta_.find(ip);
  if (it != meta_.end() && it->second.valid)
    return; // 已有条目以 ARP 结果为准
  if (count() >= kCapacity / 2)
    return;
  store(ip, mac);
  meta_[ip].confirmed = now;
}

bool NeighTable::want_request(IPv4Addr ip, uint64_t now) {
  auto &m = meta_[ip];
  if (m.is_static || now - m.requested < kRequestGapSec)
    return false;
  m.requested = now;
  return true;
}

void NeighTable::age(uint64_t now, std::vector<IPv4Addr> &refresh) {
  std::vector<IPv4Addr> dead;
  for (auto &kv : meta_) {
    const Meta &m = kv.second;
    if (m.is_static || !m.valid)
      continue;
    uint64_t idle = now - m.confirmed;
    if (idle >= kReachableSec)
      dead.push_back(kv.first);
    else if (idle >= kRefreshSec && now - m.requested >= kRequestGapSec * 5)
      refresh.push_back(kv.first);
  }
  for (auto ip : dead)
    erase(ip);
  for (auto ip : refresh)
    meta_[ip].requested = now;
  // 清理只有 requested 记录、从未学到的条目
  for (auto it = meta_.begin(); it != meta_.end();) {
    if (!it->second.valid && !it->second.is_static &&
        now - it->second.requested > kReachableSec)
      it = meta_.erase(it);
    else
      ++it;
  }
}

} // namespace l4lb
