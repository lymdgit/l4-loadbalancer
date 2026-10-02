/**
 * @file snapshot.h
 * @brief 数据面使用的只读配置快照（服务、RS、本机地址、调度表）
 *
 * 控制面（配置加载、控制命令、健康检查结果）修改的是 SnapshotManager 里的
 * "期望状态"，然后构建一份新的 Snapshot 整体替换：
 *
 *   worker:  snap = mgr.current();  ...处理一批包...  rte_rcu_qsbr_quiescent()
 *   control: 构建新 snapshot -> 原子替换指针 -> rte_rcu_qsbr_synchronize() -> 删除旧的
 *
 * 数据面读快照不加锁、不做原子 RMW；快照构建后不再修改（WRR 游标除外，per-worker）。
 *
 * RS 用全局唯一、不复用的 id 标识；会话记录 rs_id，RS 被删除、下线或权重
 * 变化后，会话通过 rs_by_id 查到它的最新状态。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CTRL_SNAPSHOT_H
#define L4LB_CTRL_SNAPSHOT_H

#include "common/config.h"
#include "common/types.h"
#include "lb/scheduler.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct rte_rcu_qsbr;

namespace l4lb {

constexpr uint32_t kMaxRsId = 1u << 16; ///< RS id 上限（不复用）

/// 快照中的一个 RS
struct RsState {
  uint32_t id;
  uint16_t svc_idx;
  IPv4Addr ip;      ///< 网络字节序
  Port port_be;     ///< 网络字节序
  uint16_t port;    ///< 主机字节序
  MacAddr mac;      ///< 静态 MAC，全 0 表示走邻居表
  uint32_t weight;  ///< 0：不接新连接，已有连接继续（drain）
  bool enabled;     ///< 管理状态；false 时已有连接也会被断开
  bool healthy;     ///< 健康检查结果

  /// 已有会话能否继续使用
  bool usable() const { return enabled && healthy; }
  /// 能否接新连接
  bool schedulable() const { return usable() && weight > 0; }
};

/// 快照中的一个服务
struct Service {
  uint16_t idx;
  std::string name;
  IPv4Addr vip;
  Port port_be;
  uint16_t port;
  uint8_t proto;
  std::vector<RsState *> rs; ///< 指向 Snapshot::rs_pool
  Scheduler sched;
};

/// 不可变的配置快照
struct Snapshot {
  uint64_t version = 0;
  ForwardMode mode = ForwardMode::NAT;
  std::vector<Service> services;
  std::vector<RsState> rs_pool;
  std::vector<RsState *> rs_by_id; ///< 下标为 RS id；被删除的 RS 为 nullptr

  // ---- 查找表（开放寻址，构建后只读）----
  const Service *find_service(IPv4Addr vip, Port port_be, uint8_t proto) const;
  bool is_vip(IPv4Addr ip) const { return set_has(vips_, ip); }
  bool is_lip(IPv4Addr ip) const { return set_has(lips_, ip); }
  /// ARP / ICMP 需要应答的本机地址（VIP + LIP + 健康检查源地址）
  bool is_local(IPv4Addr ip) const { return set_has(locals_, ip); }

  const RsState *rs(uint32_t id) const {
    return id < rs_by_id.size() ? rs_by_id[id] : nullptr;
  }

  std::vector<IPv4Addr> local_ips; ///< FULLNAT 的 LIP 列表（SNAT 源地址）
  IPv4Addr hc_src = 0;

  /// 构建查找表（Snapshot 构建完成后调用一次）
  void index();

private:
  static constexpr uint32_t kSvcSlots = 512;  ///< 2 的幂，> 2 * kMaxServices
  static constexpr uint32_t kAddrSlots = 512;

  static bool set_has(const std::vector<IPv4Addr> &set, IPv4Addr ip);
  static void set_add(std::vector<IPv4Addr> &set, IPv4Addr ip);

  std::vector<int16_t> svc_slots_; ///< 服务查找表：下标为 hash，值为服务下标
  std::vector<IPv4Addr> vips_, lips_, locals_;
};

/// 一次健康状态变化（master -> 控制线程）
struct HealthEvent {
  uint32_t rs_id;
  bool healthy;
};

/**
 * @brief 快照管理：持有期望状态，构建并发布快照
 *
 * 所有修改接口都在控制线程（或启动阶段）调用，内部加锁；
 * current() 可以在任意线程调用。
 */
class SnapshotManager {
public:
  bool init(const LbConfig &cfg, struct rte_rcu_qsbr *qsbr);
  void destroy();

  /// 数据面读取当前快照（acquire 语义）
  const Snapshot *current() const {
    return cur_.load(std::memory_order_acquire);
  }

  // ---- 控制面修改，返回错误信息（空字符串表示成功）----
  std::string set_weight(uint32_t rs_id, uint32_t weight);
  std::string set_enabled(uint32_t rs_id, bool enabled);
  std::string add_rs(uint16_t svc_idx, const RsConf &rs, uint32_t &new_id);
  std::string del_rs(uint32_t rs_id);
  void set_health(uint32_t rs_id, bool healthy);

  /// 用期望状态构建新快照并替换；等待所有 worker 离开旧快照后释放旧快照
  void publish();

  /// 文本形式的服务/RS 列表（控制命令 services）
  std::string describe() const;

private:
  struct DesiredRs {
    uint32_t id;
    RsConf conf;
    bool enabled = true;
    bool healthy = true;
  };
  struct DesiredService {
    ServiceConf conf; ///< conf.rs 不使用，RS 列表在 rs 中
    std::vector<DesiredRs> rs;
  };

  DesiredRs *find_rs(uint32_t rs_id, uint16_t *svc_idx = nullptr);
  Snapshot *build() const;

  mutable std::mutex mu_;
  ForwardMode mode_ = ForwardMode::NAT;
  std::vector<IPv4Addr> local_ips_;
  IPv4Addr hc_src_ = 0;
  std::vector<DesiredService> services_;
  uint32_t next_rs_id_ = 1; ///< 0 保留
  uint64_t version_ = 0;

  std::atomic<Snapshot *> cur_{nullptr};
  struct rte_rcu_qsbr *qsbr_ = nullptr;
};

} // namespace l4lb

#endif // L4LB_CTRL_SNAPSHOT_H
