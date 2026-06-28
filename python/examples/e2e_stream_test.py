from __future__ import annotations

import argparse
import os
import struct
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
PYTHON_DIR = SCRIPT_DIR.parent
if str(PYTHON_DIR) not in sys.path:
    sys.path.insert(0, str(PYTHON_DIR))

from ble_at import BleAtHost, ScanResult


FRAME_MAGIC = b"BATH"
HEADER_STRUCT = struct.Struct("<4sIIH")
DEFAULT_FRAME_SIZE = 190


@dataclass
class ParsedFrame:
    seq: int
    tx_ms: int
    payload_len: int


class FrameParser:
    def __init__(self, frame_size: int) -> None:
        self._frame_size = frame_size
        self._buffer = bytearray()

    def _validate_frame(self, frame: bytes) -> ParsedFrame | None:
        if len(frame) != self._frame_size:
            return None

        magic, seq, tx_ms, payload_len = HEADER_STRUCT.unpack(frame[: HEADER_STRUCT.size])
        if magic != FRAME_MAGIC or payload_len != self._frame_size:
            return None

        fill_byte = seq & 0xFF
        payload = frame[HEADER_STRUCT.size :]
        if any(byte != fill_byte for byte in payload):
            return None

        return ParsedFrame(seq=seq, tx_ms=tx_ms, payload_len=payload_len)

    def feed(self, chunk: bytes) -> list[ParsedFrame]:
        frames: list[ParsedFrame] = []
        self._buffer.extend(chunk)

        while len(self._buffer) >= HEADER_STRUCT.size:
            start = self._buffer.find(FRAME_MAGIC)
            if start < 0:
                del self._buffer[:- (len(FRAME_MAGIC) - 1)]
                break

            if start > 0:
                del self._buffer[:start]

            if len(self._buffer) < self._frame_size:
                break

            candidate = bytes(self._buffer[: self._frame_size])
            frame = self._validate_frame(candidate)
            if frame is None:
                del self._buffer[0]
                continue

            frames.append(frame)
            del self._buffer[: self._frame_size]

        return frames


class StreamStats:
    def __init__(self, frame_size: int) -> None:
        self.frame_size = frame_size
        self.tx_frames = 0
        self.rx_frames = 0
        self.reordered_frames = 0
        self.duplicate_frames = 0
        self.last_rx_seq = -1
        self.tx_timestamps: dict[int, float] = {}
        self.rx_seen: set[int] = set()
        self.rtt_samples_ms: list[float] = []
        self.lock = threading.Lock()

    def mark_tx(self, seq: int, sent_time: float) -> None:
        with self.lock:
            self.tx_frames += 1
            self.tx_timestamps[seq] = sent_time

    def mark_rx(self, frame: ParsedFrame, received_time: float) -> None:
        with self.lock:
            if frame.seq in self.rx_seen:
                self.duplicate_frames += 1
                return

            self.rx_seen.add(frame.seq)
            self.rx_frames += 1
            if self.last_rx_seq >= 0 and frame.seq < self.last_rx_seq:
                self.reordered_frames += 1
            self.last_rx_seq = max(self.last_rx_seq, frame.seq)

            sent_time = self.tx_timestamps.pop(frame.seq, None)
            if sent_time is not None:
                self.rtt_samples_ms.append((received_time - sent_time) * 1000.0)

    def outstanding(self) -> int:
        with self.lock:
            return len(self.tx_timestamps)

    def snapshot(self, elapsed_s: float) -> dict[str, float | int]:
        with self.lock:
            lost_frames = self.tx_frames - self.rx_frames
            avg_rtt_ms = (
                sum(self.rtt_samples_ms) / len(self.rtt_samples_ms)
                if self.rtt_samples_ms
                else 0.0
            )
            max_rtt_ms = max(self.rtt_samples_ms) if self.rtt_samples_ms else 0.0
            return {
                "tx_frames": self.tx_frames,
                "rx_frames": self.rx_frames,
                "lost_frames": lost_frames,
                "reordered_frames": self.reordered_frames,
                "duplicate_frames": self.duplicate_frames,
                "tx_bps": (self.tx_frames * self.frame_size) / elapsed_s if elapsed_s > 0 else 0.0,
                "rx_bps": (self.rx_frames * self.frame_size) / elapsed_s if elapsed_s > 0 else 0.0,
                "avg_rtt_ms": avg_rtt_ms,
                "max_rtt_ms": max_rtt_ms,
                "outstanding_frames": len(self.tx_timestamps),
            }


def build_frame(seq: int, frame_size: int) -> bytes:
    tx_ms = int(time.monotonic() * 1000) & 0xFFFFFFFF
    header = HEADER_STRUCT.pack(FRAME_MAGIC, seq, tx_ms, frame_size)
    fill_len = frame_size - len(header)
    return header + bytes([seq & 0xFF]) * fill_len


def resolve_target(ble: BleAtHost, args: argparse.Namespace) -> tuple[str, int]:
    if args.mac:
        return args.mac, args.addr_type

    target_name = args.target_name or os.environ.get("BLE_AT_TARGET_NAME")
    if not target_name:
        raise RuntimeError("Set --target-name or BLE_AT_TARGET_NAME, or provide --mac directly")

    results = ble.scan(duration_s=args.scan_seconds, prefix=args.scan_prefix)
    for result in results:
        if result.name == target_name:
            return result.mac, result.addr_type

    raise RuntimeError(f"Target {target_name} not found during scan")


def print_snapshot(prefix: str, snapshot: dict[str, float | int]) -> None:
    print(
        f"{prefix} TX={snapshot['tx_frames']} RX={snapshot['rx_frames']} "
        f"LOST={snapshot['lost_frames']} REORDER={snapshot['reordered_frames']} "
        f"TX_BPS={snapshot['tx_bps']:.1f} RX_BPS={snapshot['rx_bps']:.1f} "
        f"AVG_RTT_MS={snapshot['avg_rtt_ms']:.1f} MAX_RTT_MS={snapshot['max_rtt_ms']:.1f} "
        f"OUT={snapshot['outstanding_frames']}"
    )


def main() -> None:
    parser = argparse.ArgumentParser(description="BLE AT Host end-to-end stream validator")
    parser.add_argument("--port", default=os.environ.get("BLE_AT_PORT", "COM13"))
    parser.add_argument("--mac", default=os.environ.get("BLE_AT_TARGET_MAC"))
    parser.add_argument("--addr-type", type=int, default=int(os.environ.get("BLE_AT_TARGET_ADDR_TYPE", "0")))
    parser.add_argument("--target-name", default=os.environ.get("BLE_AT_TARGET_NAME"))
    parser.add_argument("--scan-prefix", default=os.environ.get("BLE_AT_SCAN_PREFIX", ""))
    parser.add_argument("--scan-seconds", type=int, default=3)
    parser.add_argument("--duration", type=float, default=60.0)
    parser.add_argument("--hz", type=float, default=20.0)
    parser.add_argument("--frame-size", type=int, default=DEFAULT_FRAME_SIZE)
    parser.add_argument("--mode", choices=("blind_rate", "echo_gated"), default="blind_rate")
    parser.add_argument("--max-outstanding", type=int, default=1)
    parser.add_argument("--settle-time", type=float, default=2.0)
    parser.add_argument("--stats-interval", type=float, default=5.0)
    args = parser.parse_args()

    if args.frame_size < HEADER_STRUCT.size:
        raise ValueError("frame-size is smaller than the protocol header")

    ble = BleAtHost(args.port)
    stats = StreamStats(args.frame_size)
    parser_state = FrameParser(args.frame_size)

    def on_stream_data(chunk: bytes) -> None:
        now = time.monotonic()
        for frame in parser_state.feed(chunk):
            stats.mark_rx(frame, now)

    ble.add_data_handler(on_stream_data)

    try:
        ble.recover_control_mode()
        mac, addr_type = resolve_target(ble, args)
        print(f"TARGET MAC={mac} ADDR_TYPE={addr_type}")
        ble.connect(mac, addr_type)
        ble.enter_data_mode()

        period_s = 1.0 / args.hz
        start = time.monotonic()
        next_send = start
        next_stats = start + args.stats_interval
        seq = 0

        while True:
            now = time.monotonic()
            if now - start >= args.duration:
                break

            if args.mode == "echo_gated" and stats.outstanding() >= args.max_outstanding:
                time.sleep(0.001)
                continue

            if now < next_send:
                sleep_time = min(next_send - now, 0.005)
                if sleep_time > 0:
                    time.sleep(sleep_time)
                continue

            frame = build_frame(seq, args.frame_size)
            stats.mark_tx(seq, time.monotonic())
            ble.write_stream(frame)
            seq += 1
            next_send += period_s

            if now >= next_stats:
                print_snapshot("LIVE", stats.snapshot(now - start))
                next_stats = now + args.stats_interval

        active_elapsed = time.monotonic() - start
        settle_deadline = time.monotonic() + args.settle_time
        while time.monotonic() < settle_deadline and stats.outstanding() > 0:
            time.sleep(0.01)

        print_snapshot("RESULT", stats.snapshot(active_elapsed))
        ble.exit_data_mode()
        print(f"STREAMSTAT {ble.get_stream_stat()}")
        ble.disconnect()
    finally:
        ble.close()


if __name__ == "__main__":
    main()
