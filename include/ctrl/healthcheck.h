/**
 * @file healthcheck.h
 * @brief RS 健康检查（数据面内的 TCP 探测，运行在 master 线程上）
 *
 * LB 的网卡被 DPDK 接管，内核协议栈无法直接连 RS，所以探测包由 master 自己
 * 构造：从 hc_src 的专用端口段（kHcPortMin~kHcPortMax）向 RS:port 发 SYN，
 *   收到 SYN-ACK -> 成功（回一个 RST 关闭半连接）
 *   收到 RST / 超时 -> 失败
 * 只探测 TCP 服务的 RS（UDP 服务无法用 TCP 握手判断，视为一直健康）。
 * 连续失败 fall 次判定 DOWN，连续成功 rise 次判定 UP。状态变化通过 ring 交给
 * 控制线程，由它发布新快照；DOWN 的 RS 不再参与调度，已有会话被断开。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CTRL_HEALTHCHECK_H
#define L4LB_CTRL_HEALTHCHECK_H

#include "common/config.h"
#include "common/types.h"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace l4lb {

struct Snapshot;
struct WorkerCtx;
struct MasterEvent;

class HealthChecker {
public:
  void init(const HealthConf &conf, IPv4Addr src_ip);

  /// 周期调用（master，约每 100ms）：发探测、处理超时、上报状态变化
  void tick(WorkerCtx &master, const Snapshot &snap, uint64_t now_ms);

  /// 处理 worker 转来的回包事件
  void on_response(WorkerCtx &master, const MasterEvent &ev, uint64_t now_ms);

  bool enabled() const { return conf_.enabled; }

private:
  struct Probe {
    uint32_t rs_id = 0;
    IPv4Addr ip = 0;
    uint16_t port = 0;   ///< RS 端口（主机字节序）
    MacAddr mac{};       ///< 静态 MAC
    bool snap_healthy = true; ///< 快照里的状态
    bool healthy = true;      ///< 本地判定的状态
    bool in_flight = false;
    uint16_t src_port = 0;    ///< 本次探测使用的源端口
    uint32_t isn = 0;
    uint64_t sent_ms = 0;
    uint64_t next_ms = 0;
    uint64_t reported_ms = 0;
    uint32_t ok = 0, fail = 0;
  };

  void send_probe(WorkerCtx &master, Probe &p, uint64_t now_ms);
  void send_rst(WorkerCtx &master, const Probe &p, const MasterEvent &ev);
  void result(Probe &p, bool ok, uint64_t now_ms);
  bool build_tcp(WorkerCtx &master, const Probe &p, uint8_t flags,
                 uint32_t seq, uint32_t ack);

  HealthConf conf_;
  IPv4Addr src_ip_ = 0;
  uint16_t next_port_ = kHcPortMin;
  uint32_t isn_seed_ = 0x5a5a1234;
  std::unordered_map<uint32_t, Probe> probes_;   ///< rs_id -> 状态
  std::unordered_map<uint16_t, uint32_t> by_port_; ///< 源端口 -> rs_id
};

} // namespace l4lb

#endif // L4LB_CTRL_HEALTHCHECK_H
