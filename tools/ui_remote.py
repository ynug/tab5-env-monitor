#!/usr/bin/env python3
"""Tab5 の開発用シリアルコンソール（CONFIG_UI_DEBUG_CONSOLE）を操作する。

  python3 tools/ui_remote.py shot out.png          画面を PNG で保存
  python3 tools/ui_remote.py tap X Y               タップ
  python3 tools/ui_remote.py drag X1 Y1 X2 Y2 MS   ドラッグ
  python3 tools/ui_remote.py press X Y / release    押したまま / 離す
  python3 tools/ui_remote.py send clock 1792213500   その他のコンソールコマンドを送る
複数のコマンドは ';' で区切って続けて実行できる（例: "tap 300 200; wait 1; shot a.png"）。
--log を付けると、待ち時間の間にボードのログを表示する。
ポートは --port（既定 /dev/cu.usbmodem21201）。ESP-IDF の Python 環境（pyserial）で実行する。
"""
import argparse
import base64
import re
import struct
import sys
import time
import zlib

import serial


def open_port(port):
    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = False
    s.rts = False
    s.open()
    # Opening the USB Serial/JTAG port may reset the board: wait until the app is up
    buf = b""
    end = time.time() + 12
    while time.time() < end:
        buf += s.read(4096)
        if b"main: started" in buf:
            time.sleep(1.0)
            break
        if not buf and time.time() > end - 11:
            break   # no boot output: the board was not reset
    s.reset_input_buffer()
    return s


def write_png(path, w, h, rgb):
    raw = b"".join(b"\x00" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 6)))
        f.write(chunk(b"IEND", b""))


def shot_once(s, timeout=60):
    s.reset_input_buffer()
    s.write(b"shot\n")
    end = time.time() + timeout
    buf = b""
    while time.time() < end:
        buf += s.read(65536)
        if b"SHOT_END" in buf:
            break
    else:
        raise RuntimeError("screenshot timed out")
    start = buf.index(b"SHOT_BEGIN")
    header, _, rest = buf[start:].partition(b"\n")
    w, h = map(int, header.split()[1:3])
    body = rest[:rest.index(b"SHOT_END")]
    chunks = {}
    for m in re.finditer(rb"S([0-9a-f]{4}):([A-Za-z0-9+/=]+)\*([0-9a-f]{2})", body):
        b64 = m.group(2)
        if sum(b64) & 0xFF == int(m.group(3), 16):
            chunks[int(m.group(1), 16)] = base64.b64decode(b64)
    n = max(chunks) + 1 if chunks else 0
    if n == 0 or len(chunks) != n:
        raise RuntimeError(f"{n - len(chunks)} corrupted lines")
    return w, h, b"".join(chunks[i] for i in range(n))


def shot(s, path, timeout=60):
    for attempt in range(3):
        try:
            w, h, data = shot_once(s, timeout)
            break
        except RuntimeError as e:
            print(f"screenshot retry ({e})")
    else:
        sys.exit("screenshot failed")
    rgb = bytearray()
    for i in range(0, len(data) - 3, 4):
        n, px = struct.unpack_from("<HH", data, i)
        r = (px >> 11) & 0x1F
        g = (px >> 5) & 0x3F
        b = px & 0x1F
        rgb += bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2))) * n
    if len(rgb) != w * h * 3:
        sys.exit(f"bad size {len(rgb)} != {w * h * 3}")
    write_png(path, w, h, bytes(rgb))
    print(f"saved {path} ({w}x{h}, {len(data)} bytes RLE)")


def pump(s, secs, log):
    """Wait secs; with log, print the board's output meanwhile"""
    if not log:
        time.sleep(secs)
        return
    end = time.time() + secs
    while time.time() < end:
        line = s.readline()
        if line:
            print(line.decode(errors="replace").rstrip())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbmodem21201")
    ap.add_argument("--log", action="store_true")
    ap.add_argument("commands", nargs="+")
    args = ap.parse_args()
    s = open_port(args.port)
    for cmd in " ".join(args.commands).split(";"):
        parts = cmd.split()
        if not parts:
            continue
        if parts[0] == "shot":
            shot(s, parts[1] if len(parts) > 1 else "shot.png")
        elif parts[0] == "wait":
            pump(s, float(parts[1]), args.log)
        elif parts[0] in ("press", "release"):
            s.write((" ".join(parts) + "\n").encode())
            pump(s, 0.4, args.log)
        elif parts[0] in ("tap", "drag"):
            s.write((" ".join(parts) + "\n").encode())
            pump(s, 0.3 if parts[0] == "tap" else int(parts[5]) / 1000 + 0.3, args.log)
        elif parts[0] == "send":
            # any other console command, e.g. "send clock 1792213500"
            s.write((" ".join(parts[1:]) + "\n").encode())
            pump(s, 0.5, args.log)
        else:
            sys.exit(f"unknown command {parts[0]}")


if __name__ == "__main__":
    main()
