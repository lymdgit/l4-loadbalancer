#!/usr/bin/env python3
"""Functional tests for l4lb. Usage: run_tests.py <l4lb binary> [-k substring]"""
import sys
import time
import traceback

from harness import LB
from pkt import (ACK, CLIENT, ICMP, LB_MAC, PSH, RS_IP, RS_MAC, SYN, TCP, UDP,
                 VIP, arp_request, eth, icmp_echo, ipv4, tcp, udp)

BIN = None
TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


def to_rs(frames):
    return [f for f in frames if f.proto in (TCP, UDP) and f.dst in RS_IP.values()]


def to_client(frames):
    """Frames the LB sent back towards clients (source rewritten to VIP)."""
    return [f for f in frames if f.proto in (TCP, UDP) and f.src == VIP
            and f.dst not in RS_IP.values()]


def syn(sport, dport=80, **kw):
    return eth(tcp(CLIENT, VIP, sport, dport, SYN, **kw))


def reply_from(f, flags=SYN | ACK):
    """Build the RS reply for a frame the LB sent to that RS."""
    rs_id = 1 if f.dst == RS_IP[1] else 2
    if f.proto == TCP:
        p = tcp(f.dst, f.src, f.dport, f.sport, flags)
    else:
        p = udp(f.dst, f.src, f.dport, f.sport, b"reply")
    return eth(p, src=RS_MAC[rs_id])


# ---------------------------------------------------------------------------
# 基本功能
# ---------------------------------------------------------------------------
@test
def nat_basic_roundtrip():
    """NAT: inbound rewritten to RS, reply rewritten back to client, checksums valid"""
    with LB(BIN) as lb:
        expect(lb.started(), "l4lb did not start:\n" + lb.log[-2000:])
        out = to_rs(lb.xchg([syn(40000 + i) for i in range(8)] +
                           [eth(udp(CLIENT, VIP, 50000, 80, b"hello"))]))
        expect(len(out) == 9, "expected 9 frames to RS, got %r" % out)
        for f in out:
            expect(f.src == VIP and f.dport == 80, "bad DNAT/SNAT %r" % f)
            expect(10000 <= f.sport <= 60000, "NAT port out of range %r" % f)
            expect(f.ttl == 63, "TTL not decremented %r" % f)
            expect(f.ip_csum_ok() and f.l4_csum_ok(), "bad checksum %r" % f)
            rs_id = 1 if f.dst == RS_IP[1] else 2
            expect(f.dst_mac == RS_MAC[rs_id], "wrong dst MAC %r" % f)
        expect(len({f.dst for f in out}) == 2, "consistent hash used one RS only")
        back = to_client(lb.xchg([reply_from(f) for f in out]))
        expect(len(back) == 9, "expected 9 replies, got %r" % back)
        sports = {f.sport for f in back}
        expect(sports == {80}, "reply sport should be VIP port: %r" % back)
        expect({f.dport for f in back} == set(range(40000, 40008)) | {50000},
               "reply dport mismatch %r" % back)
        for f in back:
            expect(f.src == VIP and f.ip_csum_ok() and f.l4_csum_ok(), "bad reply %r" % f)
            expect(f.dst_mac == bytes.fromhex("0200000000aa"), "reply MAC %r" % f)


@test
def dr_basic():
    """DR: only MACs rewritten, IP header untouched"""
    with LB(BIN, mode="dr") as lb:
        expect(lb.started(), "l4lb did not start")
        out = [f for f in lb.xchg([syn(40000 + i) for i in range(8)])
               if f.dst_mac in RS_MAC.values()]
        expect(len(out) == 8, "got %r" % out)
        expect(len({f.dst_mac for f in out}) == 2, "consistent hash used one RS only")
        f = out[0]
        expect(f.src == CLIENT and f.dst == VIP and f.sport == 40000 and f.ttl == 64,
               "DR must not touch IP/L4: %r" % f)
        expect(f.dst_mac in RS_MAC.values() and f.src_mac == LB_MAC, "DR MACs")


@test
def arp_and_icmp():
    """ARP request for VIP answered; ping VIP answered"""
    with LB(BIN) as lb:
        r = lb.xchg([arp_request(CLIENT, VIP)])
        expect(any(f.etype == 0x0806 and f.arp_op == 2 for f in r), "no ARP reply %r" % r)
        r = lb.xchg([eth(icmp_echo(CLIENT, VIP))])
        icmp = [f for f in r if f.proto == ICMP]
        expect(len(icmp) == 1 and icmp[0].icmp_type == 0 and icmp[0].dst == CLIENT,
               "no echo reply %r" % r)
        expect(icmp[0].ip_csum_ok() and icmp[0].l4_csum_ok(), "bad ICMP checksum")


# ---------------------------------------------------------------------------
# 1.3 畸形包 / 分片
# ---------------------------------------------------------------------------
@test
def malformed_dropped():
    """1.3: truncated / bad-header / fragment packets are dropped, LB keeps working"""
    full = tcp(CLIENT, VIP, 42000, 80, SYN)
    bad = [
        eth(full[:30]),                                      # 截断的 TCP 头
        eth(tcp(CLIENT, VIP, 42001, 80, SYN, ihl=4)),         # IHL < 5
        eth(tcp(CLIENT, VIP, 42002, 80, SYN, version=6)),     # 版本号错误
        eth(tcp(CLIENT, VIP, 42003, 80, SYN, total_len=500)), # total_length 超出帧长
        eth(full[:32] + bytes([0x20]) + full[33:]),           # TCP data offset = 2
        eth(full[:32] + bytes([0xF0]) + full[33:]),           # data offset 超出包长
        eth(tcp(CLIENT, VIP, 42004, 80, SYN, frag=0x2000)),   # 首片（MF=1）
        eth(tcp(CLIENT, VIP, 42005, 80, SYN, frag=0x0010)),   # 非首片
        eth(udp(CLIENT, VIP, 42006, 80, b"x")[:20 + 6]),      # 截断的 UDP 头
        eth(ipv4(CLIENT, VIP, ICMP, b"\x08\x00")),            # 截断的 ICMP
        eth(b"\x45"),                                         # 只有 1 字节 IP
    ]
    with LB(BIN) as lb:
        out = lb.xchg(bad)
        expect(out == [], "malformed packets must be dropped, got %r" % out)
        out = to_rs(lb.xchg([syn(42100)]))
        expect(len(out) == 1, "LB stopped forwarding after malformed input")
        lb.stop()
        expect(lb.proc.returncode == 0, "l4lb crashed (rc=%s)" % lb.proc.returncode)
        expect(lb.final_stat("Total Sessions") == 1,
               "malformed packets must not create sessions:\n" + lb.log[-1500:])


@test
def ethernet_padding_ignored():
    """1.3: short frames padded to 60 bytes keep correct L4 checksum after NAT"""
    with LB(BIN) as lb:
        p = udp(CLIENT, VIP, 43000, 80, b"a")   # 14+29 = 43 字节，需要填充
        out = to_rs(lb.xchg([eth(p) + b"\0" * 17]))
        expect(len(out) == 1 and out[0].l4_csum_ok() and out[0].ip_csum_ok(),
               "padded frame mishandled %r" % out)


# ---------------------------------------------------------------------------
# 1.6 VIP 端口过滤
# ---------------------------------------------------------------------------
@test
def service_port_filter():
    """1.6: only configured VIP ports are forwarded"""
    with LB(BIN, ports="80,8080") as lb:
        out = to_rs(lb.xchg([syn(44000, 22), syn(44001, 3306),
                             eth(udp(CLIENT, VIP, 44002, 53))]))
        expect(out == [], "non-service ports must be dropped, got %r" % out)
        out = to_rs(lb.xchg([syn(44003, 80), syn(44004, 8080)]))
        expect(len(out) == 2, "service ports must be forwarded, got %r" % out)


# ---------------------------------------------------------------------------
# 1.5 会话泄漏：转发失败时回滚
# ---------------------------------------------------------------------------
@test
def failed_forward_does_not_leak_session():
    """1.5: a new session whose first packet cannot be forwarded is rolled back"""
    with LB(BIN) as lb:
        out = lb.xchg([syn(45000 + i, ttl=1) for i in range(20)])
        expect(to_rs(out) == [], "TTL=1 must not be forwarded")
        lb.stop()
        expect(lb.final_stat("Active") == 0, "sessions leaked:\n" + lb.log[-1500:])


# ---------------------------------------------------------------------------
# 1.1 / 1.2 反向表回收
# ---------------------------------------------------------------------------
@test
def reverse_table_recycled():
    """1.1: expired sessions free their reverse-table slots (RCU reclaim)

    Several expiry rounds with session_timeout=1: every round must forward all
    new connections, match replies, and the expired sessions must be cleaned.
    """
    with LB(BIN, timeout=1) as lb:
        rounds, per_round = 4, 3000
        sport = 1024
        for r in range(rounds):
            frames = []
            for _ in range(per_round):
                frames.append(syn(sport))
                sport += 1
            lb.send(frames)
            got = to_rs(lb.recv(timeout=5, idle=0.5))
            expect(len(got) >= per_round * 0.95,
                   "round %d: only %d/%d forwarded" % (r, len(got), per_round))
            # 回程必须能查到会话
            back = to_client(lb.xchg([reply_from(f) for f in got[:200]], timeout=3))
            expect(len(back) == min(200, len(got)),
                   "round %d: %d/200 replies matched" % (r, len(back)))
            time.sleep(2.5)   # 等会话超时 + 每秒一次的清理
        lb.stop()
        log = lb.log
        expect("create fail: 0" in log, "NAT allocation failed:\n" + log[-1500:])
        cleaned = int(log.rsplit("cleaned: ", 1)[1].split()[0])
        expect(cleaned >= per_round * (rounds - 1),
               "expired sessions not cleaned (%d)" % cleaned)


@test
def reverse_table_capacity_cycles():
    """1.1: total sessions over time exceed reverse-table capacity (131072)

    Without slot reclaim the hash fills up after 128K sessions. Push 160K
    connections across expiry windows and check new ones still get NAT ports.
    """
    with LB(BIN, timeout=1) as lb:
        total, batch = 160000, 20000
        for r in range(total // batch):
            # 每轮换一个客户端 IP，保证五元组不重复
            src = "10.0.%d.%d" % (1 + r // 200, 1 + r % 200)
            frames = [eth(tcp(src, VIP, 1024 + i, 80, SYN)) for i in range(batch)]
            lb.send(frames)
            lb.recv(timeout=8, idle=0.5)
            time.sleep(2.5)
        # 最后一批：必须仍能分配端口并转发
        out = to_rs(lb.xchg([eth(tcp("10.9.9.9", VIP, 2000 + i, 80, SYN))
                             for i in range(100)], timeout=3))
        lb.stop()
        expect(len(out) >= 95, "after %d sessions only %d/100 forwarded:\n%s"
               % (total, len(out), lb.log[-1500:]))
        created = lb.final_stat("Total Sessions")
        expect(created and created > 131072,
               "test did not exceed capacity (created=%s)" % created)
        expect("create fail: 0" in lb.log, "allocation failed:\n" + lb.log[-1500:])


# ---------------------------------------------------------------------------
# 多核：1.2 并发写 / 1.4 队列独占
# ---------------------------------------------------------------------------
@test
def multicore_concurrent_sessions():
    """1.2/1.4: 4 lcores create and expire sessions concurrently without errors"""
    with LB(BIN, lcores="8-11", timeout=1) as lb:
        expect(lb.started(), "4-lcore start failed:\n" + lb.log[-2000:])
        expect("queue 3" in lb.log, "4 queues not assigned:\n" + lb.log[-2000:])
        for r in range(3):
            src = "10.1.%d.1" % r
            frames = [eth(tcp(src, VIP, 1024 + i, 80, SYN)) for i in range(5000)]
            lb.send(frames)
            got = to_rs(lb.recv(timeout=8, idle=0.5))
            expect(len(got) >= 4500, "round %d: %d/5000 forwarded" % (r, len(got)))
            back = to_client(lb.xchg([reply_from(f) for f in got[:500]], timeout=3))
            expect(len(back) == 500, "round %d: %d/500 replies" % (r, len(back)))
            # 同一 RS 上 NAT 端口不能重复（重复意味着两个会话共用一条反向表条目）
            pairs = [(f.dst, f.sport) for f in got]
            expect(len(pairs) == len(set(pairs)), "duplicate NAT ports")
            time.sleep(2.5)
        rc = lb.stop()
        expect(rc == 0, "l4lb exited with %s:\n%s" % (rc, lb.log[-1500:]))
        expect("create fail: 0" in lb.log, "allocation failed:\n" + lb.log[-1500:])


@test
def too_many_lcores_rejected():
    """1.4: more lcores than NIC queues -> refuse to start instead of sharing queues"""
    # 4 个 lcore，但网卡只给 2 个队列
    with LB(BIN, lcores="8-11", qpairs=2) as lb:
        expect(lb.proc.wait(10) != 0, "should exit with error")
        expect(not lb.started(), "should refuse to start")
        expect("NIC supports only 2 queues" in lb.read_log(),
               "missing error message:\n" + lb.log[-1500:])


@test
def clean_shutdown():
    """signal handler: SIGTERM exits cleanly with final statistics"""
    import signal
    with LB(BIN) as lb:
        lb.xchg([syn(46000)])
        rc = lb.stop(sig=signal.SIGTERM)
        expect(rc == 0, "rc=%s" % rc)
        expect("Received signal 15" in lb.log and "Final Statistics" in lb.log,
               "no clean shutdown log:\n" + lb.log[-800:])


def _register_feature_tests():
    import test_features
    for fn in test_features.ALL:
        def wrapper(fn=fn):
            fn(BIN)
        wrapper.__name__ = fn.__name__
        wrapper.__doc__ = fn.__doc__
        TESTS.append(wrapper)


_register_feature_tests()


def main():
    global BIN
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    BIN = args[0]
    sel = args[args.index("-k") + 1] if "-k" in args else ""
    failed = 0
    for t in TESTS:
        if sel and sel not in t.__name__:
            continue
        t0 = time.time()
        try:
            t()
            print("PASS  %-42s %5.1fs" % (t.__name__, time.time() - t0))
        except Exception as e:
            failed += 1
            print("FAIL  %-42s %5.1fs\n      %s" % (t.__name__, time.time() - t0,
                                                   str(e).replace("\n", "\n      ")))
            if not isinstance(e, AssertionError):
                traceback.print_exc()
    print("%d failed" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
