/**
 * @file tcp_state.cpp
 * @brief TCP 状态机实现
 */

#include "lb/tcp_state.h"

#include "common/config.h"
#include "protocol/ip.h"

namespace l4lb {

const char *tcp_state_name(TcpState s) {
  switch (s) {
  case TcpState::NONE:
    return "NONE";
  case TcpState::SYN_RECV:
    return "SYN_RECV";
  case TcpState::ESTABLISHED:
    return "ESTABLISHED";
  case TcpState::FIN_WAIT:
    return "FIN_WAIT";
  case TcpState::TIME_WAIT:
    return "TIME_WAIT";
  case TcpState::CLOSE:
    return "CLOSE";
  case TcpState::UDP:
    return "UDP";
  default:
    return "?";
  }
}

TcpState tcp_next_state(TcpState cur, uint8_t flags, Dir dir,
                        uint8_t &fin_seen) {
  if (cur == TcpState::UDP)
    return cur;
  if (flags & TCP_RST)
    return TcpState::CLOSE;

  const uint8_t self = dir == Dir::CLIENT_TO_RS ? 1 : 2;
  if (flags & TCP_FIN)
    fin_seen |= self;

  switch (cur) {
  case TcpState::NONE:
    return (flags & TCP_SYN) ? TcpState::SYN_RECV : cur;

  case TcpState::SYN_RECV:
    // 客户端发出握手第三个 ACK（不带 SYN）即认为建立
    if (dir == Dir::CLIENT_TO_RS && (flags & TCP_ACK) && !(flags & TCP_SYN))
      return fin_seen ? TcpState::FIN_WAIT : TcpState::ESTABLISHED;
    return cur;

  case TcpState::ESTABLISHED:
    return fin_seen ? TcpState::FIN_WAIT : cur;

  case TcpState::FIN_WAIT:
    // 双方都发了 FIN，或者对端确认了 FIN：进入 TIME_WAIT
    if (fin_seen == 3)
      return TcpState::TIME_WAIT;
    return cur;

  case TcpState::TIME_WAIT:
  case TcpState::CLOSE:
    return cur;

  default:
    return cur;
  }
}

uint32_t state_timeout(TcpState s, const TimeoutConf &t) {
  switch (s) {
  case TcpState::NONE:
  case TcpState::SYN_RECV:
    return t.tcp_syn;
  case TcpState::ESTABLISHED:
    return t.tcp_established;
  case TcpState::FIN_WAIT:
    return t.tcp_fin;
  case TcpState::TIME_WAIT:
    return t.tcp_timewait;
  case TcpState::CLOSE:
    return t.tcp_close;
  case TcpState::UDP:
  default:
    return t.udp;
  }
}

} // namespace l4lb
