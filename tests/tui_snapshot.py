#!/usr/bin/env python3
"""Drive build/midplay in a pseudo terminal and print the screen as text (needs `pip install pyte`).

    tests/tui_snapshot.py [--cols 140 --rows 40 --at 2.0 --keys 'KEY@SEC,...' --config FILE] -- ARGS...

ARGS go to midplay (add --null-audio to keep an Audio Unit silent).  Keys are pressed at the given
seconds after the first frame; names: space enter esc backspace left right up down pgup pgdn, or literal characters ("comma"
for ","). The screen is printed at --at seconds, or when midplay exits if that is sooner (--wait-exit
waits up to --at for it).  The settings file is a temporary one unless --config is given.

    demo SONG.mid    write a 16-part XG test song (tempo change, 3/4 section, Shift_JIS lyrics)
"""
import argparse
import fcntl
import os
import pty
import select
import struct
import sys
import tempfile
import termios
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
KEYS = {"space": b" ", "enter": b"\r", "esc": b"\x1b", "backspace": b"\x7f", "left": b"\x1b[D",
        "right": b"\x1b[C", "up": b"\x1b[A", "down": b"\x1b[B", "pgup": b"\x1b[5~", "pgdn": b"\x1b[6~",
        "comma": b","}


def vlq(n):
    out = [n & 0x7F]
    n >>= 7
    while n:
        out.insert(0, (n & 0x7F) | 0x80)
        n >>= 7
    return bytes(out)


def track(events):
    data, last = b"", 0
    for tick, ev in sorted(events, key=lambda e: e[0]):
        data += vlq(tick - last) + ev
        last = tick
    data += b"\x00\xff\x2f\x00"
    return b"MTrk" + struct.pack(">I", len(data)) + data


def demo(path, bars=16):
    ppq, bar = 480, 480 * 4
    t0 = [(0, b"\xff\x03" + vlq(12) + b"midplay demo"), (0, b"\xff\x51\x03" + (500000).to_bytes(3, "big")),
          (0, b"\xff\x58\x04\x04\x02\x18\x08"), (0, b"\xf0\x08\x43\x10\x4c\x00\x00\x7e\x00\xf7"),
          (bar * 8, b"\xff\x51\x03" + (400000).to_bytes(3, "big")), (bar * 12, b"\xff\x58\x04\x03\x02\x18\x08")]
    for i, word in enumerate(["さくら", "さくら", "やよいの", "そらは"]):
        text = word.encode("cp932")
        t0.append((bar * (2 + 2 * i), b"\xff\x05" + vlq(len(text)) + text))
    parts = [track(t0)]
    programs = [0, 4, 24, 32, 40, 48, 56, 65, 73, 80, 88, 11, 19, 104, 52, 61]
    chord = [60, 64, 67, 72]
    for ch in range(16):
        ev = []
        if ch != 9:
            ev += [(0, bytes([0xB0 | ch, 0, 0])), (0, bytes([0xB0 | ch, 32, 0])), (0, bytes([0xC0 | ch, programs[ch]]))]
        ev += [(1, bytes([0xB0 | ch, 7, 70 + ch * 3])), (1, bytes([0xB0 | ch, 10, (ch * 8) % 128])),
               (1, bytes([0xB0 | ch, 91, 40 + ch]))]
        tick, step, k = 0, (ppq // 2 if ch == 9 else ppq * (1 + ch % 4)), 0
        while tick < bar * bars:
            key = [36, 42, 38, 42][k % 4] if ch == 9 else chord[(k + ch) % 4] + 12 * ((ch % 5) - 2)
            vel = 60 + (k * 13 + ch * 7) % 60
            ev += [(tick, bytes([0x90 | ch, key, vel])), (tick + step * 3 // 4, bytes([0x80 | ch, key, 0]))]
            tick += step
            k += 1
        parts.append(track(ev))
    with open(path, "wb") as f:
        f.write(b"MThd" + struct.pack(">IHHH", 6, 1, len(parts), ppq) + b"".join(parts))


def shot(args, cols, rows, at, keys, config, wait_exit):
    import pyte
    plan = []
    for spec in filter(None, keys.split(",")):
        k, sec = spec.rsplit("@", 1)
        plan.append((float(sec), KEYS.get(k, k.encode())))
    plan.sort()
    env = dict(os.environ, MIDPLAY_CONFIG=config)
    pid, fd = pty.fork()
    if pid == 0:
        os.execve(os.path.join(ROOT, "build/midplay"), ["midplay", *args], env)
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))
    screen = pyte.Screen(cols, rows)
    stream = pyte.ByteStream(screen)
    start, exited, status = time.time(), False, 0
    last_screen = None
    ready = None                        # key times count from the first frame (raw mode is on by then)
    while time.time() - start < at:
        now = time.time() - start
        while ready is not None and plan and plan[0][0] <= time.time() - ready:
            os.write(fd, plan.pop(0)[1])
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                data = os.read(fd, 65536)
            except OSError:
                data = b""
            if not data:
                exited = True
                break
            if b"\x1b[?1049l" in data:      # leaving the alternate screen: keep what was shown
                last_screen = list(screen.display)
            if ready is None and b"\x1b[?1049h" in data:
                ready = time.time()
            stream.feed(data)
        done, st = os.waitpid(pid, os.WNOHANG)
        if done:
            exited, status = True, st
            break
        if not wait_exit and not plan and now >= at:
            break
    if not exited:
        os.write(fd, b"q")
        deadline = time.time() + 3
        while time.time() < deadline:
            r, _, _ = select.select([fd], [], [], 0.05)
            if r:
                try:
                    data = os.read(fd, 65536)
                except OSError:
                    break
                if b"\x1b[?1049l" in data and last_screen is None:
                    last_screen = list(screen.display)
            done, st = os.waitpid(pid, os.WNOHANG)
            if done:
                status = st
                break
    else:
        if status == 0:
            try:
                _, status = os.waitpid(pid, 0)
            except ChildProcessError:
                pass
    for line in (last_screen or screen.display):
        print(line.rstrip())
    print(f"-- {'exited by itself' if exited else 'quit with q'}, status {os.waitstatus_to_exitcode(status)}, "
          f"{time.time() - start:.1f} s")


def main():
    if len(sys.argv) > 2 and sys.argv[1] == "demo":
        demo(sys.argv[2])
        return 0
    ap = argparse.ArgumentParser()
    ap.add_argument("--cols", type=int, default=140)
    ap.add_argument("--rows", type=int, default=40)
    ap.add_argument("--at", type=float, default=2.0)
    ap.add_argument("--keys", default="")
    ap.add_argument("--config")
    ap.add_argument("--wait-exit", action="store_true")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    config = a.config or os.path.join(tempfile.mkdtemp(prefix="midplay-test-"), "config")
    shot(a.args, a.cols, a.rows, a.at, a.keys, config, a.wait_exit)
    return 0


if __name__ == "__main__":
    sys.exit(main())
