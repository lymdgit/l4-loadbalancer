"""Minimal packet builders / parsers for the functional tests (no scapy)."""
import socket
import struct

LB_MAC = bytes.fromhex("020000000001")
CLI_MAC = bytes.fromhex("0200000000aa")
RS_MAC = {1: bytes.fromhex("020000000011"), 2: bytes.fromhex("020000000012")}
VIP = "10.0.0.1"
CLIENT = "10.0.0.100"
RS_IP = {1: "10.0.0.11", 2: "10.0.0.12"}

TCP, UDP, ICMP = 6, 17, 1
SYN, ACK, PSH, FIN, RST = 0x02, 0x10, 0x08, 0x01, 0x04


def ip4(a):
    return socket.inet_aton(a)


def csum(b):
    if len(b) % 2:
        b += b"\0"
    s = sum(struct.unpack("!%dH" % (len(b) // 2), b))
    while s >> 16:
        s = (s & 0xFFFF) + (s >> 16)
    return (~s) & 0xFFFF


def ipv4(src, dst, proto, payload, ttl=64, ident=1, frag=0x4000, ihl=5,
         total_len=None, version=4):
    tl = total_len if total_len is not None else ihl * 4 + len(payload)
    h = struct.pack("!BBHHHBBH4s4s", (version << 4) | ihl, 0, tl, ident, frag,
                    ttl, proto, 0, ip4(src), ip4(dst))
    h += b"\0" * (ihl * 4 - 20)
    h = h[:10] + struct.pack("!H", csum(h)) + h[12:]
    return h + payload


def l4_csum(src, dst, proto, seg, off):
    ph = struct.pack("!4s4sBBH", ip4(src), ip4(dst), 0, proto, len(seg))
    c = csum(ph + seg[:off] + b"\0\0" + seg[off + 2:])
    if proto == UDP and c == 0:
        c = 0xFFFF
    return seg[:off] + struct.pack("!H", c) + seg[off + 2:]


def tcp(src, dst, sp, dp, flags=SYN, seq=1000, data=b"", **ipkw):
    seg = struct.pack("!HHIIBBHHH", sp, dp, seq, 0, 5 << 4, flags, 65535, 0, 0) + data
    return ipv4(src, dst, TCP, l4_csum(src, dst, TCP, seg, 16), **ipkw)


def udp(src, dst, sp, dp, data=b"x", **ipkw):
    seg = struct.pack("!HHHH", sp, dp, 8 + len(data), 0) + data
    return ipv4(src, dst, UDP, l4_csum(src, dst, UDP, seg, 6), **ipkw)


def icmp_echo(src, dst, data=b"pingdata" * 4):
    body = struct.pack("!BBHHH", 8, 0, 0, 0x1234, 1) + data
    body = body[:2] + struct.pack("!H", csum(body)) + body[4:]
    return ipv4(src, dst, ICMP, body)


def eth(payload, etype=0x0800, src=CLI_MAC, dst=LB_MAC):
    return dst + src + struct.pack("!H", etype) + payload


def arp_request(sender_ip, target_ip, sender_mac=CLI_MAC):
    a = struct.pack("!HHBBH6s4s6s4s", 1, 0x0800, 6, 4, 1, sender_mac,
                    ip4(sender_ip), b"\0" * 6, ip4(target_ip))
    return eth(a, 0x0806, src=sender_mac, dst=b"\xff" * 6)


class Frame:
    """Parsed view of a frame emitted by the LB."""

    def __init__(self, raw):
        self.raw = raw
        self.dst_mac, self.src_mac = raw[0:6], raw[6:12]
        self.etype = struct.unpack("!H", raw[12:14])[0]
        self.proto = None
        if self.etype == 0x0800:
            ip = raw[14:]
            ihl = (ip[0] & 0xF) * 4
            self.ip_hdr = ip[:ihl]
            self.ttl, self.proto = ip[8], ip[9]
            self.src = socket.inet_ntoa(ip[12:16])
            self.dst = socket.inet_ntoa(ip[16:20])
            tl = struct.unpack("!H", ip[2:4])[0]
            self.l4 = ip[ihl:tl]
            if self.proto in (TCP, UDP):
                self.sport, self.dport = struct.unpack("!HH", self.l4[:4])
            if self.proto == ICMP:
                self.icmp_type = self.l4[0]
        elif self.etype == 0x0806:
            self.arp_op = struct.unpack("!H", raw[20:22])[0]
            self.arp_target_ip = socket.inet_ntoa(raw[38:42])

    def ip_csum_ok(self):
        return csum(self.ip_hdr) == 0

    def l4_csum_ok(self):
        if self.proto == UDP and self.l4[6:8] == b"\0\0":
            return True
        if self.proto == ICMP:   # ICMP 校验和不含伪首部
            return csum(self.l4) == 0
        ph = struct.pack("!4s4sBBH", ip4(self.src), ip4(self.dst), 0,
                         self.proto, len(self.l4))
        return csum(ph + self.l4) == 0

    def __repr__(self):
        if self.proto in (TCP, UDP):
            return "<%s %s:%d -> %s:%d ttl=%d>" % (
                "TCP" if self.proto == TCP else "UDP", self.src, self.sport,
                self.dst, self.dport, self.ttl)
        return "<frame etype=0x%04x proto=%s>" % (self.etype, self.proto)
