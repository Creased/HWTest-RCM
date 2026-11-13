#!/usr/bin/env python3
"""Talk to the hwtest GUI over its USB serial port (the console's USB-C port).

The GUI opens a CDC ACM serial port (USB ID 1209:0001) as soon as it starts,
beside whichever debug console is picked, and logs everything there. Over the
same port it takes a few commands (gui/gui_push.c), so a new build can be put
on the SD card and started from the PC with nobody at the console:

    hwtest_push.py log [--seconds N] [--out FILE]   the debug log, live
    hwtest_push.py ping                              which build is running
    hwtest_push.py cmd BT SCAN [--follow N]          any command, then the log
    hwtest_push.py console uart-b|uart-c|uart-a [--gui-usb 0|1]
                                                     write hwtest/hwtest.ini
    hwtest_push.py put SRC DEST [SRC DEST ...]       files onto the SD card
    hwtest_push.py ls backup                         a folder (backup/, hwtest/)
    hwtest_push.py get PATH [DEST]                   a file off the card
    hwtest_push.py clearlogs                         delete the Dock Test logs
    hwtest_push.py report [--out FILE]               the last saved report
    hwtest_push.py run                               start hwtest.bin again
    hwtest_push.py release [--build DIR]             all four built files,
                                                     then run, then the log

DEST is one of the four files a release puts on the card (the GUI refuses
anything else): hwtest/gui.bin, hwtest/libsys_minerva.bso,
bootloader/payloads/hwtest.bin, bootloader/payloads/hwtest_ram.bin.

Needs pyserial. The port is found by its USB ID; --port overrides.
"""
import argparse
import os
import sys
import time
import zlib

import serial
import serial.tools.list_ports

VID, PID = 0x1209, 0x0001

RELEASE = [
    ("build-gui/gui.bin", "hwtest/gui.bin"),
    ("build-gui/libsys_minerva.bso", "hwtest/libsys_minerva.bso"),
    ("build-ram/hwtest_ram.bin", "bootloader/payloads/hwtest_ram.bin"),
    # Last: what !RUN starts, so a release cut short never runs a new
    # payload against an old GUI.
    ("build/hwtest.bin", "bootloader/payloads/hwtest.bin"),
]


def find_port(wait=0.0):
    """The hwtest port's device name, waiting up to `wait` seconds."""
    end = time.time() + wait
    while True:
        for p in serial.tools.list_ports.comports():
            if p.vid == VID and p.pid == PID:
                return p.device
        if time.time() >= end:
            return None
        time.sleep(0.5)


class Console:
    def __init__(self, port, echo=True):
        self.port = port
        self.echo = echo
        self.ser = serial.Serial(port, 115200, timeout=0.2)
        self.ser.dtr = True         # the GUI sends only to an open port
        self.buf = b""

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def lines(self, timeout, total=None):
        """Lines as they come, until `timeout` seconds pass with none, or
        `total` seconds in all: the GUI logs every couple of seconds while
        it draws, so a quiet spell alone may never come."""
        last = start = time.time()
        while time.time() - last < timeout and                 (total is None or time.time() - start < total):
            chunk = self.ser.read(4096)
            if chunk:
                last = time.time()
                self.buf += chunk
            while b"\n" in self.buf:
                line, self.buf = self.buf.split(b"\n", 1)
                yield line.rstrip(b"\r").decode("ascii", "replace")

    def answer(self, timeout):
        """The next +... or -ERR line within `timeout` seconds; log lines
        met on the way (a report's "--- Page ---" among them) are shown."""
        for line in self.lines(timeout, total=timeout):
            if line.startswith("+") or line.startswith("-ERR"):
                return line
            if self.echo and line:
                print("  | " + line)
        return None

    def command(self, cmd, timeout=5.0):
        self.ser.write(("!" + cmd + "\n").encode("ascii"))
        return self.answer(timeout)


def open_console(args, wait=0.0, echo=True):
    port = args.port or find_port(wait)
    if not port:
        sys.exit("No hwtest USB serial port (USB ID 1209:0001). Is the GUI "
                 "running, with the console's USB-C port on this PC?")
    return Console(port, echo)


def put(con, src, dest):
    data = open(src, "rb").read()
    crc = zlib.crc32(data) & 0xFFFFFFFF
    ans = con.command("PUT %s %d %08X" % (dest, len(data), crc))
    if not ans or not ans.startswith("+READY"):
        sys.exit("%s: %s" % (dest, ans or "no answer"))
    t0 = time.time()
    for i in range(0, len(data), 65536):
        con.ser.write(data[i:i + 65536])
    ans = con.answer(30 + len(data) / 50000)
    if not ans or not ans.startswith("+OK"):
        sys.exit("%s: %s" % (dest, ans or "no answer"))
    print("%-36s %8d bytes  crc %08X  %.1f s" % (dest, len(data), crc,
                                                 time.time() - t0))


def cmd_log(args):
    out = open(args.out, "a", encoding="utf-8") if args.out else None
    end = time.time() + args.seconds if args.seconds else None
    while end is None or time.time() < end:
        con = open_console(args, wait=60)
        print("[log] %s" % con.port)
        try:
            for line in con.lines(timeout=1e9 if end is None else end - time.time()):
                stamp = time.strftime("%H:%M:%S")
                print(line)
                if out:
                    out.write("%s %s\n" % (stamp, line))
                    out.flush()
                if end is not None and time.time() >= end:
                    break
        except serial.SerialException:
            print("[log] the port went away, waiting for it to come back")
        finally:
            con.close()
        if end is not None and time.time() >= end:
            break
        time.sleep(1)


def follow(con, seconds, out_path=None):
    """The log for `seconds`, printed and appended to out_path."""
    out = open(out_path, "a", encoding="utf-8") if out_path else None
    end = time.time() + seconds
    for line in con.lines(timeout=seconds, total=seconds):
        print("  | " + line)
        if out:
            out.write("%s %s\n" % (time.strftime("%H:%M:%S"), line))
            out.flush()
        if time.time() >= end:
            break


def get(con, path):
    """A file off the SD card (backup/ or hwtest/), as bytes."""
    ans = con.command("GET " + path, timeout=15)
    if not ans or not ans.startswith("+DATA"):
        sys.exit("%s: %s" % (path, ans or "no answer"))
    _, size, crc = ans.split()
    size, crc = int(size), int(crc, 16)
    data = bytearray()
    for line in con.lines(timeout=5, total=60 + size / 20000):
        if line.startswith("="):
            data += bytes.fromhex(line[1:])
        elif line.startswith("+END"):
            break
    if len(data) != size or (zlib.crc32(data) & 0xFFFFFFFF) != crc:
        sys.exit("%s: got %d of %d bytes, or a bad CRC" % (path, len(data), size))
    return bytes(data)


def cmd_ls(args):
    con = open_console(args, echo=False)
    print(con.command("LS " + args.path) or "no answer")
    con.close()


def cmd_clearlogs(args):
    con = open_console(args, echo=False)
    print(con.command("RMLOGS", timeout=120) or "no answer")
    con.close()


def cmd_get(args):
    con = open_console(args, echo=False)
    data = get(con, args.path)
    con.close()
    dest = args.dest or os.path.basename(args.path)
    if dest == "-":
        sys.stdout.write(data.decode("utf-8", "replace"))
    else:
        open(dest, "wb").write(data)
        print("%s -> %s, %d bytes" % (args.path, dest, len(data)))


def cmd_report(args):
    """backup/<eMMC serial>/hwtest.txt, the last report either stage saved."""
    con = open_console(args, echo=False)
    ans = con.command("LS backup") or ""
    if not ans.startswith("+OK"):
        sys.exit("backup: %s" % (ans or "no answer"))
    dirs = [w[:-1] for w in ans.split(":", 1)[1].split() if w.endswith("/")]
    for d in dirs:
        path = "backup/%s/hwtest.txt" % d
        if ("hwtest.txt" not in (con.command("LS backup/" + d) or "")):
            continue
        data = get(con, path)
        out = args.out or "hwtest_report_%s.txt" % d
        open(out, "wb").write(data)
        print("%s -> %s, %d bytes" % (path, out, len(data)))
    con.close()


def cmd_cmd(args):
    con = open_console(args)
    print(con.command(" ".join(args.text), timeout=args.timeout) or "no answer")
    if args.follow:
        follow(con, args.follow, args.out)
    con.close()


def cmd_ping(args):
    con = open_console(args)
    print(con.command("PING") or "no answer")
    con.close()


def cmd_put(args):
    if len(args.files) % 2:
        sys.exit("put takes pairs: SRC DEST [SRC DEST ...]")
    con = open_console(args)
    for i in range(0, len(args.files), 2):
        put(con, args.files[i], args.files[i + 1])
    con.close()


def run_and_reconnect(con, args, follow_s):
    try:
        ans = con.command("RUN")
    except serial.SerialException:
        # The console stopped USB to start hwtest.bin before the answer
        # was read: that is the command working.
        ans = "+OK (the port went as hwtest.bin started)"
    if not ans or not ans.startswith("+OK"):
        sys.exit("run: %s" % (ans or "no answer"))
    print(ans)
    con.close()
    # The port goes while hwtest.bin starts, and comes back with the GUI.
    time.sleep(3)
    port = find_port(wait=90) if not args.port else args.port
    if not port:
        sys.exit("The GUI did not come back on USB within 90 s.")
    time.sleep(1)
    new = Console(port)
    ans = new.command("PING", timeout=10)
    print("now running: %s" % (ans or "no answer"))
    if follow_s:
        follow(new, follow_s)
    new.close()


CONSOLE_INI = """# hwtest settings (hwtest/hwtest.ini), read at start-up.
#
# console: the debug console's UART, for hwtest.bin and the GUI.
#   uart-b  the right Joy-Con rail (the default, also without this file)
#   uart-c  the left Joy-Con rail (not on a Lite)
#   uart-a  the board's test pads (cluster C: pad 3 TX, pad 2 RX)
# All 115200 8N1 at 1.8 V.
console=%s
#
# gui_usb: THE GUI ONLY (hwtest.bin has no USB). 1 also sends the GUI's
# console over the USB-C port, a serial port on the PC (the default);
# 0 leaves USB off.
gui_usb=%d
"""


def cmd_console(args):
    import tempfile
    with tempfile.NamedTemporaryFile("w", suffix=".ini", delete=False,
                                     newline="\n") as f:
        f.write(CONSOLE_INI % (args.port_name, args.gui_usb))
        path = f.name
    con = open_console(args)
    put(con, path, "hwtest/hwtest.ini")
    con.close()
    os.unlink(path)
    print("console=%s gui_usb=%d from the next start (run, or reboot)"
          % (args.port_name, args.gui_usb))


def cmd_run(args):
    run_and_reconnect(open_console(args), args, args.follow)


def cmd_release(args):
    build = args.build
    for src, _ in RELEASE:
        if not os.path.isfile(os.path.join(build, src)):
            sys.exit("not built: %s" % os.path.join(build, src))
    con = open_console(args)
    print("running: %s" % (con.command("PING") or "no answer"))
    for src, dest in RELEASE:
        put(con, os.path.join(build, src), dest)
    run_and_reconnect(con, args, args.follow)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", help="serial port (default: found by USB ID)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("log", help="the debug log, live (reconnects)")
    p.add_argument("--seconds", type=float, default=0, help="stop after this long")
    p.add_argument("--out", help="also append it to this file")
    sub.add_parser("ping", help="which build is running")
    p = sub.add_parser("cmd", help="any command, e.g. cmd BT SCAN (without the !)")
    p.add_argument("text", nargs="+")
    p.add_argument("--timeout", type=float, default=5, help="wait for the answer")
    p.add_argument("--follow", type=float, default=0, help="then log this long")
    p.add_argument("--out", help="append the followed log to this file")
    p = sub.add_parser("put", help="files onto the SD card")
    p.add_argument("files", nargs="+", metavar="SRC DEST")
    p = sub.add_parser("ls", help="list a folder on the card (backup/, hwtest/)")
    p.add_argument("path")
    p = sub.add_parser("get", help="copy a file off the card (backup/, hwtest/)")
    p.add_argument("path")
    p.add_argument("dest", nargs="?", help="local file, - for the screen")
    sub.add_parser("clearlogs", help="delete the Dock Test logs (hwtest/tvlink_*.txt)")
    p = sub.add_parser("report", help="copy the last saved report off the card")
    p.add_argument("--out", help="local file (default hwtest_report_<serial>.txt)")
    p = sub.add_parser("console", help="write hwtest/hwtest.ini: the console's UART")
    p.add_argument("port_name", choices=["uart-b", "uart-c", "uart-a"])
    p.add_argument("--gui-usb", type=int, choices=[0, 1], default=1,
                   help="the GUI only: also over USB serial (default 1)")
    p = sub.add_parser("run", help="start hwtest.bin from the card again")
    p.add_argument("--follow", type=float, default=15, help="log seconds after")
    p = sub.add_parser("release", help="push the build, run it, show the log")
    p.add_argument("--build", default=os.path.dirname(here), help="repository root")
    p.add_argument("--follow", type=float, default=15, help="log seconds after")
    args = ap.parse_args()
    {"log": cmd_log, "ping": cmd_ping, "cmd": cmd_cmd, "put": cmd_put,
     "console": cmd_console, "run": cmd_run, "release": cmd_release,
     "ls": cmd_ls, "get": cmd_get, "report": cmd_report,
     "clearlogs": cmd_clearlogs}[args.cmd](args)


if __name__ == "__main__":
    main()
