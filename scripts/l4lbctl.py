#!/usr/bin/env python3
"""l4lb 控制命令客户端。

用法：
  l4lbctl.py [-s /run/l4lb.sock] <command...>
  l4lbctl.py stats               # 汇总；stats -v 按 worker；stats -r 按服务 / RS
  l4lbctl.py services
  l4lbctl.py weight 3 0          # RS 3 不再接新连接（排空）
  l4lbctl.py add 0 10.0.0.13:80:100
  l4lbctl.py counters            # 全部原始计数（key value）

PPS / bps（docs/pps方案.md）：压测前后各取一次计数相减
  l4lbctl.py counters > before.txt   # 压测前
  l4lbctl.py delta before.txt        # 压测后：当前计数 - before.txt
  l4lbctl.py delta a.txt b.txt       # 两个文件相减
  l4lbctl.py delta --watch           # 现在开始，Ctrl+C 结束
  l4lbctl.py delta before.txt --csv  # 一行 CSV（脚本用，--csv-header 输出表头）
"""
import argparse
import signal
import socket
import sys


def request(path, command):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(path)
    s.sendall((command + "\n").encode())
    out = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        out += chunk
    s.close()
    return out.decode(errors="replace")


def parse_counters(text):
    """counters 输出 -> dict；数值转 int，其余（地址、名字）保留字符串"""
    out = {}
    for line in text.splitlines():
        k, _, v = line.partition(" ")
        if not k or not v:
            continue
        try:
            out[k] = int(v)
        except ValueError:
            out[k] = v
    if "time_ns" not in out:
        raise ValueError("not a counters snapshot (missing time_ns)")
    return out


# ---------------------------------------------------------------------------
# 差值计算
# ---------------------------------------------------------------------------
def human(v, unit=""):
    for div, suf in ((1e9, "G"), (1e6, "M"), (1e3, "k")):
        if abs(v) >= div:
            return "%.2f%s%s" % (v / div, suf, unit)
    return "%.0f%s" % (v, unit)


def human_bytes(v):
    for div, suf in ((1 << 30, "GiB"), (1 << 20, "MiB"), (1 << 10, "KiB")):
        if v >= div:
            return "%.2f %s" % (v / div, suf)
    return "%d B" % v


class Delta:
    def __init__(self, a, b):
        if b["time_ns"] < a["time_ns"]:
            a, b = b, a
        if b.get("uptime_ns", 0) < a.get("uptime_ns", 0):
            raise ValueError("l4lb restarted between the two snapshots")
        self.a, self.b = a, b
        self.sec = (b["time_ns"] - a["time_ns"]) / 1e9

    def d(self, key):
        va, vb = self.a.get(key, 0), self.b.get(key, 0)
        if not isinstance(va, int) or not isinstance(vb, int):
            return 0
        return vb - va

    def rate(self, key):
        return self.d(key) / self.sec if self.sec > 0 else 0.0

    def bps(self, key):
        return self.d(key) * 8 / self.sec if self.sec > 0 else 0.0

    def rs_ids(self):
        ids = {k.split(".")[1] for k in self.b if k.startswith("rs.") and k.endswith(".addr")}
        return sorted(ids, key=int)

    def svc_ids(self):
        ids = {k.split(".")[1] for k in self.b if k.startswith("svc.") and k.endswith(".addr")}
        return sorted(ids, key=int)

    # FULLNAT 每个请求两个方向都经过 LB，转发 PPS = in + out；DR 只有 in
    def fwd_pps(self):
        return self.rate("fwd_in_pkts") + self.rate("fwd_out_pkts")

    def fwd_bps(self):
        return self.bps("fwd_in_bytes") + self.bps("fwd_out_bytes")

    def report(self):
        L = []
        L.append("interval %.2f s  (%s, %s, %s workers)" % (
            self.sec, self.b.get("mode", "?"), self.b.get("dataplane", "?"),
            self.b.get("workers", "?")))
        L.append("%-12s %14s %10s %12s %14s" % ("", "packets", "pps", "bytes", "bps"))
        rows = [("nic rx", "nic_ipackets", "nic_ibytes"),
                ("nic tx", "nic_opackets", "nic_obytes"),
                ("fwd in", "fwd_in_pkts", "fwd_in_bytes"),
                ("fwd out", "fwd_out_pkts", "fwd_out_bytes")]
        for name, pk, bk in rows:
            L.append("%-12s %14s %10s %12s %14s" % (
                name, "{:,}".format(self.d(pk)), human(self.rate(pk)),
                human_bytes(self.d(bk)), human(self.bps(bk), "bps")))
        fwd_pkts = self.d("fwd_in_pkts") + self.d("fwd_out_pkts")
        fwd_bytes = self.d("fwd_in_bytes") + self.d("fwd_out_bytes")
        L.append("%-12s %14s %10s %12s %14s  (= in + out)" % (
            "total fwd", "{:,}".format(fwd_pkts), human(self.fwd_pps()),
            human_bytes(fwd_bytes), human(self.fwd_bps(), "bps")))
        L.append("new conns    %14s %10s /s   active now %s" % (
            "{:,}".format(self.d("conns_new")), human(self.rate("conns_new")),
            "{:,}".format(self.b.get("conns_active", 0))))
        threads = sorted({k.split(".")[1] for k in self.b
                          if k.startswith("thread.") and k.endswith(".busy_ns")},
                         key=lambda n: (n != "receiver", n))
        if threads and self.sec > 0:
            parts = []
            for t in threads:
                busy = self.d("thread.%s.busy_ns" % t) / 1e9
                pkts = self.d("thread.%s.pkts" % t)
                # 每包耗时：只算忙碌时间，空轮询不计入
                ns = busy * 1e9 / pkts if pkts else 0
                parts.append("%s %.1f%% (%s pps, %.0f ns/pkt)" % (
                    t, 100 * busy / self.sec, human(pkts / self.sec), ns))
            L.append("busy: " + " | ".join(parts))
        L.append("drops %s  imissed %s  ierrors %s  rx_nombuf %s  rx_ring_full %s  "
                 "tx_full %s" % (self.d("drops"), self.d("nic_imissed"),
                                 self.d("nic_ierrors"), self.d("nic_rx_nombuf"),
                                 self.d("stat.drop_rx_ring"), self.d("stat.tx_full")))
        for sid in self.svc_ids():
            k = "svc.%s." % sid
            L.append("service %s %s %s   conns %s  in %s pps  out %s pps" % (
                sid, self.b.get(k + "name", ""), self.b.get(k + "addr", ""),
                "{:,}".format(self.d(k + "conns")), human(self.rate(k + "pkts_in")),
                human(self.rate(k + "pkts_out"))))
            for rid in self.rs_ids():
                r = "rs.%s." % rid
                if str(self.b.get(r + "svc")) != sid:
                    continue
                L.append("  rs %-4s %-22s conns %-10s in %-9s out %-9s pps  %s" % (
                    rid, self.b.get(r + "addr", ""), "{:,}".format(self.d(r + "conns")),
                    human(self.rate(r + "pkts_in")), human(self.rate(r + "pkts_out")),
                    human(self.bps(r + "bytes_in") + self.bps(r + "bytes_out"), "bps")))
        return "\n".join(L) + "\n"

    CSV_FIELDS = ["interval_s", "fwd_pps", "fwd_bps", "fwd_in_pps", "fwd_out_pps",
                  "nic_rx_pps", "nic_tx_pps", "nic_rx_bps", "nic_tx_bps", "cps",
                  "drops", "imissed"]

    def csv(self):
        v = [self.sec, self.fwd_pps(), self.fwd_bps(), self.rate("fwd_in_pkts"),
             self.rate("fwd_out_pkts"), self.rate("nic_ipackets"),
             self.rate("nic_opackets"), self.bps("nic_ibytes"), self.bps("nic_obytes"),
             self.rate("conns_new"), self.d("drops"), self.d("nic_imissed")]
        # interval 保留两位小数，其余取整
        return ",".join(["%.2f" % v[0]] + ["%.0f" % x for x in v[1:]])


def cmd_delta(sock, argv):
    ap = argparse.ArgumentParser(prog="l4lbctl.py delta")
    ap.add_argument("files", nargs="*", help="before [after]; after defaults to now")
    ap.add_argument("--watch", action="store_true", help="start now, stop on Ctrl+C")
    ap.add_argument("--csv", action="store_true", help="one CSV line")
    ap.add_argument("--csv-header", action="store_true", help="print the CSV header")
    args = ap.parse_args(argv)
    if args.csv_header:
        print(",".join(Delta.CSV_FIELDS))
        return 0

    def snap():
        return parse_counters(request(sock, "counters"))

    def load(path):
        with open(path) as f:
            return parse_counters(f.read())

    if args.watch:
        a = snap()
        print("counting... press Ctrl+C to stop", file=sys.stderr)
        try:
            signal.pause()
        except KeyboardInterrupt:
            pass
        b = snap()
    elif len(args.files) == 1:
        a, b = load(args.files[0]), snap()
    elif len(args.files) == 2:
        a, b = load(args.files[0]), load(args.files[1])
    else:
        ap.error("give a before file, two files, or --watch")
    d = Delta(a, b)
    sys.stdout.write(d.csv() + "\n" if args.csv else d.report())
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-s", "--socket", default="/run/l4lb.sock")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="command and its arguments (e.g. stats -v)")
    args = ap.parse_args()
    if not args.command:
        args.command = ["help"]
    try:
        if args.command[0] == "delta":
            return cmd_delta(args.socket, args.command[1:])
        text = request(args.socket, " ".join(args.command))
    except OSError as e:
        print("cannot connect to %s: %s" % (args.socket, e), file=sys.stderr)
        return 1
    except ValueError as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    sys.stdout.write(text)
    return 1 if text.startswith("error") else 0


if __name__ == "__main__":
    sys.exit(main())
