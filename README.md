# L4 Load Balancer

基于 **DPDK** 的高性能四层负载均衡器（L4），面向高并发场景下的低延迟报文转发。

## 一、 项目说明

本项目实现了基于 DPDK 的四层负载均衡功能。

**核心特性：**
- 纯数据平面转发：不终止 TCP 连接，实现按包快速改写转发。
- 多核并行：采用 Run-to-Completion 模型与 RSS 特性充分利用多核性能。
- 双业务模式：支持 FULLNAT 和 DR（Direct Routing）两种负载均衡模式。
- 高级特性：支持会话保持（正向/反向映射）、一致性哈希（MurmurHash3 + 虚拟节点）、零拷贝转发（mbuf 直通）、以及灵活的校验和策略。

**项目目录说明：**

```text
l4-loadbalancer_v3/
├── CMakeLists.txt        # CMake 项目构建配置文件
├── build/                # CMake 编译生成的构建产物目录
├── config/               # 负载均衡器运行配置文件目录
│   ├── lb.conf           # FULLNAT 模式配置文件（包含 VIP、RS 信息等）
│   └── lb_dr.conf        # DR（Direct Routing）模式配置文件
├── docs/                 # 项目核心设计文档与技术方案
│   └── RCU.md            # RCU 机制设计与锁优化文档等
├── include/              # 核心 C++ 头文件与模块化设计目录
│   ├── common/           # 基础公共组件（config.h 解析器, logger.h 日志库, types.h 结构体定义）
│   ├── core/             # 核心业务控制模块（loadbalancer.h 流量调配主逻辑）
│   ├── forward/          # 报文转发处理器实现（nat_forwarder.h, dr_forwarder.h 等策略实现）
│   ├── lb/               # 负载均衡算法及状态组件（consistent_hash.h 一致性哈希, session.h 会话保持与映射）
│   └── protocol/         # 协议交互及解析层定义
├── src/                  # 核心源代码目录
│   └── main.cpp          # 项目主入口及基于 Run-to-Completion 模型的核心事件调度循环
└── 学习笔记与数据文件    # 涵盖 DPDK 学习、校验处理机制梳理、网卡及测试数据说明（如 learn.md, 校验处理.md, dpdk网卡绑定.txt 等）
```

## 二、 DPDK 配置方式

在运行本负载均衡器前，需要先编译 DPDK 并挂载相应的网卡驱动，具体步骤如下：

### 1. 编译 DPDK
```bash
# Compile DPDK 会生成对应的驱动程序，一会需要挂载
cd dpdk/
# igb_uio is about 5% more efficient than vfio-pci, so continue using it.
meson -Denable_kmods=true build
ninja -C build
ninja -C build install
```

### 2. 配置系统大页内存 (Hugepage)

```bash
# Set hugepage (Linux only)
# 方式一：单节点系统（1024个2MB的page，让操作系统自己去分配）
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# or NUMA (Linux only)
# 方式二：如果有多CPU多内存条
echo 1024 > /sys/devices/system/node/node0/hugepages/hugepages-2048kB/nr_hugepages
echo 1024 > /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages

# Using Hugepage with the DPDK (Linux only)
# 这里是为了让 dpdk 去分配和使用大页
mkdir -p /mnt/huge
mount -t hugetlbfs nodev /mnt/huge
```

### 3. 网卡驱动解绑与挂载
```bash
# Offload NIC
# 告诉内核我要准备搞驱动了，提前准备好
modprobe uio
# 加载用户态的驱动（为了用户态直接操作硬件），请根据实际路径确认 .ko 文件位置
insmod /data/f-stack/dpdk/build/kernel/linux/igb_uio/igb_uio.ko

# 查看当前网卡状态
dpdk-devbind.py --status

# 卸载原网卡驱动（示例中为 ens192）并绑定至 igb_uio
ifconfig ens192 down
./dpdk-devbind.py --bind=igb_uio ens192
```

## 三、 测试方式说明

本项目支持 FULLNAT 和 DR 两种模式，可使用对应配置文件启动应用：

```bash
# FULLNAT 模式启动
./l4lb -l 1-4 -n 4 -- --lb-config ../config/lb.conf

# DR 模式启动
./l4lb -l 1-4 -n 4 -- --lb-config ../config/lb_dr.conf
```

> **⚠️ 注意：DR 模式的特殊配置**
> 在 DR 模式下，因为 L4 均衡器不会去修改报文的 VIP，所以需要我们在后端真实服务器 (Real Server, RS) 进行如下操作：

```bash
# 1. 在回环网卡 (loopback) 上添加 VIP
sudo ip addr add 192.168.154.132/32 dev lo

# 2. 禁止 RS 响应 VIP 的 ARP 请求（避免同网络下 ARP 冲突）
sudo sysctl -w net.ipv4.conf.all.arp_ignore=1
sudo sysctl -w net.ipv4.conf.all.arp_announce=2
sudo sysctl -w net.ipv4.conf.lo.arp_ignore=1
sudo sysctl -w net.ipv4.conf.lo.arp_announce=2

# 3. 验证配置是否成功生效
ip addr show lo
```

## 四、 性能测试结果对比

各场景下的核心性能指标（平均延迟、QPS）对比如下：

| 测试场景 | QPS (Req/Sec) | 平均延迟 (Avg Latency) |
| :--- | :---: | :---: |
| **不经过 L4 均衡器 (直连 RS)** | 131,716 | 12.34 ms |
| **经过 L4 均衡器 (DR 模式)** | 120,051 | 12.54 ms |
| **经过 L4 均衡器 (FULLNAT 模式)** | 108,635 | 13.05 ms |

*(测试条件参考：`wrk -t4 -c2000 -d30s`，4 线程 2000 连接，压测时长 30 秒)*





对于后端服务器，需要进行如下配置，调整内核相关参数，否则容易出现延迟巨大甚至丢包的情况：

```bash
  # 连接队列    
  # 全连接队列 + 半连接 + 网卡接收队列
  sudo sysctl -w net.core.somaxconn=65535
  sudo sysctl -w net.ipv4.tcp_max_syn_backlog=65535
  sudo sysctl -w net.core.netdev_max_backlog=65535

  # TIME_WAIT 加速回收（2000 连接高并发必须）
  # 允许在安全范围内进行，可以参与分配了
  sudo sysctl -w net.ipv4.tcp_tw_reuse=1
  # 这个一般是2MSL = 60S，这里修改为15s
  sudo sysctl -w net.ipv4.tcp_fin_timeout=15

  # 文件描述符上限（2000 连接 × 4 线程需要足够 fd）
  ulimit -n 100000



	#对于mpstat -P ALL 1所发现的%soft不均匀问题进行如下操作
	# 假设网卡名是 ens33，把软中断分散到所有 4 个核（0xF = 二进制 1111）
	echo "f" > /sys/class/net/ens33/queues/rx-0/rps_cpus
	# 启用 RFS（Receive Flow Steering）让流量自动路由到处理该连接的 CPU
	echo 32768 > /proc/sys/net/core/rps_sock_flow_entries
	echo 32768 > /sys/class/net/ens33/queues/rx-0/rps_flow_cnt


```



**目前本项目计划引入TREX工具进行pps的测试：**

测试工具：

```bash
# 绑定网卡
0000:03:00.0 'VMXNET3 Ethernet Controller' if=ens160 drv=vmxnet3 unused=igb_uio,vfio-pci,uio_pci_generic 
0000:0b:00.0 'VMXNET3 Ethernet Controller' if=ens192 drv=vmxnet3 unused=igb_uio,vfio-pci,uio_pci_generic

sudo vi /etc/trex_cfg.yaml
# 将下面两个地址替换为你刚才通过 ./dpdk_setup_ports.py -s 找到的真实 PCI 地址
# 顺序建议：先写 ens160 的 PCI，再写 ens192 的 PCI
  interfaces: ["0000:03:00.0", "0000:0b:00.0"]
  port_info:
    - ip: 192.168.154.135        # 对应 Port 0 (原 ens160) 的模拟 IP
      default_gw: 192.168.154.130  # 网关指向你的 130 DPDK 负载均衡器
    - ip: 192.168.154.136        # 对应 Port 1 (原 ens192) 的模拟 IP
      default_gw: 192.168.154.130  # 网关同样指向 130 节点
      
# 把配置信心加载进去
sudo ./dpdk_setup_ports.py -c /etc/trex_cfg.yaml      

# 分配大页内存
sudo sh -c 'echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages'

# 启动服务端
sudo ./t-rex-64 -i -c 2


# 启动测试端
./trex-console
trex> service
trex> arp
trex> service --off

# 运行脚本，开始测试
trex> start -f lb_test.py -m 10kpps -p 0
# 实时显示
trex> tui
# 关闭测试
trex> stop -a


```

