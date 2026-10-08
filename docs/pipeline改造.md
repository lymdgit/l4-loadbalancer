# pipeline 改造方案

## 一、背景

### 1. 现在的数据面模型（run-to-completion）

每个 lcore 是一个 worker，独占网卡的一对 RX/TX 队列，自己完成收包、查会话、改包和发包（`src/dataplane/worker.cpp` 的 `worker_loop`）：

```
            ┌──────── 网卡 RSS 分队列 ────────┐
            ▼                ▼                ▼
   worker 0 (queue 0)  worker 1 (queue 1)  worker 2 (queue 2)
   收包+处理+发包       收包+处理+发包       收包+处理+发包
   + master 周期任务
```

会话表按 worker 划分。同一条连接的两个方向必须由同一个 worker（owner）处理。owner 由 `Steering` 计算（`include/dataplane/steering.h`）：

- **HW 模式**（网卡有 RSS）：owner 等于网卡 RSS 的结果。SNAT 端口专门挑"回程也落回本核"的端口，所以网卡会把两个方向的包直接送到 owner，基本不需要转交。
- **SW 模式**（网卡没有 RSS）：正向 owner 为 `jhash(五元组) % N`，回程 owner 为 `nat_port % N`。

收到包的 worker 如果发现自己不是 owner，就通过 `redirect_ring` 转交给 owner（`processor.cpp` 中的 `redirect()`）。

这个模型的前提是网卡真的会把流量分散到各个队列。

### 2. VMware 下 RSS 不可用

VMware Workstation 的 vmxnet3 能配置 8 个队列，`rte_eth_dev_configure` 设置 RSS 也会成功，但实际测下来：

- 所有包都只进队列 0，其他队列一直是空的；
- 包上没有 RSS hash，`rss_no_hash` 计数持续增长。

要让 RSS 生效，需要在关机状态下修改 `.vmx`（`ethernetX.pnicFeatures="4"`），或者换 ESXi、KVM virtio 多队列、物理网卡。目前不打算折腾环境。

### 3. 在这个环境下，现在的模型有什么问题

以 `-l 1-4`（4 个 worker）为例：

```
网卡 ──全部流量──> queue 0 ──> worker 0：收全部包
                                       ├─ 约 1/4 自己处理
                                       ├─ 约 3/4 解析后转交 worker 1~3
                                       └─ 还要跑 master 任务：健康检查、ARP、免费 ARP、10 秒统计
                  queue 1~3 空转 ──> worker 1~3：只处理转交来的包
```

- **worker 0 身兼三职**：收包分发、转发、控制任务。它一定是第一个撑满的核，其他核在等它。
- **转交前的工作被浪费**：转交之前，worker 0 已经对每个包做过完整解析，甚至查过服务。转交后 owner 还要再做一遍。
- **性能数据很难解读**：压测时只能看到"整体到顶了"，分不清是收包、分发、转发还是控制任务在拖后腿。各个核的负载也不对称，加核以后的收益没法预期。

### 4. 目标

在 RSS 不可用的环境下，改成职责清晰、结构简单的 pipeline：

```
                      ┌──ring──> worker 0：处理 + 发包（TX queue 0）
网卡 ──> receiver ────┤
       （收包 + 分发） └──ring──> worker 1：处理 + 发包（TX queue 1）

master 线程（普通线程，不占 lcore）：健康检查、ARP、免费 ARP、统计（TX queue 2）
```

- 1 个 receiver 核：只收包，算出 owner 后放进对应 worker 的 ring，不做转发。
- 2 个 worker 核：只从自己的 ring 取包处理，经自己的 TX 队列发出。两个 worker 运行完全相同的代码，没有哪个承担额外任务。
- master 任务从 worker 0 移到一个普通线程。
- 性能测试时，每个核的职责都单一。看各核的 CPU 占用和 ring 丢包计数，就能判断瓶颈在 receiver 还是 worker。

不追求通用性，也不做多 receiver、动态负载均衡、rte_flow 这些。

## 二、设计

### 1. 运行模式与核分配

`[global]` 新增一个配置项：

```ini
# 数据面模式：rtc = 每核收包+处理（网卡有 RSS 时用）；pipeline = 1 个核收包分发，其余核处理
dataplane = pipeline
```

- 代码里默认是 `rtc`，保持现有行为，也方便以后上了支持 RSS 的网卡做对比。`config/lb.conf`、`tests/perf/lab.conf` 改成写 `pipeline`。
- pipeline 模式下，EAL 的 main lcore 是 receiver，其余 lcore 依次是 worker 0、1、……。`-l 1-3` 就表示 lcore 1 做 receiver，lcore 2、3 做 worker 0、1。master 线程和控制线程不占用 `-l` 里的核（见第 5 节）。
- 至少需要 2 个 lcore（1 个 receiver 加 1 个 worker），否则启动报错。
- 设计上支持任意多个 worker，"1 + 2" 只是这次推荐的用法。

### 2. 网卡队列

- RSS 关闭，steering 强制使用 SW 模式，N = worker 数。SNAT 端口按 `port % N == worker` 分配，这部分逻辑已经有了（`Steering::init_sw`、`port_step`）。
- TX 队列数为 worker 数 + 1：worker i 独占 TX 队列 i，master 线程独占最后一个。TX 队列不能多个线程共用，每个线程一个，就不需要加锁。
- RX 队列数取 TX 队列数向上取到 2 的幂，再不超过网卡上限（vmxnet3 要求 RX 队列数是 2 的幂；af_packet 的队列数由 `qpairs` 固定）。
- rtc 模式下 worker i 轮询 `q % worker 数 == i` 的 RX 队列（RX 队列可能比 worker 多）；master 线程不收包。
- receiver 轮询所有 RX 队列。vmxnet3 实际上只有队列 0 有包，其他队列空转一次的开销可以忽略。af_packet（功能测试用）会按 fanout 把包分到每个 qpair，不轮询全部的话会漏包。
- receiver 不发包，不占用 TX 队列。

### 3. receiver 的分发规则

receiver 只做轻量解析（以太网头、IPv4 头、L4 端口），不查会话也不改包。规则如下：

| 包 | 送往 |
|---|---|
| TCP/UDP，FULLNAT 模式下目的地址是 LIP、端口在 SNAT 端口段（回程） | `ret_owner(五元组)`，也就是 `nat_port % N` |
| 其他 TCP/UDP（入站、健康检查回包） | `fwd_owner(五元组)`，也就是 `jhash % N` |
| ICMP、IP 分片、其他 IP 协议 | `hash(源 IP, 目的 IP) % N` |
| ARP、非 IPv4、头部不完整 | worker 0（量很小，不影响对称） |

不再有专门接收控制包的 worker。ARP 应答、ICMP echo、健康检查回包在哪个 worker 上处理都可以，它们产生的事件（学到邻居、HC 回包）通过已有的 `master_ring` 交给 master 线程。

worker 侧的 owner 检查和 `redirect` 保持不变，作为兜底。receiver 没有细分的情况（比如 ICMP 差错要按内层五元组找 owner），worker 会像现在一样再转交一次，正确性不受影响。正常流量下 `redirect_out` 应该接近 0，可以用它来验证分发是否正确。

LIP 列表是启动配置，运行中不会变，receiver 启动时自己保存一份，所以不用读 snapshot，也不需要注册成 RCU 读者。

### 4. receiver 到 worker 的 ring

- 每个 worker 新增一个 `rx_ring`：单生产者（receiver）、单消费者（worker），大小 4096。原来的 `redirect_ring` 保留，只用于兜底转交。
- receiver 每次收一批（64 个包），先按目标 worker 分组，再每组 `rte_ring_sp_enqueue_burst` 一次。
- ring 满时直接丢包，计入新增的计数器 `drop_rx_ring`。不等待、不重试，避免 receiver 被慢的 worker 拖住。

### 5. worker 循环

`worker_loop` 中的收包来源按模式区分：

```
rtc:       rte_eth_rx_burst(port, w.idx)   → handle()
pipeline:  rte_ring_sc_dequeue_burst(w.rx_ring) → handle()
两种模式都要： redirect_ring → handle(redirected=true)，以及 tx_flush、RCU、时间轮、master 任务
```

`process_packet` 及之后的处理逻辑都不改。worker 循环里所有 `if (master)` 分支都删掉，每个 worker 只负责转发和自己会话表的超时回收。

### 6. master 线程

原来 worker 0 上的 master 任务全部移到一个普通线程 `l4lb-master` 里。这个线程每 1ms 醒来一次：

- 处理 `master_ring` 事件：ARP 学习、邻居提示、ARP 缺失、健康检查回包；
- 每 100ms 跑一次健康检查，每 1s 做一次邻居老化和预解析，按节奏发免费 ARP，每 10s 打一次统计；
- 要发的包（ARP 请求、免费 ARP、HC 的 SYN/RST）从自己独占的 TX 队列发出，不经过 worker。

实现细节：

- 线程用 `rte_thread_create_control()` 创建，DPDK 会把它绑到 `-l` 之外的 CPU 上，不和 receiver、worker 抢核。控制 socket 线程现在是 EAL 初始化后创建的 `std::thread`，会继承 main lcore 的亲和性，跟 receiver 跑在同一个核上，所以也一并改用这种方式创建。
- master 线程读 snapshot 和邻居表，所以要注册成 RCU 读者（使用一个不和 lcore 冲突的 thread id），每轮报告一次静默期。
- 邻居表仍然只有一个写者，也就是 master 线程，并发模型不变。
- rtc 模式下也使用 master 线程，这样两种模式的 worker 代码完全一样。

master 线程每毫秒最多被延迟一点，影响的是 ARP 学习和健康检查的响应时间（毫秒级），对这两件事都没有影响。

### 7. 统计

- receiver 有自己的一份计数器：`rx`、`drop_rx_ring`。master 线程也有一份（它发出的 ARP、HC 包计入 `tx`）。`stats_total()` 汇总时都算进去。
- pipeline 模式下，网卡收包只由 receiver 计入 `rx`；worker 从 ring 取到的包计入新的 `ring_in`，避免重复计数。
- `l4lbctl.py stats -v` 增加一行 receiver，并显示每个 worker 的 ring 占用。这样压测时能直接看出是 receiver 收不过来（网卡 imissed 增长），还是 worker 处理不过来（ring 满、`drop_rx_ring` 增长）。

### 8. 启动、退出与资源

- mbuf 池的大小要加上 `worker 数 × rx_ring 大小`，以及 master 线程的 TX 队列。
- receiver 在 main lcore 上运行 `receiver_loop`，worker 通过 `rte_eal_remote_launch` 启动，master 线程在 worker 之前启动。
- 退出时 receiver、worker、master 线程都按 `g_running` 停止。先等 lcore 结束，再 join master 线程，最后释放 ring 里残留的 mbuf。

## 三、改动范围

| 文件 | 改动 |
|---|---|
| `include/common/config.h`、`src/common/config.cpp` | 新增 `dataplane = rtc / pipeline` 配置项，加校验，`dump` 中打印 |
| `src/dataplane/port.cpp` | pipeline 模式下关闭 RSS，强制 SW steering |
| `include/dataplane/context.h` | `WorkerCtx` 增加 `rx_ring`；新增 receiver、master 的上下文 |
| `src/dataplane/receiver.cpp`（新） | `receiver_loop`、轻量解析和分发规则 |
| `src/dataplane/master.cpp`（新） | 从 `worker.cpp` 移出来的 master 任务，加上 `master_loop` |
| `src/dataplane/worker.cpp` | 删除 master 分支；收包来源按模式切换；`stats -v` 增加 receiver、master 信息 |
| `src/ctrl/control.cpp` | 控制线程改用 `rte_thread_create_control` 创建 |
| `include/common/stats.h` | 新增 `drop_rx_ring`、`ring_in` 计数器 |
| `src/main.cpp` | 按模式分配 lcore、配置队列、创建 ring、启动 receiver 和 master 线程、退出清理 |
| `config/lb.conf`、`tests/perf/lab.conf`、`tests/perf/lab.sh` | 改为 `dataplane = pipeline`，`LB_CORES` 默认改成 `1-3` |
| `tests/` | 见下节 |

`processor.cpp`、会话表、调度、健康检查逻辑都不改（健康检查只是换到 master 线程上调用）。

## 四、测试

- **单元测试**：针对分发规则，构造入站、回程、ARP、ICMP、健康检查回包，检查 receiver 给出的目标 worker 与 `fwd_owner` / `ret_owner` 是否一致。
- **功能测试**（veth + af_packet）：`harness.LB` 增加 `dataplane` 参数，用 `-l 8-10`（1 个 receiver 加 2 个 worker）跑下面几项：
  - NAT 往返、DR 基本转发
  - 多核并发建连和过期（参照 `multicore_concurrent_sessions`），并检查 `redirect_out` 接近 0
  - 健康检查、控制面命令（验证 master 线程能正常发包、收事件）
  - 只给 1 个 lcore 时拒绝启动
  - 原有的 rtc 用例全部保持通过（harness 的默认 `qpairs` 改成 lcore 数 + 1，给 master 线程留一个 TX 队列）
- **实机**（ens160 / vmxnet3）：`lab.sh lb` 加 `lab.sh bench`。观察 receiver 和两个 worker 的 CPU 占用（`perf top -C <lcore>`）、`stats -v`，确认两个 worker 的负载大致相当、`drop_rx_ring` 为 0，master 线程不在 `-l` 的核上。

## 五、取舍

- **receiver 是单点上限**：整机吞吐不会超过一个核的收包加分发能力。在 VMware Workstation 下，vmxnet3 本身的瓶颈通常更低，所以可以接受。
- **每个包多一次 ring 转交**：会多出一次入队出队，mbuf 头部也要跨核搬一次 cache line，延迟会略有增加。
- **master 线程需要一个额外的 TX 队列**（功能测试的 af_packet `qpairs` 也要多给一个）：rtc 模式下，队列数要求从"等于 lcore 数"变成"lcore 数 + 1"。vmxnet3 有 8 个，够用。
- **有 RSS 的网卡上，rtc 模式更好**：所有核都能收包，而且不需要转交。所以保留 rtc 模式，以后换环境时改一行配置就能切回去，也能拿两种模式做对比测试。

## 六、实现结果（2026-10-08）

- 单元测试 8 个全过（新增 `tests/unit/test_dispatch.cpp`）；功能测试 23 个在 rtc 和 pipeline 两种模式下都通过（`L4T_DATAPLANE=pipeline run_tests.py ...` 用 pipeline 跑全部用例）。
- ens160（vmxnet3）`-l 1-3`：4 RX / 3 TX 队列；receiver 在 lcore 1，worker 在 lcore 2/3，`l4lb-master`、`l4lb-ctl` 线程落在 `-l` 之外的 CPU。
- `lab.sh bench`（5 秒）：keep-alive 13901 QPS、短连接 2396 CPS，0 错误；两个 worker 的 ring_in 为 138329 / 152604，`redirect_out` 0，`rx_ring_full` 0，Dropped 0。这个数字受本机 loadgen 和 RS 共用 CPU、vmxnet3 的限制，只用作和后续改动对比的基线。
