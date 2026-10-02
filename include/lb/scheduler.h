/**
 * @file scheduler.h
 * @brief 后端调度：加权轮询（WRR）与 Maglev 一致性哈希
 *
 * 调度器在控制面构建（每次配置或 RS 状态变化时重建），构建后只读，
 * 多个 worker 并发查询无需加锁：
 * - WRR：预先展开成长度为总权重的平滑序列，每个 worker 有自己的游标，
 *   选择是 O(1)
 * - Maglev：构建 M（素数）个槽位的查找表，按五元组哈希取槽，O(1)；
 *   RS 增减时只有约 1/N 的槽位改变，多台 LB 用同样的输入会得到同样的表，
 *   ECMP 场景下无需同步会话也能把同一连接调度到同一 RS
 *
 * 只把 UP 且权重 > 0 的 RS 放进调度表；没有可用 RS 时 pick() 返回 -1。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_LB_SCHEDULER_H
#define L4LB_LB_SCHEDULER_H

#include "common/config.h"
#include "common/types.h"
#include <array>
#include <cstdint>
#include <vector>

#include <rte_config.h> // RTE_MAX_LCORE

namespace l4lb {

/// 参与调度的后端（下标对应 service 内 RS 的位置）
struct SchedBackend {
  IPv4Addr ip;
  uint16_t port;
  uint32_t weight; ///< 0 表示不参与调度
};

class Scheduler {
public:
  static constexpr uint32_t kMaglevSize = 65537; ///< 素数，至少为后端数的 100 倍
  static constexpr uint32_t kWrrMaxSeq = 4096;   ///< WRR 序列长度上限

  Scheduler() = default;

  /// 构建调度表；backends 中 weight == 0 的不参与调度
  void build(SchedulerType type, const std::vector<SchedBackend> &backends);

  /**
   * @brief 为新连接选择后端
   * @param worker 调用方 worker 下标（WRR 游标是 per-worker 的）
   * @return backends 中的下标，-1 表示没有可用后端
   */
  int pick(const FiveTuple &tuple, unsigned worker) const;

  SchedulerType type() const { return type_; }
  bool empty() const { return table_.empty(); }

  /// 每个后端在调度表中占的槽位数（测试/展示用）
  std::vector<uint32_t> distribution(size_t n_backends) const;

private:
  void build_wrr(const std::vector<SchedBackend> &b);
  void build_maglev(const std::vector<SchedBackend> &b);

  SchedulerType type_ = SchedulerType::WRR;
  std::vector<uint16_t> table_; ///< WRR 序列 / Maglev 查找表，元素为后端下标

  /// per-worker WRR 游标；pick() 是 const，游标只由对应 worker 修改
  struct alignas(64) Cursor {
    uint32_t v;
  };
  mutable std::array<Cursor, RTE_MAX_LCORE> cursor_{};
};

} // namespace l4lb

#endif // L4LB_LB_SCHEDULER_H
