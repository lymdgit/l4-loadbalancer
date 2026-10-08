/**
 * @file control.h
 * @brief 控制面：unix socket 命令接口 + 发布健康检查结果
 *
 * 运行在独立线程（非 EAL lcore），不参与收发包。每个连接执行一条命令，
 * 返回文本结果后关闭。客户端见 scripts/l4lbctl.py，也可以直接：
 *   echo stats | socat - UNIX-CONNECT:/run/l4lb.sock
 *
 * 命令：
 *   help | stats [-v] | services
 *   weight <rs_id> <w>          调整权重（0 = 不接新连接，已有连接保持）
 *   enable <rs_id> | disable <rs_id>
 *   add <service_idx> <ip:port[:weight]>
 *   del <rs_id>
 *   log <debug|info|warn|error>
 *   quit                        退出 l4lb
 *
 * socket 权限为 0600，只有 root 可以连接。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_CTRL_CONTROL_H
#define L4LB_CTRL_CONTROL_H

#include <atomic>
#include <cstdint>
#include <string>

#include <rte_thread.h>

namespace l4lb {

class ControlServer {
public:
  bool start(const std::string &path);
  void stop();

  /// 执行一条命令（测试和 socket 共用）
  static std::string execute(const std::string &line);

private:
  void run();
  void drain_health();
  static uint32_t thread_main(void *self);

  std::string path_;
  int listen_fd_ = -1;
  rte_thread_t thread_{};
  bool started_ = false;
  std::atomic<bool> stop_{false};
};

} // namespace l4lb

#endif // L4LB_CTRL_CONTROL_H
