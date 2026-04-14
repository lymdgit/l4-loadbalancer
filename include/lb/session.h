/**
 * @file session.h
 * @brief Session manager - per-lcore tables with rte_hash reverse table.
 *
 * 反向会话表使用 DPDK rte_hash 实现：
 * - 零动态内存分配（Mempool/rte_malloc 预分配）
 * - Lock-free 读路径（RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF）
 * - SIMD 加速批量 Key 比对 + 紧凑连续内存布局
 */

#ifndef L4LB_LB_SESSION_H
#define L4LB_LB_SESSION_H

#include "common/logger.h"
#include "common/types.h"
#include <array>
#include <atomic>
#include <unordered_map>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_malloc.h>
//  单例模式优点：
//  实例放在静态存储区，生命周期由编译器控制，程序员不需要担心内存泄漏
//  不用就不进行实例化，更加灵活
//  全局访问很简单，SessionManager::instance()就可以获取到实例(引用)
//  局部静态变量的初始化是线程安全的：多个核都抢着建立，C++会拦住所有并发，只让一个创建成功

namespace l4lb {

struct SessionDebugStats {
  uint64_t lookup_hit = 0;
  uint64_t lookup_miss = 0;
  uint64_t reverse_hit = 0;
  uint64_t reverse_miss = 0;
  uint64_t create = 0;
  uint64_t update_miss = 0;
  uint64_t cleanup_removed = 0;
};

class SessionManager {
public:
  static SessionManager &instance() {
    // 当有多从instance时，这个函数会被多次调用，但是static变量只会被初始化一次
    static SessionManager mgr;
    // mgr就是引用，与指针不同的是，可以保证实例一定存在
    // 而对于指针，有可能为nullptr。这是最大的不同
    return mgr;
  }

  /**
   * @brief 初始化反向哈希表（必须在 EAL 初始化之后调用）
   *
   * rte_hash 依赖 DPDK hugepage 内存，因此不能在构造函数中完成初始化，
   * 必须在 rte_eal_init() 之后显式调用。
   *
   * @return true 初始化成功
   */
  bool init() {
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
    reverse_data_ = static_cast<ReverseEntry *>(rte_zmalloc(
        "reverse_data", sizeof(ReverseEntry) * kReverseCapacity,
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

  /**
   * @brief 释放反向哈希表资源
   */
  void cleanup() {
    if (reverse_hash_) {
      rte_hash_free(reverse_hash_);
      reverse_hash_ = nullptr;
    }
    if (reverse_data_) {
      rte_free(reverse_data_);
      reverse_data_ = nullptr;
    }
  }

  void set_timeout(uint32_t seconds) {
    timeout_sec_ = seconds;
    timeout_tsc_ = rte_get_tsc_hz() * seconds;
    touch_tsc_ = timeout_tsc_ / 4;
    cleanup_interval_tsc_ = rte_get_tsc_hz(); // 1s
  }

  bool lookup(const FiveTuple &tuple, Session &session) {
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

  /**
   * @brief 反向查找（Lock-Free 读路径）
   *
   * 使用 rte_hash_lookup 进行无锁查找，直接通过返回的数组下标
   * 访问预分配的连续内存，无指针追逐、无锁、无 malloc。
   */
  bool lookup_reverse(const FiveTuple &reverse_tuple, Session &session) {
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

  Port create(const FiveTuple &client_tuple, uint32_t server_id,
              IPv4Addr rs_ip = 0, Port rs_port = 0) {
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

  void update_stats(const FiveTuple &tuple, uint64_t bytes) {
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

  size_t cleanup_local(uint64_t now_tsc) {
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

  Statistics get_stats() const {
    Statistics s{};
    s.active_sessions = active_sessions_.load(std::memory_order_relaxed);
    s.total_sessions = total_sessions_.load(std::memory_order_relaxed);
    return s;
  }

  SessionDebugStats get_debug_stats() const {
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

private:
  SessionManager() : timeout_sec_(300), timeout_tsc_(0) {}

  /**
   * @brief 反向表条目（存储在预分配的连续数组中）
   */
  struct ReverseEntry {
    FiveTuple client_tuple;
    uint32_t real_server_id;
  };

  struct Table {
    std::unordered_map<FiveTuple, Session, FiveTupleHash> sessions;
    uint64_t last_cleanup_tsc = 0;
  };

  Table &local_table() {
    unsigned lcore = rte_lcore_id();
    if (lcore >= RTE_MAX_LCORE)
      lcore = 0;
    return tables_[lcore];
  }

  /**
   * @brief 分配 NAT 源端口（使用 rte_hash 替代原来的 shard.map）
   *
   * rte_hash_add_key 返回的下标就是 reverse_data_[] 的索引，
   * 直接写入 value，零 malloc。
   */
  Port allocate_nat_src_port(IPv4Addr rs_ip, Port rs_port,
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

  uint32_t timeout_sec_;
  uint64_t timeout_tsc_;
  uint64_t touch_tsc_{0};
  uint64_t cleanup_interval_tsc_{0};
  std::atomic<uint64_t> active_sessions_{0};
  std::atomic<uint64_t> total_sessions_{0};
  std::atomic<uint32_t> next_nat_port_{0};
  std::atomic<uint64_t> lookup_hit_{0};
  std::atomic<uint64_t> lookup_miss_{0};
  std::atomic<uint64_t> reverse_hit_{0};
  std::atomic<uint64_t> reverse_miss_{0};
  std::atomic<uint64_t> create_{0};
  std::atomic<uint64_t> update_miss_{0};
  std::atomic<uint64_t> cleanup_removed_{0};
  // 正向表（per-lcore，无跨核访问）
  std::array<Table, RTE_MAX_LCORE> tables_;

  // =========================================================================
  // 反向表 — rte_hash + 预分配连续数组
  //
  // 替代原来的 1024 个 ReverseShard (spinlock + unordered_map)：
  // - rte_hash: 管理 key(FiveTuple) 的存储和查找，SIMD 加速
  // - reverse_data_: 连续内存数组，通过 rte_hash 返回的下标索引 value
  // - Lock-Free 读路径: RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF
  // =========================================================================
  static const uint32_t kReverseCapacity = 131072; // 128K 条目
  struct rte_hash *reverse_hash_ = nullptr;
  ReverseEntry *reverse_data_ = nullptr;

  SessionManager(const SessionManager &) = delete;
  SessionManager &operator=(const SessionManager &) = delete;
};

} // namespace l4lb

#endif // L4LB_LB_SESSION_H
