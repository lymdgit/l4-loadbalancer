/**
 * @file stats.h
 * @brief per-lcore 统计计数器
 *
 * 数据面计数器的写法约定：
 * - 每个 worker 只写自己的那一份，按 cache line 对齐，避免伪共享
 * - 计数器是 std::atomic，但只用 relaxed 的 load + store（不用 fetch_add），
 *   在 x86 上编译成普通的 mov，没有 lock 前缀
 * - 读取方（统计打印、控制面）跨核汇总，relaxed load 保证读到完整的 64 位值
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_COMMON_STATS_H
#define L4LB_COMMON_STATS_H

#include <array>
#include <atomic>
#include <cstdint>

#include <rte_common.h> // RTE_CACHE_LINE_SIZE
#include <rte_cycles.h> // rte_rdtsc

namespace l4lb {

/// 计数器：只能由所属 lcore 调用
inline void stat_add(std::atomic<uint64_t> &c, uint64_t n = 1) {
  c.store(c.load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}

inline uint64_t stat_get(const std::atomic<uint64_t> &c) {
  return c.load(std::memory_order_relaxed);
}

/// worker 计数器 ID；名字表见 stat_name()
enum Stat : uint32_t {
  // 网卡
  ST_RX,
  ST_TX,
  ST_TX_FULL,      ///< TX 队列满丢弃
  // 分类
  ST_ARP,
  ST_ICMP,
  ST_TCP,
  ST_UDP,
  // 转发
  ST_FWD_IN,       ///< Client -> RS
  ST_FWD_OUT,      ///< RS -> Client（FULLNAT）
  ST_ICMP_ERR_FWD, ///< 转发的 ICMP 差错
  // 丢弃原因
  ST_DROP_MALFORMED,
  ST_DROP_FRAGMENT,
  ST_DROP_CKSUM,      ///< 网卡报告校验和错误
  ST_DROP_NOT_LOCAL,  ///< 目的地址不是本机
  ST_DROP_NO_SERVICE, ///< VIP 上没有这个端口/协议
  ST_DROP_NO_SESSION, ///< 非 SYN 且无会话 / 回程无会话
  ST_DROP_NO_RS,      ///< 没有可用 RS
  ST_DROP_RS_DOWN,    ///< 会话的 RS 已下线
  ST_DROP_TABLE_FULL, ///< 会话表满
  ST_DROP_NO_PORT,    ///< SNAT 端口耗尽
  ST_DROP_NO_NEIGH,   ///< 下一跳 MAC 未解析
  ST_DROP_TTL,
  ST_DROP_REDIRECT,   ///< 转交其他 worker 时 ring 满
  ST_DROP_OTHER,
  // 会话
  ST_SESS_NEW,
  ST_SESS_EXPIRED,
  ST_SESS_CLOSED,     ///< RST/RS 下线等主动结束
  // 多核分发
  ST_REDIRECT_OUT,    ///< 本核收到、转交给 owner
  ST_REDIRECT_IN,     ///< 从其他核转交来
  ST_RING_IN,         ///< pipeline：从 receiver 的 ring 取到
  ST_DROP_RX_RING,    ///< pipeline：worker 的 rx_ring 满，receiver 丢弃
  ST_RSS_MISMATCH,    ///< 网卡 RSS hash 与软件计算不一致（自检）
  ST_RSS_NO_HASH,     ///< HW 模式下网卡没有给出 RSS hash（RSS 实际未生效）
  // FULLNAT 选项处理
  ST_TOA_ADDED,
  ST_TOA_NO_ROOM,
  ST_TS_STRIPPED,
  ST_HC_RESP,         ///< 收到健康检查回包
  ST_COUNT
};

const char *stat_name(Stat id);

/// 字节计数 ID（与包计数分开，见 docs/pps方案.md）；名字表见 byte_stat_name()
enum ByteStat : uint32_t {
  BS_RX,      ///< 网卡收包（rtc: worker，pipeline: receiver）
  BS_TX,      ///< 网卡发出（tx_burst 成功的部分）
  BS_FWD_IN,  ///< 转发 Client -> RS
  BS_FWD_OUT, ///< 转发 RS -> Client（FULLNAT）
  BS_COUNT
};

const char *byte_stat_name(ByteStat id);

/// 每个 RS 的计数（每个 worker 一份，按 RS id 索引，只有所属 worker 写）
struct RsCounters {
  std::atomic<uint64_t> conns{0};
  std::atomic<uint64_t> pkts_in{0}, bytes_in{0};   ///< Client -> RS
  std::atomic<uint64_t> pkts_out{0}, bytes_out{0}; ///< RS -> Client（FULLNAT）
};

/// RS 计数数组的容量（RS id 上限，id 不复用）
constexpr uint32_t kMaxRsCounters = 16384;

/// 一个 worker 的全部计数器
struct alignas(RTE_CACHE_LINE_SIZE) WorkerStats {
  std::array<std::atomic<uint64_t>, ST_COUNT> c{};
  std::array<std::atomic<uint64_t>, BS_COUNT> bytes{};
  /// 忙碌时间：有包处理的那些轮次花掉的 TSC 周期（忙轮询下 CPU 永远 100%，
  /// 用它除以经过的 TSC 得到真实负载）
  std::atomic<uint64_t> busy_tsc{0};

  void add(Stat id, uint64_t n = 1) { stat_add(c[id], n); }
  uint64_t get(Stat id) const { return stat_get(c[id]); }
  void add_bytes(ByteStat id, uint64_t n) { stat_add(bytes[id], n); }
  uint64_t get_bytes(ByteStat id) const { return stat_get(bytes[id]); }
  void add_busy(uint64_t cycles) { stat_add(busy_tsc, cycles); }
  uint64_t busy() const { return stat_get(busy_tsc); }
};

/**
 * @brief 忙碌率采样：每个轮询线程一个（栈上），不跨线程
 *
 * 不在每轮循环里读 TSC：VMware 等虚拟化环境可能拦截 rdtsc（实测本机每次约 4 µs，
 * 正常约 10 ns），每轮读两次会让读时钟本身占满 CPU。改为每 kWindow 轮读一次：
 *   - 整个窗口都是空轮询时，用它校准"一次空轮询"的耗时（指数平均）
 *   - 其他窗口：忙碌时间 = 窗口耗时 - 空轮询次数 × 空轮询耗时
 * 还没校准过时（启动后一直满载）退化为按轮次比例估算。
 */
class BusyMeter {
public:
  static constexpr uint32_t kWindow = 256;

  /// 每轮循环调用一次
  inline void loop(bool had_work, WorkerStats &st) {
    idle_ += !had_work;
    if (++n_ < kWindow)
      return;
    uint64_t now = rte_rdtsc();
    if (last_) {
      uint64_t el = now - last_;
      if (idle_ == n_) {
        uint64_t c = el / n_;
        idle_cost_ = idle_cost_ ? (idle_cost_ * 7 + c) / 8 : c;
      } else {
        uint64_t idle_t = idle_cost_ ? uint64_t(idle_) * idle_cost_ : el * idle_ / n_;
        st.add_busy(idle_t < el ? el - idle_t : 0);
      }
    }
    last_ = now;
    n_ = idle_ = 0;
  }

private:
  uint64_t last_ = 0, idle_cost_ = 0;
  uint32_t n_ = 0, idle_ = 0;
};

/// 汇总后的快照
struct StatsTotal {
  std::array<uint64_t, ST_COUNT> c{};
  std::array<uint64_t, BS_COUNT> bytes{};
  uint64_t operator[](Stat id) const { return c[id]; }
  uint64_t operator[](ByteStat id) const { return bytes[id]; }
  uint64_t drops() const;
};

} // namespace l4lb

#endif // L4LB_COMMON_STATS_H
