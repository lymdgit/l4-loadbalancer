/**
 * @file tcp_state.h
 * @brief 简化的 TCP 连接状态机（参考 IPVS）
 *
 * FULLNAT 模式下 LB 能看到双向报文；DR 模式只能看到客户端方向，
 * 状态机在两种模式下共用，DR 下只是收不到 RS 方向的事件。
 *
 *   NONE --SYN--> SYN_RECV --ACK--> ESTABLISHED --FIN--> FIN_WAIT
 *                                                          |
 *                                     对端 FIN / ACK       v
 *                                                      TIME_WAIT
 *   任意状态收到 RST -> CLOSE
 *
 * 每个状态有独立超时（见 TimeoutConf），握手和挥手阶段的会话会很快被回收，
 * 降低 SYN Flood 对会话表的占用。
 *
 * @author L4 Load Balancer Project
 */

#ifndef L4LB_LB_TCP_STATE_H
#define L4LB_LB_TCP_STATE_H

#include <cstdint>

namespace l4lb {

struct TimeoutConf;

enum class TcpState : uint8_t {
  NONE = 0,
  SYN_RECV,
  ESTABLISHED,
  FIN_WAIT,
  TIME_WAIT,
  CLOSE,
  UDP, ///< UDP 会话不区分状态
  COUNT
};

/// 报文方向
enum class Dir : uint8_t {
  CLIENT_TO_RS = 0,
  RS_TO_CLIENT = 1,
};

const char *tcp_state_name(TcpState s);

/**
 * @brief 根据当前状态和报文标志位计算新状态
 *
 * @param fin_seen [in/out] 已收到 FIN 的方向位图（bit0 = 客户端，bit1 = RS）
 */
TcpState tcp_next_state(TcpState cur, uint8_t tcp_flags, Dir dir,
                        uint8_t &fin_seen);

/// 状态对应的超时（秒）
uint32_t state_timeout(TcpState s, const TimeoutConf &t);

/// 这个状态下收到新的 SYN 时，是否应丢弃旧会话、按新连接处理
inline bool tcp_state_reusable(TcpState s) {
  return s == TcpState::TIME_WAIT || s == TcpState::CLOSE;
}

} // namespace l4lb

#endif // L4LB_LB_TCP_STATE_H
