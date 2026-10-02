// TCP 状态机单元测试
#include "common/config.h"
#include "lb/tcp_state.h"
#include "protocol/ip.h"
#include "test.h"

using namespace l4lb;

static TcpState run(std::initializer_list<std::pair<uint8_t, Dir>> pkts) {
  TcpState s = TcpState::NONE;
  uint8_t fin = 0;
  for (auto &p : pkts)
    s = tcp_next_state(s, p.first, p.second, fin);
  return s;
}

constexpr Dir C = Dir::CLIENT_TO_RS, R = Dir::RS_TO_CLIENT;

TEST(handshake) {
  CHECK(run({{TCP_SYN, C}}) == TcpState::SYN_RECV);
  CHECK(run({{TCP_SYN, C}, {TCP_SYN | TCP_ACK, R}}) == TcpState::SYN_RECV);
  CHECK(run({{TCP_SYN, C}, {TCP_SYN | TCP_ACK, R}, {TCP_ACK, C}}) ==
        TcpState::ESTABLISHED);
}

TEST(dr_mode_handshake_client_only) {
  // DR 只看到客户端方向
  CHECK(run({{TCP_SYN, C}, {TCP_ACK, C}}) == TcpState::ESTABLISHED);
}

TEST(close_sequence) {
  auto est = {std::make_pair(TCP_SYN, C), std::make_pair(uint8_t(TCP_SYN | TCP_ACK), R),
              std::make_pair(TCP_ACK, C)};
  (void)est;
  TcpState s = TcpState::NONE;
  uint8_t fin = 0;
  s = tcp_next_state(s, TCP_SYN, C, fin);
  s = tcp_next_state(s, TCP_ACK, C, fin);
  s = tcp_next_state(s, TCP_FIN | TCP_ACK, C, fin);
  CHECK(s == TcpState::FIN_WAIT);
  s = tcp_next_state(s, TCP_ACK, R, fin);
  CHECK(s == TcpState::FIN_WAIT);
  s = tcp_next_state(s, TCP_FIN | TCP_ACK, R, fin);
  CHECK(s == TcpState::TIME_WAIT);
}

TEST(rst_closes) {
  CHECK(run({{TCP_SYN, C}, {TCP_RST, R}}) == TcpState::CLOSE);
  CHECK(run({{TCP_SYN, C}, {TCP_ACK, C}, {TCP_RST | TCP_ACK, C}}) ==
        TcpState::CLOSE);
}

TEST(timeouts) {
  TimeoutConf t;
  CHECK_EQ(state_timeout(TcpState::SYN_RECV, t), t.tcp_syn);
  CHECK_EQ(state_timeout(TcpState::ESTABLISHED, t), t.tcp_established);
  CHECK_EQ(state_timeout(TcpState::TIME_WAIT, t), t.tcp_timewait);
  CHECK_EQ(state_timeout(TcpState::UDP, t), t.udp);
  CHECK(tcp_state_reusable(TcpState::TIME_WAIT));
  CHECK(!tcp_state_reusable(TcpState::ESTABLISHED));
}

int main() { return ut::run_all(); }
