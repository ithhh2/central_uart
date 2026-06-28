from __future__ import annotations

import queue
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

import serial


@dataclass
class ScanResult:
    index: int
    mac: str
    name: str
    addr_type: int
    rssi: int


class BleAtHost:
    _EXIT_EVENT = b"+IND=DATAMODE,0\r\n"

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 0.1) -> None:
        self.ser = serial.Serial(port=port, baudrate=baudrate, timeout=timeout)
        self.ser.dtr = True
        self._line_queue: "queue.Queue[str]" = queue.Queue()
        self._data_queue: "queue.Queue[bytes]" = queue.Queue()
        self._event_handlers: list[Callable[[str], None]] = []
        self._data_handlers: list[Callable[[bytes], None]] = []
        self._stop = threading.Event()
        self._data_mode = threading.Event()
        self._exit_pending = threading.Event()
        self._line_buffer = bytearray()
        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()

    def close(self) -> None:
        self._stop.set()
        self._reader.join(timeout=1.0)
        self.ser.close()

    def add_event_handler(self, handler: Callable[[str], None]) -> None:
        self._event_handlers.append(handler)

    def add_data_handler(self, handler: Callable[[bytes], None]) -> None:
        self._data_handlers.append(handler)

    def _emit_event(self, line: str) -> None:
        for handler in self._event_handlers:
            handler(line)
        self._line_queue.put(line)

    def _emit_data(self, chunk: bytes) -> None:
        if not chunk:
            return
        self._data_queue.put(chunk)
        for handler in self._data_handlers:
            handler(chunk)

    def _process_control_bytes(self, chunk: bytes) -> None:
        for ch in chunk:
            if ch == ord(">") and not self._line_buffer:
                self._emit_event(">")
                continue

            self._line_buffer.append(ch)
            if ch not in (ord("\n"), ord("\r")):
                continue

            line = self._line_buffer.decode(errors="replace").strip()
            self._line_buffer.clear()
            if line:
                self._emit_event(line)

    def _pending_exit_prefix_len(self, buffer: bytearray) -> int:
        limit = min(len(buffer), len(self._EXIT_EVENT) - 1)
        for prefix_len in range(limit, 0, -1):
            if self._EXIT_EVENT.startswith(buffer[-prefix_len:]):
                return prefix_len
        return 0

    def _handle_exit_pending_chunk(self, exit_buffer: bytearray, chunk: bytes) -> None:
        exit_buffer.extend(chunk)

        while exit_buffer:
            event_start = exit_buffer.find(self._EXIT_EVENT)
            if event_start == 0:
                del exit_buffer[: len(self._EXIT_EVENT)]
                self._exit_pending.clear()
                self._data_mode.clear()
                self._emit_event(self._EXIT_EVENT.decode("ascii").strip())
                if exit_buffer:
                    remaining = bytes(exit_buffer)
                    exit_buffer.clear()
                    self._process_control_bytes(remaining)
                return

            if event_start > 0:
                self._emit_data(bytes(exit_buffer[:event_start]))
                del exit_buffer[:event_start]
                continue

            prefix_len = self._pending_exit_prefix_len(exit_buffer)
            flush_len = len(exit_buffer) - prefix_len
            if flush_len <= 0:
                return

            self._emit_data(bytes(exit_buffer[:flush_len]))
            del exit_buffer[:flush_len]

    def _reader_loop(self) -> None:
        exit_buffer = bytearray()

        while not self._stop.is_set():
            if self._data_mode.is_set():
                chunk = self.ser.read(256)
                if not chunk:
                    continue

                if self._exit_pending.is_set():
                    self._handle_exit_pending_chunk(exit_buffer, chunk)
                    continue

                self._emit_data(chunk)
                continue

            chunk = self.ser.read(1)
            if not chunk:
                continue

            self._process_control_bytes(chunk)

    def _write_line(self, command: str) -> None:
        self.ser.write(command.encode("ascii") + b"\r\n")

    def _read_until(self, predicate: Callable[[str], bool], timeout: float = 5.0) -> str:
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("Timeout waiting for AT response")
            try:
                line = self._line_queue.get(timeout=remaining)
            except queue.Empty as exc:
                raise TimeoutError("Timeout waiting for AT response") from exc
            if predicate(line):
                return line

    def _wait_ok(self, timeout: float = 5.0) -> None:
        line = self._read_until(lambda item: item.startswith("+OK") or item.startswith("+ERR"), timeout)
        if not line.startswith("+OK"):
            raise RuntimeError(line)

    def _drain_line_queue(self) -> None:
        while True:
            try:
                self._line_queue.get_nowait()
            except queue.Empty:
                return

    def scan(self, duration_s: int, prefix: Optional[str] = None, timeout: float = 10.0) -> list[ScanResult]:
        results: list[ScanResult] = []
        seen: set[str] = set()
        if prefix:
            self._write_line(f"AT+STARTSCAN={duration_s},{prefix}")
        else:
            self._write_line(f"AT+STARTSCAN={duration_s}")
        self._wait_ok(timeout=timeout)

        while True:
            line = self._read_until(
                lambda item: item.startswith("+IND=SCDA") or item == "+IND=SCED",
                timeout=timeout,
            )
            if line == "+IND=SCED":
                return results

            if line in seen:
                continue

            seen.add(line)
            _, payload = line.split("=", 1)
            _, index, mac, name, addr_type, rssi = payload.split(",", 5)
            results.append(
                ScanResult(
                    index=int(index),
                    mac=mac,
                    name=name,
                    addr_type=int(addr_type),
                    rssi=int(rssi),
                )
            )

    def connect(self, mac: str, addr_type: int, timeout: float = 10.0) -> None:
        self._write_line(f"AT+STARTCON={mac},{addr_type}")
        self._wait_ok(timeout=timeout)
        self._read_until(lambda item: item == "+IND=BLMC,0", timeout)
        self._read_until(lambda item: item.startswith("+IND=BLMN,0"), timeout)

    def disconnect(self, timeout: float = 5.0) -> None:
        self._write_line("AT+DISCON=0")
        self._wait_ok(timeout=timeout)

    def send(self, payload: bytes, timeout: float = 5.0) -> None:
        self._write_line(f"AT+SEND=0,{len(payload)}")
        prompt = self._read_until(lambda item: item == ">", timeout)
        if prompt != ">":
            raise RuntimeError(f"Unexpected prompt: {prompt}")
        self.ser.write(payload)
        self._wait_ok(timeout=timeout)

    def get_status(self, timeout: float = 5.0) -> list[str]:
        lines: list[str] = []
        self._write_line("AT+CONSTATUS")
        while True:
            line = self._read_until(lambda item: item.startswith("+OK") or item.startswith("0:"), timeout)
            lines.append(line)
            if line.startswith("+OK"):
                return lines

    def set_profile(self, service_uuid: str, notify_uuid: str, write_uuid: str, timeout: float = 5.0) -> None:
        self._write_line(f"AT+SETSVCUUID={service_uuid},{notify_uuid},{write_uuid}")
        self._wait_ok(timeout=timeout)

    def get_profile(self, timeout: float = 5.0) -> str:
        self._write_line("AT+GETSVCUUID")
        line = self._read_until(lambda item: item.startswith("+OK=") or item.startswith("+ERR"), timeout)
        if not line.startswith("+OK="):
            raise RuntimeError(line)
        return line[len("+OK=") :]

    def get_stream_stat(self, timeout: float = 5.0) -> str:
        self._write_line("AT+STREAMSTAT")
        line = self._read_until(lambda item: item.startswith("+OK=") or item.startswith("+ERR"), timeout)
        if not line.startswith("+OK="):
            raise RuntimeError(line)
        return line[len("+OK=") :]

    def enter_data_mode(self, timeout: float = 5.0) -> None:
        self._write_line("AT+ENTERDATAMODE=0")
        self._wait_ok(timeout=timeout)
        self._read_until(lambda item: item == "+IND=DATAMODE,1", timeout)
        self._data_mode.set()

    def drain_stream(self, timeout: float = 0.05) -> bytes:
        chunks: list[bytes] = []
        while True:
            try:
                chunks.append(self._data_queue.get(timeout=timeout if not chunks else 0.0))
            except queue.Empty:
                return b"".join(chunks)

    def sync_control_mode(self, timeout: float = 2.0, attempts: int = 3) -> None:
        if self._data_mode.is_set():
            raise RuntimeError("Data mode is still active")

        self.ser.reset_input_buffer()
        self._line_buffer.clear()
        self._drain_line_queue()

        deadline = time.monotonic() + timeout
        for _ in range(attempts):
            self._write_line("AT")
            try:
                self._read_until(lambda item: item == "+OK", max(0.2, deadline - time.monotonic()))
                return
            except TimeoutError:
                if time.monotonic() >= deadline:
                    break

        raise TimeoutError("Timeout resynchronizing AT control mode")

    def recover_control_mode(self, timeout: float = 2.0, guard_time_s: float = 1.1) -> None:
        try:
            self.sync_control_mode(timeout=timeout, attempts=2)
            return
        except TimeoutError:
            pass

        self.ser.reset_input_buffer()
        self._line_buffer.clear()
        self._drain_line_queue()
        self._exit_pending.clear()
        self._data_mode.clear()

        time.sleep(guard_time_s)
        self.ser.write(b"+++")

        try:
            self._read_until(
                lambda item: item == "+IND=DATAMODE,0" or item.startswith("+ERR"),
                timeout=timeout + guard_time_s,
            )
        except TimeoutError:
            pass

        self._exit_pending.clear()
        self._data_mode.clear()
        self.sync_control_mode(timeout=max(timeout, 3.0), attempts=3)

    def exit_data_mode(self, timeout: float = 5.0, guard_time_s: float = 1.1) -> None:
        time.sleep(guard_time_s)
        self.drain_stream(timeout=0.01)
        self._exit_pending.set()
        self.ser.write(b"+++")
        line = self._read_until(lambda item: item == "+IND=DATAMODE,0", timeout=timeout + guard_time_s)
        if line != "+IND=DATAMODE,0":
            raise RuntimeError(f"Unexpected data mode exit line: {line}")
        self.sync_control_mode(timeout=min(2.0, timeout))

    def write_stream(self, payload: bytes) -> None:
        if not self._data_mode.is_set():
            raise RuntimeError("Data mode is not active")
        self.ser.write(payload)

    def read_stream(self, timeout: float = 1.0) -> bytes:
        try:
            return self._data_queue.get(timeout=timeout)
        except queue.Empty as exc:
            raise TimeoutError("Timeout waiting for stream data") from exc
