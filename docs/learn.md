# L4 负载均衡器学习教程

> 本文第一部分按当前代码整理（2026-10-09）；第二部分"我的梳理过程"是学习过程中的笔记，个别地方加了更正。
> 更详细的设计见 `docs/架构图.md`、`docs/pipeline改造.md`，运行和命令见根目录 README 与 `docs/控制命令.md`。

## 项目概述

这是一个基于纯 DPDK 的四层负载均衡器，参考 DPVS、LVS 和腾讯 TGW 的思路实现。数据包在用户态按包改写转发，不终止 TCP 连接，不依赖内核协议栈或 F-Stack。

### 核心特性

- **真正的 L4**：数据包级别转发，不终止 TCP 连接
- **两种转发模式**：FULLNAT（跨网段，双向都经过 LB）和 DR（同二层，回包不经过 LB）
- **两种数据面模式**：pipeline（1 个收包核 + N 个转发核，适合不支持 RSS 的网卡）和 rtc（每核独立收发，网卡 RSS 分流）
- **无锁多核**：会话表每个转发核一份；配置以 RCU 快照发布；热路径无锁、无 malloc
- **调度**：平滑加权轮询（WRR）、Maglev 一致性哈希
- **FULLNAT 细节**：多 LIP 的 SNAT 端口池、TCP 状态机、TOA 透传客户端地址、去 SYN timestamp、ICMP 差错转换
- **校验和**：网卡支持时用 TX offload，否则 L4 增量更新（TCP 选项变化时全量重算）
- **运维**：ARP 解析与老化、免费 ARP、TCP 健康检查、unix socket 控制命令（动态增删后端）
- **可观测性**：异步日志、按核忙碌率、按 RS 的包/字节计数、压测区间 PPS

### 性能表现

VMware Workstation，vmxnet3，pipeline 1 个收包核 + 2 个转发核，后端 1 台 nginx，`wrk -t4 -c1000 -d60s`：

| 场景 | QPS | LB 转发 PPS | 转发核负载 |
|---------|-----|------|------|
| 直连 nginx | 161,674 | – | – |
| FULLNAT | 126,145（78%） | 22.4 万（双向） | 67% |
| DR | 133,090（82%） | 12.1 万（只有入向） | 33% |

瓶颈在虚拟交换机和后端，LB 自身还有余量。说明和复现方法见 README 第六节、`docs/压测方案.md`。

## 架构介绍

### 项目结构

```
l4-loadbalancer/
├── CMakeLists.txt / build.sh   # 构建（./build.sh 一键编译）
├── config/                     # lb.conf（FULLNAT）、lb_dr.conf（DR）、bench*.conf（压测）
├── include/ src/               # 一一对应
│   ├── common/                 # 配置、日志（异步）、统计计数器、公共类型
│   ├── protocol/               # 以太网/IP/TCP/UDP 头、解析器、校验和、ARP/ICMP
│   ├── lb/                     # 会话表、TCP 状态机、调度器（WRR / Maglev）
│   ├── forward/                # FULLNAT / DR 改写、TOA、ICMP 差错转换
│   ├── net/                    # 邻居表（ARP）、路由
│   ├── ctrl/                   # 配置快照（RCU）、健康检查、控制命令
│   ├── dataplane/              # 端口、分发（steering）、receiver / worker / master 线程
│   └── core/                   # 报文分类与处理主流程（processor）
├── src/main.cpp                # 启动、装配、退出
├── scripts/                    # setup_dpdk_env.sh（网卡接管）、l4lbctl.py（控制命令）
├── tests/unit/                 # 单元测试（ctest）
├── tests/functional/           # 功能测试（veth + net_af_packet，不需要真实网卡）
├── tests/perf/                 # 压测脚本
└── docs/                       # 设计文档、学习笔记
```

### 核心架构

```
                       pipeline 模式（VMware vmxnet3 下使用）
网卡 ──> receiver（收包核）──rx_ring──> worker 0：查会话 → 调度 → 改写 → 发包
         按五元组 / SNAT 端口   └──rx_ring──> worker 1：同上
         算出会话所在的核
master 线程（普通线程）：ARP、邻居老化、免费 ARP、健康检查、周期统计
控制线程：unix socket 命令 → 构建新配置快照 → RCU 发布
```

1. **收发包**：DPDK 轮询网卡，`rte_eth_rx_burst` / `rte_eth_tx_burst` 批量收发
2. **分发**：保证同一条连接的双向包都由同一个转发核处理（会话表不跨核共享）
3. **协议解析**：以太网、IPv4、TCP/UDP，提取五元组
4. **负载均衡**：新连接按 WRR 或 Maglev 选 RS，建会话
5. **转发**：FULLNAT 改 IP/端口/MAC 和校验和；DR 只改 MAC

### FULLNAT vs DR 模式

#### FULLNAT
- LB 同时修改源地址（改成 LIP:SNAT 端口）和目的地址（改成 RS）
- RS 看到的客户端是 LB，回包必须经过 LB；需要 TOA 才能拿到真实客户端 IP
- 可以跨网段部署，RS 端口可以与 VIP 端口不同
- LB 要处理双向流量

#### DR 模式 (Direct Routing)
- LB 只修改二层 MAC 地址，IP 和端口完全不动
- 返回流量直接从 RS 到 Client，不经过 LB
- LB 只处理入向，同样流量下负载约为 FULLNAT 的一半
- 要求 LB 和 RS 在同一二层网络，RS 的 lo 上要配 VIP 并关闭 VIP 的 ARP 响应，RS 端口必须与 VIP 端口相同

## 环境搭建

### 系统要求

- Linux x86_64（当前在 Anolis OS 8 / 内核 5.10 上开发）
- DPDK 24.11（本机安装在 `/root/dpvs/dpvs/dpdk-24.11/dpdklib`）
- GCC 8+（C++17）、CMake 3.16+、pkg-config
- 大页内存；DPDK 能用的网卡（vfio-pci / uio_pci_generic），或者只跑功能测试时用 veth

### DPDK 安装

```bash
wget https://fast.dpdk.org/rel/dpdk-24.11.tar.xz
tar xf dpdk-24.11.tar.xz && cd dpdk-24.11
meson setup build --prefix=/opt/dpdk
ninja -C build && sudo ninja -C build install && sudo ldconfig
```

### 项目编译

```bash
./build.sh                          # 自动查找 libdpdk.pc，产物 build/l4lb
./build.sh test                     # 编译 + 单元测试
DPDK_PREFIX=/opt/dpdk ./build.sh    # DPDK 不在默认位置时
```

## 代码结构讲解

### 入口文件：main.cpp

只负责启动流程：参数解析 → 加载并校验配置 → 打开日志文件 → EAL 初始化 → 规划并初始化网卡队列 → 创建 RCU、邻居表、配置快照、各 worker 的会话表和 ring → 启动控制线程、master 线程 → 在各 lcore 上启动 worker（pipeline 下 main lcore 跑 receiver）→ 收到信号后退出并打印统计。

### 数据面线程：dataplane/

- `receiver.cpp`：pipeline 的收包核。`Dispatcher::target()` 按规则算出目标 worker（入站 `fwd_owner`、FULLNAT 回程 `ret_owner`、ICMP/分片按地址哈希、ARP 给 worker 0）
- `worker.cpp`：`worker_loop()`，每轮取包 → `process_packet()` → 批量发包 → 报告 RCU 静默期，每 1024 轮推进会话时间轮；也包含统计输出（`stats` / `counters` / `rate`）
- `master.cpp`：master 线程，每 1ms 处理 worker 上报的事件（ARP 学习、邻居缺失、健康检查回包），跑周期任务，从独占的 TX 队列发 ARP 和健康检查报文
- `steering.cpp`：会话 owner 的计算。软件模式：入站 `jhash % N`，回程 `nat_port % N`；硬件 RSS 模式：软件复现网卡的 Toeplitz 哈希
- `port.cpp`：队列规划（TX = worker 数 + 1，RX 取 2 的幂）、RSS/RETA、offload

### 报文处理：core/processor.cpp

`process_packet()` 是处理入口，按以太类型、协议、目的地址分到：
- `handle_arp()`：应答本机地址的 ARP，学习邻居（交给 master）
- `handle_icmp()`：echo 应答、ICMP 差错转换
- `handle_inbound()`：客户端 → VIP，查会话或新建会话，改写后发给 RS
- `handle_return()`：FULLNAT 回程，RS → LIP，改写后发回客户端
- `handle_hc_response()`：健康检查回包，交给 master

### 负载均衡与会话：lb/

- `scheduler.cpp`：平滑加权轮询（WRR，展开成表，O(1)）和 Maglev 一致性哈希（65537 个槽位的查找表，按五元组 jhash 取槽，O(1)；增删后端时只有少量连接被重新映射）。调度表在控制面构建，数据面只读
- `session.cpp`：每个 worker 一张会话表，rte_hash 做索引 + 预分配的会话数组 + 1 秒粒度的时间轮。FULLNAT 会话有正向（Client → VIP）和回程（RS → LIP）两个 key，指向同一个会话。只由所属 worker 访问，不需要锁
- `tcp_state.cpp`：SYN_RECV / ESTABLISHED / FIN_WAIT / TIME_WAIT / CLOSE，各状态独立超时

### 转发：forward/nat_forwarder.cpp

- `nat_rewrite()`：FULLNAT 改写源/目的 IP、端口、MAC、TTL，更新校验和，按需插入 TOA、去掉 SYN 的 timestamp
- `dr_rewrite()`：只改 MAC
- `nat_rewrite_icmp_error()`：ICMP 差错报文的内外层地址转换

校验和策略见 `docs/校验处理.md` 第 5 节。

### 控制面：ctrl/

- `snapshot.cpp`：期望状态（服务、RS、权重、健康状态）→ 构建只读快照 → 原子替换 → RCU 宽限期后释放旧快照
- `healthcheck.cpp`：在 master 线程里从 `hc_src` 发 TCP SYN 探测 RS，连续失败摘除、连续成功加回
- `control.cpp`：unix socket 命令（`services`、`add`、`del`、`weight`、`enable`、`disable`、`stats`、`rate`、`counters`、`log`、`quit`）

## 构建和运行

### 配置文件

`config/lb.conf`（节选，完整说明见文件内注释）：

```ini
[global]
mode = nat                 # nat（FULLNAT）或 dr
dataplane = pipeline       # pipeline 或 rtc
log_dir = /data/logs/l4
max_sessions = 1048576
tcp_established_timeout = 900

[network]
netmask = 255.255.255.0
gateway = 192.168.154.2
local_ips = 192.168.154.131, 192.168.154.134, 192.168.154.135, 192.168.154.136

[healthcheck]
enabled = true

[control]
socket = /run/l4lb.sock

[service.http]
vip = 192.168.154.130
port = 80
proto = tcp
scheduler = wrr            # wrr 或 maglev
server1 = 192.168.154.140:80:1        # ip:port[:weight[:mac]]，mac 留空则走 ARP
server2 = 192.168.154.140:8080:1
```

旧格式（`[vip]` + `[realserver]`，`session_timeout`）仍然兼容。

### 运行命令

```bash
# 大页 + 驱动 + 网卡接管（每次重启后执行）
sudo DPDK_NIC=ens160 scripts/setup_dpdk_env.sh up

# 先检查配置
./build/l4lb -- --lb-config config/lb.conf --check-config

# pipeline：lcore 1 收包分发，lcore 2、3 两个转发核
sudo ./build/l4lb -l 1-3 -- --lb-config config/lb.conf

# 运行时查看和调整
scripts/l4lbctl.py services
scripts/l4lbctl.py stats -v
tail -f /data/logs/l4/l4lb.log
```

### 多核与队列

- pipeline：`-l` 里第一个 lcore 是 receiver，其余是 worker；至少 2 个 lcore
- rtc：每个 lcore 都是 worker
- 两种模式下网卡 TX 队列数都要不少于 worker 数 + 1（最后一个给 master 线程）
- VMware Workstation 的 vmxnet3 不做 RSS（所有包进同一个队列），所以用 pipeline；在支持 RSS 的网卡上可以用 rtc

## 测试

### 单元测试

```bash
./build.sh test        # 或者 cd build && ctest
```

8 个测试程序：配置解析、日志、receiver 分发规则、报文改写与校验和、调度器、会话表与时间轮、steering（Toeplitz 标准向量）、TCP 状态机。

### 功能测试

```bash
sudo python3 tests/functional/run_tests.py build/l4lb
sudo L4T_DATAPLANE=pipeline python3 tests/functional/run_tests.py build/l4lb   # pipeline 模式跑一遍
```

在 veth 上用 `net_af_packet` 启动 l4lb，用 raw socket 收发构造的报文，覆盖 FULLNAT/DR 转发、畸形包、会话回收、TCP 状态机、多 LIP、TOA、ICMP 差错、ARP/网关、健康检查、控制命令、多核、pipeline 分发、包/字节计数、日志文件。

### 性能测试

客户端、LB、RS 分别放在不同的 VM 上，步骤见 `docs/压测方案.md`：

```bash
wrk -t4 -c1000 -d60s --latency http://192.168.154.130/     # 在 client 上
scripts/l4lbctl.py counters > /tmp/before.txt              # 在 LB 上，压测前
scripts/l4lbctl.py delta /tmp/before.txt                   # 在 LB 上，压测后：PPS、忙碌率
```

### 抓包调试

DPDK 接管的网卡（ens160）在内核里看不到，`tcpdump -i ens160` 抓不到包。可以在 client 或 RS 上抓：

```shell
sudo tcpdump -i ens33 -n -e -v tcp port 80
```

## 常见问题排查

### 问题 1：连接被 RST 或不通

**排查**：
- `scripts/l4lbctl.py stats` 看 `Drops:` 一行的丢包原因（对照表见 `docs/控制命令.md` 第四节）
- 在 RS 上 `tcpdump -v`，看收到的包是否 `cksum correct`
- 校验和问题可以用 `cmake -DL4LB_HW_CKSUM=OFF` 编译，改为软件计算，排除 offload 的影响

### 问题 2：日志出现 `No available backend`

所有后端都是 `down`、`disabled` 或权重为 0。用 `scripts/l4lbctl.py services` 查看，健康检查失败时检查 RS 端口和防火墙；也可以用 `add` / `del` 命令在运行时换后端（`docs/控制命令.md` 第三节）。

### 问题 3：DR 模式下 RS 收不到包或客户端绕过了 LB

RS 没有配置 VIP，或者 RS 应答了 VIP 的 ARP：

```bash
sudo tests/perf/dr_rs.sh up      # 在 RS 上：先改 arp_ignore/arp_announce，再在 lo 上加 VIP
ip neigh flush 192.168.154.130   # 在 client 上清掉 VIP 的旧 ARP 缓存
```

## 后端开发面试准备

### 核心概念

#### 1. L4 vs L7 负载均衡

| 特性 | L7 代理 | 真正的 L4 |
|------|--------|----------|
| 工作层级 | Socket 层 | 数据包层 |
| TCP 连接 | LB 终止连接 | 端到端保持 |
| 性能 | 较低 | 高（零拷贝） |
| 实现方式 | accept() + connect() | 直接修改数据包头部 |

#### 2. FULLNAT vs DR 模式

**面试题**：DR 模式为什么性能高？

**答案**：LB 只处理入站流量，响应（通常更大）由 RS 直接返回客户端。本项目实测：同样流量下 DR 的转发核负载约为 FULLNAT 的一半。

**面试题**：DR 模式的限制是什么？

**答案**：LB 和 RS 要在同一二层网络；RS 要在 lo 上配置 VIP 并关闭 VIP 的 ARP 响应；不能改端口，RS 端口必须等于 VIP 端口。

**面试题**：FULLNAT 下回程包怎么回到创建会话的那个核？

**答案**：回程包的目的端口就是 LB 分配的 SNAT 端口。分配端口时只挑"回程会被分到本核"的端口（软件分发：`port % N == 本核`；硬件 RSS：软件算 Toeplitz 哈希，挑落到本核队列的端口），收到回程包时按同样的规则就能算出会话在哪个核。

#### 3. 一致性哈希（Maglev）

**面试题**：Maglev 和哈希环有什么区别？

**答案**：哈希环靠虚拟节点保证均匀，查找要二分；Maglev 预先构建一张大小为素数 M（本项目 65537）的查找表，每个后端按自己的 offset/skip 序列轮流填槽，查找时 `table[hash % M]`，O(1)，负载更均匀，增删后端时被重新映射的连接也很少。

**面试题**：一致性哈希的优势？

**答案**：后端增减时只有少量连接改变去向，大部分连接不受影响。

#### 4. DPDK 性能优化

**面试题**：DPDK 为什么快？

**答案**：
- 用户态驱动 + 轮询，避免中断和系统调用
- 大页内存，减少 TLB miss
- 线程绑核，减少缓存失效和调度
- 批量收发（burst），摊薄每包的固定开销
- 每核私有数据（会话表、计数器、mbuf 本地缓存），避免锁和伪共享

#### 5. 零拷贝

**面试题**：什么是零拷贝？项目中如何实现？

**答案**：数据在收发过程中不在内存里来回拷贝。DPDK 的网卡直接 DMA 到大页上的 mbuf，程序在原 mbuf 上改写包头后直接发出，整个过程没有内核态和用户态之间的拷贝。

#### 6. 无锁与 RCU

**面试题**：运行时修改后端，转发核怎么保证读到一致的配置又不加锁？

**答案**：控制线程把新配置构建成一个只读快照，原子地替换指针；转发核每处理完一批包报告一次"静默期"（不再引用旧快照）；控制线程等所有读者都报告过之后再释放旧快照（DPDK 的 `rte_rcu_qsbr`）。读的一侧只有一次原子 load。

### 常见面试题

1. **如何保证会话亲和性？**
   - 新连接按调度算法选 RS 后建立会话，之后同一连接的包都查会话表，不再调度
   - Maglev 一致性哈希在会话表丢失时（例如换了 LB 实例）也能让大部分连接落到原来的 RS

2. **如何处理连接超时？**
   - TCP 各状态独立超时（SYN 10 秒、ESTABLISHED 900 秒、TIME_WAIT 10 秒等）
   - 1 秒粒度的时间轮：每包只更新过期时间，不移动链表；时间轮扫到时再判断是否真的过期

3. **如何实现高可用？**
   - 多 LB 实例共享 VIP（VRRP 主备，或 BGP ECMP 多活）
   - 健康检查自动摘除故障后端（本项目已实现）
   - 会话同步（本项目未实现）

4. **性能瓶颈在哪里？如何定位？**
   - 看每个线程的忙碌率（只算处理包的 TSC 周期，不能看 top）、网卡 `imissed`、ring 是否满
   - 本项目在 VMware 下转发核负载 67% 时 QPS 已经到顶，网卡也没丢包，说明瓶颈在虚拟交换机和后端，不在 LB

### 学习建议

1. **深入理解 DPDK**：mbuf、mempool、ethdev 队列、rte_ring、rte_hash、rte_rcu_qsbr
2. **网络协议栈**：TCP/IP 头部格式、TCP 状态机、校验和与伪头部
3. **并发编程**：无锁数据结构、内存序（acquire/release、relaxed）、RCU、伪共享
4. **性能分析**：perf、火焰图，以及忙碌率、PPS 这类数据面指标
5. **实践**：尝试修改代码，例如加一个调度算法、实现 SYN proxy，或者在支持 RSS 的环境里对比 rtc 和 pipeline



# 我的梳理过程

## **1.FULLNAT数据的修改和流向**

（原图是本机 Typora 的本地图片，没有放进仓库。FULLNAT 的四步改写见 `docs/架构图.md` 第五节。）

## **2.项目的应用场景**

### 真实场景：L4 (你) + L7 (Nginx) 的配合

你的理解完全正确！在企业级架构中，通常就是 **L4 (你的项目) -> L7 (Nginx) -> 业务代码**。

#### 为什么要这么做？

- **你的 L4 (DPDK)**：负责**抗压**。处理 TCP 连接建立，清洗 DDoS 攻击流量，通过多核并行把海量并发分发给后端的 Nginx 集群。你只管“快”。
- **Nginx (L7)**：负责**逻辑**。它解析 HTTP 协议，根据 URL (`/api`, `/image`) 把请求分发给不同的业务服务器，处理 SSL/TLS 卸载（HTTPS 解密）。

#### Nginx (L7) 又会修改什么？

当你的 L4 把包扔给 Nginx 后，Nginx 的行为和 L4 截然不同：

1. **TCP 终结**：你的 L4 只是转发 TCP 包，**Client 和 RS 还是在直接进行 TCP 对话**（虽然 IP 变了）。但 Nginx 会**终结**客户端的 TCP 连接，然后自己**新建**一个 TCP 连接去连后端的业务服务器。
2. **数据重组**：Nginx 会把收到的 TCP 包拼成完整的 HTTP 请求（Header + Body）。
3. **修改内容**：
    - **HTTP Header**：Nginx 会添加 `X-Forwarded-For`。
        - *为什么要加？* 因为你的 L4 使用了 FullNAT，把源 IP 改成了 `2.2.2.2`。业务服务器看到的 IP 是 `2.2.2.2`，而不是真实的客户 IP。Nginx 必须把真实 IP 塞进 HTTP 头里，业务逻辑才能知道是谁在访问。
    - **路径重写 (Rewrite)**：比如把 `/api/v1/user` 改成 `/user` 发给后端。

如果不传真实 IP，Nginx 看到的来源全是 `2.2.2.2`（你的 L4 VIP），这会导致：

1. **安全失效**：无法封禁恶意 IP，因为所有请求看起来都一样。
2. **日志废了**：访问日志里全是内网 IP。
3. **限流失效**：Nginx 的 `limit_req` 会把所有用户当成一个人来限流，瞬间卡死。

在 **FullNAT** 模式下（即你现在的架构），业界主要有两套主流方案把 IP 传给 Nginx：

------

### 方案一：TOA (TCP Option Address) —— 你猜对了！

这就是你提到的“放到 TCP 可选字节中”。这是 **LVS (Linux Virtual Server)** 和许多大厂（包括阿里、腾讯早期的 TGW）常用的方案。

- **原理**：

    利用 TCP Header 里的 `Options` 字段。
    （本项目的实现：不放在 SYN 里，而是在握手完成前客户端方向的非 SYN 包里（通常是第三个 ACK）插入，选项号 254，与 DPVS 的 toa.ko 一致；配置 `toa = on` 开启。）

    你自定义一个 `Option ID`（通常是 `254` 或 `200`），然后把 `Client IP` 和 `Client Port` 塞进去。

- **你的 L4 要做的事**：

    1. 解析 Client 发来的 SYN 包。
    2. 在转发给 RS（Nginx）之前，**扩展 TCP Header**（注意：这会改变包的大小，需要重新计算 Checksum）。
    3. 把真实 IP (4字节) + 端口 (2字节) 写入 Options 区域。

- **后端 Nginx 要做的事**：

    Nginx 默认是读不到 TCP Options 的。

    - **方法 A（内核级）**：在 RS 的操作系统内核加载一个 `toa.ko` 模块。它会拦截内核的 `tcp_v4_rcv`，把 Options 里的 IP 取出来，覆盖掉内核 socket 结构体里的 `sk_daddr`。这样 Nginx `accept()` 拿到的就是真实 IP，Nginx **完全无感知**，以为就是公网直连的。
    - **方法 B（应用级）**：Nginx 使用第三方模块（如 `ngx_http_toa_module`）去读取。

- **优点**：对应用层完全透明，Nginx 不需要改配置，性能极高。

- **缺点**：**开发难度极大**。你需要熟练操作 TCP 协议头，计算校验和，而且后端服务器需要通过内核模块配合（侵入性强）。

------

### 方案二：Proxy Protocol —— 现代标准（推荐）

这是由 **HAProxy** 发明的协议，现在已经是通过 L4 传递 IP 的**世界标准**（AWS ELB、Google Cloud LB、Nginx 都原生支持）。

- **原理**：

    不在 TCP 头里藏东西，而是在 **TCP 建立连接后，发送真实数据之前**，先发一段“纯文本”或“二进制”的元数据。

- **你的 L4 要做的事**：

    1. 你和 RS（Nginx）完成三次握手。

    2. 在转发 Client 的**第一个数据包**之前，先往 Socket 里塞入一行字符串：

        ```Plaintext
        PROXY TCP4 1.1.1.1 2.2.2.2 12345 80\r\n
        ```

        （格式：协议 源IP 目的IP 源端口 目的端口）

    3. 然后再发送 Client 真正的数据。

- **后端 Nginx 要做的事**：

    Nginx 原生支持！只需要在配置文件里加一行：

    ```Nginx
    server {
        listen 80 proxy_protocol; # 开启 proxy_protocol 支持
        ...
    }
    ```

- **优点**：

    1. **开发简单**：你只需要在 payload 前面拼一段字符串，不需要改 TCP Header，不需要重算复杂校验和。
    2. **兼容性好**：不用去动 RS 的 Linux 内核，Nginx 开箱即用。

- **缺点**：如果后端应用（如某些自研 Server）不支持解析这个协议，会把这行字符串当成乱码报错。

------

### 总结与建议

| **特性**          | **TOA (TCP Options)**                    | **Proxy Protocol**                |
| ----------------- | ---------------------------------------- | --------------------------------- |
| **实现方式**      | 修改 TCP 头部 (L4)                       | 插入 Data Payload (L4/L5)         |
| **你的工作量**    | **极高** (涉及 Checksum/Header 长度调整) | **中等** (拼接数据包)             |
| **后端要求**      | 需要加载内核模块 (toa.ko)                | Nginx 配置开启 `proxy_protocol`   |
| **性能**          | 极致                                     | 非常好                            |
| **调试难度**      | 很难 (Wireshark 看起来是正常包)          | 容易 (可以直接看到字符串)         |
| **腾讯/阿里做法** | 早期主要用 TOA，现在混合使用             | 公有云对外通常支持 Proxy Protocol |

**给你的建议：**

既然你是为了学习 DPDK 和高性能网络：

1. **先做 Proxy Protocol**：这能让你快速跑通整个流程（L4 -> Nginx -> 真实 IP 显示）。这在 DPDK 里就是构建一个 `rte_mbuf`，填上字符串，链在真实数据包前面发出去。
2. **进阶挑战 TOA**：如果你想挑战腾讯 TGW 的底层黑科技，再去尝试修改 TCP Options。这需要你对 `rte_tcp_hdr` 结构体和 TCP 校验和算法（Checksum Offload）有非常深的理解。

> **最终的选择**：本项目实现的是 TOA（`src/forward/nat_forwarder.cpp` 的 `insert_toa()`），没有实现 Proxy Protocol。原因是 Proxy Protocol 要求 LB 往 TCP 流里插入数据，之后整条连接的序列号都要做偏移，LB 就不再是纯粹的按包改写，复杂度高得多。TOA 只改一个包的 TCP 选项，序列号不变。

所以，如果你的目标是“应用到实际场景”，**Proxy Protocol 是目前最通用的答案**。如果您想试试怎么写，我可以给你展示一段 Proxy Protocol 的报文示例。

但这和你想象的“完全断开的代理（如 Nginx 反向代理）”有一点微妙的区别。为了让你彻底理解，我们对比一下 **普通 FullNAT** 和 **带 Proxy Protocol 的 FullNAT**。

### 1. 普通 FullNAT（你现在的做法）

你目前的 L4 只是一个“修改者”。

- **流程**：客户端发来 `SYN` -> 你修改 IP/Port -> 转发给 Nginx。
- **实质**：你不需要自己构造新的数据包，你只是在**透传**并修改已有的包。
- **连接状态**：虽然 Nginx 觉得是和你建连，但实际上你只是个“中间人”，你不需要自己产生 TCP 序列号（Sequence Number），你沿用客户端的序列号（或者做个偏移）。

### 2. 带 Proxy Protocol 的 FullNAT（你要做的）

这就是区别所在了。因为 Proxy Protocol 要求**在 TCP 连接建立后，任何应用数据发送前**，先发送一段协议头字符串（如 `PROXY TCP4 ...`）。

**关键点来了：这段字符串，客户端是不会发的！**

所以，你的 L4 不能再只做“透传”了，你必须**凭空制造**一个数据包发给 Nginx。

#### 具体的“作案”流程是这样的：

1. **三次握手阶段（Handshake）**：
    - Client 发 `SYN` 给 你。
    - 你（L4）修改后发 `SYN` 给 Nginx。
    - Nginx 回 `SYN+ACK` 给你。
    - 你修改后回 `SYN+ACK` 给 Client。
    - Client 发 `ACK` 给你。 **<-- 握手完成**
2. **注入阶段（Injection）—— 也就是你疑惑的地方**：
    - 收到 Client 的 `ACK` 后，你的 L4 **不能直接转发** 这个 `ACK` 给 Nginx 就完事了。
    - **动作 A**：你转发修改后的 `ACK` 给 Nginx（完成后端握手）。
    - **动作 B（关键）**：紧接着，你的代码必须**自己构造一个 TCP PUSH 数据包**，里面包含 `PROXY TCP4 1.1.1.1 2.2.2.2 ...` 这串字符，发送给 Nginx。
3. **数据传输阶段**：
    - Client 发送真正的 HTTP 请求（`GET /...`）。
    - 你（L4）收到后，修改 IP/Port，转发给 Nginx。
    - **注意**：因为你插队发送了一个 Proxy Protocol 包，你必须维护**序列号（Sequence Number）的差值（Delta）**。
        - Nginx 认为它收到了 X 字节的数据（Proxy Protocol 字符串长度）。
        - Client 认为它还没发数据。
        - 以后所有交互，你都要把 Client 发来的序列号 + X，才能对得上 Nginx 的账。

### 总结你的疑惑

> “是不是就要nginx和我的L4建立TCP连接了呀？”

**是的。** 在 Proxy Protocol 模式下，你的 L4 性质发生了一点变化：

1. **以前（纯 FullNAT）**：你更像是一个**路由器**（尽管改了 IP），你只转发，不生产数据。
2. **现在（Proxy Protocol）**：你变成了一个**数据生产者**。对于 Nginx 来说，TCP 连接的前几毫秒，就是在和**你（L4 代码）**对话，接收你发给它的元数据。等你发完了，你才退回“路由器”的角色，继续转发客户端的数据。





## nginx的深刻理解

这是一个非常棒的切入点。既然你已经亲手实现了 L4（基于 DPDK 的四层负载均衡），那么理解 Nginx（七层负载均衡/反向代理）对你来说会非常直观，因为你只需要关注**“网络包被拆开后发生了什么”**。

结合你的 L4 项目背景，我将从**架构定位**、**核心能力**以及**底层原理**三个层面，带你深刻理解 Nginx 在现代互联网中的统治地位。

------

### 1. 架构定位：L4 是“搬运工”，Nginx 是“分拣员”

在你的 L4 项目中，你处理的是 **TCP/IP 包**。你关心的是 IP 地址、端口号、SEQ/ACK 号。你并不关心包里装的是图片、HTML 还是 JSON 数据，因为你看不见（也不想看，拆包太耗性能）。

**Nginx 的位置就在你的 L4 之后。** 当 TCP 连接在 Nginx 处终结（Termination）后，Nginx 把数据包里的“信”取出来，这封信就是 **HTTP 请求**。

- **你的 L4 (VIP)**：只负责把流量“粗暴”且“极速”地分发给后端的 Nginx 集群。
- **Nginx**：负责把这些流量“精细”地处理，再分发给具体的业务服务器（如 Java/Go/Python）。

**没有 Nginx 会怎样？**

如果只有你的 L4，客户端请求 `http://2.2.2.2/video` 和 `http://2.2.2.2/payment`，你的 L4 只能根据 IP 哈希把它们发到同一台后端服务器。但现实中，我们希望 `/video` 流量去庞大的存储服务器集群，而 `/payment` 流量去高安全性的交易服务器集群。**L4 做不到这一点，必须靠 Nginx。**

------

### 2. Nginx 的三大核心作用（结合 L4 视角）

#### A. 七层路由（L7 Routing）—— "看懂内容再分发"

这是 Nginx 最核心的能力。因为它解析了 HTTP 协议，它能看到 URL、Cookie、Header。

- **动静分离**：Nginx 看到 `.jpg`、`.css` 结尾的请求，直接从自己硬盘读文件返回（速度极快）；看到 `/api/` 开头的请求，才转发给后端的 Tomcat/Go 服务。
- **基于域名的虚拟主机**：你的 L4 VIP 是 `2.2.2.2`。但我可以将 `a.com` 和 `b.com` 都解析到这个 VIP。Nginx 收到请求后，看 `Host` 字段是 `a.com` 还是 `b.com`，从而分发给不同的业务部门。

#### B. 卸载（Offloading）—— "替后端干脏活累活"

后端业务服务器（RS）通常运行着复杂的业务逻辑（Java/Python），CPU 资源宝贵。Nginx 也就是所谓的“反向代理”，挡在前面把脏活累活干了：

- **SSL/TLS 卸载**：HTTPS 的加解密非常消耗 CPU。通常策略是：**Client <-> (HTTPS) <-> Nginx <-> (HTTP) <-> 业务服务器**。Nginx 解密后，通过内网明文传给业务，大大减轻业务服务器负担。
- **GZIP 压缩**：Nginx 把网页压缩后再发给客户端，节省带宽。
- **Keep-Alive 维持**：Client 和 Nginx 保持长连接，而 Nginx 和后端使用短连接或连接池，减少后端连接数压力。

#### C. 高级负载均衡与容错

你的 L4 通常使用 Round Robin（轮询）或 Source Hash。Nginx 能做得更细：

- **Consistent Hash（一致性哈希）**：基于 URL 哈希，保证同一个文件的请求永远打到同一台缓存服务器。
- **Fair 算法**：谁响应快，就发给谁。
- **被动健康检查**：如果 Nginx 发现转发给某台 RS 的请求超时了，它不仅会标记那台 RS 为“故障”，还会自动把刚才失败的请求**重试**转发给另一台健康的 RS。这对于用户来说是无感知的，这是 L4 很难做到的（L4 没法重发 TCP 流中的某一段）。

------

### 3. 为什么 Nginx 这么快？（深刻理解底层）

你可能会问：*“我的 L4 用 DPDK 也就是为了快，Nginx 既然要解析 HTTP 这种复杂的文本协议，为什么还能支撑数万并发？”*

这就要说到 Nginx 的架构设计的精髓了，这和你的 DPDK 有异曲同工之妙，但处于不同层面。

- **传统服务器（如 Apache）**：这也是早期的模式。来一个连接开一个线程（Thread）。如果有 10,000 个连接，就要开 10,000 个线程。CPU 会在线程切换（Context Switch）中崩溃，内存也会被耗光。
- **Nginx（事件驱动 + 异步非阻塞）**：
    - Nginx 通常只有 **CPU 核数** 那么多个 Worker 进程（比如 8 核就 8 个进程）。
    - 它使用了 Linux 内核的 **epoll** 技术（IO 多路复用）。
    - **原理**：一个 Worker 进程就像一个极其高效的服务员。它不傻等厨师（后端）做菜。它接单后扔给后端，立刻去接下一个客人的单。一旦有菜做好了（epoll 通知），它再回来端菜。
    - **结果**：**一个进程就能同时维护成千上万个连接**。这就解决了著名的 **C10K 问题**（单机 1 万并发）。

------

### 4. 总结：你的 L4 和 Nginx 的配合关系

在大型互联网架构（如淘宝、腾讯、抖音）中，流量的路径是这样的：

1. **海量流量入口**：数据包到达机房核心交换机。
2. **L4 层（你的项目/LVS/TGW）**：
    - **任务**：抗住数以亿计的 PPS（包/秒），清洗 DDoS 攻击。
    - **动作**：基于 5 元组，利用 DR 或 NAT 模式，将流量分发给后面 100 台 Nginx。
    - **关键**：稳、快、无状态。
3. **L7 层（Nginx 集群）**：
    - **任务**：处理业务路由、SSL 卸载、缓存。
    - **动作**：解析 HTTP，基于 URL 分发给后面 1000 台业务服务器。
    - **关键**：灵活、智能、懂业务。
4. **应用层（Business Logic）**：
    - **任务**：查数据库、算逻辑、生成 JSON。

**一句话总结：**

你的 L4 项目是**“高速公路的收费站”**，只管快速放行车辆（TCP 包）； Nginx 是**“物流中心的分拣流水线”**，要拆开包裹看地址（URL），把包裹重新打包（SSL/GZIP），最后送到具体的收件人（业务服务器）手里。





## DR模式下数据流向

见 `docs/架构图.md` 第五节和 `docs/压测方案.md` 第十节：LB 只把目的 MAC 改成 RS 的 MAC，RS 的 lo 上有 VIP 所以会接收，回包源地址是 VIP，直接发给客户端。

## DPDK的常用的数据结构

本项目用到的：`rte_mbuf`（报文）、`rte_mempool`（mbuf 池）、`rte_ring`（线程间传递 mbuf 和事件）、`rte_hash`（会话表、邻居表）、`rte_rcu_qsbr`（快照和邻居表的无锁读）。

## DPDK常用的API函数

- 创建内存池：
- 这是 DPDK 的 rte_pktmbuf_pool_create，作用是**创建 mbuf 内存池**，供收包/发包分配 mbuf 使用。参数解释如下（对应你这行）：
    - "MBUF_POOL"：内存池名字
    - NUM_MBUFS：池里 mbuf 总数（越大越抗高并发）
    - MBUF_CACHE_SIZE：每个 lcore 的本地缓存数量，减少全局锁竞争
    - 0：私有数据大小（每个 mbuf 额外私有区长度），这里为 0
    - RTE_MBUF_DEFAULT_BUF_SIZE：单个 mbuf 的数据区大小（默认可容纳一个常见以太网包）
    - rte_socket_id()：NUMA socket id，尽量让内存和 CPU 同 NUMA 结点

```c
  g_mbuf_pool =
      rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
                              RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
```

（这是早期代码。现在 `src/main.cpp` 按队列描述符数、ring 大小和每核缓存计算 mbuf 数量，并分配在网卡所在的 NUMA 节点：`rte_pktmbuf_pool_create("MBUF_POOL", nb_mbufs, MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, g_dp.socket_id)`。）

