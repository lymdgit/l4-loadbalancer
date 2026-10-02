// 报文解析与改写单元测试：畸形包、NAT 后校验和、TOA 插入、timestamp 去除、ICMP 差错
#include "eal_fixture.h"
#include "forward/nat_forwarder.h"
#include "protocol/checksum.h"
#include "protocol/icmp.h"
#include "protocol/ip.h"
#include "protocol/parser.h"

#include <arpa/inet.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include <rte_mbuf.h>
#include <rte_mempool.h>

using namespace l4lb;

static rte_mempool *g_pool;

// ---- 构包 ----
static uint16_t csum(const uint8_t *p, size_t n) {
  return IpChecksum::calculate(p, n);
}

static std::vector<uint8_t> tcp_frame(const std::vector<uint8_t> &opts,
                                      const std::string &payload, uint8_t flags,
                                      uint16_t frag = 0x4000) {
  size_t thl = 20 + opts.size();
  size_t ipl = 20 + thl + payload.size();
  std::vector<uint8_t> f(14 + ipl, 0);
  f[12] = 0x08;
  uint8_t *ip = &f[14];
  ip[0] = 0x45;
  ip[2] = ipl >> 8;
  ip[3] = ipl & 0xff;
  ip[6] = frag >> 8;
  ip[7] = frag & 0xff;
  ip[8] = 64;
  ip[9] = 6;
  *reinterpret_cast<uint32_t *>(ip + 12) = inet_addr("1.2.3.4");
  *reinterpret_cast<uint32_t *>(ip + 16) = inet_addr("10.0.0.1");
  uint16_t c = csum(ip, 20);
  memcpy(ip + 10, &c, 2);
  auto *tcp = reinterpret_cast<TcpHeader *>(ip + 20);
  tcp->src_port = htons(40000);
  tcp->dst_port = htons(80);
  tcp->seq_num = htonl(1);
  tcp->data_offset = static_cast<uint8_t>((thl / 4) << 4);
  tcp->flags = flags;
  tcp->window = htons(1000);
  memcpy(ip + 40, opts.data(), opts.size());
  memcpy(ip + 20 + thl, payload.data(), payload.size());
  L4Checksum::recalculate_tcp_checksum(reinterpret_cast<IPv4Header *>(ip), tcp,
                                       thl + payload.size());
  return f;
}

static rte_mbuf *to_mbuf(const std::vector<uint8_t> &f) {
  rte_mbuf *m = rte_pktmbuf_alloc(g_pool);
  memcpy(rte_pktmbuf_append(m, f.size()), f.data(), f.size());
  return m;
}

static bool checksums_ok(rte_mbuf *m) {
  auto *base = rte_pktmbuf_mtod(m, uint8_t *);
  auto *ip = reinterpret_cast<IPv4Header *>(base + 14);
  if (csum(base + 14, ip->get_header_len()) != 0)
    return false;
  size_t l4len = ip->get_total_length() - ip->get_header_len();
  // 伪首部 + L4 全量求和应为 0
  std::vector<uint8_t> buf(12 + l4len);
  memcpy(&buf[0], &ip->src_ip, 4);
  memcpy(&buf[4], &ip->dst_ip, 4);
  buf[9] = ip->protocol;
  buf[10] = l4len >> 8;
  buf[11] = l4len & 0xff;
  memcpy(&buf[12], base + 14 + ip->get_header_len(), l4len);
  return csum(buf.data(), buf.size()) == 0;
}

static NatRewrite rw() {
  NatRewrite r{};
  r.src_ip = inet_addr("10.0.0.2");
  r.dst_ip = inet_addr("10.0.0.11");
  r.src_port = htons(10007);
  r.dst_port = htons(8080);
  return r;
}

// ---- 解析 ----

TEST(parse_valid_and_padded) {
  auto f = tcp_frame({}, "", 0x02);
  f.resize(60, 0); // 以太网最小帧填充
  PacketMeta meta;
  CHECK(ProtocolParser::parse(f.data(), f.size(), meta) == ParseResult::OK);
  CHECK_EQ(meta.total_len, size_t(14 + 40));
  CHECK_EQ(int(meta.tcp_flags), 0x02);
}

TEST(parse_rejects_malformed) {
  PacketMeta meta;
  auto f = tcp_frame({}, "x", 0x02);
  CHECK(ProtocolParser::parse(f.data(), 30, meta) == ParseResult::MALFORMED);
  auto g = f;
  g[14] = 0x44; // IHL = 4
  CHECK(ProtocolParser::parse(g.data(), g.size(), meta) == ParseResult::MALFORMED);
  g = f;
  g[14 + 3] = 0xff; // total_length 超出帧长
  CHECK(ProtocolParser::parse(g.data(), g.size(), meta) == ParseResult::MALFORMED);
  g = f;
  g[14 + 32] = 0x20; // TCP doff = 2
  CHECK(ProtocolParser::parse(g.data(), g.size(), meta) == ParseResult::MALFORMED);
  auto fr = tcp_frame({}, "x", 0x02, 0x2000); // MF
  CHECK(ProtocolParser::parse(fr.data(), fr.size(), meta) == ParseResult::FRAGMENT);
}

// ---- 改写 ----

TEST(nat_rewrite_incremental_checksum) {
  rte_mbuf *m = to_mbuf(tcp_frame({}, "hello world", 0x18));
  PacketMeta meta;
  CHECK(ProtocolParser::parse(rte_pktmbuf_mtod(m, uint8_t *),
                              rte_pktmbuf_data_len(m), meta) == ParseResult::OK);
  RewriteOutcome out;
  CHECK(nat_rewrite(m, meta, rw(), NatOptions{}, out) == RewriteResult::OK);
  CHECK(checksums_ok(m));
  auto *ip = rte_pktmbuf_mtod_offset(m, IPv4Header *, 14);
  CHECK_EQ(int(ip->ttl), 63);
  CHECK_EQ(ip->dst_ip, inet_addr("10.0.0.11"));
  rte_pktmbuf_free(m);
}

TEST(nat_rewrite_ttl) {
  auto f = tcp_frame({}, "", 0x10);
  f[14 + 8] = 1;
  rte_mbuf *m = to_mbuf(f);
  PacketMeta meta;
  ProtocolParser::parse(rte_pktmbuf_mtod(m, uint8_t *), rte_pktmbuf_data_len(m), meta);
  RewriteOutcome out;
  CHECK(nat_rewrite(m, meta, rw(), NatOptions{}, out) == RewriteResult::TTL_EXCEEDED);
  rte_pktmbuf_free(m);
}

TEST(toa_inserted) {
  // 带 MSS 选项和数据的 ACK：TOA 插在基本头之后，数据完整保留
  std::vector<uint8_t> mss = {2, 4, 0x05, 0xb4};
  auto f = tcp_frame(mss, "GET / HTTP/1.1", 0x18);
  f.resize(f.size() + 6, 0); // 尾部再加一些填充，验证会被去掉
  rte_mbuf *m = to_mbuf(f);
  PacketMeta meta;
  CHECK(ProtocolParser::parse(rte_pktmbuf_mtod(m, uint8_t *),
                              rte_pktmbuf_data_len(m), meta) == ParseResult::OK);
  NatOptions opt;
  opt.add_toa = true;
  opt.toa_ip = inet_addr("1.2.3.4");
  opt.toa_port = htons(40000);
  RewriteOutcome out;
  CHECK(nat_rewrite(m, meta, rw(), opt, out) == RewriteResult::OK);
  CHECK(out.toa_added);
  CHECK(checksums_ok(m));
  auto *base = rte_pktmbuf_mtod(m, uint8_t *);
  auto *tcp = reinterpret_cast<TcpHeader *>(base + 34);
  CHECK_EQ(tcp->get_header_len(), size_t(32));
  const uint8_t *opt0 = base + 34 + 20;
  CHECK_EQ(int(opt0[0]), int(TCPOPT_TOA));
  CHECK_EQ(int(opt0[1]), 8);
  CHECK(memcmp(opt0 + 2, &opt.toa_port, 2) == 0);
  CHECK(memcmp(opt0 + 4, &opt.toa_ip, 4) == 0);
  CHECK_EQ(int(opt0[8]), 2); // 原 MSS 选项后移
  CHECK(memcmp(base + 34 + 32, "GET / HTTP/1.1", 14) == 0);
  CHECK_EQ(size_t(rte_pktmbuf_data_len(m)), size_t(14 + 20 + 32 + 14));
  rte_pktmbuf_free(m);
}

TEST(toa_no_room) {
  std::vector<uint8_t> opts(36, 1); // 已有 36 字节选项（NOP），TCP 头 56 字节
  rte_mbuf *m = to_mbuf(tcp_frame(opts, "", 0x10));
  PacketMeta meta;
  ProtocolParser::parse(rte_pktmbuf_mtod(m, uint8_t *), rte_pktmbuf_data_len(m), meta);
  NatOptions opt;
  opt.add_toa = true;
  RewriteOutcome out;
  CHECK(nat_rewrite(m, meta, rw(), opt, out) == RewriteResult::OK);
  CHECK(out.toa_no_room && !out.toa_added);
  CHECK(checksums_ok(m));
  rte_pktmbuf_free(m);
}

TEST(strip_timestamp_on_syn) {
  std::vector<uint8_t> opts = {2, 4, 0x05, 0xb4, 1, 1, 8, 10, 0, 0,
                               0, 1, 0, 0, 0, 0, 1, 1, 4, 2};
  rte_mbuf *m = to_mbuf(tcp_frame(opts, "", 0x02));
  PacketMeta meta;
  ProtocolParser::parse(rte_pktmbuf_mtod(m, uint8_t *), rte_pktmbuf_data_len(m), meta);
  NatOptions opt;
  opt.strip_ts = true;
  RewriteOutcome out;
  CHECK(nat_rewrite(m, meta, rw(), opt, out) == RewriteResult::OK);
  CHECK(out.ts_stripped);
  CHECK(checksums_ok(m));
  const uint8_t *o = rte_pktmbuf_mtod_offset(m, uint8_t *, 54);
  for (int i = 6; i < 16; ++i)
    CHECK_EQ(int(o[i]), 1);
  CHECK_EQ(int(o[0]), 2);  // MSS 保留
  CHECK_EQ(int(o[18]), 4); // SACK permitted 保留
  rte_pktmbuf_free(m);
}

TEST(icmp_error_translation) {
  // 外层：路由器 -> LIP，ICMP frag-needed，内层：LIP:10007 -> RS:8080
  std::vector<uint8_t> f(14 + 20 + 8 + 20 + 8, 0);
  f[12] = 0x08;
  uint8_t *ip = &f[14];
  ip[0] = 0x45;
  ip[3] = 56;
  ip[8] = 64;
  ip[9] = 1;
  *reinterpret_cast<uint32_t *>(ip + 12) = inet_addr("10.0.0.254");
  *reinterpret_cast<uint32_t *>(ip + 16) = inet_addr("10.0.0.2");
  uint8_t *icmp = ip + 20;
  icmp[0] = 3;
  icmp[1] = 4;
  uint8_t *iip = icmp + 8;
  iip[0] = 0x45;
  iip[3] = 60;
  iip[9] = 6;
  *reinterpret_cast<uint32_t *>(iip + 12) = inet_addr("10.0.0.2");
  *reinterpret_cast<uint32_t *>(iip + 16) = inet_addr("10.0.0.11");
  *reinterpret_cast<uint16_t *>(iip + 20) = htons(10007);
  *reinterpret_cast<uint16_t *>(iip + 22) = htons(8080);
  rte_mbuf *m = to_mbuf(f);
  PacketMeta meta;
  CHECK(ProtocolParser::parse(rte_pktmbuf_mtod(m, uint8_t *),
                              rte_pktmbuf_data_len(m), meta) == ParseResult::OK);
  IcmpErrorInfo info;
  CHECK(ProtocolParser::parse_icmp_error(rte_pktmbuf_mtod(m, uint8_t *), meta, info));
  CHECK_EQ(rte_be_to_cpu_16(info.inner.src_port), 10007);
  FiveTuple client(inet_addr("1.2.3.4"), inet_addr("10.0.0.1"), htons(40000),
                   htons(80), 6);
  CHECK(nat_rewrite_icmp_error(m, meta, info, client.dst_ip, client.src_ip,
                               client, MacAddr{}, MacAddr{}) ==
        RewriteResult::OK);
  auto *base = rte_pktmbuf_mtod(m, uint8_t *);
  CHECK_EQ(csum(base + 14, 20), 0);
  CHECK_EQ(csum(base + 34, 36), 0);  // ICMP 校验和
  CHECK_EQ(csum(base + 42, 20), 0);  // 内层 IP 校验和
  CHECK_EQ(*reinterpret_cast<uint32_t *>(base + 14 + 16), inet_addr("1.2.3.4"));
  CHECK_EQ(*reinterpret_cast<uint16_t *>(base + 62), htons(40000));
  rte_pktmbuf_free(m);
}

int main(int argc, char **argv) {
  struct Init {
    static void pool() {
      g_pool = rte_pktmbuf_pool_create("ut_pool", 255, 0, 0,
                                       RTE_MBUF_DEFAULT_BUF_SIZE, SOCKET_ID_ANY);
    }
  };
  static ut::Register first("init_pool", [] {
    Init::pool();
    CHECK(g_pool != nullptr);
  });
  // init_pool 必须先于其他用例执行
  auto &r = ut::registry();
  std::rotate(r.begin(), r.end() - 1, r.end());
  return run_with_eal(argc, argv);
}
