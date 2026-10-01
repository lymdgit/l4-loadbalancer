/**
 * @file session.h
 * @brief Session manager - per-lcore tables with rte_hash reverse table.
 *
 * 反向会话表使用 DPDK rte_hash 实现：
 * - 零动态内存分配（Mempool/rte_malloc 预分配）
 * - Lock-free 读路径（RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF）
 * - SIMD 加速批量 Key 比对 + 紧凑连续内存布局
 *
 * 实现见 src/lb/session.cpp
 */

#ifndef L4LB_LB_SESSION_H
#define L4LB_LB_SESSION_H

#include "common/types.h"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <unordered_map>

#include <rte_config.h> // RTE_MAX_LCORE

struct rte_hash;

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
    // static 变量只会被初始化一次；返回引用保证实例一定存在
    static SessionManager mgr;
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
  bool init();

  /// 释放反向哈希表资源
  void cleanup();

  /// 设置会话超时（秒）
  void set_timeout(uint32_t seconds);

  /// 在本 lcore 的正向表中查找会话
  bool lookup(const FiveTuple &tuple, Session &session);

  /**
   * @brief 反向查找（Lock-Free 读路径）
   *
   * 使用 rte_hash_lookup 进行无锁查找，直接通过返回的数组下标
   * 访问预分配的连续内存，无指针追逐、无锁、无 malloc。
   */
  bool lookup_reverse(const FiveTuple &reverse_tuple, Session &session);

  /**
   * @brief 创建会话
   * @return 分配的 NAT 源端口（网络字节序），rs_ip 为 0 时返回 0
   */
  Port create(const FiveTuple &client_tuple, uint32_t server_id,
              IPv4Addr rs_ip = 0, Port rs_port = 0);

  /// 更新本 lcore 正向表中会话的活跃时间和计数
  void update_stats(const FiveTuple &tuple, uint64_t bytes);

  /// 清理本 lcore 的过期会话，返回清理数量
  size_t cleanup_local(uint64_t now_tsc);

  Statistics get_stats() const;

  SessionDebugStats get_debug_stats() const;

private:
  SessionManager() : timeout_sec_(300), timeout_tsc_(0) {}

  /// 反向表条目（存储在预分配的连续数组中）
  struct ReverseEntry {
    FiveTuple client_tuple;
    uint32_t real_server_id;
  };

  struct Table {
    std::unordered_map<FiveTuple, Session, FiveTupleHash> sessions;
    uint64_t last_cleanup_tsc = 0;
  };

  /// 当前 lcore 的正向表
  Table &local_table();

  /**
   * @brief 分配 NAT 源端口（使用 rte_hash 替代原来的 shard.map）
   *
   * rte_hash_add_key 返回的下标就是 reverse_data_[] 的索引，
   * 直接写入 value，零 malloc。
   */
  Port allocate_nat_src_port(IPv4Addr rs_ip, Port rs_port,
                             const FiveTuple &client_tuple,
                             uint32_t server_id);

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
