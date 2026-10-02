/**
 * @file session.h
 * @brief per-worker 会话表：rte_hash 索引 + 预分配会话数组 + 时间轮
 *
 * 每个 worker 一张表，只由所属 worker 访问，没有锁、没有原子操作、
 * 热路径上没有 malloc：
 * - 一个会话在 hash 里有两个 key：客户端方向五元组（Client -> VIP）和
 *   FULLNAT 回程五元组（RS -> LIP），都指向同一个会话下标
 * - 会话对象放在启动时一次性分配的数组里，空闲下标用栈管理
 * - 超时用 1 秒粒度的时间轮：每包只更新 expire_tick（不移动链表），
 *   时间轮扫到时再判断是否真的过期，未过期的重新挂到新位置；
 *   每次推进有处理数量上限，不会出现全表扫描造成的停顿
 *
 * 回程包能回到创建会话的 worker 由 dataplane/steering.h 保证。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_LB_SESSION_H
#define L4LB_LB_SESSION_H

#include "common/types.h"
#include "lb/tcp_state.h"
#include <cstddef>
#include <cstdint>

struct rte_hash;

namespace l4lb {

/// 会话标志
enum SessionFlag : uint8_t {
  SF_TOA_PENDING = 0x01, ///< 还需要在客户端方向的包里插入 TOA
  SF_FULLNAT = 0x02,     ///< 有回程 key（FULLNAT）
};

struct Session {
  FiveTuple client; ///< Client -> VIP（入站方向，网络字节序）
  FiveTuple server; ///< RS -> LIP（FULLNAT 回程方向）；DR 模式全 0
  uint32_t rs_id;   ///< snapshot 中的 RS id
  uint16_t svc_idx; ///< snapshot 中的服务下标（只用于统计/展示）
  TcpState state;
  uint8_t fin_seen; ///< bit0 客户端发过 FIN，bit1 RS 发过 FIN
  uint8_t flags;    ///< SessionFlag

  // 下一跳 MAC 缓存：邻居表 generation 变化时重新查询
  MacAddr rs_mac;
  MacAddr cli_mac;
  MacAddr cli_src_mac; ///< 客户端首包的源 MAC（直连且邻居表未解析时兜底）
  uint32_t rs_mac_gen;
  uint32_t cli_mac_gen;

  uint64_t created_tick;
  uint64_t expire_tick; ///< 秒级 tick，>= 这个值即过期（存活时间 >= 超时）

  // 统计
  uint64_t pkts_in, bytes_in;   ///< Client -> RS
  uint64_t pkts_out, bytes_out; ///< RS -> Client

  // 时间轮链表（内部使用）
  uint64_t slot_tick;
  uint32_t wheel_prev, wheel_next;
  bool in_pending;
};

class SessionTable {
public:
  static constexpr uint32_t kNil = UINT32_MAX;
  static constexpr uint32_t kWheelSlots = 4096; ///< 2 的幂；超时可以超过它
  static constexpr size_t kExpireBudget = 4096; ///< 每次推进最多处理的会话数

  SessionTable() = default;
  ~SessionTable();
  SessionTable(const SessionTable &) = delete;
  SessionTable &operator=(const SessionTable &) = delete;

  /**
   * @param name rte_hash 名字（全局唯一）
   * @param capacity 最大会话数
   * @param now_tick 当前秒级 tick
   */
  bool init(const char *name, uint32_t capacity, int socket_id,
            uint64_t now_tick);
  void destroy();

  /// 按任一方向的五元组查找
  Session *lookup(const FiveTuple &key) const;

  /// key 是否已被占用（SNAT 端口分配用）
  bool key_in_use(const FiveTuple &key) const { return lookup(key) != nullptr; }

  /**
   * @brief 创建会话并插入 hash
   * @param server 非空时（FULLNAT）同时插入回程 key
   * @return nullptr 表示会话表满或 key 冲突
   */
  Session *create(const FiveTuple &client, const FiveTuple *server,
                  uint64_t now_tick, uint32_t timeout);

  /// 删除会话（两个 key 都删除）
  void remove(Session *s);

  /// 刷新超时；超时变短时立即移动到对应的时间轮槽位
  void touch(Session *s, uint64_t now_tick, uint32_t timeout);

  /**
   * @brief 推进时间轮，删除已过期的会话
   * @return 本次删除的会话数
   */
  size_t expire(uint64_t now_tick);

  uint32_t active() const { return capacity_ - free_top_; }
  uint32_t capacity() const { return capacity_; }
  Session *at(uint32_t idx) { return &sessions_[idx]; }
  uint32_t index_of(const Session *s) const {
    return static_cast<uint32_t>(s - sessions_);
  }

  /// 遍历所有活跃会话（控制面展示用，只能在所属 worker 上调用）
  template <typename F> void for_each(F &&fn) {
    for (uint32_t i = 0; i < capacity_; ++i)
      if (sessions_[i].wheel_prev != kFree)
        fn(sessions_[i]);
  }

private:
  static constexpr uint32_t kFree = UINT32_MAX - 1; ///< wheel_prev 标记空闲

  void wheel_link(uint32_t idx, uint64_t tick);
  void wheel_unlink(uint32_t idx);
  void pending_push(uint32_t idx);
  uint32_t pending_pop();

  struct rte_hash *hash_ = nullptr;
  Session *sessions_ = nullptr;
  uint32_t *free_stack_ = nullptr;
  uint32_t free_top_ = 0; ///< 空闲下标数量
  uint32_t capacity_ = 0;

  uint32_t *slots_ = nullptr; ///< 每个槽位的链表头
  uint32_t pending_ = kNil;   ///< 正在处理的槽位（处理到一半时剩余的部分）
  uint64_t cur_tick_ = 0;     ///< 下一个要处理的 tick
};

} // namespace l4lb

#endif // L4LB_LB_SESSION_H
