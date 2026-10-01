/**
 * @file session.h
 * @brief Session manager - per-lcore 正向表 + 全局 rte_hash 反向表
 *
 * 正向表：per-lcore unordered_map，只由所属 lcore 访问，无锁。
 *
 * 反向表：全局 rte_hash，FULLNAT 回程包可能落在任意 lcore 上，所以需要共享：
 * - RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF：读路径无锁
 * - RTE_HASH_EXTRA_FLAGS_MULTI_WRITER_ADD：多个 lcore 同时增删时由 rte_hash 内部加锁
 * - 内置 RCU QSBR（defer queue 模式）：删除后的 key 槽位和 value 要等所有
 *   worker 都经过一次静默期（rte_rcu_qsbr_quiescent）后才回收，
 *   保证正在读的 lcore 不会读到被复用的槽位
 * - value 是从 mempool 取出的 ReverseEntry 指针：先填好再用 add_key_data 发布，
 *   回收时由 RCU 回调放回 mempool
 *
 * 阶段 3 会改为 per-lcore 反向表（回程包导回本核），届时不再需要 RCU。
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
#include <rte_rcu_qsbr.h>

struct rte_hash;
struct rte_mempool;

namespace l4lb {

struct SessionDebugStats {
  uint64_t lookup_hit = 0;
  uint64_t lookup_miss = 0;
  uint64_t reverse_hit = 0;
  uint64_t reverse_miss = 0;
  uint64_t create = 0;
  uint64_t create_fail = 0; ///< NAT 端口或反向表分配失败
  uint64_t replaced = 0;    ///< 同一五元组的旧会话被替换
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
   * @brief 初始化反向哈希表和 RCU（必须在 EAL 初始化之后调用）
   *
   * rte_hash / mempool 依赖 DPDK hugepage 内存，因此不能在构造函数中完成，
   * 必须在 rte_eal_init() 之后显式调用。
   */
  bool init();

  /// 释放反向哈希表资源（所有 worker 必须已调用 worker_offline）
  void cleanup();

  /// 设置会话超时（秒）
  void set_timeout(uint32_t seconds);

  // ---------------------------------------------------------------------------
  // RCU：每个 worker lcore 进入循环前 online，每轮循环 quiescent，退出前 offline
  // ---------------------------------------------------------------------------
  void worker_online(unsigned lcore_id);
  void worker_offline(unsigned lcore_id);

  /// 报告静默期：本 lcore 此刻不持有任何反向表 value 指针
  void quiescent(unsigned lcore_id) {
    rte_rcu_qsbr_quiescent(qsbr_, lcore_id);
  }

  // ---------------------------------------------------------------------------
  // 会话操作（正向表只访问当前 lcore 的那一份）
  // ---------------------------------------------------------------------------

  /// 在本 lcore 的正向表中查找会话
  bool lookup(const FiveTuple &tuple, Session &session);

  /**
   * @brief 反向查找（Lock-Free 读路径）
   *
   * 读到的 value 在本 lcore 下一次 quiescent() 之前一直有效。
   */
  bool lookup_reverse(const FiveTuple &reverse_tuple, Session &session);

  /**
   * @brief 创建会话；同一五元组的旧会话（及其反向表条目）会先被删除
   *
   * @param rs_ip 非 0 时（NAT 模式）分配 SNAT 源端口并插入反向表
   * @param nat_src_port [out] 分配的 NAT 源端口（网络字节序），DR 模式为 0
   * @return false NAT 端口或反向表空间耗尽，会话未创建
   */
  bool create(const FiveTuple &client_tuple, uint32_t server_id,
              IPv4Addr rs_ip, Port rs_port, Port &nat_src_port);

  /// 删除本 lcore 正向表中的会话及其反向表条目
  bool remove(const FiveTuple &client_tuple);

  /// 更新本 lcore 正向表中会话的活跃时间和计数
  void update_stats(const FiveTuple &tuple, uint64_t bytes);

  /// 清理本 lcore 的过期会话，返回清理数量
  size_t cleanup_local(uint64_t now_tsc);

  /// 汇总所有 lcore 的会话数
  Statistics get_stats() const;

  SessionDebugStats get_debug_stats() const;

private:
  SessionManager() : timeout_sec_(300), timeout_tsc_(0) {}

  /// 反向表 value（从 mempool 分配，RCU 回收）
  struct ReverseEntry {
    FiveTuple client_tuple;
    uint32_t real_server_id;
  };

  struct Table {
    std::unordered_map<FiveTuple, Session, FiveTupleHash> sessions;
    uint64_t last_cleanup_tsc = 0;
  };

  /// per-lcore 计数器，只由所属 lcore 写（见 common/stats.h）
  struct alignas(RTE_CACHE_LINE_SIZE) LcoreCounters {
    std::atomic<uint64_t> created{0};
    std::atomic<uint64_t> removed{0};
    std::atomic<uint64_t> lookup_hit{0};
    std::atomic<uint64_t> lookup_miss{0};
    std::atomic<uint64_t> reverse_hit{0};
    std::atomic<uint64_t> reverse_miss{0};
    std::atomic<uint64_t> create_fail{0};
    std::atomic<uint64_t> replaced{0};
    std::atomic<uint64_t> update_miss{0};
    std::atomic<uint64_t> cleanup_removed{0};
  };

  /// 当前 lcore 的正向表
  Table &local_table();
  LcoreCounters &local_counters();

  /// 删除一条正向表条目及其反向表条目，返回下一个迭代器
  using SessionIter =
      std::unordered_map<FiveTuple, Session, FiveTupleHash>::iterator;
  SessionIter erase_session(Table &tbl, SessionIter it);

  /**
   * @brief 分配 NAT 源端口并插入反向表
   * @return true 成功，nat_port 为网络字节序端口
   */
  bool allocate_nat_src_port(IPv4Addr rs_ip, Port rs_port,
                             const FiveTuple &client_tuple,
                             uint32_t server_id, Port &nat_port);

  /// RCU 回收回调：把 ReverseEntry 放回 mempool
  static void free_reverse_entry(void *p, void *key_data);

  uint32_t timeout_sec_;
  uint64_t timeout_tsc_;
  uint64_t touch_tsc_{0};
  uint64_t cleanup_interval_tsc_{0};
  std::atomic<uint32_t> next_nat_port_{0};

  // 正向表和计数器（per-lcore，无跨核写）
  std::array<Table, RTE_MAX_LCORE> tables_;
  std::array<LcoreCounters, RTE_MAX_LCORE> counters_;

  // 反向表
  static const uint32_t kReverseCapacity = 131072; // 128K 条目
  struct rte_hash *reverse_hash_ = nullptr;
  struct rte_mempool *reverse_pool_ = nullptr;
  struct rte_rcu_qsbr *qsbr_ = nullptr;

  SessionManager(const SessionManager &) = delete;
  SessionManager &operator=(const SessionManager &) = delete;
};

} // namespace l4lb

#endif // L4LB_LB_SESSION_H
