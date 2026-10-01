/**
 * @file session.cpp
 * @brief 会话管理实现
 */

#include "lb/session.h"

#include "common/logger.h"

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_errno.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>

namespace l4lb {

bool SessionManager::init() {
  // 创建 rte_hash：反向会话表
  struct rte_hash_parameters params = {};
  params.name = "reverse_session_hash";
  params.entries = kReverseCapacity;
  params.key_len = sizeof(FiveTuple);
  params.hash_func = rte_jhash;
  params.hash_func_init_val = 0;
  params.socket_id = rte_socket_id();
  // 开启 Lock-Free 读写并发模式
  // 读路径完全无锁，写路径内部使用 CAS 原子操作
  params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;

  reverse_hash_ = rte_hash_create(&params);
  if (!reverse_hash_) {
    LOG_ERROR("Failed to create rte_hash for reverse session table: %s",
              rte_strerror(rte_errno));
    return false;
  }

  // 预分配 value 数组（rte_hash 只管 key 的存储和查找，value 由用户管理）
  // 使用 rte_zmalloc 从 hugepage 分配，保证 cache line 对齐
  reverse_data_ = static_cast<ReverseEntry *>(
      rte_zmalloc("reverse_data", sizeof(ReverseEntry) * kReverseCapacity,
                  RTE_CACHE_LINE_SIZE));
  if (!reverse_data_) {
    LOG_ERROR("Failed to allocate reverse data array");
    rte_hash_free(reverse_hash_);
    reverse_hash_ = nullptr;
    return false;
  }

  LOG_INFO("Reverse session hash table initialized: capacity=%u, "
           "key_len=%zu, lock-free mode",
           kReverseCapacity, sizeof(FiveTuple));
  return true;
}

void SessionManager::cleanup() {
  if (reverse_hash_) {
    rte_hash_free(reverse_hash_);
    reverse_hash_ = nullptr;
  }
  if (reverse_data_) {
    rte_free(reverse_data_);
    reverse_data_ = nullptr;
  }
}

void SessionManager::set_timeout(uint32_t seconds) {
  timeout_sec_ = seconds;
  timeout_tsc_ = rte_get_tsc_hz() * seconds;
  touch_tsc_ = timeout_tsc_ / 4;
  cleanup_interval_tsc_ = rte_get_tsc_hz(); // 1s
}

bool SessionManager::lookup(const FiveTuple &tuple, Session &session) {
  auto &tbl = local_table();
  auto it = tbl.sessions.find(tuple);
  if (it == tbl.sessions.end()) {
    lookup_miss_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  lookup_hit_.fetch_add(1, std::memory_order_relaxed);
  uint64_t now_tsc = rte_get_tsc_cycles();
  if (now_tsc - it->second.last_active > touch_tsc_) {
    it->second.last_active = now_tsc;
  }
  session = it->second;
  return true;
}

bool SessionManager::lookup_reverse(const FiveTuple &reverse_tuple,
                                    Session &session) {
  int32_t idx = rte_hash_lookup(reverse_hash_,
                                static_cast<const void *>(&reverse_tuple));
  if (idx < 0) {
    reverse_miss_.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  // 直接数组下标访问，无锁、无指针追逐
  session.client_tuple = reverse_data_[idx].client_tuple;
  session.real_server_id = reverse_data_[idx].real_server_id;
  reverse_hit_.fetch_add(1, std::memory_order_relaxed);
  session.server_tuple = reverse_tuple;
  return true;
}

Port SessionManager::create(const FiveTuple &client_tuple, uint32_t server_id,
                            IPv4Addr rs_ip, Port rs_port) {
  uint64_t tsc = rte_get_tsc_cycles();
  Session session;
  session.client_tuple = client_tuple;
  session.real_server_id = server_id;
  session.nat_src_port = 0;
  session.create_time = tsc;
  session.last_active = tsc;
  session.packets = 0;
  session.bytes = 0;

  auto &tbl = local_table();

  if (rs_ip != 0) {
    session.nat_src_port =
        allocate_nat_src_port(rs_ip, rs_port, client_tuple, server_id);
    session.server_tuple =
        FiveTuple(rs_ip, client_tuple.dst_ip, rs_port, session.nat_src_port,
                  client_tuple.protocol);
  }

  tbl.sessions[client_tuple] = session;

  total_sessions_.fetch_add(1, std::memory_order_relaxed);
  active_sessions_.fetch_add(1, std::memory_order_relaxed);
  create_.fetch_add(1, std::memory_order_relaxed);
  return session.nat_src_port;
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
    update_miss_.fetch_add(1, std::memory_order_relaxed);
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
      // 删除反向表条目
      if (it->second.server_tuple.src_ip != 0) {
        rte_hash_del_key(reverse_hash_, &it->second.server_tuple);
      }
      it = tbl.sessions.erase(it);
      ++removed_count;
      active_sessions_.fetch_sub(1, std::memory_order_relaxed);
    } else {
      ++it;
    }
  }
  tbl.last_cleanup_tsc = now_tsc;
  cleanup_removed_.fetch_add(removed_count, std::memory_order_relaxed);
  return removed_count;
}

Statistics SessionManager::get_stats() const {
  Statistics s{};
  s.active_sessions = active_sessions_.load(std::memory_order_relaxed);
  s.total_sessions = total_sessions_.load(std::memory_order_relaxed);
  return s;
}

SessionDebugStats SessionManager::get_debug_stats() const {
  SessionDebugStats s;
  s.lookup_hit = lookup_hit_.load(std::memory_order_relaxed);
  s.lookup_miss = lookup_miss_.load(std::memory_order_relaxed);
  s.reverse_hit = reverse_hit_.load(std::memory_order_relaxed);
  s.reverse_miss = reverse_miss_.load(std::memory_order_relaxed);
  s.create = create_.load(std::memory_order_relaxed);
  s.update_miss = update_miss_.load(std::memory_order_relaxed);
  s.cleanup_removed = cleanup_removed_.load(std::memory_order_relaxed);
  return s;
}

SessionManager::Table &SessionManager::local_table() {
  unsigned lcore = rte_lcore_id();
  if (lcore >= RTE_MAX_LCORE)
    lcore = 0;
  return tables_[lcore];
}

Port SessionManager::allocate_nat_src_port(IPv4Addr rs_ip, Port rs_port,
                                           const FiveTuple &client_tuple,
                                           uint32_t server_id) {
  if (!rs_ip || !rs_port)
    return 0;

  static const uint16_t kPortMin = 10000;
  static const uint16_t kPortMax = 60000;
  static const uint32_t kPortRange = kPortMax - kPortMin + 1;

  for (uint32_t i = 0; i < kPortRange; ++i) {
    uint32_t next = next_nat_port_.fetch_add(1, std::memory_order_relaxed);
    uint16_t host_port = (uint16_t)(kPortMin + (next % kPortRange));
    Port nat_port = rte_cpu_to_be_16(host_port);

    FiveTuple reverse_tuple(rs_ip, client_tuple.dst_ip, rs_port, nat_port,
                            client_tuple.protocol);

    // 先查是否已存在
    int32_t idx = rte_hash_lookup(reverse_hash_, &reverse_tuple);
    if (idx >= 0) {
      // 该端口已被占用，尝试下一个
      continue;
    }

    // 不存在，插入新条目
    idx = rte_hash_add_key(reverse_hash_, &reverse_tuple);
    if (idx < 0) {
      // hash 表已满或插入失败，尝试下一个端口
      LOG_WARN("rte_hash_add_key failed: %s", rte_strerror(-idx));
      continue;
    }

    // 写入 value（通过数组下标，零 malloc）
    reverse_data_[idx].client_tuple = client_tuple;
    reverse_data_[idx].real_server_id = server_id;
    return nat_port;
  }

  LOG_WARN("NAT port allocation exhausted, fallback to client src_port");
  return client_tuple.src_port;
}

} // namespace l4lb
