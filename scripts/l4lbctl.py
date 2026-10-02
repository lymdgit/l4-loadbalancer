#!/usr/bin/env python3
"""l4lb 控制命令客户端。

用法：
  l4lbctl.py [-s /run/l4lb.sock] <command...>
  l4lbctl.py stats
  l4lbctl.py services
  l4lbctl.py weight 3 0          # RS 3 不再接新连接（排空）
  l4lbctl.py add 0 10.0.0.13:80:100
"""
import argparse
import socket
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-s", "--socket", default="/run/l4lb.sock")
    ap.add_argument("command", nargs=argparse.REMAINDER,
                    help="command and its arguments (e.g. stats -v)")
    args = ap.parse_args()
    if not args.command:
        args.command = ["help"]
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    try:
        s.connect(args.socket)
    except OSError as e:
        print("cannot connect to %s: %s" % (args.socket, e), file=sys.stderr)
        return 1
    s.sendall((" ".join(args.command) + "\n").encode())
    out = b""
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        out += chunk
    text = out.decode(errors="replace")
    sys.stdout.write(text)
    return 1 if text.startswith("error") else 0


if __name__ == "__main__":
    sys.exit(main())
