/**
 * @file context.h
 * @brief 数据面上下文：每个 worker 的私有状态 + 全局共享对象
 *
 * WorkerCtx 只由所属 worker 访问（统计计数器除外，允许其他线程只读）；
 * Dataplane 中的对象在启动时创建，运行期间只读或自带并发控制：
 *   steering / route / cfg    只读
 *   neigh                     master 写，worker 无锁读
 *   snapshots                 控制线程写，worker 通过 RCU 读
 *   master_ring / health_ring 多生产者 / 单消费者 ring
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_DATAPLANE_CONTEXT_H
#define L4LB_DATAPLANE_CONTEXT_H

#include "common/config.h"
#include "common/stats.h"
#include "ctrl/snapshot.h"
#include "dataplane/steering.h"
#include "lb/session.h"
#include "net/neigh.h"
#include "net/route.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

#include <rte_config.h>

struct rte_mbuf;
struct rte_mempool;
struct rte_ring;
struct rte_rcu_qsbr;

namespace l4lb {

// ============================================================================
// DPDK 参数
// ============================================================================
constexpr uint16_t RX_RING_SIZE = 2048;
constexpr uint16_t TX_RING_SIZE = 4096;
constexpr unsigned MBUF_CACHE_SIZE = 512;
constexpr uint16_t BURST_SIZE = 64;
constexpr unsigned REDIRECT_RING_SIZE = 4096;
constexpr unsigned MASTER_RING_SIZE = 8192;

/// TX 批量缓冲
struct TxBuffer {
  struct rte_mbuf *pkts[BURST_SIZE];
  uint16_t count = 0;
};

/// worker -> master 的事件
enum MasterEventType : uint8_t {
  EV_ARP_FOR_US = 1, ///< 收到请求本机地址的 ARP（学习发送方）
  EV_ARP_REPLY,      ///< 收到发给本机的 ARP 应答
  EV_NEIGH_HINT,     ///< 新会话的直连客户端 MAC
  EV_NEIGH_MISS,     ///< 下一跳未解析，需要发 ARP 请求
  EV_HC_RESP,        ///< 健康检查回包
};

struct MasterEvent {
  uint8_t type;
  uint8_t tcp_flags;
  uint16_t port;     ///< EV_HC_RESP：本机探测端口（主机字节序）
  IPv4Addr ip;
  uint32_t seq, ack; ///< EV_HC_RESP：回包的 seq / ack（主机字节序）
  MacAddr mac;
  uint16_t aux;      ///< EV_HC_RESP：RS 端口（主机字节序）
};
static_assert(sizeof(MasterEvent) % 4 == 0, "ring element size");

/// 一个 worker（一个 lcore + 一组独占的 RX/TX 队列）
struct WorkerCtx {
  uint16_t idx = 0;      ///< worker 下标，等于队列号；0 是 master
  unsigned lcore_id = 0;
  SessionTable sessions;
  struct rte_ring *redirect_ring = nullptr; ///< 其他 worker 转交来的包
  TxBuffer tx;
  WorkerStats stats;
  std::vector<uint32_t> lip_cursor; ///< 每个 LIP 的 SNAT 端口游标
  uint32_t lip_rr = 0;              ///< LIP 轮询
  uint64_t now_tick = 1;            ///< 秒级时间（启动时为 1）
  uint64_t now_ms = 0;

  bool is_master() const { return idx == 0; }
};

/// 全局数据面对象
struct Dataplane {
  uint16_t port_id = 0;
  int socket_id = 0;
  struct rte_mempool *pool = nullptr;
  MacAddr local_mac{};
  uint64_t tx_offloads = 0;
  uint16_t mtu = 1500;

  LbConfig cfg;
  Steering steering;
  Route route;
  NeighTable neigh;
  SnapshotManager snapshots;
  struct rte_rcu_qsbr *qsbr = nullptr;
  struct rte_ring *master_ring = nullptr; ///< worker -> master（MP/SC）
  struct rte_ring *health_ring = nullptr; ///< master -> 控制线程（SP/SC）

  uint16_t num_workers = 0;
  std::array<WorkerCtx *, RTE_MAX_LCORE> workers{};

  uint64_t tsc_hz = 0;
  uint64_t start_tsc = 0;
};

extern Dataplane g_dp;

/// 运行标志：信号处理函数只写这个变量（lock-free atomic，async-signal-safe）
extern std::atomic<bool> g_running;

/// 汇总所有 worker 的计数器
StatsTotal stats_total();

/// 当前活跃会话数（所有 worker）
uint64_t sessions_active(const StatsTotal &t);

/// 向 master 发送事件（任意 worker 调用，ring 满时丢弃）
void post_master_event(const MasterEvent &ev);

} // namespace l4lb

#endif // L4LB_DATAPLANE_CONTEXT_H
