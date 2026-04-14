# 反向会话表性能优化：从 unordered_map + spinlock 到 rte_hash (Lock-Free)

## 一、为什么反向会话表必须换成 rte_hash？

### 1. 破除数据面热路径上的动态内存分配 (malloc) 噩梦

**当前问题**：你的 `shard.map.emplace(reverse_tuple, ReverseEntry{...})` 使用的是 `std::unordered_map`。
在 C++ 中，每次往 `unordered_map` 里插入新元素，底层都会调用 `new/malloc` 去堆上动态分配一个节点（Node）。在 DPDK 每秒千万包的数据面热路径上做系统调用级别的内存分配是绝对的禁忌，不仅耗时，还会导致严重的锁竞争（glibc malloc 的锁）。

**rte_hash 的优势**：`rte_hash` 初始化时必须绑定预先分配好的内存池（Mempool 或连续大内存）。它的插入（Add）就是利用现有内存，完全实现 **0 动态内存分配**。

```
改造前（每次插入都 malloc）:
┌─────────────────────────────────────────────────────┐
│ shard.map.emplace(key, value)                       │
│   └─→ operator new() → malloc() → 系统调用/锁竞争  │
│         ↑ 每秒被调用数万~数十万次                    │
└─────────────────────────────────────────────────────┘

改造后（预分配，零 malloc）:
┌─────────────────────────────────────────────────────┐
│ idx = rte_hash_add_key(hash, &key)                  │
│ reverse_data_[idx] = value;  // 直接写预分配数组    │
│   └─→ 无任何 malloc，O(1) 时间                      │
└─────────────────────────────────────────────────────┘
```

### 2. 解决读写互斥，实现真正的无锁化查找 (Lock-Free Lookup)

**当前问题**：你在 `lookup_reverse`（反向查找）时使用了 `rte_spinlock_lock(&shard.lock)`，这意味着读操作和读操作之间也是互斥的。如果有多个 Worker 核同时收到属于同一个 Shard 哈希槽的回程报文，它们会相互堵塞等待自旋锁。

**rte_hash 的优势**：`rte_hash` 基于 **Cuckoo Hash（布谷鸟哈希）**，并且支持 RCU（Read-Copy-Update）机制。你可以开启 `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF` 标志，实现多线程无锁读、带锁/无锁写。这样回程报文（只读查询）将完全不需要等待锁，性能呈指数级提升。

```
改造前（spinlock 互斥读）:
Core 0: lock → lookup → unlock
Core 1:        [阻塞等待 Core 0 释放锁] → lock → lookup → unlock
Core 2:                                          [继续阻塞...]

改造后（Lock-Free 并发读）:
Core 0: rte_hash_lookup() → 直接返回    ← 无锁
Core 1: rte_hash_lookup() → 直接返回    ← 无锁
Core 2: rte_hash_lookup() → 直接返回    ← 无锁
（同时执行，零等待）
```

### 3. CPU 缓存命中率 (Cache Locality)

**当前问题**：`std::unordered_map` 内部是通过拉链法实现的，它的节点在内存中是离散分布的。这意味着每次冲突查找都需要做"指针追逐（Pointer Chasing）"，极大降低了 CPU L1/L2 缓存命中率。

**rte_hash 的优势**：数据结构紧凑，不仅对 CPU Cache 极其友好，且底层大量使用了 **SIMD（SSE/AVX/NEON）** 指令集来加速 Key 的批量哈希和对比比对。

```
unordered_map 的内存布局（链式、离散）:
Bucket[0] → Node@0x7fff1234 → Node@0x7fff8888 → nullptr
Bucket[1] → nullptr
Bucket[2] → Node@0x7fff5678 → nullptr
              ↑ 每跳一次指针 = 一次潜在的 Cache Miss

rte_hash 的内存布局（连续、紧凑）:
┌────────────────────────────────────────────┐
│ Slot[0] │ Slot[1] │ Slot[2] │ Slot[3] │...│  ← 连续内存，SIMD 批量比对
└────────────────────────────────────────────┘
```

---

## 二、rte_hash 核心原理

### 2.1 Cuckoo Hashing（布谷鸟哈希）

`rte_hash` 使用的核心算法是 **Cuckoo Hashing**：

- 每个 key 有**两个候选位置**（由两个不同的 hash 函数计算）
- 插入时，若位置 1 已满，将已有 key "踢"到其备选位置，腾出空间
- 查找时，只需检查**最多 2 个位置**，时间复杂度 **O(1)**

```
Hash1(key) ──→ Bucket A  ─┐
                           ├─ 只查这 2 个位置，O(1)
Hash2(key) ──→ Bucket B  ─┘
```

### 2.2 RCU (Read-Copy-Update) 无锁读机制

当开启 `RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF` 后：

```
                    ┌─────────────────────────────────┐
                    │        rte_hash 内部实现         │
                    ├─────────────────────────────────┤
   读线程(Worker)   │                                 │
   rte_hash_lookup  │ → 无锁读取 bucket + signature  │
                    │ → SIMD 批量比对 key             │
                    │ → 返回 index 或 -ENOENT        │
                    ├─────────────────────────────────┤
   写线程(Writer)   │                                 │
   rte_hash_add_key │ → CAS 原子写入新 entry          │
                    │ → 旧 entry 标记为待回收         │
   rte_hash_del_key │ → CAS 原子标记删除              │
                    │ → 延迟回收（RCU grace period）  │
                    └─────────────────────────────────┘
```

**关键特性**：
- **读路径完全无锁**：使用 `__atomic_load` 原子加载 signature，无需任何 mutex/spinlock
- **写路径使用 CAS**：多个写者之间通过 Compare-And-Swap 保证一致性
- **延迟回收**：删除的 key 不会立即释放 slot，等所有读者退出后才真正回收（避免 use-after-free）

### 2.3 SIMD 加速

`rte_hash` 在 key 匹配阶段使用 SIMD 指令：

```c
// 伪代码：一次比对一个 bucket 内的多个 signature（16-bit）
__m128i bucket_sigs = _mm_load_si128(bucket->signatures);  // 一次加载 8 个 sig
__m128i target_sig  = _mm_set1_epi16(hash_signature);       // 广播目标 sig
__m128i cmp_result  = _mm_cmpeq_epi16(bucket_sigs, target_sig); // 8 路并行比较
int mask = _mm_movemask_epi8(cmp_result);                    // 得到匹配位掩码
// mask 非零 → 找到候选 slot，再做 full key 比较
```

**一条 SSE 指令同时比对 8 个 key 的签名**，比逐个遍历链表快一个数量级。

---

## 三、改造详情

### 3.1 涉及文件

| 文件 | 改动 |
|------|------|
| `include/common/types.h` | `FiveTuple` 添加 `__attribute__((packed))` + `memset` 构造 |
| `include/lb/session.h` | 核心改造：rte_hash 替代 1024 个 ReverseShard |
| `src/main.cpp` | 新增 `init()` / `cleanup()` 调用 |

### 3.2 types.h — FiveTuple 对齐修复

**问题**：`rte_hash` 使用 `memcmp` 比较 key。原 `FiveTuple` 布局：

```
src_ip(4) + dst_ip(4) + src_port(2) + dst_port(2) + protocol(1) = 13 bytes
编译器自动填充 3 bytes padding → sizeof = 16
```

这 3 个 padding 字节**未初始化**，包含随机垃圾值。两个逻辑上相同的 `FiveTuple`，其 padding 字节可能不同 → `memcmp` 返回不等 → **hash lookup 失败**。

**修复**：

```cpp
// 1. packed 消除 padding → sizeof = 13，精确匹配
struct __attribute__((packed)) FiveTuple {
    IPv4Addr src_ip;   // 4
    IPv4Addr dst_ip;   // 4
    Port     src_port; // 2
    Port     dst_port; // 2
    uint8_t  protocol; // 1
    // 总计 13 bytes，无 padding

    // 2. memset 保证所有字节为 0
    FiveTuple() { memset(this, 0, sizeof(*this)); }

    FiveTuple(IPv4Addr sip, IPv4Addr dip, Port sp, Port dp, uint8_t proto) {
        memset(this, 0, sizeof(*this));
        src_ip = sip; dst_ip = dip;
        src_port = sp; dst_port = dp;
        protocol = proto;
    }

    // 3. operator== 也改为 memcmp，与 rte_hash 保持一致
    bool operator==(const FiveTuple &other) const {
        return memcmp(this, &other, sizeof(FiveTuple)) == 0;
    }
};
```

### 3.3 session.h — 核心改造

#### 删除的旧结构

```cpp
// ❌ 删除：1024 个分片，每个含 spinlock + unordered_map
struct ReverseShard {
    rte_spinlock_t lock;
    std::unordered_map<FiveTuple, ReverseEntry, FiveTupleHash> map;
};
static const size_t kReverseShards = 1024;
std::array<ReverseShard, kReverseShards> reverse_shards_;
```

#### 新增的 rte_hash 结构

```cpp
// ✅ 新增：单一 rte_hash + 预分配连续数组
static const uint32_t kReverseCapacity = 131072;  // 128K 条目
struct rte_hash *reverse_hash_ = nullptr;          // DPDK 哈希表
ReverseEntry    *reverse_data_ = nullptr;          // 连续内存 value 数组
```

#### init() — 初始化（必须在 EAL 之后调用）

```cpp
bool init() {
    struct rte_hash_parameters params = {};
    params.name       = "reverse_session_hash";
    params.entries    = kReverseCapacity;          // 128K
    params.key_len    = sizeof(FiveTuple);         // 13 bytes (packed)
    params.hash_func  = rte_jhash;                 // Jenkins Hash
    params.socket_id  = rte_socket_id();           // NUMA 感知
    // 关键：开启 Lock-Free 读写并发
    params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY_LF;

    reverse_hash_ = rte_hash_create(&params);
    if (!reverse_hash_) return false;

    // value 数组：rte_hash 只管 key，value 由用户通过返回的 index 管理
    reverse_data_ = (ReverseEntry*)rte_zmalloc(
        "reverse_data",
        sizeof(ReverseEntry) * kReverseCapacity,
        RTE_CACHE_LINE_SIZE);                      // 64 字节对齐
    if (!reverse_data_) { rte_hash_free(reverse_hash_); return false; }
    return true;
}
```

#### lookup_reverse() — 无锁读

```cpp
// 改造前：
bool lookup_reverse(const FiveTuple &t, Session &s) {
    auto &shard = reverse_shard(t);
    rte_spinlock_lock(&shard.lock);           // ❌ 加锁
    auto it = shard.map.find(t);              // ❌ 链式查找，cache miss
    if (it == shard.map.end()) {
        rte_spinlock_unlock(&shard.lock);     // ❌ 解锁
        return false;
    }
    s.client_tuple = it->second.client_tuple;
    rte_spinlock_unlock(&shard.lock);         // ❌ 解锁
    return true;
}

// 改造后：
bool lookup_reverse(const FiveTuple &t, Session &s) {
    int32_t idx = rte_hash_lookup(reverse_hash_, &t);  // ✅ 无锁，SIMD 加速
    if (idx < 0) return false;
    s.client_tuple   = reverse_data_[idx].client_tuple;  // ✅ 数组下标，O(1)
    s.real_server_id = reverse_data_[idx].real_server_id;
    s.server_tuple   = t;
    return true;
}
```

#### allocate_nat_src_port() — 零 malloc 插入

```cpp
// 改造前：
shard.map.emplace(reverse_tuple, ReverseEntry{client_tuple, server_id});
//                 ↑ 每次 emplace 都触发 new/malloc

// 改造后：
int32_t idx = rte_hash_add_key(reverse_hash_, &reverse_tuple);
if (idx < 0) continue;  // hash 满
reverse_data_[idx] = ReverseEntry{client_tuple, server_id};
//                   ↑ 直接写预分配数组，零 malloc
```

#### cleanup_local() — 简洁删除

```cpp
// 改造前：
rte_spinlock_lock(&shard.lock);
shard.map.erase(it->second.server_tuple);
rte_spinlock_unlock(&shard.lock);

// 改造后：
rte_hash_del_key(reverse_hash_, &it->second.server_tuple);
// 内部 CAS 原子标记删除，延迟回收
```

### 3.4 main.cpp — 初始化流程

```
rte_eal_init()                              // 1. DPDK EAL 初始化
    ↓
port_init()                                 // 2. 网卡端口初始化
    ↓
SessionManager::instance().init()           // 3. ★ 新增：创建 rte_hash
    ↓
g_lb.init(config_file)                      // 4. 负载均衡器初始化
    ↓
worker_loop()                               // 5. Worker 开始轮询收包
    ↓
SessionManager::instance().cleanup()        // 6. ★ 新增：释放 rte_hash
```

---

## 四、性能对比预期

| 维度 | 改造前 | 改造后 | 提升原因 |
|------|--------|--------|----------|
| **内存分配** | 每包 `malloc/free` | 零分配 | 预分配数组 |
| **锁开销** | `spinlock` 互斥 | 读路径完全无锁 | RCU + CAS |
| **Cache Miss** | 链式指针追逐 | 连续数组 + SIMD | 数据紧凑 |
| **查找复杂度** | O(1) 平均，O(n) 最坏 | O(1) 确定性 | Cuckoo Hash |
| **多核扩展性** | 随核数增加锁竞争加剧 | 读路径线性扩展 | 无锁读 |

---

## 五、rte_hash 关键 API 速查

| API | 用途 | 复杂度 |
|-----|------|--------|
| `rte_hash_create()` | 创建哈希表（指定容量、key 长度） | 一次性 |
| `rte_hash_lookup()` | 查找 key，返回 index 或 -ENOENT | O(1)，无锁 |
| `rte_hash_add_key()` | 插入 key，返回分配的 index | O(1)，CAS |
| `rte_hash_del_key()` | 删除 key，标记 slot 可回收 | O(1)，CAS |
| `rte_hash_free()` | 释放整个哈希表 | 一次性 |
| `rte_hash_lookup_bulk()` | 批量查找（更高吞吐） | O(n)，无锁 |

> **rte_hash 只管 key 的存储和查找**，value 需要用户自己管理。
> API 返回的 `int32_t index` 就是用户 value 数组的下标。

---

## 六、后端测试服务器 BUG 修复：EPOLLET + 单次 recv() 导致 QPS 断崖

### 6.1 现象

使用 wrk 直接压测后端 RS（绕过 LB），QPS 在前几秒正常（~60K），然后**断崖式跌到 ~100**：

```
[STATS] QPS: 55975    ← 正常
[STATS] QPS: 65234    ← 正常
[STATS] QPS: 62715    ← 正常
[STATS] QPS: 49757    ← 开始下降
[STATS] QPS: 28020    ← 急剧下降
[STATS] QPS: 89       ← 断崖！
[STATS] QPS: 161      ← 几乎死了
[STATS] QPS: 153
[STATS] QPS: 201
```

wrk 报告 3366 个 timeout，说明大量连接卡死再也收不到响应。

### 6.2 根因：EPOLLET 的铁律被违反

**Edge-Triggered 模式的核心规则**：epoll 只在**状态变化**（新数据到达）时通知**一次**。
你**必须循环 `recv()` 直到返回 `EAGAIN`**，把内核缓冲区彻底读空。

原代码的致命错误：

```c
ev.events = EPOLLIN | EPOLLET;  // ← 使用了 Edge-Triggered

// ...

char buffer[1024];
int len = recv(fd, buffer, sizeof(buffer), 0);  // ❌ 只读了一次！
if (len > 0) {
    send(fd, response, response_len, 0);
}
```

### 6.3 为什么前几秒正常、后面断崖？

```
时间线：
────────────────────────────────────────────────────────────
t=0s  连接刚建立，每个 fd 上只有 1 个 GET 请求
      recv() 一次就读完了 → 正常响应 → QPS=60K ✅

t=3s  wrk 用 keep-alive 在同一 fd 上快速发多个请求
      内核缓冲区积压了多个请求：

      内核 recv buffer: [GET /\r\n][GET /\r\n][GET /\r\n]
                             ↑
                       你只读了这一个

      剩余 2 个请求永远留在缓冲区 ← ET 不会再通知！

t=5s  几乎所有连接都"僵死"
      客户端等响应 → timeout → QPS=100 ❌
────────────────────────────────────────────────────────────
```

**对比两种模式**：

```
Level-Triggered (LT，默认模式):
  缓冲区有数据 → epoll_wait 每次都返回 → 即使你只读一次也没事
  下次 epoll_wait 还会通知你 → 不会丢数据

Edge-Triggered (ET):
  缓冲区有新数据到达 → epoll_wait 只通知一次
  你没读完？ → 没有下一次通知 → 数据永远卡在内核里
  必须 while(recv) 直到 EAGAIN → 才安全
```

### 6.4 修复

```c
// ❌ 修复前：单次 recv
char buffer[1024];
int len = recv(fd, buffer, sizeof(buffer), 0);
if (len > 0) {
    send(fd, response, response_len, 0);
    __sync_fetch_and_add(&total_requests, 1);
} else if (len == 0 || (len < 0 && errno != EAGAIN)) {
    close(fd);
}

// ✅ 修复后：循环读到 EAGAIN
char buffer[1024];
while (1) {
    int len = recv(fd, buffer, sizeof(buffer), 0);
    if (len > 0) {
        send(fd, response, response_len, MSG_NOSIGNAL);
        __sync_fetch_and_add(&total_requests, 1);
    } else if (len == 0) {
        close(fd);   // 对端关闭
        break;
    } else {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;   // ★ 读完了！安全退出，等下次 ET 通知
        }
        close(fd);   // 真正的错误
        break;
    }
}
```

### 6.5 EPOLLET 使用检查清单

| 检查项 | 必须做到 |
|--------|---------|
| `recv()` | 必须 `while` 循环到 `EAGAIN` |
| `accept()` | 必须 `while` 循环到 `EAGAIN`（原代码这里是对的）|
| `send()` | 应检查返回值，处理 `EAGAIN`（可注册 `EPOLLOUT`）|
| fd 必须非阻塞 | `fcntl(fd, F_SETFL, O_NONBLOCK)`（原代码这里是对的）|

> **经验总结**：如果不确定，用 **Level-Triggered（默认模式）** 更安全。
> EPOLLET 性能更好但容错率为零，任何遗漏都会导致连接僵死。

