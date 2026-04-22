/**
 * @file session.h
 * @brief Session manager - per-lcore tables with rte_hash reverse table.
 *
 * 无锁化优化（相比原版本）：
 *
 * 1. 调试计数器全部 atomic → per-lcore 本地累加
 *    原来 7 个 std::atomic<uint64_t>，热路径每次 fetch_add 都争抢同一
 *    cache line（即使用 relaxed 语序，CAS 仍需总线锁定）。
 *    改为 per-lcore LocalCounters（alignas 128B），每核独立累加，
 *    get_debug_stats() 仅在控制面聚合。
 *
 * 2. active_sessions_ / total_sessions_ atomic → per-lcore
 *    同样的 false sharing 问题，改为 per-lcore 累加后聚合。
 *
 * 3. next_nat_port_ atomic → per-lcore 端口分区，彻底消除 TOCTOU 竞争
 *    原来所有 lcore 竞争同一个原子计数器，并且存在：
 *      rte_hash_lookup → ... → rte_hash_add_key 之间的 TOCTOU 竞争
 *    改为：每个 lcore 分配独立的端口范围，端口计数器只被单个 lcore 访问，
 *    既消除 TOCTOU，也消除端口计数的 atomic 开销。
 *
 * 4. 保留：正向会话表 per-lcore（原已实现，无跨核访问）
 * 5. 保留：反向会话表 rte_hash + RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF
 *    + RCU QSBR 延迟回收：del_key 后 slot 进入 defer queue，
 *      等所有 lcore 报告静默后自动释放，避免 slot 泄漏和 Use-After-Free。
 */

#ifndef L4LB_LB_SESSION_H
#define L4LB_LB_SESSION_H

#include <array>
#include <unordered_map>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_hash.h>
#include <rte_jhash.h>
#include <rte_lcore.h>
#include <rte_rcu_qsbr.h>
#include <rte_malloc.h>

#include "common/logger.h"
#include "common/types.h"

namespace l4lb {

struct SessionDebugStats {
    uint64_t lookup_hit      = 0;
    uint64_t lookup_miss     = 0;
    uint64_t reverse_hit     = 0;
    uint64_t reverse_miss    = 0;
    uint64_t create          = 0;
    uint64_t update_miss     = 0;
    uint64_t cleanup_removed = 0;
};

class SessionManager {
public:
    static SessionManager& instance() {
        static SessionManager mgr;
        return mgr;
    }

    bool init() {
        struct rte_hash_parameters params = {};
        params.name             = "reverse_session_hash";
        params.entries          = kReverseCapacity;
        params.key_len          = sizeof(FiveTuple);
        params.hash_func        = rte_jhash;
        params.hash_func_init_val = 0;
        params.socket_id        = rte_socket_id();
        // Lock-Free 读路径，写路径内部 CAS
        params.extra_flag       = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;
        // 创建 rte_hash 
        reverse_hash_ = rte_hash_create(&params);
        if (!reverse_hash_) {
            LOG_ERROR("Failed to create rte_hash for reverse session table: %s",
                      rte_strerror(rte_errno));
            return false;
        }
        // 分配一块内存 用来存储结构体数组
        reverse_data_ = static_cast<ReverseEntry*>(
            rte_zmalloc("reverse_data",
                        sizeof(ReverseEntry) * kReverseCapacity,
                        RTE_CACHE_LINE_SIZE));
        if (!reverse_data_) {
            LOG_ERROR("Failed to allocate reverse data array");
            rte_hash_free(reverse_hash_);
            reverse_hash_ = nullptr;
            return false;
        }

        // ---- RCU QSBR 初始化 ----
        // RW_CONCURRENCY_LF 下 del_key 不释放 slot，需要 RCU 延迟回收
        size_t qsbr_sz = rte_rcu_qsbr_get_memsize(RTE_MAX_LCORE);
        qsbr_ = static_cast<struct rte_rcu_qsbr*>(
            rte_zmalloc("session_qsbr", qsbr_sz, RTE_CACHE_LINE_SIZE));
        if (!qsbr_) {
            LOG_ERROR("Failed to allocate RCU QSBR variable");
            rte_free(reverse_data_); reverse_data_ = nullptr;
            rte_hash_free(reverse_hash_); reverse_hash_ = nullptr;
            return false;
        }
        // 初始化QSBR变量，支持最多4核心
        if (rte_rcu_qsbr_init(qsbr_, RTE_MAX_LCORE) != 0) {
            LOG_ERROR("Failed to init RCU QSBR");
            rte_free(qsbr_); qsbr_ = nullptr;
            rte_free(reverse_data_); reverse_data_ = nullptr;
            rte_hash_free(reverse_hash_); reverse_hash_ = nullptr;
            return false;
        }
        // 配置RCU相关设置
        // 将 QSBR 关联到 rte_hash，启用 DQ 模式（延迟队列自动回收 slot）
        struct rte_hash_rcu_config rcu_cfg = {};
        rcu_cfg.v = qsbr_;
        rcu_cfg.mode = RTE_HASH_QSBR_MODE_DQ;
        rcu_cfg.dq_size = 1024;
        rcu_cfg.trigger_reclaim_limit = 0;
        rcu_cfg.max_reclaim_size = 0;
        // 关联RCU到rte_hash
        int rcu_ret = rte_hash_rcu_qsbr_add(reverse_hash_, &rcu_cfg);
        if (rcu_ret != 0) {
            LOG_ERROR("Failed to add RCU QSBR to hash: %s", rte_strerror(-rcu_ret));
            rte_free(qsbr_); qsbr_ = nullptr;
            rte_free(reverse_data_); reverse_data_ = nullptr;
            rte_hash_free(reverse_hash_); reverse_hash_ = nullptr;
            return false;
        }

        LOG_INFO("Reverse session hash: capacity=%u, key_len=%zu, lock-free + RCU QSBR (DQ)",
                 kReverseCapacity, sizeof(FiveTuple));
        return true;
    }

    void cleanup() {
        if (reverse_hash_) { rte_hash_free(reverse_hash_); reverse_hash_ = nullptr; }
        if (reverse_data_) { rte_free(reverse_data_);      reverse_data_ = nullptr; }
        if (qsbr_)         { rte_free(qsbr_);              qsbr_ = nullptr;         }
    }

    void set_timeout(uint32_t seconds) {
        timeout_sec_            = seconds;
        timeout_tsc_            = rte_get_tsc_hz() * seconds;
        touch_tsc_              = timeout_tsc_ / 4;
        cleanup_interval_tsc_   = rte_get_tsc_hz(); // 1s
    }

    // =========================================================================
    // RCU QSBR 线程管理（worker 入口/出口调用）
    // =========================================================================

    /** 注册当前 lcore 为 RCU QSBR 读者线程（worker 启动时调用） */
    void register_lcore() {
        unsigned lcore = rte_lcore_id();
        rte_rcu_qsbr_thread_register(qsbr_, lcore);
        rte_rcu_qsbr_thread_online(qsbr_, lcore);
        LOG_INFO("RCU QSBR: lcore %u registered & online", lcore);
    }

    /** 注销当前 lcore 的 RCU QSBR 读者身份（worker 退出时调用） */
    void unregister_lcore() {
        unsigned lcore = rte_lcore_id();
        rte_rcu_qsbr_thread_offline(qsbr_, lcore);
        rte_rcu_qsbr_thread_unregister(qsbr_, lcore);
        LOG_INFO("RCU QSBR: lcore %u offline & unregistered", lcore);
    }

    /** 报告当前 lcore 经过一个静默期（worker 主循环周期性调用） */
    void quiescent_state() {
        rte_rcu_qsbr_quiescent(qsbr_, rte_lcore_id());
    }

    // =========================================================================
    // 热路径：正向查找（per-lcore，无锁）
    // =========================================================================

    bool lookup(const FiveTuple& tuple, Session& session) {
        auto& tbl = local_table();
        auto it = tbl.sessions.find(tuple);
        if (it == tbl.sessions.end()) {
            local_counters().lookup_miss++;
            return false;
        }
        local_counters().lookup_hit++;
        uint64_t now_tsc = rte_get_tsc_cycles();
        if (now_tsc - it->second.last_active > touch_tsc_) {
            it->second.last_active = now_tsc;
        }
        session = it->second;
        return true;
    }

    // =========================================================================
    // 热路径：反向查找（rte_hash Lock-Free 读路径）
    // =========================================================================

    bool lookup_reverse(const FiveTuple& reverse_tuple, Session& session) {
        int32_t idx = rte_hash_lookup(reverse_hash_,
                                      static_cast<const void*>(&reverse_tuple));
        if (idx < 0) {
            local_counters().reverse_miss++;
            return false;
        }
        session.client_tuple    = reverse_data_[idx].client_tuple;
        session.real_server_id  = reverse_data_[idx].real_server_id;
        session.server_tuple    = reverse_tuple;
        local_counters().reverse_hit++;
        return true;
    }

    // =========================================================================
    // 热路径：创建会话（写 per-lcore 正向表 + 写反向 rte_hash）
    // =========================================================================

    Port create(const FiveTuple& client_tuple, uint32_t server_id,
                IPv4Addr rs_ip = 0, Port rs_port = 0) {
        uint64_t tsc = rte_get_tsc_cycles();
        Session session;
        session.client_tuple    = client_tuple;
        session.real_server_id  = server_id;
        session.nat_src_port    = 0;
        session.create_time     = tsc;
        session.last_active     = tsc;
        session.packets         = 0;
        session.bytes           = 0;

        auto& tbl = local_table();

        if (rs_ip != 0) {
            session.nat_src_port = allocate_nat_src_port(
                rs_ip, rs_port, client_tuple, server_id);
            session.server_tuple = FiveTuple(
                rs_ip, client_tuple.dst_ip, rs_port,
                session.nat_src_port, client_tuple.protocol);
        }

        tbl.sessions[client_tuple] = session;

        auto& cnt = local_counters();
        cnt.total_sessions++;
        cnt.active_sessions++;
        cnt.create++;

        return session.nat_src_port;
    }

    void update_stats(const FiveTuple& tuple, uint64_t bytes) {
        auto& tbl = local_table();
        auto it = tbl.sessions.find(tuple);
        if (it != tbl.sessions.end()) {
            uint64_t now_tsc = rte_get_tsc_cycles();
            if (now_tsc - it->second.last_active > touch_tsc_) {
                it->second.last_active = now_tsc;
            }
            ++it->second.packets;
            it->second.bytes += bytes;
        } else {
            local_counters().update_miss++;
        }
    }

    size_t cleanup_local(uint64_t now_tsc) {
        auto& tbl = local_table();
        if (now_tsc - tbl.last_cleanup_tsc < cleanup_interval_tsc_) return 0;

        size_t removed = 0;
        for (auto it = tbl.sessions.begin(); it != tbl.sessions.end();) {
            if (it->second.is_expired(now_tsc, timeout_tsc_)) {
                if (it->second.server_tuple.src_ip != 0) {
                    rte_hash_del_key(reverse_hash_, &it->second.server_tuple);
                }
                it = tbl.sessions.erase(it);
                ++removed;
            } else {
                ++it;
            }
        }

        if (removed > 0) {
            auto& cnt = local_counters();
            cnt.active_sessions   -= removed;
            cnt.cleanup_removed   += removed;
        }
        tbl.last_cleanup_tsc = now_tsc;
        return removed;
    }

    // =========================================================================
    // 控制面：聚合所有 lcore 的统计
    // =========================================================================

    Statistics get_stats() const {
        Statistics s{};
        for (unsigned i = 0; i < RTE_MAX_LCORE; ++i) {
            s.active_sessions += counters_[i].active_sessions;
            s.total_sessions  += counters_[i].total_sessions;
        }
        return s;
    }

    SessionDebugStats get_debug_stats() const {
        SessionDebugStats total{};
        for (unsigned i = 0; i < RTE_MAX_LCORE; ++i) {
            total.lookup_hit      += counters_[i].lookup_hit;
            total.lookup_miss     += counters_[i].lookup_miss;
            total.reverse_hit     += counters_[i].reverse_hit;
            total.reverse_miss    += counters_[i].reverse_miss;
            total.create          += counters_[i].create;
            total.update_miss     += counters_[i].update_miss;
            total.cleanup_removed += counters_[i].cleanup_removed;
        }
        return total;
    }

private:
    SessionManager() : timeout_sec_(300), timeout_tsc_(0) {}

    // =========================================================================
    // 反向表条目
    // =========================================================================
    struct ReverseEntry {
        FiveTuple client_tuple;
        uint32_t  real_server_id;
    };

    // =========================================================================
    // Per-lcore 正向会话表
    // =========================================================================
    struct Table {
        std::unordered_map<FiveTuple, Session, FiveTupleHash> sessions;
        uint64_t last_cleanup_tsc = 0;
    };

    Table& local_table() {
        unsigned lcore = rte_lcore_id();
        if (lcore >= RTE_MAX_LCORE) lcore = 0;
        return tables_[lcore];
    }

    // =========================================================================
    // Per-lcore 计数器（替代 std::atomic，消除 cache line 竞争）
    //
    // sizeof = 9 × uint64_t = 72 字节，填充到 128（2 × cache_line）
    // 每个 lcore 的计数器占据独立的 cache line，彻底消除 false sharing
    // =========================================================================
    struct alignas(RTE_CACHE_LINE_SIZE) LocalCounters {
        uint64_t lookup_hit      = 0;
        uint64_t lookup_miss     = 0;
        uint64_t reverse_hit     = 0;
        uint64_t reverse_miss    = 0;
        uint64_t create          = 0;
        uint64_t update_miss     = 0;
        uint64_t cleanup_removed = 0;
        uint64_t active_sessions = 0;
        uint64_t total_sessions  = 0;
        uint8_t  _pad[128 - 9 * sizeof(uint64_t)]; // → 128 字节
    };
    static_assert(sizeof(LocalCounters) == 128, "LocalCounters must be 128 bytes");

    LocalCounters& local_counters() {
        unsigned lcore = rte_lcore_id();
        if (lcore >= RTE_MAX_LCORE) lcore = 0;
        return counters_[lcore];
    }

    // =========================================================================
    // NAT 端口按 lcore 分区分配（消除 TOCTOU + atomic 竞争）
    //
    // 原来所有 lcore 共享 next_nat_port_ atomic，且存在：
    //   rte_hash_lookup → (其他 lcore 抢占) → rte_hash_add_key 的 TOCTOU 竞争
    //
    // 新方案：将端口范围 [kPortMin, kPortMax] 均分给每个 lcore：
    //   lcore 0: [10000, 10000 + range - 1]
    //   lcore 1: [10000 + range, ...]
    //   ...
    // 每个 lcore 只在自己的子范围内轮转分配，无需原子操作，无 TOCTOU。
    // =========================================================================
    Port allocate_nat_src_port(IPv4Addr rs_ip, Port rs_port,
                               const FiveTuple& client_tuple,
                               uint32_t server_id) {
        if (!rs_ip || !rs_port) return 0;

        static constexpr uint16_t kPortMin   = 10000;
        static constexpr uint16_t kPortMax   = 60000;
        static constexpr uint32_t kTotalRange = kPortMax - kPortMin + 1;

        unsigned lcore      = rte_lcore_id();
        if (lcore >= RTE_MAX_LCORE) lcore = 0;
        uint32_t num_lcores = rte_lcore_count();
        if (num_lcores == 0) num_lcores = 1;

        // 每个 lcore 分配的端口子范围大小
        uint32_t range    = kTotalRange / num_lcores;
        if (range == 0)   range = 1;
        uint16_t my_min   = static_cast<uint16_t>(kPortMin + lcore * range);
        uint16_t my_max   = static_cast<uint16_t>(
            (lcore + 1 < num_lcores) ? (my_min + range - 1) : kPortMax);
        uint32_t my_range = my_max - my_min + 1;

        // per-lcore 端口计数器（只被单个 lcore 访问，无需原子）
        uint32_t& port_ctr = lcore_port_ctr_[lcore];

        for (uint32_t i = 0; i < my_range; ++i) {
            uint16_t host_port = static_cast<uint16_t>(
                my_min + (port_ctr % my_range));
            port_ctr++;
            Port nat_port = rte_cpu_to_be_16(host_port);

            FiveTuple reverse_tuple(rs_ip, client_tuple.dst_ip,
                                    rs_port, nat_port, client_tuple.protocol);

            // 先查（Lock-Free 读路径）
            int32_t idx = rte_hash_lookup(reverse_hash_, &reverse_tuple);
            if (idx >= 0) continue; // 已被占用（极少发生，因为范围已分区）

            // 写入（同一端口的写操作在同一 lcore 上，消除了 TOCTOU）
            idx = rte_hash_add_key(reverse_hash_, &reverse_tuple);
            if (idx < 0) {
                LOG_WARN("rte_hash_add_key failed: %s", rte_strerror(-idx));
                continue;
            }

            reverse_data_[idx].client_tuple    = client_tuple;
            reverse_data_[idx].real_server_id  = server_id;
            return nat_port;
        }

        LOG_WARN("NAT port allocation exhausted for lcore %u, fallback", lcore);
        return client_tuple.src_port;
    }

    // ─── 配置 ────────────────────────────────────────────────────────────────
    uint32_t timeout_sec_;
    uint64_t timeout_tsc_;
    uint64_t touch_tsc_{0};
    uint64_t cleanup_interval_tsc_{0};

    // ─── Per-lcore 正向表 ────────────────────────────────────────────────────
    std::array<Table, RTE_MAX_LCORE> tables_;

    // ─── Per-lcore 计数器（无锁）────────────────────────────────────────────
    LocalCounters counters_[RTE_MAX_LCORE]{};

    // ─── Per-lcore NAT 端口计数器（无锁，替代 next_nat_port_ atomic）────────
    uint32_t lcore_port_ctr_[RTE_MAX_LCORE]{};

    // ─── 反向表（rte_hash Lock-Free 读路径 + RCU QSBR 延迟回收）─────────────
    static const uint32_t kReverseCapacity = 131072;
    struct rte_hash*      reverse_hash_ = nullptr;
    ReverseEntry*         reverse_data_ = nullptr;
    struct rte_rcu_qsbr*  qsbr_         = nullptr;

    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;
};

} // namespace l4lb

#endif // L4LB_LB_SESSION_H
