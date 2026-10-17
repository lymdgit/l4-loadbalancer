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
build.sh           一键编译（./build.sh help）
config/            lb.conf（FULLNAT）、lb_dr.conf（DR）
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
tests/perf/        压测：memif_bench.sh + tools/memif_gen.c（net_memif 单机压测）
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
./build.sh              # 编译，产物 build/l4lb
./build.sh test         # 编译 + 单元测试
./build.sh clean        # 清空 build/ 后重新编译
./build.sh asan         # ASan + UBSan，输出到 build-asan/
DPDK_PREFIX=<dpdk-install> ./build.sh   # DPDK 不在默认位置时
```

`build.sh` 会自动查找 `libdpdk.pc`（已有的 `PKG_CONFIG_PATH`、本机 DPDK 安装目录、`/usr/local`、`/opt/dpdk`），额外的 cmake 参数通过 `CMAKE_ARGS` 传入。手动编译：

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
sudo sysctl -w net.ipv4.conf.all.arp_ignore=1 net.ipv4.conf.all.arp_announce=2
sudo sysctl -w net.ipv4.conf.lo.arp_ignore=1 net.ipv4.conf.lo.arp_announce=2
sudo ip addr add <VIP>/32 dev lo
# 还原：sysctl -w ...=0，ip addr del <VIP>/32 dev lo
```

这些是运行时设置，RS 重启后自动失效。

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
sudo WORKERS=1 tests/perf/memif_bench.sh                   # 性能测试（memif，LB 本机）
```

功能测试在 veth 上用 `net_af_packet` 运行 l4lb，覆盖 FULLNAT/DR 转发、畸形包、会话回收、TCP 状态机、LIP、TOA、ICMP 差错、ARP/网关、健康检查、控制命令、多核、pipeline 分发、包/字节计数、日志文件。性能测试用 DPDK `net_memif` 给 l4lb 直接喂包，步骤和结果解读见 `docs/压测方案.md`、`tests/perf/README.md`。

## 六、性能数据

### 环境

| | |
| :--- | :--- |
| 宿主机 | i7-13650HX 笔记本，VMware Workstation，12 vCPU VM |
| 被测 | l4lb，**pipeline：1 个收包核 + N 个转发核**（DPDK 24.11） |
| 流量 | DPDK `net_memif` 共享内存直连，64B UDP（+FCS），10000 条会话，每包走完整 FULLNAT 入站 + 回程 |

用 memif 而不是真实网卡，是因为 vmxnet3 和宿主机的虚拟交换机在几十万 pps 就到顶了，压不满 LB；memif 绕开这两层，测到的才是 l4lb 代码本身的处理能力。

### 结果（2026-10-09）

| 转发核 | LB 转发 PPS（丢包率 < 0.1%） | 饱和时 |
| :--- | ---: | :--- |
| 1 | **400 万** | 约 670 万，转发核 94% |
| 2 | **600 万** | 尽力发送 950 万（丢包 0.8%），转发核 81%，受限于发包器 |

- 单核每包约 150 ns，热点是会话表查找（`rte_hash_lookup_data` 约 53%）；2 核时转发核还没满，瓶颈在发包器（单核约 480 万/秒）。
- 判据是"丢包率低于 0.1% 时的最大转发速率"，同时要求 `发包数 ≥ 入站转发 ≥ 回程转发 = 往返成功` 互相对得上。
- 这组数据**不含真实网卡的收发开销**，不能与物理网卡上的端到端 Mpps 或 DPVS、Katran 的公开数据直接对比。

复现：`sudo WORKERS=2 tests/perf/memif_bench.sh`。完整方案见 `docs/压测方案.md`，排查过程见 `docs/PPS测试与性能排查.md`。

