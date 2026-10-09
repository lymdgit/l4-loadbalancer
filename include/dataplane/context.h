/**
 * @file context.h
 * @brief 数据面上下文：每个 worker 的私有状态 + 全局共享对象
 *
 * 线程（docs/pipeline改造.md）：
 *   worker    每个 lcore 一个（pipeline 模式下 main lcore 除外），只做转发
 *   receiver  pipeline 模式下的 main lcore：收包，按 owner 放进 worker 的 rx_ring
 *   master    普通线程：ARP / 邻居表 / 健康检查 / 周期统计，独占最后一个 TX 队列
 *
 * WorkerCtx 只由所属 worker 访问（统计计数器除外，允许其他线程只读）；
 * Dataplane 中的对象在启动时创建，运行期间只读或自带并发控制：
 *   steering / route / cfg    只读
 *   neigh                     master 写，worker 无锁读
 *   snapshots                 控制线程写，worker / master 通过 RCU 读
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
constexpr unsigned RX_RING_ELEMS = 4096;   ///< pipeline：receiver -> worker
/// master 线程的 RCU thread id（lcore id 都小于 RTE_MAX_LCORE）
constexpr unsigned kMasterRcuId = RTE_MAX_LCORE;
constexpr unsigned kRcuMaxThreads = RTE_MAX_LCORE + 1;
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

/// 一个 worker（一个 lcore + 独占的 TX 队列；rtc 模式下还独占同号 RX 队列）
struct WorkerCtx {
  uint16_t idx = 0;      ///< worker 下标，等于 TX 队列号
  unsigned lcore_id = 0;
  SessionTable sessions;
  struct rte_ring *redirect_ring = nullptr; ///< 其他 worker 转交来的包
  struct rte_ring *rx_ring = nullptr;       ///< pipeline：receiver 分发来的包
  TxBuffer tx;
  WorkerStats stats;
  RsCounters *rs_stats = nullptr;    ///< [kMaxRsCounters]，按 RS id 索引
  std::vector<uint32_t> lip_cursor; ///< 每个 LIP 的 SNAT 端口游标
  uint32_t lip_rr = 0;              ///< LIP 轮询
  uint64_t now_tick = 1;            ///< 秒级时间（启动时为 1）
  uint64_t now_ms = 0;
};



/// 网卡统计（rte_eth_stats_get），由 master 线程定期刷新，其他线程只读
struct NicStats {
  std::atomic<uint64_t> ipackets{0}, opackets{0};
  std::atomic<uint64_t> ibytes{0}, obytes{0};
  std::atomic<uint64_t> imissed{0};   ///< 网卡 RX 队列满丢弃（收包跟不上）
  std::atomic<uint64_t> ierrors{0}, oerrors{0};
  std::atomic<uint64_t> rx_nombuf{0}; ///< mbuf 不够丢弃
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

  bool pipeline = false;      ///< DataplaneMode::PIPELINE
  uint16_t num_rx_queues = 0; ///< rtc：worker i 轮询 q % num_workers == i 的队列
  /// master 线程的上下文：idx = 它独占的 TX 队列（= num_workers），不建会话表
  WorkerCtx *master = nullptr;
  WorkerStats *receiver_stats = nullptr; ///< pipeline 模式下有效
  NicStats nic;

  uint64_t tsc_hz = 0;
  uint64_t start_tsc = 0;
};

extern Dataplane g_dp;

/// 运行标志：信号处理函数只写这个变量（lock-free atomic，async-signal-safe）
extern std::atomic<bool> g_running;

/// 汇总所有 worker 的计数器
StatsTotal stats_total();

/// 一个 RS 在所有 worker 上的计数之和
struct RsTotal {
  uint64_t conns = 0, pkts_in = 0, bytes_in = 0, pkts_out = 0, bytes_out = 0;
};
RsTotal rs_total(uint32_t rs_id);

/// 当前活跃会话数（所有 worker）
uint64_t sessions_active(const StatsTotal &t);

/// 向 master 发送事件（任意 worker 调用，ring 满时丢弃）
void post_master_event(const MasterEvent &ev);

} // namespace l4lb

#endif // L4LB_DATAPLANE_CONTEXT_H
