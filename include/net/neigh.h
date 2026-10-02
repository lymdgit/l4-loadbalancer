/**
 * @file neigh.h
 * @brief 邻居表（IPv4 -> MAC）
 *
 * - 只有 master lcore 写（学习、老化、发 ARP 请求），worker 只读
 * - rte_hash 无锁读模式；value 直接存打包后的 MAC（64 位，原子可见），
 *   删除由 RCU（worker 共用的 QSBR）在宽限期后回收槽位
 * - generation 每次变化 +1，会话里缓存的 MAC 据此判断是否失效，
 *   热路径上大多数包不需要查表
 * - 只学习：本机发出的请求得到的应答、请求本机地址的直连主机、
 *   以及新会话的直连客户端（不覆盖已有条目）。不会因为任意报文改写已有条目
 * - 配置中写死的 RS MAC 作为静态条目，不老化
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_NET_NEIGH_H
#define L4LB_NET_NEIGH_H

#include "common/types.h"
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct rte_hash;
struct rte_rcu_qsbr;

namespace l4lb {

class NeighTable {
public:
  static constexpr uint32_t kCapacity = 65536;
  static constexpr uint64_t kReachableSec = 300; ///< 多久没确认就老化
  static constexpr uint64_t kRefreshSec = 240;   ///< 主动发请求刷新的时间点
  static constexpr uint64_t kRequestGapSec = 1;  ///< 同一 IP 请求间隔

  bool init(struct rte_rcu_qsbr *qsbr, int socket_id);
  void destroy();

  // ---- 读（任意 worker）----
  bool lookup(IPv4Addr ip, MacAddr &mac) const;
  uint32_t generation() const {
    return gen_.load(std::memory_order_acquire);
  }
  uint32_t count() const { return count_.load(std::memory_order_relaxed); }

  // ---- 写（只能在 master 上调用）----
  /// 静态条目（配置的 RS MAC），不老化
  void add_static(IPv4Addr ip, const MacAddr &mac);
  /// ARP 学习；solicited = 是本机请求过的地址
  void learn(IPv4Addr ip, const MacAddr &mac, bool solicited, uint64_t now);
  /// 新会话的直连客户端：只在没有条目且表不太满时加入
  void hint(IPv4Addr ip, const MacAddr &mac, uint64_t now);
  /// 是否需要（且允许）发 ARP 请求；返回 true 时调用方负责发出
  bool want_request(IPv4Addr ip, uint64_t now);
  /**
   * @brief 老化：删除长期未确认的条目，返回需要刷新的 IP（调用方发请求）
   * @param refresh [out] 即将过期、需要主动刷新的地址
   */
  void age(uint64_t now, std::vector<IPv4Addr> &refresh);

private:
  struct Meta {
    uint64_t confirmed = 0; ///< 最近一次确认的时间（秒）
    uint64_t requested = 0; ///< 最近一次发请求的时间
    bool is_static = false;
    bool valid = false;     ///< 已写入 hash
  };

  void store(IPv4Addr ip, const MacAddr &mac);
  void erase(IPv4Addr ip);

  struct rte_hash *hash_ = nullptr;
  std::unordered_map<IPv4Addr, Meta> meta_; ///< master 私有
  std::atomic<uint32_t> gen_{1};
  std::atomic<uint32_t> count_{0};
};

} // namespace l4lb

#endif // L4LB_NET_NEIGH_H
