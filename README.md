# L4 Load Balancer

基于 **DPDK** 的四层负载均衡器，支持 FULLNAT 和 DR 两种转发模式。

## 一、特性

- **纯数据面转发**：不终止 TCP 连接，按包改写转发
- **两种数据面模式**（`dataplane`）：
  - `pipeline`：1 个收包核按会话 owner 分发到 N 个转发核，适合不支持 RSS 的网卡（如 VMware vmxnet3），见 `docs/pipeline改造.md`
  - `rtc`：Run-to-Completion，每个核独占 RX/TX 队列，网卡 RSS 分流
- **无锁多核**：会话表 per-worker（rte_hash + 预分配数组 + 时间轮），热路径无锁、无原子 RMW、无 malloc。FULLNAT 回程按 SNAT 端口回到创建会话的核（RSS 时按 Toeplitz 反算选端口，不依赖 rte_flow）。ARP、健康检查、统计在独立的 master 线程，所有转发核代码相同
- **FULLNAT**：Local IP 池、TCP 状态机（各状态独立超时）、TOA 透传客户端地址、去除 SYN timestamp、ICMP 差错报文转换
- **调度**：加权轮询（平滑 WRR）、Maglev 一致性哈希，按服务配置
- **邻居与路由**：ARP 主动解析与老化、免费 ARP、直连/网关路由，下一跳 MAC 缓存在会话中
- **健康检查**：数据面内 TCP 探测，RS 故障自动摘除、恢复后加回
- **控制面**：unix socket 命令，运行时调整权重、排空、增删 RS；配置以 RCU 快照发布，数据面无锁读取
- **可观测性**：异步落盘日志（不阻塞转发核，按大小轮转）；按核忙碌率（只计处理包的 TSC 周期）、网卡丢包、按 RS 的包/字节计数；`delta` 计算压测区间的 PPS / bps

## 二、目录

```text
CMakeLists.txt
config/            lb.conf（FULLNAT）、lb_dr.conf（DR）、bench.conf / bench_dr.conf（压测）
scripts/           setup_dpdk_env.sh（大页/驱动/网卡接管）、l4lbctl.py（控制命令）
include/ src/      一一对应：
  common/          配置、日志、统计、公共类型
  protocol/        协议头、解析、校验和、ARP/ICMP 报文构造
  lb/              会话表、TCP 状态机、调度器（WRR/Maglev）
  forward/         FULLNAT/DR 报文改写、TOA、ICMP 差错转换
  net/             邻居表、路由
  ctrl/            配置快照、健康检查、控制命令
  dataplane/       端口初始化、多核分发（steering）、receiver / worker / master 线程
  core/            报文分类与处理主流程
tests/unit/        单元测试（ctest）
tests/functional/  功能测试（veth + net_af_packet，无需真实网卡）
tests/perf/        压测脚本：run_bench.sh（wrk）、tune.sh（系统调优）、dr_rs.sh（DR 的 RS 配置）
docs/              设计文档：pipeline改造、控制命令、压测方案、pps方案、日志改造方案等
项目重构.md         重构计划与完成情况
```

## 三、环境准备

### 1. 编译 DPDK

```bash
cd dpdk/
meson setup build && ninja -C build && ninja -C build install
```

### 2. 大页、驱动、网卡接管（每次重启后执行）

```bash
# 查看网卡和当前状态
sudo scripts/setup_dpdk_env.sh status
# 配置大页 + 加载驱动 + 把网卡交给 DPDK
sudo DPDK_NIC=ens160 scripts/setup_dpdk_env.sh up
# 还给内核
sudo DPDK_NIC=ens160 scripts/setup_dpdk_env.sh down
# 开机自动执行（参数写入 /etc/l4lb/dpdk-env.conf）
sudo DPDK_NIC=ens160 scripts/setup_dpdk_env.sh install
```

- 驱动默认 `auto`：有 IOMMU 用 `vfio-pci`，没有（虚拟机常见）用 `uio_pci_generic`。也可以 `DPDK_DRIVER=igb_uio IGB_UIO_KO=/path/igb_uio.ko`
- 无 IOMMU 时强行用 vfio-pci 需要 `ALLOW_NOIOMMU=1`（会关闭 DMA 隔离，只适合测试机）
- 脚本拒绝接管带 IP 地址、承载默认路由或当前 SSH 会话的网卡，避免管理口失联

### 3. 编译

```bash
PKG_CONFIG_PATH=<dpdk-install>/lib64/pkgconfig cmake -S . -B build
cmake --build build -j
ctest --test-dir build          # 单元测试（cmake >= 3.20；旧版本 cd build && ctest）
```

CMake 选项：`-DL4LB_NATIVE=OFF`（不用 -march=native）、`-DL4LB_SANITIZE=ON`（ASan + UBSan）、`-DBUILD_TESTS=OFF`。

## 四、运行

```bash
# 先检查配置
./build/l4lb -- --lb-config config/lb.conf --check-config
# pipeline（配置 dataplane = pipeline）：lcore 1 收包分发，lcore 2、3 两个 worker
sudo ./build/l4lb -l 1-3 -- --lb-config config/lb.conf
# rtc（dataplane = rtc，网卡有 RSS 时）：4 个 worker，网卡 TX 队列数必须 >= lcore 数 + 1
sudo ./build/l4lb -l 1-4 -- --lb-config config/lb.conf
```

配置格式见 `config/lb.conf` 中的注释；旧格式（`[vip]` + `[realserver]`）仍然兼容。

pipeline 模式需要至少 2 个 lcore；两种模式下网卡 TX 队列数都要不少于 worker 数 + 1（最后一个给 master 线程）。

### DR 模式的 RS 配置

DR 只改写目的 MAC，RS 直接回包。RS 的 lo 上要配 VIP，并且不能应答 VIP 的 ARP（先改 sysctl，再加 VIP）：

```bash
sudo tests/perf/dr_rs.sh up       # 还原：dr_rs.sh down
```

等价于：

```bash
sudo sysctl -w net.ipv4.conf.all.arp_ignore=1 net.ipv4.conf.all.arp_announce=2
sudo sysctl -w net.ipv4.conf.lo.arp_ignore=1 net.ipv4.conf.lo.arp_announce=2
sudo ip addr add <VIP>/32 dev lo
```

DR 不改端口，RS 端口必须与 VIP 端口相同。

### FULLNAT 获取客户端真实 IP

配置 `toa = on`，并在 RS 上加载 TOA 内核模块（例如 DPVS 的 `kmod/toa`）。没有加载模块时，RS 会忽略这个 TCP 选项。

### 控制命令

运行时增删后端、调整权重、查询统计，完整说明见 `docs/控制命令.md`。

```bash
scripts/l4lbctl.py services         # 服务、RS 及状态
scripts/l4lbctl.py add 0 192.168.154.140:80:1   # 给服务 0 添加后端
scripts/l4lbctl.py weight 2 0       # RS 2 排空：不接新连接，已有连接保持
scripts/l4lbctl.py disable 2        # RS 2 下线：已有连接也断开
scripts/l4lbctl.py del 2
scripts/l4lbctl.py stats            # 汇总计数（-v 按线程，-r 按服务 / RS）
scripts/l4lbctl.py rate             # 最近一个周期的速率和各线程忙碌率
scripts/l4lbctl.py counters > before.txt; ...; scripts/l4lbctl.py delta before.txt   # 区间 PPS / bps
```

### 日志

默认写入 `/data/logs/l4/l4lb.log`（`[global] log_dir`，`--log-dir` 覆盖，留空只输出终端），DPDK 自身日志以 `[dpdk]` 标记写入同一文件。超过 `log_max_size_mb` 后轮转为 `l4lb.log.1 ~ .N`；`kill -HUP` 重新打开文件，可配合 logrotate。

```bash
tail -f /data/logs/l4/l4lb.log
grep -E 'Rate:|Busy:|NIC:' /data/logs/l4/l4lb.log   # 每 stats_interval 秒的速率、忙碌率、网卡丢包
```

## 五、测试

```bash
ctest --test-dir build                                    # 单元测试
sudo python3 tests/functional/run_tests.py build/l4lb     # 功能测试（需要 root 和大页）
sudo L4T_DATAPLANE=pipeline python3 tests/functional/run_tests.py build/l4lb   # 用 pipeline 模式跑全部功能测试
tests/perf/run_bench.sh http://<VIP>/ <标签>               # 性能测试（在客户端机器上）
```

功能测试在 veth 上用 `net_af_packet` 运行 l4lb，覆盖 FULLNAT/DR 转发、畸形包、会话回收、TCP 状态机、LIP、TOA、ICMP 差错、ARP/网关、健康检查、控制命令、多核、pipeline 分发、包/字节计数、日志文件。压测步骤见 `docs/压测方案.md`。

## 六、性能数据

### 环境

| | |
| :--- | :--- |
| 宿主机 | i7-13650HX 笔记本，VMware Workstation，所有 VM 在同一个 vmnet |
| LB | vmxnet3（DPDK 24.11，uio_pci_generic），**pipeline：1 个收包核 + 2 个转发核**（`-l 1-3`） |
| 后端 | 1 台 nginx VM，80 / 8080 两个端口模拟两个 RS（DR 下只用 80） |
| 客户端 | 4 vCPU VM，`wrk -t4 -c1000 -d60s`，HTTP 长连接 |

vmxnet3 在 VMware Workstation 下不做 RSS（所有包进同一个队列），所以使用 pipeline 模式。

### 结果（2026-10-09）

| 场景 | QPS | p50 | p99 | LB 转发 PPS | 收包核忙碌率 | 转发核忙碌率 | 丢包 |
| :--- | ---: | ---: | ---: | ---: | ---: | ---: | :---: |
| 直连 nginx（基线） | 161,674 | 5.18 ms | 89.50 ms | – | – | – | – |
| FULLNAT | 126,145（78%） | 6.95 ms | 48.88 ms | 223.9k（双向） | 26.5% | 67.3% / 67.4% | 1 |
| DR | 133,090（82%） | 6.72 ms | 36.74 ms | 121.1k（只有入向） | 14.7% | 32.9% / 33.3% | 0 |

- PPS 和忙碌率来自压测前后 `l4lbctl.py counters` / `delta` 的差值；丢包指 LB 侧的 `imissed`、`rx_ring_full`、`tx_full` 和软件丢包之和。FULLNAT 的 1 个是软件丢包（1500 万个包中的 1 个，大概率是 wrk 结束时连接收尾的包），网卡和 ring 都没有丢包。
- FULLNAT 每个请求的去程和回程都经过 LB，PPS ≈ 2 × QPS；DR 的回包由 RS 直接发给客户端，LB 只处理入向，PPS ≈ QPS。两种模式的 PPS 口径不同，不能直接比较大小；可比较的是同等流量下的 LB 负载：DR 比 FULLNAT 低约一半。
- 瓶颈不在 LB：转发核负载 67%、网卡无丢包，而直连 nginx 本身只有 16 万 QPS。FULLNAT 比直连每个请求多经过两次虚拟交换机，DR 多一次，这部分开销由宿主机 CPU 承担，是 QPS 差距的主要来源。
- 虚拟化环境下每包耗时偏高（转发核约 6 µs/包），推测主要来自 vmxnet3 的虚拟化开销（如 TX doorbell 引起的 VM exit）和小批量收包，尚未用 perf 确认。数据只代表这个环境，不能与物理网卡上的 Mpps 级结果对比。

复现：`docs/压测方案.md`（拓扑、nginx 配置、`tests/perf/tune.sh` 调优、DR 步骤）。

<details>
<summary>重构前的数据（旧架构，仅供参考）</summary>

| 测试场景 | QPS | 平均延迟 |
| :--- | :---: | :---: |
| 直连 RS | 131,716 | 12.34 ms |
| DR | 120,051 | 12.54 ms |
| FULLNAT | 108,635 | 13.05 ms |

`wrk -t4 -c2000 -d30s`，长连接，环境与上表不同。

</details>
