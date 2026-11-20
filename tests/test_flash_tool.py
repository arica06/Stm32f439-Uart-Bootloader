import struct
import sys
import zlib
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import flash_tool as ft  # noqa: E402


class FakeSerial:
    """Minimal stand-in for serial.Serial: serves bytes from a buffer."""

    def __init__(self, data: bytes):
        self.buf = bytearray(data)
        self.timeout = None

    def read(self, n: int) -> bytes:
        out = bytes(self.buf[:n])
        del self.buf[:n]
        return out


def test_frame_layout():
    f = ft.make_frame(ft.CMD_DATA, b"\x01\x02")
    assert f[0] == 0xA5
    assert f[1] == ft.CMD_DATA
    assert f[2:4] == b"\x02\x00"                       # little-endian length
    assert struct.unpack("<I", f[-4:])[0] == zlib.crc32(f[1:-4])


def test_roundtrip():
    frame = ft.make_frame(ft.ACK, b"abc")
    cmd, payload = ft.read_frame(FakeSerial(frame), 0.1)
    assert cmd == ft.ACK
    assert payload == b"abc"


def test_empty_payload_roundtrip():
    cmd, payload = ft.read_frame(FakeSerial(ft.make_frame(ft.ACK)), 0.1)
    assert (cmd, payload) == (ft.ACK, b"")


def test_garbage_before_sof_is_skipped():
    frame = b"\x00\x11\x22" + ft.make_frame(ft.NACK, b"\x05")
    cmd, payload = ft.read_frame(FakeSerial(frame), 0.1)
    assert cmd == ft.NACK
    assert payload == b"\x05"


def test_bad_crc_is_detected():
    frame = bytearray(ft.make_frame(ft.ACK, b"abc"))
    frame[-1] ^= 0xFF
    with pytest.raises(IOError):
        ft.read_frame(FakeSerial(bytes(frame)), 0.1)


def test_timeout_on_empty_line():
    with pytest.raises(TimeoutError):
        ft.read_frame(FakeSerial(b""), 0.1)


def test_incremental_crc_matches_whole():
    # The firmware's bl_crc32(crc, data, len) continues from a previous value,
    # exactly like zlib.crc32(data, crc).
    a, b = b"firmware-", b"image"
    assert zlib.crc32(b, zlib.crc32(a)) == zlib.crc32(a + b)
