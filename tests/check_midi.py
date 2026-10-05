#!/usr/bin/env python3
"""Play a file to a virtual MIDI destination and compare what arrived with the file.

    tests/check_midi.py SONG.mid [--python PY]

Starts build/midi_sink, runs `midplay -o "midi:midplay test sink" --exit-at-end SONG` in a pseudo
terminal (tests/tui_snapshot.py), then checks: the messages arrive in the file's order (the reset that
midplay sends when it opens a song, CC 123/120/121, is allowed before the first event, and All Notes
Off / All Sound Off when it stops only after the last), and the
arrival times against the tempo map (the largest deviation is printed).
"""
import argparse
import os
import signal
import struct
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def vlq(d, off):
    v = 0
    while True:
        b = d[off]
        off += 1
        v = (v << 7) | (b & 0x7F)
        if not b & 0x80:
            return v, off


def parse(path):
    d = open(path, "rb").read()
    ntrk, div = struct.unpack(">HH", d[10:14])
    off, order, events, tempos = 8 + struct.unpack(">I", d[4:8])[0], 0, [], []
    for _ in range(ntrk):
        tlen = struct.unpack(">I", d[off + 4:off + 8])[0]
        pos, end, tick, run = off + 8, off + 8 + tlen, 0, None
        while pos < end:
            delta, pos = vlq(d, pos)
            tick += delta
            st = d[pos]
            if st == 0xFF:
                typ = d[pos + 1]
                ln, pos = vlq(d, pos + 2)
                if typ == 0x51:
                    tempos.append((tick, order, int.from_bytes(d[pos:pos + 3], "big")))
                    order += 1
                pos += ln
                run = None
            elif st in (0xF0, 0xF7):
                ln, pos = vlq(d, pos + 1)
                if st == 0xF0:
                    events.append((tick, order, b"\xf0" + d[pos:pos + ln]))
                order += 1
                pos += ln
                run = None
            else:
                if st & 0x80:
                    run = st
                    pos += 1
                else:
                    st = run
                n = 1 if st & 0xF0 in (0xC0, 0xD0) else 2
                events.append((tick, order, bytes([st]) + d[pos:pos + n]))
                order += 1
                pos += n
        off = end
    events.sort(key=lambda e: (e[0], e[1]))
    tempos.sort(key=lambda e: (e[0], e[1]))
    tempos = tempos or [(0, 0, 500000)]
    out, ti, cur_tick, cur_us, sec = [], 0, 0, tempos[0][2], 0.0
    for tick, _, m in events:
        while ti + 1 < len(tempos) and tempos[ti + 1][0] <= tick:
            sec += (tempos[ti + 1][0] - cur_tick) * cur_us / 1e6 / div
            cur_tick, cur_us = tempos[ti + 1][0], tempos[ti + 1][2]
            ti += 1
        out.append((sec + (tick - cur_tick) * cur_us / 1e6 / div, m))
    return out


def split(log):
    msgs = []
    for line in log.splitlines():
        f = line.split()
        t, b = float(f[0]) / 1000.0, bytes(int(x, 16) for x in f[1:])
        i = 0
        while i < len(b):
            if b[i] == 0xF0:
                j = b.index(0xF7, i) + 1
            else:
                j = i + (2 if b[i] & 0xF0 in (0xC0, 0xD0) else 3)
            msgs.append((t, b[i:j]))
            i = j
    return msgs


def is_mode(m):
    return len(m) == 3 and m[0] & 0xF0 == 0xB0 and m[1] in (120, 121, 123)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("song")
    ap.add_argument("--python", default=sys.executable)
    a = ap.parse_args()
    want = parse(a.song)
    log_path = os.path.join("/tmp" if not os.environ.get("TMPDIR") else os.environ["TMPDIR"], "midplay_sink.log")
    with open(log_path, "w") as log:
        sink = subprocess.Popen([os.path.join(ROOT, "build/midi_sink")], stdout=log, stderr=subprocess.PIPE)
        sink.stderr.readline()                 # "listening"
        time.sleep(0.5)
        length = want[-1][0] + 3 if want else 3
        r = subprocess.run([a.python, os.path.join(ROOT, "tests/tui_snapshot.py"), "--at", str(length + 5), "--wait-exit",
                            "--", "-o", "midi:midplay test sink", "--exit-at-end", a.song],
                           capture_output=True, text=True)
        time.sleep(0.3)
        sink.send_signal(signal.SIGTERM)
        sink.wait()
    print(r.stdout.strip().splitlines()[-1])
    got = split(open(log_path).read())
    # midplay resets the destination when it opens a song (CC 123, 120, 121 on every channel)
    head = 0
    while head < len(got) and is_mode(got[head][1]) and not (want and got[head][1] == want[0][1]):
        head += 1
    got = got[head:]
    n = len(want)
    tail = got[n:]
    order_ok = [m for _, m in got[:n]] == [m for _, m in want]
    tail_ok = all(is_mode(m) and m[1] in (120, 123) for _, m in tail)
    print(f"file events {n}, received {len(got) + head} ({head} reset messages before the first, {len(tail)} after the last: "
          f"{'all notes/sound off' if tail_ok else 'UNEXPECTED'})")
    print("order and bytes: " + ("identical" if order_ok else "DIFFERENT"))
    if order_ok and n:
        t0 = got[0][0] - want[0][0]
        dev = [abs((g[0] - t0) - w[0]) for g, w in zip(got, want)]
        worst = max(range(n), key=lambda i: dev[i])
        print(f"timing: max deviation {dev[worst] * 1000:.2f} ms (event {worst}, at {want[worst][0]:.3f} s), "
              f"mean {sum(dev) / n * 1000:.2f} ms")
    return 0 if order_ok and tail_ok else 1


if __name__ == "__main__":
    sys.exit(main())
