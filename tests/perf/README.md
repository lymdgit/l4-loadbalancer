# 性能测试

对应《项目重构.md》第四部分。LB、RS、压测客户端要放在不同的机器（至少不同的 VM）上，否则测到的是资源竞争。完整的压测步骤（拓扑、nginx 配置、DR、结果解读）见 `docs/压测方案.md`，最新结果见根目录 README 第六节。

| 脚本 | 在哪台机器运行 | 作用 |
| :--- | :--- | :--- |
| `tune.sh <client\|rs\|lb> apply\|show\|revert` | 三台都运行 | 文件句柄、端口范围、TIME_WAIT、conntrack 等调优，可还原 |
| `run_bench.sh <url> <标签>` | client | wrk / wrk2 的整套测试，设置 `LB_HOST` 时同时记录 LB 的 PPS 和忙碌率 |
| `dr_rs.sh up\|down\|status` | RS | DR 模式下 lo 绑 VIP、关闭 VIP 的 ARP 响应，可还原 |
| `nic_delta.sh [秒数]` | 任意（DR 时在 RS） | 读 `/proc/net/dev` 算网卡 pps / bps，DR 下用来统计回程 |
| `lab.sh` | LB 本机 | 单机实验（第 3 节），只用于功能验证 |
| `memif_bench.sh` | LB 本机 | 单核转发能力：l4lb + 发包器 `tools/memif_gen.c` 通过 net_memif 直连，逐档测 PPS 和忙碌率 |

## 1. HTTP 压测（wrk / wrk2）

在客户端机器上：

```bash
# 基线：直连 RS
THREADS=4 CONNS="100 400 1000" tests/perf/run_bench.sh http://192.168.154.140/ direct
# 经过 LB（LB 用 config/bench.conf 或 bench_dr.conf 启动）
THREADS=4 CONNS="100 400 1000" LB_HOST=root@192.168.154.142 \
  tests/perf/run_bench.sh http://192.168.154.130/ fullnat-pipe-1r2w
```

覆盖：长连接 QPS（不同并发）、短连接 CPS（`Connection: close`）、固定速率尾延迟（wrk2）。每项默认重复 3 次，`results/<时间>-<标签>/mean.csv` 是均值（含 `avg_lb_pps`、`avg_lb_bps`），`env.txt` 记录环境，`lb_delta_*.txt` 是每项的 LB 计数差值报告。

手动用 wrk 时，在 LB 上前后各执行一次计数：

```bash
scripts/l4lbctl.py counters > /tmp/before.txt     # 压测前
scripts/l4lbctl.py delta /tmp/before.txt          # 压测后：PPS、bps、各线程忙碌率、各 RS 分布
```

短连接测试是验证会话回收的关键：持续 30 秒、每秒数万新建连接时，累计会话数远超会话表容量。压测后在 LB 上执行 `l4lbctl.py stats`，`drop_table_full` 和 `drop_no_port` 应为 0。

## 2. 小包转发能力（TRex / pktgen）

wrk 测的是整条链路（客户端 + LB + RS 协议栈），RS 往往先成为瓶颈。测 LB 本身的 Mpps 需要发包工具，例如 TRex：

- 64B / 512B / 1518B TCP SYN，源 IP/端口随机，目的 VIP:80
- 逐步提高速率，记录 LB `rx` 与 `fwd_in` 开始出现差距（`tx_full` / 网卡 `imissed` 增长）时的速率
- 分别用 1 / 2 / 4 / N 个 lcore 测，检查是否近似线性扩展
- 本机已有方案：`sudo tests/perf/memif_bench.sh`（`WORKERS=2` 测 2 个转发核），用 DPDK `net_memif` 共享内存直连，不需要外部发包机，结果见 `docs/压测方案.md` 第十一节

## 3. 单机实验（本机同时跑 RS、LB、客户端）

```bash
sudo tests/perf/lab.sh rs-start   # 两个 RS：192.168.154.142:8081 / 8082
sudo tests/perf/lab.sh lb         # 另一个终端，前台运行 LB
sudo tests/perf/lab.sh curl       # 看请求被分到 rs=a / rs=b
sudo tests/perf/lab.sh bench      # 长连接 QPS + 短连接 CPS
sudo tests/perf/lab.sh rs-stop
```

工具：`tools/rs_server.c`（响应 body 带 RS 名字）、`tools/loadgen.c`（wrk 不可用时的压测工具）。

### vmxnet3 的 RSS

VMware Workstation 默认不在 vmxnet3 的多个队列间做 RSS 分发，所有包都进队列 0。
rtc 模式下 `stats` 中 `rss no hash` 持续增长、`redirect out` 约为收包数的 (N-1)/N，
转发仍然正确（由收包的核转交给会话所在的核），但多核扩展失效。
这种情况用 pipeline 模式（`dataplane = pipeline`，见 `docs/pipeline改造.md`）。
想让 RSS 生效，可以关机后在虚拟机的 .vmx 中为 DPDK 使用的网卡加上（X 为网卡序号）：

```
ethernetX.pnicFeatures = "4"
ethernetX.udpRSS = "1"
```

## 4. LB 内部观察

```bash
scripts/l4lbctl.py rate        # 最近一个周期：pps、bps、各线程忙碌率、网卡丢包
scripts/l4lbctl.py stats -v    # 累计计数，每个线程一行
scripts/l4lbctl.py stats -r    # 按服务 / RS 的连接、包、字节
```

DPDK 是忙轮询，top 看到的 CPU 永远是 100%。判断 LB 负载要看 `rate` / `delta` 中的忙碌率：只统计处理到包的轮次花掉的 TSC 周期。命令的完整说明见 `docs/控制命令.md`。

需要关注：

| 计数 | 含义 |
| :--- | :--- |
| `Busy:` 忙碌率 | 接近 100% 说明这个线程到顶了；远低于 100% 而 QPS 上不去，说明瓶颈在 LB 之外 |
| `imissed` | 网卡 RX 队列满丢弃，收包跟不上 |
| `rx_ring_full` | pipeline 下转发核的 ring 满，转发核处理不过来 |
| `redirect_out` | 收包的核不是会话 owner、转交给其他核的包数。pipeline 下和硬件 RSS 生效时（`steering: hw-rss`）应接近 0 |
| `rss_mismatch` | 网卡 RSS hash 与软件计算不一致的 SYN 数，应为 0 |
| `rss_no_hash` | 硬件 RSS 模式下网卡没有给出 hash 的 SYN 数，应为 0；不为 0 说明 RSS 实际没有生效 |
| `drop_*` | 各类丢包原因 |
| `tx_full` | TX 队列满，说明发送跟不上 |

配合 `perf top` / 火焰图定位热点（CMake 默认 `RelWithDebInfo` 并保留帧指针）。

## 5. 记录规范

每次测试记录：CPU 型号和频率、核数、NUMA、网卡型号和驱动、DPDK 版本、l4lb 的 git commit 和编译参数、vCPU 是否绑核、宿主机负载。每个重构阶段结束后用同一套脚本回归，结果存档对比。
