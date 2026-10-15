#!/usr/bin/env python3
"""Tell the robot where this Mac's ClaudeUsageBar is (its LAN service and token): the launcher's screensaver shows
Claude's usage from it.

  uv run --with pyserial python3 tools/stackpet_usage_setup.py            set it up (this Mac, its token, its
                                                                           timezone if the robot has none yet)
  uv run --with pyserial python3 tools/stackpet_usage_setup.py --show     what the robot has (the token's length only)
  uv run --with pyserial python3 tools/stackpet_usage_setup.py --clear    forget it
  uv run --with pyserial python3 tools/stackpet_usage_setup.py --style rings   the screensaver's style
                                                                           (claude, rings, terminal, sleepy)
  options: --port /dev/cu.usbmodem1101 (the robot), --host <address> (default: en0's, else en1's),
           --mac-port <port> (default: ClaudeUsageBar's setting, else 8787)

The token is read from ClaudeUsageBar's settings (defaults read local.claude-usage-bar http.token) and goes to the
robot over USB (its USB-Serial/JTAG port): never printed, never written to a file. The robot keeps it in NVS. The
robot has to be on its home screen (the launcher reads the line). Opening the port leaves the line state as it is,
the robot does not reset. Made again in ClaudeUsageBar's pairing window, the token has to be set up again.
"""
import argparse
import re
import subprocess
import sys
import time

import serial

DOMAIN = "local.claude-usage-bar"
ANSI = re.compile(r"\x1b\[[0-9;]*m")
ANSWERS = ("set up:", "cleared", "the Mac at", "not set up", "bad line", "timezone", "screensaver style")
TZ_CHARS = re.compile(r"^[A-Za-z0-9+\-,.:/<>]{3,63}$")


def defaults(key):
    r = subprocess.run(["defaults", "read", DOMAIN, key], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else ""


def mac_address():
    for iface in ("en0", "en1"):
        r = subprocess.run(["ipconfig", "getifaddr", iface], capture_output=True, text=True)
        if r.returncode == 0 and r.stdout.strip():
            return r.stdout.strip()
    return ""


def mac_timezone():
    """This Mac's POSIX TZ string ("CST-8"): the footer of /etc/localtime (TZif version 2 and later)."""
    try:
        data = open("/etc/localtime", "rb").read()
    except OSError:
        return ""
    if not data.startswith(b"TZif"):
        return ""
    tail = data.rstrip(b"\n").rsplit(b"\n", 1)
    tz = tail[1].decode("ascii", "ignore").strip() if len(tail) == 2 else ""
    return tz if TZ_CHARS.match(tz) else ""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", default="/dev/cu.usbmodem1101", help="the robot's USB port")
    ap.add_argument("--host", help="this Mac's address as the robot reaches it (default: en0's, else en1's)")
    ap.add_argument("--mac-port", type=int, help="ClaudeUsageBar's port (default: its setting, else 8787)")
    what = ap.add_mutually_exclusive_group()
    what.add_argument("--show", action="store_true", help="what the robot has")
    what.add_argument("--clear", action="store_true", help="make the robot forget it")
    what.add_argument("--style", help="the screensaver's style: claude, rings, terminal, sleepy")
    args = ap.parse_args()

    lines = []
    if args.clear:
        line, said = "stackpet usage clear", "clear"
    elif args.show:
        line, said = "stackpet usage show", "show"
    elif args.style:
        line, said = f"stackpet usage style {args.style}", f"style {args.style}"
    else:
        host = args.host or mac_address()
        port = args.mac_port or int(defaults("http.port") or 8787)
        token = defaults("http.token")
        if not host:
            sys.exit("no address for this Mac: pass --host")
        if not token:
            sys.exit("no token in ClaudeUsageBar's settings: start it once (it makes one)")
        line, said = f"stackpet usage {host} {port} {token}", f"{host}:{port}, a token of {len(token)} characters"
        tz = mac_timezone()
        if tz:
            lines.append((f"stackpet usage tz {tz}", f"timezone {tz}"))
    lines.insert(0, (line, said))

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    ser.open()  # the line state as it is: no reset
    try:
        ser.reset_input_buffer()
        status = 0
        for line, said in lines:
            ser.write((line + "\n").encode())
            ser.flush()
            print(f"sent to {args.port}: {said}")
            answer = wait_answer(ser)
            if answer is None:
                print("no answer from the robot: is it on its home screen? (the apps do not read the line)")
                return 1
            if "bad line" in answer:
                status = 1
        return status
    finally:
        line = None
        lines = None
        ser.close()


def wait_answer(ser):
    pending = b""
    deadline = time.time() + 5
    while time.time() < deadline:
        pending += ser.read(4096)
        *got, pending = pending.split(b"\n")
        for raw in got:
            text = ANSI.sub("", raw.decode("utf-8", "replace")).strip()
            if "[Usage]" in text and any(a in text for a in ANSWERS):
                answer = text[text.index("[Usage]"):]
                print("robot:", answer)
                return answer
    return None


if __name__ == "__main__":
    sys.exit(main())
