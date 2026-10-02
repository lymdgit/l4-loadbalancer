# 性能测试

对应《项目重构.md》第四部分。LB、RS、压测客户端尽量放在不同的物理机（或至少不同的宿主机）上，否则测到的是宿主机资源竞争。

## 1. HTTP 压测（wrk / wrk2）

在客户端机器上：

```bash
# 基线：直连 RS
tests/perf/run_bench.sh http://192.168.154.133/ direct
# 经过 LB（分别用 FULLNAT / DR 配置启动 l4lb 后各跑一次）
LB_HOST=root@192.168.154.142 tests/perf/run_bench.sh http://192.168.154.130/ fullnat-4core
```

覆盖：长连接 QPS（不同并发）、短连接 CPS（`Connection: close`）、固定速率尾延迟（wrk2）。每项默认重复 3 次，`results/<时间>-<标签>/mean.csv` 是均值，`env.txt` 记录环境。

短连接测试是验证会话回收的关键：持续 30 秒、每秒数万新建连接时，累计会话数远超会话表容量。压测后在 LB 上执行 `l4lbctl.py stats`，`drop_table_full` 和 `drop_no_port` 应为 0。

## 2. 小包转发能力（TRex / pktgen）

wrk 测的是整条链路（客户端 + LB + RS 协议栈），RS 往往先成为瓶颈。测 LB 本身的 Mpps 需要发包工具，例如 TRex：

- 64B / 512B / 1518B TCP SYN，源 IP/端口随机，目的 VIP:80
- 逐步提高速率，记录 LB `rx` 与 `fwd_in` 开始出现差距（`tx_full` / 网卡 `imissed` 增长）时的速率
- 分别用 1 / 2 / 4 / N 个 lcore 测，检查是否近似线性扩展

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

VMware 默认可能不在 vmxnet3 的多个队列间做 RSS 分发，所有包都进队列 0。
此时 `stats` 中 `rss no hash` 持续增长、`redirect out` 约为收包数的 (N-1)/N，
转发仍然正确（由 worker 0 转交给会话所在的核），但多核扩展失效。
关机后在虚拟机的 .vmx 中为 DPDK 使用的网卡加上（X 为网卡序号）：

```
ethernetX.pnicFeatures = "4"
ethernetX.udpRSS = "1"
```

## 4. LB 内部观察

```bash
scripts/l4lbctl.py stats -v
```

需要关注：

| 计数 | 含义 |
| :--- | :--- |
| `redirect_out` | 收包的核不是会话 owner、转交给其他核的包数。硬件 RSS 生效时（`steering: hw-rss`）TCP 应接近 0 |
| `rss_mismatch` | 网卡 RSS hash 与软件计算不一致的 SYN 数，应为 0 |
| `rss_no_hash` | 硬件 RSS 模式下网卡没有给出 hash 的 SYN 数，应为 0；不为 0 说明 RSS 实际没有生效 |
| `drop_*` | 各类丢包原因 |
| `tx_full` | TX 队列满，说明发送跟不上 |

配合 `perf top` / 火焰图定位热点（CMake 默认 `RelWithDebInfo` 并保留帧指针）。

## 5. 记录规范

每次测试记录：CPU 型号和频率、核数、NUMA、网卡型号和驱动、DPDK 版本、l4lb 的 git commit 和编译参数、vCPU 是否绑核、宿主机负载。每个重构阶段结束后用同一套脚本回归，结果存档对比。
