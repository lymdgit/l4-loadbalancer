/**
 * @file session.cpp
 * @brief 会话管理实现
 */

#include "lb/session.h"

#include "common/logger.h"
#include "common/stats.h"

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
#include <rte_mempool.h>

namespace l4lb {

bool SessionManager::init() {
  // 1. RCU QSBR 变量：每个 lcore 一个线程槽位
  size_t qsbr_size = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
  qsbr_ = static_cast<struct rte_rcu_qsbr *>(
      rte_zmalloc("session_qsbr", qsbr_size, RTE_CACHE_LINE_SIZE));
  if (!qsbr_ || rte_rcu_qsbr_init(qsbr_, RTE_MAX_LCORE) != 0) {
    LOG_ERROR("Failed to init RCU QSBR variable");
    cleanup();
    return false;
  }

  // 2. 反向表 value 池：容量与 hash 一致，另留出 lcore 缓存和 defer queue 中
  //    尚未回收的余量
  reverse_pool_ = rte_mempool_create(
      "reverse_entry_pool", kReverseCapacity * 2 - 1, sizeof(ReverseEntry), 256,
      0, nullptr, nullptr, nullptr, nullptr, rte_socket_id(), 0);
  if (!reverse_pool_) {
    LOG_ERROR("Failed to create reverse entry mempool: %s",
              rte_strerror(rte_errno));
    cleanup();
    return false;
  }

  // 3. 反向表 rte_hash
  struct rte_hash_parameters params = {};
  params.name = "reverse_session_hash";
  params.entries = kReverseCapacity;
  params.key_len = sizeof(FiveTuple);
  params.hash_func = rte_jhash;
  params.hash_func_init_val = 0;
  params.socket_id = rte_socket_id();
  // 读路径无锁；多个 lcore 都会增删，因此还需要多写者支持
  params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF |
                      RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD;

  reverse_hash_ = rte_hash_create(&params);
  if (!reverse_hash_) {
    LOG_ERROR("Failed to create rte_hash for reverse session table: %s",
              rte_strerror(rte_errno));
    cleanup();
    return false;
  }

  // 4. 挂接内置 RCU：LF 模式下 del 不会立即释放槽位，必须由 RCU 在宽限期后
  //    回收槽位并通过回调释放 value。defer queue 满或 add 找不到空槽时，
  //    rte_hash 会自动触发回收。
  struct rte_hash_rcu_config rcu_cfg = {};
  rcu_cfg.v = qsbr_;
  rcu_cfg.mode = RTE_HASH_QSBR_MODE_DQ;
  rcu_cfg.dq_size = 0; // 默认等于表容量
  rcu_cfg.trigger_reclaim_limit = 1024;
  rcu_cfg.max_reclaim_size = 256;
  rcu_cfg.key_data_ptr = this;
  rcu_cfg.free_key_data_func = &SessionManager::free_reverse_entry;
  if (rte_hash_rcu_qsbr_add(reverse_hash_, &rcu_cfg) != 0) {
    LOG_ERROR("Failed to attach RCU QSBR to reverse hash: %s",
              rte_strerror(rte_errno));
    cleanup();
    return false;
  }

  LOG_INFO("Reverse session hash table initialized: capacity=%u, "
           "key_len=%zu, lock-free read + multi-writer + RCU reclaim",
           kReverseCapacity, sizeof(FiveTuple));
  return true;
}

void SessionManager::cleanup() {
  // rte_hash_free 会先回收 defer queue 中剩余的条目（调用 free_reverse_entry），
  // 所以必须先于 mempool 释放
  if (reverse_hash_) {
    rte_hash_free(reverse_hash_);
    reverse_hash_ = nullptr;
  }
  if (reverse_pool_) {
    rte_mempool_free(reverse_pool_);
    reverse_pool_ = nullptr;
  }
  if (qsbr_) {
    rte_free(qsbr_);
    qsbr_ = nullptr;
  }
}

void SessionManager::free_reverse_entry(void *p, void *key_data) {
  auto *self = static_cast<SessionManager *>(p);
  if (key_data)
    rte_mempool_put(self->reverse_pool_, key_data);
}

void SessionManager::set_timeout(uint32_t seconds) {
  timeout_sec_ = seconds;
  timeout_tsc_ = rte_get_tsc_hz() * seconds;
  touch_tsc_ = timeout_tsc_ / 4;
  cleanup_interval_tsc_ = rte_get_tsc_hz(); // 1s
}

void SessionManager::worker_online(unsigned lcore_id) {
  rte_rcu_qsbr_thread_register(qsbr_, lcore_id);
  rte_rcu_qsbr_thread_online(qsbr_, lcore_id);
}

void SessionManager::worker_offline(unsigned lcore_id) {
  rte_rcu_qsbr_thread_offline(qsbr_, lcore_id);
  rte_rcu_qsbr_thread_unregister(qsbr_, lcore_id);
}

bool SessionManager::lookup(const FiveTuple &tuple, Session &session) {
  auto &tbl = local_table();
  auto &cnt = local_counters();
  auto it = tbl.sessions.find(tuple);
  if (it == tbl.sessions.end()) {
    stat_add(cnt.lookup_miss);
    return false;
  }
  stat_add(cnt.lookup_hit);
  uint64_t now_tsc = rte_get_tsc_cycles();
  if (now_tsc - it->second.last_active > touch_tsc_) {
    it->second.last_active = now_tsc;
  }
  session = it->second;
  return true;
}

bool SessionManager::lookup_reverse(const FiveTuple &reverse_tuple,
                                    Session &session) {
  auto &cnt = local_counters();
  void *data = nullptr;
  if (rte_hash_lookup_data(reverse_hash_, &reverse_tuple, &data) < 0) {
    stat_add(cnt.reverse_miss);
    return false;
  }
  // value 在本 lcore 下一次 quiescent() 之前不会被回收
  const auto *entry = static_cast<const ReverseEntry *>(data);
  session.client_tuple = entry->client_tuple;
  session.real_server_id = entry->real_server_id;
  session.server_tuple = reverse_tuple;
  stat_add(cnt.reverse_hit);
  return true;
}

bool SessionManager::create(const FiveTuple &client_tuple, uint32_t server_id,
                            IPv4Addr rs_ip, Port rs_port, Port &nat_src_port) {
  auto &tbl = local_table();
  auto &cnt = local_counters();
  nat_src_port = 0;

  // 同一五元组已有会话（例如原 RS 下线后重新选择）：先删掉旧会话，
  // 否则旧的反向表条目会永远泄漏
  auto old = tbl.sessions.find(client_tuple);
  if (old != tbl.sessions.end()) {
    erase_session(tbl, old);
    stat_add(cnt.replaced);
  }

  uint64_t tsc = rte_get_tsc_cycles();
  Session session;
  session.client_tuple = client_tuple;
  session.real_server_id = server_id;
  session.nat_src_port = 0;
  session.create_time = tsc;
  session.last_active = tsc;
  session.packets = 0;
  session.bytes = 0;

  if (rs_ip != 0) {
    if (!allocate_nat_src_port(rs_ip, rs_port, client_tuple, server_id,
                               session.nat_src_port)) {
      stat_add(cnt.create_fail);
      return false;
    }
    session.server_tuple =
        FiveTuple(rs_ip, client_tuple.dst_ip, rs_port, session.nat_src_port,
                  client_tuple.protocol);
  }

  tbl.sessions.emplace(client_tuple, session);
  stat_add(cnt.created);
  nat_src_port = session.nat_src_port;
  return true;
}

bool SessionManager::remove(const FiveTuple &client_tuple) {
  auto &tbl = local_table();
  auto it = tbl.sessions.find(client_tuple);
  if (it == tbl.sessions.end())
    return false;
  erase_session(tbl, it);
  return true;
}

SessionManager::SessionIter SessionManager::erase_session(Table &tbl,
                                                          SessionIter it) {
  // 删除反向表条目：槽位和 value 由 RCU 在宽限期后回收
  if (it->second.server_tuple.src_ip != 0) {
    rte_hash_del_key(reverse_hash_, &it->second.server_tuple);
  }
  stat_add(local_counters().removed);
  return tbl.sessions.erase(it);
}

void SessionManager::update_stats(const FiveTuple &tuple, uint64_t bytes) {
  auto &tbl = local_table();
  auto it = tbl.sessions.find(tuple);
  if (it != tbl.sessions.end()) {
    uint64_t now_tsc = rte_get_tsc_cycles();
    if (now_tsc - it->second.last_active > touch_tsc_) {
      it->second.last_active = now_tsc;
    }
    ++it->second.packets;
    it->second.bytes += bytes;
  } else {
    stat_add(local_counters().update_miss);
  }
}

size_t SessionManager::cleanup_local(uint64_t now_tsc) {
  auto &tbl = local_table();
  if (now_tsc - tbl.last_cleanup_tsc < cleanup_interval_tsc_) {
    return 0;
  }

  size_t removed_count = 0;
  for (auto it = tbl.sessions.begin(); it != tbl.sessions.end();) {
    if (it->second.is_expired(now_tsc, timeout_tsc_)) {
      it = erase_session(tbl, it);
      ++removed_count;
    } else {
      ++it;
    }
  }
  tbl.last_cleanup_tsc = now_tsc;
  stat_add(local_counters().cleanup_removed, removed_count);
  return removed_count;
}

Statistics SessionManager::get_stats() const {
  Statistics s{};
  uint64_t created = 0, removed = 0;
  for (const auto &c : counters_) {
    created += stat_get(c.created);
    removed += stat_get(c.removed);
  }
  s.total_sessions = created;
  s.active_sessions = created - removed;
  return s;
}

SessionDebugStats SessionManager::get_debug_stats() const {
  SessionDebugStats s;
  for (const auto &c : counters_) {
    s.lookup_hit += stat_get(c.lookup_hit);
    s.lookup_miss += stat_get(c.lookup_miss);
    s.reverse_hit += stat_get(c.reverse_hit);
    s.reverse_miss += stat_get(c.reverse_miss);
    s.create += stat_get(c.created);
    s.create_fail += stat_get(c.create_fail);
    s.replaced += stat_get(c.replaced);
    s.update_miss += stat_get(c.update_miss);
    s.cleanup_removed += stat_get(c.cleanup_removed);
  }
  return s;
}

SessionManager::Table &SessionManager::local_table() {
  return tables_[stat_lcore()];
}

SessionManager::LcoreCounters &SessionManager::local_counters() {
  return counters_[stat_lcore()];
}

bool SessionManager::allocate_nat_src_port(IPv4Addr rs_ip, Port rs_port,
                                           const FiveTuple &client_tuple,
                                           uint32_t server_id,
                                           Port &nat_port) {
  if (!rs_ip || !rs_port)
    return false;

  static const uint16_t kPortMin = 10000;
  static const uint16_t kPortMax = 60000;
  static const uint32_t kPortRange = kPortMax - kPortMin + 1;
  // 端口被占用时最多再试这么多次；表满时快速失败，不在热路径上扫完整个端口段
  static const uint32_t kMaxProbe = 64;

  void *obj = nullptr;
  if (rte_mempool_get(reverse_pool_, &obj) != 0) {
    LOG_RATELIMIT(l4lb::LogLevel::WARN, 1, "Reverse entry pool exhausted");
    return false;
  }
  auto *entry = static_cast<ReverseEntry *>(obj);
  entry->client_tuple = client_tuple;
  entry->real_server_id = server_id;

  // 跨核共享的端口游标，阶段 3 改为 per-lcore 端口段
  for (uint32_t i = 0; i < kMaxProbe; ++i) {
    uint32_t next = next_nat_port_.fetch_add(1, std::memory_order_relaxed);
    uint16_t host_port = (uint16_t)(kPortMin + (next % kPortRange));
    Port candidate = rte_cpu_to_be_16(host_port);

    FiveTuple reverse_tuple(rs_ip, client_tuple.dst_ip, rs_port, candidate,
                            client_tuple.protocol);

    if (rte_hash_lookup(reverse_hash_, &reverse_tuple) >= 0) {
      continue; // 该端口已被占用
    }

    // value 先写好再发布：add_key_data 对读者是原子可见的
    int ret = rte_hash_add_key_data(reverse_hash_, &reverse_tuple, entry);
    if (ret == 0) {
      nat_port = candidate;
      return true;
    }
    if (ret == -ENOSPC) {
      break; // 表已满（rte_hash 已自动尝试过 RCU 回收）
    }
  }

  rte_mempool_put(reverse_pool_, entry);
  LOG_RATELIMIT(l4lb::LogLevel::WARN, 1,
                "NAT port allocation failed (reverse table full or ports busy)");
  return false;
}

} // namespace l4lb
