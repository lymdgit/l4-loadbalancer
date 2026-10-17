# 性能测试（memif）

只保留一套方案：**用 DPDK `net_memif` 共享内存网卡给 l4lb 直接喂包**，测的是 l4lb 代码本身的转发能力，不经过物理网卡、虚拟交换机（vSwitch）和内核协议栈。

```
memif_gen（发包器，lcore 8-10）              l4lb（pipeline，lcore 1 收包 + 2.. 转发）
  发包核：Client → VIP:9 的 64B UDP ──net_memif──> receiver → worker（FULLNAT 入站）→ 发给 RS
          10000 条流，按目标速率发送
  收包核：发给 RS 的包原路回射     <──────────────
          （交换 MAC/IP/端口，相当于 RS 回包）────> worker（FULLNAT 回程）→ 发给 Client
          发给 Client 的包计数后丢弃 <──────────────
```

每个 Client 包在 LB 上产生 2 次转发（入站 + 回程），完整走会话查找、FULLNAT 改写、校验和、SNAT 回程路径。

| 文件 | 作用 |
| :--- | :--- |
| `memif_bench.sh` | 一键测试：编译发包器、启动 l4lb（memif server）、逐档调速率、用 `counters` / `delta` 收集每档数据 |
| `tools/memif_gen.c` | 发包器（memif client）：按速率发包、回射模拟 RS、输出机器可读结果 |
| `results/` | 每次运行的原始数据，`<时间>-memif/` |

## 1. 运行

```bash
./build.sh                                   # 先编译 l4lb
sudo tests/perf/memif_bench.sh               # 1 个转发核，按默认速率逐档测
sudo WORKERS=2 tests/perf/memif_bench.sh     # 2 个转发核
sudo RATES="2000000 3000000" DURATION=20 tests/perf/memif_bench.sh
```

可调环境变量：

| 变量 | 默认 | 说明 |
| :--- | :--- | :--- |
| `WORKERS` | `1` | 转发核数（pipeline 模式下另加 1 个收包核） |
| `LB_LCORES` | `1-$(1+WORKERS)` | l4lb 使用的 lcore |
| `GEN_LCORES` | `8-10` | 发包器的 lcore（main + 发包核 + 收包核） |
| `RATES` | `500000 1000000 2000000 3000000 0` | 发包速率（pps），`0` 表示尽力发送，每档单独一轮 |
| `DURATION` | `10` | 每档持续秒数 |
| `FLOWS` | `10000` | 并发流（会话）数 |
| `SIZE` | `60` | 帧长（不含 FCS，60 即 64 字节小包） |
| `REFLECT` | `1` | `1` 回射（入站 + 回程）；`0` 只测入站 |

要求：

- root 权限，已配置大页；运行前停掉其它 l4lb / memif_gen（会抢 lcore，结果只剩一半且丢包严重）。
- 脚本用 `--no-pci` 启动，不碰物理网卡；控制 socket 走独立临时路径，不影响正在跑的 l4lb。
- 两端队列数自动对齐：l4lb TX = worker 数 + 1，RX 向上取 2 的幂；发包器 RX/TX 与之相反。手动跑 `memif_gen` 时必须自己保证 `--rxq` / `--txq` 一致，否则数据会悄悄出错。

## 2. 结果怎么看

每次运行产生 `tests/perf/results/<时间>-memif/`：

| 文件 | 内容 |
| :--- | :--- |
| `summary.txt` | 汇总表（每次运行追加一行） |
| `delta_<速率>.txt` | 该档的 l4lb 计数报告（`counters` / `delta` 差值） |
| `gen_<速率>.txt` | 发包器原始输出（`tx_pps`、`client_pps`、往返成功数等） |

`summary.txt` 的列：

| 列 | 含义 |
| :--- | :--- |
| `target` | 目标速率（`max` 表示尽力发送） |
| `gen_tx` | 发包器实际发送 pps |
| `gen_back` | 完成一次完整往返（Client → LB → RS → LB → Client）的 pps |
| `fwd_in` / `fwd_out` / `fwd_total` | l4lb 入站 / 回程 / 总转发 pps |
| `rcv_busy` / `wk_busy` | 收包核 / 转发核忙碌率 |
| `lb_loss` | l4lb 丢包率（`drops ÷ (处理的包 + drops)`，含 `rx_ring_full`、`tx_full`） |

## 3. 判据

1. **计数必须互相对得上**：`gen_tx ≥ fwd_in ≥ fwd_out = gen_back`。对不上说明测试环境本身有问题（会话串了、包在环、队列数不匹配），该轮数据作废。
2. **按丢包率取能力值**：取 `lb_loss < 0.1%` 时的最大转发速率（RFC 2544 的思路），这比"尽力发送"的数字更能代表可用能力。
3. **加 worker 看是否近似线性**：同时观察 `wk_busy`，如果转发核没满（< 85%）而转发量不再上涨，瓶颈在发包器，不在 l4lb。

## 4. 注意

- 数据**不含真实网卡的收发开销**，不能与物理网卡上的结果或 DPVS / Katran 的公开数据直接对比。
- memif 默认是 copy 模式（两端用不同 `--file-prefix`），每包多一次内存拷贝，不影响横向对比。
- `FLOWS` 条会话在测试开始时建立，之后都是已有会话的转发，测的是转发路径，不是新建连接速度。
- 看单核热点，压测时另开终端：

```bash
perf record -F 2999 -C 2 -- sleep 5
perf report --no-children --sort symbol
```

这台 VM 不支持硬件计数器（`perf stat` 的 cycles 显示 not supported），只能采样。

完整方案、实测结果和排查记录见 `docs/压测方案.md`、`docs/PPS测试与性能排查.md`。
