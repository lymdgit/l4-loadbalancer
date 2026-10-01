# 功能测试（无需真实网卡）

在一对 veth 上用 DPDK `net_af_packet` 驱动运行 l4lb，从对端用 raw socket 注入报文并检查 LB 发出的报文。需要 root 和可用的 hugepage。

```bash
# 先构建
PKG_CONFIG_PATH=<dpdk>/lib64/pkgconfig cmake -S . -B build && cmake --build build -j

# 运行全部用例
sudo python3 tests/functional/run_tests.py build/l4lb
# 只运行名字包含 nat 的用例
sudo python3 tests/functional/run_tests.py build/l4lb -k nat
```

用例覆盖重构计划第一部分的 bug（会话回收、畸形包、端口过滤等），每个用例会单独启动一次 l4lb。
