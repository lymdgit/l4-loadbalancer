"""Start l4lb on a veth pair (net_af_packet) and exchange frames with it."""
import os
import re
import select
import signal
import socket
import subprocess
import tempfile
import time

from pkt import Frame

IFACE_LB, IFACE_PEER = "l4t0", "l4t1"

BASE_CONF = """
[global]
mode = {mode}
session_timeout = {timeout}

[vip]
ip = 10.0.0.1
ports = {ports}
mac = 02:00:00:00:00:01

[realserver]
count = {rs_count}
server1 = 10.0.0.11:80:50:{rs1_mac}
server2 = 10.0.0.12:80:50:02:00:00:00:00:12
"""


def sh(cmd):
    subprocess.run(cmd, shell=True, check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


class LB:
    """One l4lb process bound to a fresh veth pair."""

    def __init__(self, binary, mode="nat", ports="80", timeout=300, lcores="11",
                 rs1_mac="02:00:00:00:00:11", rs_count=2, extra_args=(),
                 qpairs=None, conf=None):
        self.binary = binary
        self.ctl_path = "/tmp/l4t%d.sock" % os.getpid()
        self.conf = conf if conf is not None else BASE_CONF.format(
            mode=mode, ports=ports, timeout=timeout, rs1_mac=rs1_mac,
            rs_count=rs_count)
        if "[control]" not in self.conf:
            self.conf += "\n[control]\nsocket = %s\n" % self.ctl_path
        self.lcores = lcores
        self.extra_args = list(extra_args)
        # 默认每个 lcore 一对队列；af_packet 多队列用 PACKET_FANOUT_HASH 分流
        self.qpairs = qpairs or len(expand_lcores(lcores))
        self.proc = None
        self.log = ""

    def __enter__(self):
        sh("ip link del %s 2>/dev/null || true" % IFACE_LB)
        sh("ip link add %s type veth peer name %s" % (IFACE_LB, IFACE_PEER))
        for i in (IFACE_LB, IFACE_PEER):
            sh("sysctl -qw net.ipv6.conf.%s.disable_ipv6=1" % i)
            sh("ip link set %s up" % i)
        fd, self.conf_path = tempfile.mkstemp(suffix=".conf")
        os.write(fd, self.conf.encode())
        os.close(fd)
        self.log_file = tempfile.NamedTemporaryFile(suffix=".log", delete=False)
        cmd = [self.binary, "-l", self.lcores, "--in-memory", "--no-pci",
               "--file-prefix", "l4t%d" % os.getpid(), "-m", "1024",
               "--vdev=net_af_packet0,iface=%s,qpairs=%d,framecnt=32768" % (IFACE_LB, self.qpairs),
               "--", "--lb-config", self.conf_path] + self.extra_args
        self.proc = subprocess.Popen(cmd, stdout=self.log_file,
                                     stderr=subprocess.STDOUT)
        deadline = time.time() + 15
        while time.time() < deadline:
            if "L4 Load Balancer is running" in self.read_log():
                break
            if self.proc.poll() is not None:
                break
            time.sleep(0.1)
        self.sock = socket.socket(socket.AF_PACKET, socket.SOCK_RAW,
                                  socket.htons(0x0003))
        self.sock.setsockopt(socket.SOL_SOCKET, 33, 64 << 20)  # SO_RCVBUFFORCE
        self.sock.setsockopt(socket.SOL_SOCKET, 32, 16 << 20)  # SO_SNDBUFFORCE
        self.sock.bind((IFACE_PEER, 0))
        self.sock.setblocking(False)
        self.startup_frames = self.recv(timeout=0.3, idle=0.3)
        return self

    def started(self):
        return "L4 Load Balancer is running" in self.read_log()

    def read_log(self):
        with open(self.log_file.name, errors="replace") as f:
            self.log = f.read()
        return self.log

    def send(self, frames, gap=0.0):
        for f in frames:
            while True:
                try:
                    self.sock.send(f)
                    break
                except BlockingIOError:
                    time.sleep(0.001)
            if gap:
                time.sleep(gap)

    def recv(self, timeout=1.0, idle=0.3):
        """Collect frames emitted by the LB until `idle` seconds of silence."""
        got = []
        deadline = time.time() + timeout
        last = time.time()
        while time.time() < deadline and time.time() - last < idle:
            if select.select([self.sock], [], [], 0.05)[0]:
                while True:
                    try:
                        raw, addr = self.sock.recvfrom(65535)
                    except BlockingIOError:
                        break
                    if addr[2] == socket.PACKET_OUTGOING:
                        continue
                    got.append(Frame(raw))
                    last = time.time()
        return got

    def drain(self, t=0.2):
        self.recv(timeout=t, idle=t)

    def xchg(self, frames, timeout=1.0, gap=0.0):
        self.send(frames, gap)
        return self.recv(timeout=timeout)

    def stop(self, sig=signal.SIGINT, timeout=10):
        if self.proc and self.proc.poll() is None:
            self.proc.send_signal(sig)
            try:
                self.proc.wait(timeout)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.read_log()
        return self.proc.returncode

    def __exit__(self, *exc):
        self.stop()
        self.sock.close()
        self.read_log()
        os.unlink(self.conf_path)
        os.unlink(self.log_file.name)
        sh("ip link del %s 2>/dev/null || true" % IFACE_LB)

    def ctl(self, cmd):
        """Run a control command over the unix socket, return the reply."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self.ctl_path)
        s.sendall((cmd + "\n").encode())
        out = b""
        while True:
            c = s.recv(65536)
            if not c:
                break
            out += c
        s.close()
        return out.decode()

    def final_stat(self, name):
        """Parse a counter from the 'Final Statistics' block."""
        m = re.findall(r"%s: (\d+)" % re.escape(name), self.read_log())
        return int(m[-1]) if m else None


def expand_lcores(spec):
    out = []
    for part in spec.split(","):
        if "-" in part:
            a, b = part.split("-")
            out.extend(range(int(a), int(b) + 1))
        else:
            out.append(int(part))
    return out
