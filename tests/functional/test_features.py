"""Functional tests for stage 3/4 features (imported by run_tests.py)."""
import socket
import struct
import time

from harness import LB
from pkt import (ACK, CLIENT, FIN, ICMP, LB_MAC, RS_IP, RS_MAC, RST, SYN, TCP,
                 UDP, VIP, arp_reply, eth, ip4, ipv4, tcp, tcp_opts, udp, csum)

TOA_KIND = 254
TS_KIND = 8


def _rt():
    import run_tests
    return run_tests


def expect(cond, msg):
    if not cond:
        raise AssertionError(msg)


HC_SRC = "10.0.0.9"


def to_rs(frames):
    """Frames forwarded to an RS (health-check probes from HC_SRC excluded)."""
    return [f for f in frames if f.proto in (TCP, UDP) and
            f.dst in RS_IP.values() and f.src != HC_SRC]


def to_client(frames, client=None):
    return [f for f in frames if f.proto in (TCP, UDP) and
            f.dst not in RS_IP.values() and (client is None or f.dst == client)]


def rs_reply(f, flags=SYN | ACK, seq=5000, data=b""):
    rs_id = 1 if f.dst == RS_IP[1] else 2
    return eth(tcp(f.dst, f.src, f.dport, f.sport, flags, seq=seq, data=data),
               src=RS_MAC[rs_id])


def handshake(lb, sport, client=CLIENT, src_mac=None, **kw):
    """Client SYN -> RS, RS SYN-ACK -> client, client ACK -> RS. Returns the
    (syn, synack, ack) frames as seen on the wire after the LB."""
    m = {} if src_mac is None else {"src": src_mac}
    s = to_rs(lb.xchg([eth(tcp(client, VIP, sport, 80, SYN, seq=100, **kw), **m)]))
    expect(len(s) == 1, "SYN not forwarded: %r" % s)
    sa = to_client(lb.xchg([rs_reply(s[0])]))
    expect(len(sa) == 1, "SYN-ACK not returned: %r" % sa)
    a = to_rs(lb.xchg([eth(tcp(client, VIP, sport, 80, ACK, seq=101, ack=5001), **m)]))
    expect(len(a) == 1, "ACK not forwarded: %r" % a)
    return s[0], sa[0], a[0]


# ---------------------------------------------------------------------------
# 2.3 TCP 状态机
# ---------------------------------------------------------------------------
def tcp_state_machine(binary):
    """2.3: only SYN creates a session; RST ends it; retransmitted SYN reuses it"""
    with LB(binary) as lb:
        out = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 47000, 80, ACK)),
                             eth(tcp(CLIENT, VIP, 47001, 80, PSH_ACK, data=b"x")),
                             eth(tcp(CLIENT, VIP, 47002, 80, RST))]))
        expect(out == [], "non-SYN without session must be dropped: %r" % out)

        a = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 47010, 80, SYN))]))
        b = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 47010, 80, SYN))]))  # 重传
        expect(len(a) == 1 and len(b) == 1 and a[0].sport == b[0].sport and
               a[0].dst == b[0].dst, "retransmitted SYN must reuse session")
        lb.xchg([eth(tcp(CLIENT, VIP, 47010, 80, RST))])
        c = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 47010, 80, SYN))]))
        expect(len(c) == 1 and c[0].sport != a[0].sport,
               "SYN after RST must create a new session (new NAT port)")
        lb.stop()
        expect(lb.final_stat("Total Sessions") == 2, lb.log[-1200:])
        expect("drop_no_session: 3" in lb.log, lb.log[-1200:])


PSH_ACK = 0x18


# ---------------------------------------------------------------------------
# 2.2 LIP + 多服务
# ---------------------------------------------------------------------------
MULTI_CONF = """
[global]
mode = nat
[network]
local_ips = 10.0.0.2, 10.0.0.3
[service.web]
vip = 10.0.0.1
port = 80
scheduler = maglev
server1 = 10.0.0.11:8080:1:02:00:00:00:00:11
[service.dns]
vip = 10.0.0.1
port = 53
proto = udp
server1 = 10.0.0.12:5353:1:02:00:00:00:00:12
"""


def local_ips_and_services(binary):
    """2.2: SNAT source is a LIP (round-robin), services map to their own RS:port"""
    with LB(binary, conf=MULTI_CONF) as lb:
        out = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 48000 + i, 80, SYN)) for i in range(6)] +
                            [eth(udp(CLIENT, VIP, 48100, 53, b"q"))]))
        tcp_out = [f for f in out if f.proto == TCP]
        udp_out = [f for f in out if f.proto == UDP]
        expect(len(tcp_out) == 6 and len(udp_out) == 1, "got %r" % out)
        expect({f.src for f in tcp_out} == {"10.0.0.2", "10.0.0.3"},
               "LIPs not used round-robin: %r" % tcp_out)
        expect(all(f.dport == 8080 and f.dst == RS_IP[1] for f in tcp_out),
               "web must go to RS1:8080")
        expect(udp_out[0].dport == 5353 and udp_out[0].dst == RS_IP[2],
               "dns must go to RS2:5353")
        back = to_client(lb.xchg([rs_reply(f) for f in tcp_out]))
        expect(len(back) == 6 and all(f.src == VIP and f.sport == 80 for f in back),
               "replies via LIP must come back from VIP:80: %r" % back)
        # 没有配置的端口、LIP 本身的服务端口都不转发
        expect(to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 48200, 443, SYN)),
                              eth(tcp(CLIENT, "10.0.0.2", 48201, 80, SYN))])) == [],
               "unknown service forwarded")
        # LIP 也要应答 ARP
        from pkt import arp_request
        r = lb.xchg([arp_request(CLIENT, "10.0.0.3")])
        expect(any(f.etype == 0x0806 and f.arp_op == 2 for f in r), "LIP ARP")


# ---------------------------------------------------------------------------
# 2.5 TOA / timestamp
# ---------------------------------------------------------------------------
def toa_and_timestamp(binary):
    """2.5: TOA carries client ip:port after the handshake; SYN timestamp removed"""
    conf = MULTI_CONF.replace("mode = nat", "mode = nat\ntoa = on")
    ts = bytes([1, 1, 8, 10]) + struct.pack("!II", 7, 0)  # NOP NOP TS
    with LB(binary, conf=conf) as lb:
        syn, synack, ack = handshake(lb, 49000, opts=ts)
        expect(TS_KIND not in [k for k, _ in tcp_opts(syn)],
               "timestamp not stripped: %r" % tcp_opts(syn))
        expect(syn.l4_csum_ok() and ack.l4_csum_ok(), "bad checksum after option edit")
        toa = [v for k, v in tcp_opts(ack) if k == TOA_KIND]
        expect(len(toa) == 1, "no TOA on third ACK: %r" % tcp_opts(ack))
        port, = struct.unpack("!H", toa[0][:2])
        expect(port == 49000 and socket.inet_ntoa(toa[0][2:6]) == CLIENT,
               "TOA content wrong: %r" % toa[0])
        # RS 回了数据之后不再插入
        lb.xchg([eth(tcp(RS_IP[1], syn.src, 8080, syn.sport, PSH_ACK, seq=5001,
                         data=b"hi"), src=RS_MAC[1])])
        d = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 49000, 80, PSH_ACK, seq=101,
                                   data=b"more"))]))
        expect(len(d) == 1 and not [k for k, _ in tcp_opts(d[0]) if k == TOA_KIND],
               "TOA must stop after RS answered")
        expect(d[0].payload == b"more", "payload corrupted")


# ---------------------------------------------------------------------------
# 2.5 ICMP 差错
# ---------------------------------------------------------------------------
def icmp_errors(binary):
    """2.5: ICMP errors referring to a session are translated and forwarded"""
    with LB(binary, conf=MULTI_CONF) as lb:
        syn, _, _ = handshake(lb, 49100)
        lip = syn.src
        # 路由器 -> LIP：frag-needed，内层是 LB 发给 RS 的包
        inner = tcp(lip, RS_IP[1], syn.sport, 8080, ACK)[:28]
        body = struct.pack("!BBHHH", 3, 4, 0, 0, 1400) + inner
        body = body[:2] + struct.pack("!H", csum(body)) + body[4:]
        out = lb.xchg([eth(ipv4("10.0.0.254", lip, ICMP, body), src=RS_MAC[1])])
        icmp = [f for f in out if f.proto == ICMP]
        expect(len(icmp) == 1, "ICMP error not forwarded to client: %r" % out)
        f = icmp[0]
        expect(f.src == VIP and f.dst == CLIENT and f.l4_csum_ok(), "outer wrong")
        iip = f.l4[8:28]
        expect(socket.inet_ntoa(iip[12:16]) == CLIENT and
               socket.inet_ntoa(iip[16:20]) == VIP and csum(iip) == 0,
               "inner IP not translated")
        sp, dp = struct.unpack("!HH", f.l4[28:32])
        expect((sp, dp) == (49100, 80), "inner ports not translated: %d %d" % (sp, dp))

        # 客户端侧 -> VIP：差错针对 VIP:80 -> Client:49100，转换后发给 RS
        inner = tcp(VIP, CLIENT, 80, 49100, ACK)[:28]
        body = struct.pack("!BBHHH", 3, 3, 0, 0, 0) + inner
        body = body[:2] + struct.pack("!H", csum(body)) + body[4:]
        out = lb.xchg([eth(ipv4("10.0.0.254", VIP, ICMP, body))])
        icmp = [f for f in out if f.proto == ICMP]
        expect(len(icmp) == 1 and icmp[0].dst == RS_IP[1] and icmp[0].src == lip,
               "client-side ICMP error not forwarded to RS: %r" % out)
        sp, dp = struct.unpack("!HH", icmp[0].l4[28:32])
        expect((sp, dp) == (8080, syn.sport), "inner ports for RS: %d %d" % (sp, dp))
        # 不属于任何会话的差错丢弃
        inner = tcp(lip, RS_IP[1], 12345, 8080, ACK)[:28]
        body = struct.pack("!BBHHH", 3, 4, 0, 0, 1400) + inner
        body = body[:2] + struct.pack("!H", csum(body)) + body[4:]
        expect([f for f in lb.xchg([eth(ipv4("10.0.0.254", lip, ICMP, body))])
                if f.proto == ICMP] == [], "unknown ICMP error forwarded")


# ---------------------------------------------------------------------------
# 2.4 邻居与路由
# ---------------------------------------------------------------------------
def arp_frames(frames, target=None):
    return [f for f in frames if f.etype == 0x0806 and f.arp_op == 1 and
            (target is None or f.arp_target_ip == target)]


def neighbor_resolution(binary):
    """2.4: RS without static MAC is resolved by ARP; no broadcast data frames;
    gratuitous ARP for VIP at startup"""
    conf = MULTI_CONF.replace("server1 = 10.0.0.11:8080:1:02:00:00:00:00:11",
                              "server1 = 10.0.0.11:8080:1")
    with LB(binary, conf=conf) as lb:
        frames = lb.startup_frames + lb.recv(timeout=2.5, idle=2.5)
        garp = [f for f in frames if f.etype == 0x0806 and
                f.arp_sender_ip == f.arp_target_ip]
        expect({f.arp_target_ip for f in garp} >= {VIP, "10.0.0.2", "10.0.0.3"},
               "no gratuitous ARP for VIP/LIPs")
        expect(arp_frames(frames, RS_IP[1]), "no ARP request for RS: %r" % frames)
        # 未解析时不转发、也不广播
        out = lb.xchg([eth(tcp(CLIENT, VIP, 49200, 80, SYN))])
        expect(not [f for f in out if f.proto == TCP], "forwarded without MAC: %r" % out)
        lb.xchg([arp_reply(RS_IP[1], RS_MAC[1], "10.0.0.2")])
        out = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 49201, 80, SYN))]))
        expect(len(out) == 1 and out[0].dst_mac == RS_MAC[1], "not using ARP result")


GW_MAC = bytes.fromhex("0200000000fe")


def gateway_routing(binary):
    """2.4: off-link clients are reached via the gateway, not via their src MAC"""
    conf = MULTI_CONF.replace("[network]", "[network]\nnetmask = 255.255.255.0\n"
                              "gateway = 10.0.0.254")
    with LB(binary, conf=conf) as lb:
        frames = lb.startup_frames + lb.recv(timeout=2.5, idle=2.5)
        expect(arp_frames(frames, "10.0.0.254"), "gateway not resolved: %r" % frames)
        lb.xchg([arp_reply("10.0.0.254", GW_MAC, "10.0.0.2")])
        far = "192.168.9.9"
        syn = to_rs(lb.xchg([eth(tcp(far, VIP, 49300, 80, SYN), src=GW_MAC)]))
        expect(len(syn) == 1, "SYN from off-link client not forwarded")
        back = to_client(lb.xchg([rs_reply(syn[0])]), far)
        expect(len(back) == 1 and back[0].dst_mac == GW_MAC,
               "reply must go to the gateway MAC: %r" % back)


# ---------------------------------------------------------------------------
# 2.5 健康检查
# ---------------------------------------------------------------------------
HC_CONF = """
[global]
mode = nat
[network]
hc_src = 10.0.0.9
[healthcheck]
enabled = yes
interval = 1
timeout = 400
failure_threshold = 2
success_threshold = 2
[service.web]
vip = 10.0.0.1
port = 80
server1 = 10.0.0.11:80:1:02:00:00:00:00:11
server2 = 10.0.0.12:80:1:02:00:00:00:00:12
"""


def _answer_probes(lb, seconds, rs2_up):
    """Act as both RS: answer health-check SYNs from hc_src."""
    end = time.time() + seconds
    probes = 0
    while time.time() < end:
        for f in lb.recv(timeout=0.2, idle=0.2):
            if f.proto == TCP and f.src == "10.0.0.9" and f.flags & SYN:
                probes += 1
                rs = 1 if f.dst == RS_IP[1] else 2
                if rs == 2 and not rs2_up:
                    flags, ack = RST | ACK, f.seq + 1
                else:
                    flags, ack = SYN | ACK, f.seq + 1
                lb.send([eth(tcp(f.dst, f.src, f.dport, f.sport, flags, seq=777,
                                 ack=ack), src=RS_MAC[rs])])
    return probes


def health_check(binary):
    """2.5: failing RS is removed from scheduling and comes back when healthy"""
    with LB(binary, conf=HC_CONF) as lb:
        probes = _answer_probes(lb, 4.0, rs2_up=False)
        expect(probes >= 4, "too few probes: %d" % probes)
        svc = lb.ctl("services")
        expect("10.0.0.12:80 weight 1 enabled down" in svc and
               "10.0.0.11:80 weight 1 enabled up" in svc, svc)
        out = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 49400 + i, 80, SYN)) for i in range(20)]))
        expect(len(out) == 20 and {f.dst for f in out} == {RS_IP[1]},
               "DOWN RS still scheduled: %r" % {f.dst for f in out})
        _answer_probes(lb, 4.0, rs2_up=True)
        svc = lb.ctl("services")
        expect("10.0.0.12:80 weight 1 enabled up" in svc, svc)
        out = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 49500 + i, 80, SYN)) for i in range(20)]))
        expect({f.dst for f in out} == set(RS_IP.values()), "RS not back in rotation")
        lb.stop()
        expect("is DOWN" in lb.log and "is UP" in lb.log, lb.log[-1500:])


# ---------------------------------------------------------------------------
# 2.5 控制面
# ---------------------------------------------------------------------------
def control_plane(binary):
    """2.5: weight 0 drains, disable cuts, add/del RS at runtime"""
    with LB(binary, conf=HC_CONF.replace("enabled = yes", "enabled = no")) as lb:
        svc = lb.ctl("services")
        expect("rs 1 10.0.0.11:80" in svc and "rs 2 10.0.0.12:80" in svc, svc)
        # 先建一条到 RS2 的连接
        sport, conn = 49600, None
        while conn is None:
            s = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, sport, 80, SYN))]))
            if s and s[0].dst == RS_IP[2]:
                conn = (sport, s[0])
            sport += 1
        expect(lb.ctl("weight 2 0").startswith("ok"), "weight cmd failed")
        out = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, 49700 + i, 80, SYN)) for i in range(10)]))
        expect({f.dst for f in out} == {RS_IP[1]}, "weight 0 RS still gets new conns")
        old = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, conn[0], 80, ACK))]))
        expect(len(old) == 1 and old[0].dst == RS_IP[2], "drain must keep old conns")
        expect(lb.ctl("disable 2").startswith("ok"), "disable failed")
        old = to_rs(lb.xchg([eth(tcp(CLIENT, VIP, conn[0], 80, ACK))]))
        expect(old == [], "disabled RS must not get packets")

        r = lb.ctl("add 0 10.0.0.13:80:5")
        expect(r.startswith("ok rs 3"), r)
        lb.ctl("del 1")
        new_rs = "10.0.0.13"
        frames = lb.recv(timeout=2.5, idle=2.5)
        expect(arp_frames(frames, new_rs), "added RS not resolved")
        mac13 = bytes.fromhex("020000000013")
        lb.xchg([arp_reply(new_rs, mac13, "10.0.0.1")])
        out = [f for f in lb.xchg([eth(tcp(CLIENT, VIP, 49800 + i, 80, SYN))
                                   for i in range(5)]) if f.proto == TCP]
        dst = {f.dst for f in out}
        expect(len(out) == 5 and dst == {new_rs}, "new RS not used: %r" % out)
        expect(all(f.dst_mac == mac13 for f in out), "wrong MAC for new RS")
        expect(lb.ctl("bogus").startswith("error"), "bad command accepted")
        expect("Sessions: active" in lb.ctl("stats"), "stats")


ALL = [tcp_state_machine, local_ips_and_services, toa_and_timestamp, icmp_errors,
       neighbor_resolution, gateway_routing, health_check, control_plane]
