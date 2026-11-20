#!/usr/bin/env python3
"""Host tool for the STM32F439 UART bootloader.

Usage:
    python flash_tool.py COM5 app.bin --version 3 --boot
    python flash_tool.py /dev/ttyACM0 app.bin --baud 115200 --wait 15

Press RESET (or hold B1 + RESET) on the board after starting; the tool spams
INFO requests until the bootloader answers, then uploads the image.
"""
import argparse
import struct
import sys
import time
import zlib

import serial  # pip install pyserial

SOF = 0xA5
CMD_INFO, CMD_BEGIN, CMD_DATA, CMD_END, CMD_BOOT = 1, 2, 3, 4, 5
ACK, NACK = 0x06, 0x15
CHUNK = 256
ERR_FRAME = 1
ERRORS = {1: "frame", 2: "state", 3: "param", 4: "flash", 5: "crc", 6: "no app", 7: "bad image"}


def make_frame(cmd, payload=b""):
    body = bytes([cmd]) + struct.pack("<H", len(payload)) + payload
    return bytes([SOF]) + body + struct.pack("<I", zlib.crc32(body))


def read_frame(ser, timeout):
    ser.timeout = timeout
    while True:
        b = ser.read(1)
        if not b:
            raise TimeoutError("no response")
        if b[0] == SOF:
            break
    hdr = ser.read(3)
    if len(hdr) < 3:
        raise TimeoutError("short header")
    cmd, ln = hdr[0], struct.unpack("<H", hdr[1:3])[0]
    rest = ser.read(ln + 4)
    if len(rest) < ln + 4:
        raise TimeoutError("short frame")
    payload = rest[:ln]
    (crc,) = struct.unpack("<I", rest[ln:])
    if zlib.crc32(hdr + payload) != crc:
        raise IOError("bad CRC from target")
    return cmd, payload


def transact(ser, cmd, payload=b"", timeout=2.0, retries=3):
    last = None
    for _ in range(retries):
        ser.write(make_frame(cmd, payload))
        try:
            rcmd, rpayload = read_frame(ser, timeout)
        except (TimeoutError, IOError) as e:
            last = e
            continue
        if rcmd == ACK:
            return rpayload
        code = rpayload[0] if rpayload else 0
        if code == ERR_FRAME:          # corrupted on the wire, resend
            last = IOError("target saw a corrupted frame")
            continue
        raise RuntimeError(f"target NACK: {ERRORS.get(code, code)}")
    raise RuntimeError(f"no valid answer after {retries} tries: {last}")


def sync(ser, wait_s):
    deadline = time.time() + wait_s
    frame = make_frame(CMD_INFO)
    while time.time() < deadline:
        ser.write(frame)
        try:
            cmd, payload = read_frame(ser, 0.05)
            if cmd == ACK and len(payload) == 13:
                return payload
        except (TimeoutError, IOError):
            pass
    raise SystemExit("bootloader did not answer; press RESET while the tool is running")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("image", help="application .bin linked at 0x08010000")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--version", type=int, default=1)
    ap.add_argument("--wait", type=float, default=20.0, help="seconds to wait for the bootloader")
    ap.add_argument("--boot", action="store_true", help="start the app after a successful update")
    args = ap.parse_args()

    data = open(args.image, "rb").read()
    data += b"\xff" * (-len(data) % 4)            # word-align for flash programming
    crc = zlib.crc32(data)

    ser = serial.Serial(args.port, args.baud, timeout=1)
    ser.reset_input_buffer()

    info = sync(ser, args.wait)
    bl_ver, app_addr, max_size = struct.unpack("<III", info[:12])
    print(f"bootloader v{bl_ver >> 16}.{(bl_ver >> 8) & 0xFF}.{bl_ver & 0xFF}, "
          f"app @ 0x{app_addr:08X}, max {max_size // 1024} KB, app valid: {bool(info[12])}")
    if len(data) > max_size:
        raise SystemExit(f"image too large: {len(data)} > {max_size}")

    print(f"image: {len(data)} bytes, crc32 0x{crc:08X}")
    print("erasing...")
    transact(ser, CMD_BEGIN, struct.pack("<III", len(data), crc, args.version), timeout=60.0)

    t0 = time.time()
    for off in range(0, len(data), CHUNK):
        chunk = data[off:off + CHUNK]
        transact(ser, CMD_DATA, struct.pack("<I", off) + chunk, timeout=2.0, retries=5)
        pct = (off + len(chunk)) * 100 // len(data)
        print(f"\rwriting {pct:3d}%", end="", flush=True)
    print(f"\nwritten in {time.time() - t0:.1f} s")

    transact(ser, CMD_END, timeout=10.0)
    print("verified, metadata written")

    if args.boot:
        transact(ser, CMD_BOOT)
        print("application started")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, serial.SerialException) as e:
        sys.exit(f"error: {e}")
