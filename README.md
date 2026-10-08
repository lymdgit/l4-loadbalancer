# L4 Load Balancer

基于 **DPDK** 的四层负载均衡器，支持 FULLNAT 和 DR 两种转发模式。

## 一、特性

- **纯数据面转发**：不终止 TCP 连接，按包改写转发；Run-to-Completion，每个 lcore 独占一组 RX/TX 队列
- **无锁多核**：会话表 per-worker（rte_hash + 预分配数组 + 时间轮），热路径无锁、无原子 RMW、无 malloc。FULLNAT 回程包通过"按 RSS 反算选择 SNAT 端口"回到创建会话的核，不依赖 rte_flow（vmxnet3 可用）；网卡无 RSS 时自动退化为软件分发 + ring 转交
- **FULLNAT**：Local IP 池、TCP 状态机（各状态独立超时）、TOA 透传客户端地址、去除 SYN timestamp、ICMP 差错报文转换
- **调度**：加权轮询（平滑 WRR）、Maglev 一致性哈希，按服务配置
- **邻居与路由**：ARP 主动解析与老化、免费 ARP、直连/网关路由，下一跳 MAC 缓存在会话中
- **健康检查**：数据面内 TCP 探测，RS 故障自动摘除、恢复后加回
- **控制面**：unix socket 命令，运行时调整权重、排空、增删 RS；配置以 RCU 快照发布，数据面无锁读取

## 二、目录

```text
CMakeLists.txt
config/            lb.conf（FULLNAT）、lb_dr.conf（DR）
scripts/           setup_dpdk_env.sh（大页/驱动/网卡接管）、l4lbctl.py（控制命令）
include/ src/      一一对应：
  common/          配置、日志、统计、公共类型
  protocol/        协议头、解析、校验和、ARP/ICMP 报文构造
  lb/              会话表、TCP 状态机、调度器（WRR/Maglev）
  forward/         FULLNAT/DR 报文改写、TOA、ICMP 差错转换
  net/             邻居表、路由
  ctrl/            配置快照、健康检查、控制命令
  dataplane/       端口初始化、多核分发（steering）、worker 循环
  core/            报文分类与处理主流程
tests/unit/        单元测试（ctest）
tests/functional/  功能测试（veth + net_af_packet，无需真实网卡）
tests/perf/        性能测试脚本
docs/              设计文档、学习笔记
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

### DR 模式的 RS 配置

DR 只改写目的 MAC，RS 直接回包，RS 上需要：

```bash
sudo ip addr add <VIP>/32 dev lo
sudo sysctl -w net.ipv4.conf.all.arp_ignore=1 net.ipv4.conf.all.arp_announce=2
sudo sysctl -w net.ipv4.conf.lo.arp_ignore=1 net.ipv4.conf.lo.arp_announce=2
```

### FULLNAT 获取客户端真实 IP

配置 `toa = on`，并在 RS 上加载 TOA 内核模块（例如 DPVS 的 `kmod/toa`）。没有加载模块时，RS 会忽略这个 TCP 选项。

### 控制命令

```bash
scripts/l4lbctl.py stats            # 计数器（-v 显示全部和每个 worker）
scripts/l4lbctl.py services         # 服务、RS 及状态
scripts/l4lbctl.py weight 2 0       # RS 2 排空：不接新连接，已有连接保持
scripts/l4lbctl.py disable 2        # RS 2 下线：已有连接也断开
scripts/l4lbctl.py add 0 192.168.154.134:80:50
scripts/l4lbctl.py del 2
```

### 日志

默认写入 `/data/logs/l4/l4lb.log`（`[global] log_dir`，`--log-dir` 覆盖，留空只输出终端），DPDK 自身日志以 `[dpdk]` 标记写入同一文件。超过 `log_max_size_mb` 后轮转为 `l4lb.log.1 ~ .N`；`kill -HUP` 重新打开文件，可配合 logrotate。

```bash
tail -f /data/logs/l4/l4lb.log
grep 'Sessions:' /data/logs/l4/l4lb.log   # 每 10 秒一次的统计
```

## 五、测试

```bash
ctest --test-dir build                                    # 单元测试
sudo python3 tests/functional/run_tests.py build/l4lb     # 功能测试（需要 root 和大页）
tests/perf/run_bench.sh http://<VIP>/ <标签>               # 性能测试（在客户端机器上）
```

功能测试在 veth 上用 `net_af_packet` 运行 l4lb，覆盖 FULLNAT/DR 转发、畸形包、会话回收、TCP 状态机、LIP、TOA、ICMP 差错、ARP/网关、健康检查、控制命令、多核。

## 六、历史性能数据（重构前，仅供参考）

| 测试场景 | QPS | 平均延迟 |
| :--- | :---: | :---: |
| 直连 RS | 131,716 | 12.34 ms |
| DR | 120,051 | 12.54 ms |
| FULLNAT | 108,635 | 13.05 ms |

`wrk -t4 -c2000 -d30s`，长连接。这组数据的瓶颈在 RS 和虚拟机，并不反映 LB 本身的能力，测试方法见 `tests/perf/README.md`。
