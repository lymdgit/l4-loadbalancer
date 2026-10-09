# PPS 统计方案

## 一、背景

wrk 只能给出 QPS，不知道 LB 实际转发了多少个包。DPVS 的做法是：wrk 打 HTTP 流量，同时在 LB 上记录压测前后的包计数（`ipvsadm -ln --stats`、`dpip -s link show`），用差值除以时间得到 PPS。

l4lb 也应该提供同样的查询方式：压测前后各读一次计数，自己算差值，或者交给脚本算。

## 二、现状

已经有的：

| 来源 | 内容 | 不足 |
|---|---|---|
| `stats` | 全局累计的 `DPDK RX/TX`（软件计数）、`fwd in/out`、会话数、各类丢包 | 只有包数，没有字节数；没有按服务、按 RS 的拆分 |
| `stats` 的 NIC 一行 | `rte_eth_stats_get` 的 ipackets、opackets、imissed 等 | 由 master 每秒刷新，最多晚 1 秒；没有字节数 |
| `rate` | 最近一个 `stats_interval` 周期的 pps 和忙碌率 | 是周期采样，和 wrk 的起止时间对不齐 |
| 会话里的 `pkts_in/bytes_in/pkts_out/bytes_out` | 每个会话都在计数 | 没有汇总，会话结束后计数就丢了 |

缺的：

1. **字节数**：算不出 bps，也就没法和网卡带宽比较。
2. **按服务、按 RS 的计数**：对应 `ipvsadm -ln --stats`，可以确认流量确实均匀分给了每个 RS。
3. **稳定的机器可读输出**：现在的 `stats` 是给人看的文本，格式随时可能改，不适合用脚本解析。
4. **现成的前后差值工具**：现在只能靠手算。

## 三、设计

### 1. 计数器

全部沿用现有写法：每个 worker 只写自己那一份，按 cache line 对齐，只用 relaxed 的 load + store（x86 上就是普通的 mov）。读取时跨核求和，热路径不加锁，也不用原子 RMW 指令。

**全局（每个 worker 一份，加在 `WorkerStats` 里）**：

| 计数 | 位置 | 含义 |
|---|---|---|
| `rx_bytes` | 收包处（rtc 是 worker，pipeline 是 receiver） | 收到的字节数 |
| `tx_bytes` | `tx_flush` 中 `rte_eth_tx_burst` 成功的那部分 | 发出的字节数 |
| `fwd_in_bytes` / `fwd_out_bytes` | 与 `fwd_in` / `fwd_out` 同一处 | 转发的字节数（客户端到 RS、RS 到客户端） |

**按服务 / 按 RS（每个 worker 一个数组，按 RS id 索引）**：

```
struct RsCounters { uint64_t conns, pkts_in, bytes_in, pkts_out, bytes_out; };
WorkerCtx::rs_stats[kMaxRsId]
```

- `conns`：新建会话时加 1。
- `pkts_in/bytes_in`：入站转发成功时累加（客户端到 RS）。
- `pkts_out/bytes_out`：FULLNAT 回程转发成功时累加（RS 到客户端）。DR 模式下回包不经过 LB，这两项为 0。
- 会话里已经有 `rs_id`，转发时直接对 `rs_stats[s->rs_id]` 累加，每个包只多一次本地内存写。
- 服务级的计数在查询时由它下面所有 RS 的计数相加得到，不单独维护。
- 内存：现在 RS id 的上限 `kMaxRsId` 是 65536，按这个上限每个 worker 要 2.5MB。实际最多只有 64 个服务 × 256 个 RS，所以数组大小改按"已分配的 RS id 上限"来定（启动时分配，`add` 命令分配新 id 时如果超出就扩容）。也可以直接用固定的 16384 项，每个 worker 640KB。具体在实现时选一种。
- RS 删除后 id 不会复用，它的计数保留不动，查询时跳过已删除的 RS。

**网卡（`rte_eth_stats_get`）**：

- 增加 `ibytes`、`obytes`。
- 查询命令执行时，在控制线程里当场调用 `rte_eth_stats_get`，不再用 master 每秒刷新的缓存，保证读到的是那一刻的值。`rte_eth_stats_get` 在其他线程收发包的同时调用是安全的，DPDK 的 `testpmd` 和 `dpdk-proc-info` 都是这么用的。

### 2. 查询命令

**`stats -r`：服务和 RS 计数（对应 `ipvsadm -ln --stats`）**

```
service 0 http 192.168.154.130:80/tcp  conns 12869  in 146309 pkts 13.2M bytes  out 144583 pkts 19.8M bytes
  rs 1 192.168.154.140:80    conns 6435  in 73155 pkts ...  out 72290 pkts ...
  rs 2 192.168.154.140:8080  conns 6434  in 73154 pkts ...  out 72293 pkts ...
```

**`counters`：机器可读的全部原始计数（脚本使用）**

一行一个 `key value`，key 的名字固定不变，值是从启动开始的累计值：

```
time_ns 1759912345123456789
uptime_ns 123456789000
nic_ipackets 290933
nic_opackets 290982
nic_ibytes 23812345
nic_obytes 27301234
nic_imissed 0
nic_ierrors 0
nic_oerrors 0
nic_rx_nombuf 0
rx_pkts 290933
rx_bytes 23812345
tx_pkts 290982
tx_bytes 27301234
fwd_in_pkts 146309
fwd_in_bytes 11234567
fwd_out_pkts 144583
fwd_out_bytes 15987654
conns_new 12869
drops 0
drop_rx_ring 0
...（其余 stat_name 计数）
svc.0.conns 12869
svc.0.pkts_in 146309
...
rs.1.conns 6435
rs.1.pkts_in 73155
...
```

- 带时间戳 `time_ns`（CLOCK_REALTIME），两次快照相减就能得到准确的时间间隔，不依赖 wrk 的 `-d` 参数。
- 所有计数单调递增，不提供清零命令，避免多人或多个脚本互相干扰，和 `ipvsadm --zero` 的做法不同。

### 3. 前后差值工具：`l4lbctl.py delta`

把"压测前读一次、压测后读一次、相减"做成一条命令：

```bash
# 方式 1：压测前后各执行一次
scripts/l4lbctl.py counters > /tmp/before.txt
wrk -t4 -c1000 -d60s http://192.168.154.130/       # 在 client 上执行
scripts/l4lbctl.py delta /tmp/before.txt           # 再读一次，与 before 相减

# 方式 2：一直等到 Ctrl+C
scripts/l4lbctl.py delta --watch                   # 开始时读一次，Ctrl+C 时再读一次
```

输出示例（数字为示意）：

```
interval 60.02 s
              packets        pps          bytes          bps
nic rx       13,762,000    229.3k     1.13 GB     150.4 Mbps
nic tx       13,760,500    229.3k     1.31 GB     174.7 Mbps
fwd in        6,880,105    114.6k
fwd out       6,881,895    114.7k
total fwd    13,762,000    229.3k       (= in + out)
new conns         1,000      16.7 /s
drops                 0   imissed 0   rx_ring_full 0
rs 1 192.168.154.140:80     in 57.3k pps   out 57.3k pps   conns 500
rs 2 192.168.154.140:8080   in 57.3k pps   out 57.3k pps   conns 500
```

- 以 `fwd in + fwd out` 作为 LB 的转发 PPS（FULLNAT 每个请求双向都经过 LB）。DR 模式下只有 `fwd in`。
- 网卡的 rx/tx 和转发计数同时列出，两者的差就是 ARP、健康检查、丢弃的包，方便核对。
- 差值计算放在 Python 里做，C++ 这边只负责提供原始计数，代码改动最小。

### 4. 周期日志

`Rate:` 那一行加上 bps，例如 `rx 229.3k pps 150.4 Mbps`，方便在 `tail -f` 里直接看到带宽。

### 5. run_bench.sh

每项压测开始前抓一次 `counters`，结束后再抓一次。把差值算出的 PPS 和 bps 写进 `summary.csv`，新增 `lb_pps`、`lb_bps` 两列，这样每个 QPS 数字旁边都有对应的 PPS。

## 四、性能影响

- 每个转发包多出：全局字节计数 1 次、RS 计数 2 次（pkts 和 bytes），都是本核内存上的普通加法，cache line 只有本核在写。
- `rx_bytes` 和 `tx_bytes` 每个 burst 累加一次（在 burst 循环里先求和再写）。
- 预计开销在 1% 以内。实现后用 `lab.sh bench` 和 `perf stat` 对比改动前后，在 `docs/pps方案.md` 里补上实测数据。

## 五、改动范围

| 文件 | 改动 |
|---|---|
| `include/common/stats.h`、`src/common/stats.cpp` | 字节计数（单独的数组，和包计数 `Stat` 分开，不影响现有的 `stat_name`） |
| `include/dataplane/context.h` | `WorkerCtx` 增加 `rs_stats` |
| `src/core/processor.cpp` | 转发成功和新建会话处累加 RS 计数、字节计数 |
| `src/dataplane/worker.cpp`、`receiver.cpp` | `rx_bytes`、`tx_bytes`；`counters` 和 `stats -r` 的输出；`Rate:` 行加 bps |
| `src/ctrl/control.cpp` | 新命令 `counters`、`stats -r`；help |
| `scripts/l4lbctl.py` | `delta`（读文件或 `--watch`） |
| `tests/perf/run_bench.sh` | 每项前后抓 `counters`，汇总 PPS |
| 测试 | 单元测试：差值与格式化；功能测试：发 N 个包后 `counters` 中 fwd、rs 计数和字节数正确，`delta` 输出正确 |

## 六、待你确认

1. RS 计数数组用固定 16384 项（每个 worker 640KB，简单），还是按实际 RS id 动态扩容（省内存，代码多一点）？我倾向固定大小，`add` 命令分配 id 超过上限时报错。
2. 要不要提供清零命令（类似 `ipvsadm --zero`）？我倾向不提供，用 `delta` 做前后差值就够了，也不会和多人同时查询互相影响。

## 七、实现结果（2026-10-08）

两个待确认项按建议实现：RS 计数数组固定 16384 项（`kMaxRsCounters`，RS id 上限随之改为 16384），不提供清零命令。

命令：

```bash
scripts/l4lbctl.py stats -r                      # 按服务 / RS（类似 ipvsadm -ln --stats）
scripts/l4lbctl.py counters > before.txt         # 压测前
scripts/l4lbctl.py delta before.txt              # 压测后
scripts/l4lbctl.py delta --watch                 # 或者：现在开始，Ctrl+C 结束
```

`delta` 输出示例（单机 lab，loadgen 8 秒长连接）：

```
interval 8.06 s  (fullnat, pipeline, 2 workers)
                    packets        pps        bytes            bps
nic rx              289,968     35.97k    34.14 MiB      35.53Mbps
nic tx              289,975     35.97k    33.12 MiB      34.46Mbps
fwd in              145,293     18.02k    16.55 MiB      17.22Mbps
fwd out             144,668     17.95k    16.57 MiB      17.24Mbps
total fwd           289,961     35.96k    33.12 MiB      34.45Mbps  (= in + out)
new conns               100         12 /s   active now 100
busy: receiver 11.1% (35.96k pps, 3095 ns/pkt) | worker0 22.4% (14.37k pps, 15570 ns/pkt) | ...
drops 0  imissed 0  ierrors 0  rx_nombuf 0  rx_ring_full 0  tx_full 0
service 0 web 192.168.154.130:80/tcp   conns 100  in 18.02k pps  out 17.95k pps
  rs 1    192.168.154.142:8081   conns 50   in 9.0k   out 9.0k   pps  17.2Mbps
  rs 2    192.168.154.142:8082   conns 50   in 9.0k   out 9.0k   pps  17.3Mbps
```

比方案多了一行 `busy:`：`counters` 里加了各线程的忙碌时间（`thread.*.busy_ns`）和处理包数，`delta` 据此算出区间内的忙碌率和每包耗时。`ns/pkt` 只计忙碌轮次的时间，包少的时候每批包数小，数字会偏大，只适合在压测时参考。

`run_bench.sh` 设置了 `LB_HOST` 时，每一项测试前后都会在 LB 上执行 `counters` / `delta`，`summary.csv` 和 `mean.csv` 新增 `lb_pps`、`lb_bps` 两列，完整报告保存为 `lb_delta_<测试项>.txt`。

开销：单机 lab 用 loadgen 跑了 10 秒、各 2 轮，改动前 19687 / 18753 QPS，改动后 19787 / 18312 QPS，worker 忙碌率都在 23%~28% 之间，差异在测量噪声范围内。这个环境测不出 1% 量级的差别，要在压测环境里用同一套参数再确认。

测试：新增功能测试 `packet_counters`（20 条连接往返后检查 fwd 包数、字节数、每个 RS 的计数、`stats -r`、`delta` 的报告和 CSV），rtc 和 pipeline 两种模式下全部 24 个功能测试通过，单元测试 8 个通过。
