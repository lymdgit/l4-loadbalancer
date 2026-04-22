# 反向会话表 RCU QSBR 集成方案

> 本文档记录 `reverse_session_hash`（rte_hash）集成 DPDK RCU QSBR 机制的背景、问题分析、修改方案和验证方法。

---

## 一、问题背景

### 1.1 原始架构

`SessionManager` 使用 `rte_hash` 作为反向会话表（`reverse_hash_`），配合 `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF` 实现 Lock-Free 读写并发：

| 操作 | 函数 | 并发特性 |
|------|------|---------|
| 查找 | `rte_hash_lookup()` | Lock-Free 无锁读 |
| 插入 | `rte_hash_add_key()` | 内部 CAS 多写者安全 |
| 删除 | `rte_hash_del_key()` | 内部 CAS 标记删除 |

### 1.2 发现的关键问题

根据 DPDK 官方文档：

> **`RTE_HASH_EXTRA_FLAGS_NO_FREE_ON_DEL` is enabled by default when `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF` is enabled.**
>
> If `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF` is enabled and internal RCU is NOT enabled, the key index returned by `rte_hash_add_key_xxx` APIs will **not** be freed by `rte_hash_del_key`. `rte_hash_free_key_with_position` API must be called additionally to free the index associated with the key.

这意味着：**`rte_hash_del_key()` 只把 key 标记为「已删除」，但不释放内部 key slot（index）**。

不处理的后果：

1. **Key slot 泄漏**：每次 `cleanup_local()` 删除过期会话后，内部 slot 永远不被回收
2. **哈希表空间耗尽**：`rte_hash_add_key()` 最终返回 `-ENOSPC`，新连接无法建立
3. **Use-After-Free 风险**：如果手动调 `rte_hash_free_key_with_position()` 释放 slot，其他 lcore 可能正在通过旧 index 读取 `reverse_data_[idx]`

### 1.3 并发竞态场景

```
lcore 0 (cleanup_local):              lcore 1 (lookup_reverse):
─────────────────────                 ─────────────────────
rte_hash_del_key(key_A)               idx = rte_hash_lookup(key_A) → idx=42
  // key_A 标记删除                     session = reverse_data_[42]
  // 但 slot 42 不释放                  // 如果 slot 42 被立即回收并重用给 key_B,
                                        // 这里就读到了 key_B 的数据 → 脏读！
```

**RCU QSBR 延迟回收是唯一正确的解决方案**：确保所有读者都经过静默期（quiescent state）后，才真正释放 slot。

---

## 二、RCU QSBR 原理简述

### 2.1 什么是 QSBR

QSBR (Quiescent State Based Reclamation) 是 RCU 的一种实现方式：

```
  lcore 0                lcore 1               defer queue
  ───────                ───────               ───────────
                         lookup(key_A) → idx=42
                         读 reverse_data_[42]
                         
  del_key(key_A)                               slot 42 进入 DQ
    // 标记删除
    // slot 42 不释放

                         quiescent()           检查：lcore 1 已静默 ✓
  quiescent()                                  检查：lcore 0 已静默 ✓
                                               → 所有 lcore 静默
                                               → 释放 slot 42 ✓
                                               → slot 42 可安全重用
```

### 2.2 关键概念

| 概念 | 含义 |
|------|------|
| **QSBR 变量** | 跟踪所有已注册线程的静默状态的共享数据结构 |
| **注册/注销** | worker 启动时 `thread_register` + `thread_online`，退出时 `thread_offline` + `thread_unregister` |
| **静默状态报告** | worker 定期调用 `rte_rcu_qsbr_quiescent()`，声明此刻不持有任何旧引用 |
| **Defer Queue (DQ)** | 被删除的 slot 进入 DQ 等待，所有线程都报告静默后才真正释放 |

### 2.3 DQ vs SYNC 模式

| 模式 | `RTE_HASH_QSBR_MODE_DQ` | `RTE_HASH_QSBR_MODE_SYNC` |
|------|--------------------------|---------------------------|
| 回收方式 | 异步，放入延迟队列 | 同步阻塞，`del_key` 时阻塞等待静默 |
| 性能影响 | 写路径不阻塞，高性能 | 写路径可能阻塞，延迟不可控 |
| 适用场景 | **数据面热路径**（推荐） | 控制面低频操作 |

本项目使用 **DQ 模式**。

---

## 三、具体修改

### 3.1 修改文件总览

| 文件 | 修改类型 | 修改内容 |
|------|---------|---------|
| `include/lb/session.h` | MODIFY | RCU QSBR 初始化 + 线程注册/注销 + 静默报告 API |
| `src/main.cpp` | MODIFY | worker 入口注册 + 主循环报告静默 + 退出注销 |

### 3.2 session.h 修改详情

#### 3.2.1 新增头文件

```diff
 #include <rte_hash.h>
 #include <rte_jhash.h>
 #include <rte_lcore.h>
+#include <rte_rcu_qsbr.h>
 #include <rte_malloc.h>
```

#### 3.2.2 新增成员变量

```diff
     // ─── 反向表（rte_hash Lock-Free 读路径 + RCU QSBR 延迟回收）─────────────
     static const uint32_t kReverseCapacity = 131072;
     struct rte_hash*      reverse_hash_ = nullptr;
     ReverseEntry*         reverse_data_ = nullptr;
+    struct rte_rcu_qsbr*  qsbr_         = nullptr;
```

#### 3.2.3 init() 函数扩展

在 `rte_hash_create()` 和 `rte_zmalloc("reverse_data")` 之后，新增 RCU 初始化：

```cpp
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

if (rte_rcu_qsbr_init(qsbr_, RTE_MAX_LCORE) != 0) {
    LOG_ERROR("Failed to init RCU QSBR");
    rte_free(qsbr_); qsbr_ = nullptr;
    rte_free(reverse_data_); reverse_data_ = nullptr;
    rte_hash_free(reverse_hash_); reverse_hash_ = nullptr;
    return false;
}

// 将 QSBR 关联到 rte_hash，启用 DQ 模式（延迟队列自动回收 slot）
struct rte_hash_rcu_config rcu_cfg = {};
rcu_cfg.v = qsbr_;
rcu_cfg.mode = RTE_HASH_QSBR_MODE_DQ;
rcu_cfg.dq_size = 1024;              // defer queue 最多缓存 1024 个待回收 slot
rcu_cfg.trigger_reclaim_limit = 0;   // 每次 del 后尝试回收
rcu_cfg.max_reclaim_size = 0;        // 0 = 无上限，尽量多回收

int rcu_ret = rte_hash_rcu_qsbr_add(reverse_hash_, &rcu_cfg);
if (rcu_ret != 0) {
    LOG_ERROR("Failed to add RCU QSBR to hash: %s", rte_strerror(-rcu_ret));
    rte_free(qsbr_); qsbr_ = nullptr;
    rte_free(reverse_data_); reverse_data_ = nullptr;
    rte_hash_free(reverse_hash_); reverse_hash_ = nullptr;
    return false;
}
```

**配置参数说明**：

| 参数 | 值 | 含义 |
|------|-----|------|
| `mode` | `RTE_HASH_QSBR_MODE_DQ` | 使用延迟队列，非阻塞回收 |
| `dq_size` | `1024` | 最多 1024 个 slot 在队列中等待回收 |
| `trigger_reclaim_limit` | `0` | 每次 `del_key` 都尝试回收（0=总是尝试） |
| `max_reclaim_size` | `0` | 一次回收尝试中无上限（0=全部回收） |

#### 3.2.4 新增 RCU 线程管理 API

```cpp
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
```

#### 3.2.5 cleanup() 扩展

```diff
 void cleanup() {
     if (reverse_hash_) { rte_hash_free(reverse_hash_); reverse_hash_ = nullptr; }
     if (reverse_data_) { rte_free(reverse_data_);      reverse_data_ = nullptr; }
+    if (qsbr_)         { rte_free(qsbr_);              qsbr_ = nullptr;         }
 }
```

#### 3.2.6 cleanup_local() 不需要修改

启用 internal RCU 后，`rte_hash_del_key()` 自动将待删除的 key index 放入 defer queue，等所有 lcore 报告静默后自动释放。原有的 `rte_hash_del_key()` 调用逻辑保持不变。

### 3.3 main.cpp 修改详情

#### 3.3.1 worker 入口注册 RCU

```diff
   LOG_INFO("Worker started on lcore %u, queue %u%s (batch TX enabled)",
            lcore_id, queue_id, is_master ? " (master)" : "");

+  // 注册当前 lcore 为 RCU QSBR 读者线程
+  SessionManager::instance().register_lcore();
+
   while (g_running) {
```

#### 3.3.2 主循环报告静默状态

在每 1024 次循环的降频检查块中添加：

```diff
     if (unlikely((local_loop_count & 1023) == 0)) {
       cur_tsc = rte_get_tsc_cycles();

+      // 报告 RCU 静默状态：此刻没有持有任何旧的 hash slot 引用
+      SessionManager::instance().quiescent_state();
+
       // 定期刷新 TX buffer ...
```

**为什么放在这个位置**：
- `rte_rcu_qsbr_quiescent()` 本身只是一个 **原子 store**（`__atomic_store_n`），开销极低
- 每 1024 次循环调用一次，频率足够高以保证 defer queue 及时回收
- 此时不持有任何正在使用的 hash index，是天然的静默点

#### 3.3.3 worker 退出注销 RCU

```diff
   // 退出前刷新剩余的 TX buffer
   tx_buffer_flush(&tx_buf, g_port_id, queue_id);

+  // 注销当前 lcore 的 RCU 读者身份
+  SessionManager::instance().unregister_lcore();
+
   LOG_INFO("Worker on lcore %u exiting", lcore_id);
```

---

## 四、修改后的并发安全性总结

| 操作 | 并发安全机制 | 说明 |
|------|-------------|------|
| `rte_hash_lookup()` | Lock-Free 读路径 | `RW_CONCURRENCY_LF` 保证 |
| `rte_hash_add_key()` | 内部 CAS + 端口分区消除 TOCTOU | 同一端口只被单一 lcore 写 |
| `rte_hash_del_key()` | 标记删除 → DQ → 静默后自动释放 | RCU QSBR 保证 |
| `reverse_data_[idx]` 读取 | RCU 保证 idx 在读者持有期间不被重用 | 静默报告保证 |

### 完整生命周期时序

```
                 ┌─────────────────────────────────────────────────┐
                 │            SessionManager 生命周期               │
                 ├─────────────────────────────────────────────────┤
                 │                                                 │
  main()         │  init()                                         │
  ──────         │  ├─ rte_hash_create(RW_CONCURRENCY_LF)         │
                 │  ├─ rte_zmalloc(reverse_data_)                  │
                 │  ├─ rte_zmalloc(qsbr_)                          │
                 │  ├─ rte_rcu_qsbr_init(qsbr_)                    │
                 │  └─ rte_hash_rcu_qsbr_add(hash, DQ mode)        │
                 │                                                 │
  worker_loop()  │  register_lcore()                               │
  ───────────    │  ├─ rte_rcu_qsbr_thread_register(lcore_id)      │
                 │  └─ rte_rcu_qsbr_thread_online(lcore_id)        │
                 │                                                 │
  热路径循环     │  每 1024 次:                                      │
                 │  └─ quiescent_state()                            │
                 │     └─ rte_rcu_qsbr_quiescent(lcore_id)         │
                 │                                                 │
                 │  cleanup_local():                                │
                 │  └─ rte_hash_del_key() → slot 进 DQ              │
                 │     → 下次某 lcore 调 del_key/quiescent 时触发回收 │
                 │                                                 │
  退出           │  unregister_lcore()                              │
                 │  ├─ rte_rcu_qsbr_thread_offline(lcore_id)        │
                 │  └─ rte_rcu_qsbr_thread_unregister(lcore_id)    │
                 │                                                 │
  main() 清理   │  cleanup()                                       │
                 │  ├─ rte_hash_free(reverse_hash_)                │
                 │  ├─ rte_free(reverse_data_)                      │
                 │  └─ rte_free(qsbr_)                              │
                 └─────────────────────────────────────────────────┘
```

---

## 五、验证方法

### 5.1 编译验证

```bash
cd build && cmake .. && make -j$(nproc)
```

### 5.2 运行时日志检查

启动后应看到以下日志（顺序）：

```
Reverse session hash: capacity=131072, key_len=13, lock-free + RCU QSBR (DQ)
RCU QSBR: lcore 0 registered & online
RCU QSBR: lcore 1 registered & online
...
```

停止时应看到：

```
RCU QSBR: lcore 1 offline & unregistered
RCU QSBR: lcore 0 offline & unregistered
```

### 5.3 长时间压测验证

关键指标：`rte_hash_count()` 应在 session timeout 周期后趋于稳定，不应持续增长。如果不使用 RCU，该值只增不减（slot 泄漏）。

### 5.4 `dq_size` 调优

如果 defer queue 满（`dq_size = 1024`），`rte_hash_del_key()` 可能返回 `-ENOSPC`。观察日志中是否出现此错误。如果出现，需增大 `dq_size`（如 4096），或降低 cleanup 频率。

---

## 六、参考资料

| 资料 | 链接 |
|------|------|
| DPDK rte_hash API | https://doc.dpdk.org/api/rte__hash_8h.html |
| DPDK RCU QSBR API | https://doc.dpdk.org/api/rte__rcu__qsbr_8h.html |
| `rte_hash_rcu_qsbr_add()` | https://doc.dpdk.org/api/rte__hash_8h.html#a40454ae5731eba4137956dc68acfb62f |
| `RTE_HASH_EXTRA_FLAGS_NO_FREE_ON_DEL` | https://doc.dpdk.org/api/rte__hash_8h.html#a6665149d7d36473ed2ff1175a7c56d02 |
