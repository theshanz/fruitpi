"""
serial_bridge.py — Abstracts Serial communication with the ESP32.

Works identically in mock mode (no hardware) and real mode (USB Serial).
The UI calls send() and receives parsed JSON dicts via a callback.
"""

import json
import queue
import threading
import time

from mock_esp32 import MockESP32


class SerialBridge:
    """Unified interface: send commands, receive JSON responses."""

    def __init__(self, on_message, port="/dev/ttyUSB0", baud=115200,
                 mock=False, n_taps=3):
        self.on_message = on_message
        self.port = port
        self.baud = baud
        self.mock = mock

        self._to_esp = queue.Queue()
        self._from_esp = queue.Queue()

        self._reader_thread = None
        self._running = False
        self._serial = None
        self._mock = None

        if mock:
            self._mock = MockESP32(
                outgoing=self._from_esp,
                incoming=self._to_esp,
                n_taps=n_taps,
            )

    def connect(self, fruit="mango", label="unripe"):
        self._running = True

        if self.mock:
            self._mock.start(fruit=fruit, label=label)
        else:
            import serial
            self._serial = serial.Serial(self.port, self.baud, timeout=1)
            time.sleep(0.1)

        self._reader_thread = threading.Thread(
            target=self._reader_loop, daemon=True
        )
        self._reader_thread.start()

    def disconnect(self):
        self._running = False
        if self._mock:
            self._mock.stop()
        if self._serial and self._serial.is_open:
            self._serial.close()

    def send(self, command: str):
        self._to_esp.put(command)
        if self._serial and self._serial.is_open:
            self._serial.write((command + "\n").encode("ascii"))
            self._serial.flush()

    def is_connected(self):
        if self.mock:
            return self._running
        return self._serial is not None and self._serial.is_open

    def _reader_loop(self):
        if self.mock:
            self._reader_loop_mock()
        else:
            self._reader_loop_real()

    def _reader_loop_mock(self):
        while self._running:
            try:
                raw = self._from_esp.get(timeout=0.1)
                self.on_message(json.loads(raw))
            except queue.Empty:
                continue
            except (json.JSONDecodeError, TypeError):
                continue

    def _reader_loop_real(self):
        while self._running:
            try:
                if self._serial and self._serial.in_waiting:
                    line = self._serial.readline().decode("ascii", errors="ignore").strip()
                    if line:
                        obj = json.loads(line)
                        self.on_message(obj)
                else:
                    time.sleep(0.01)
            except (json.JSONDecodeError, UnicodeDecodeError):
                continue
            except Exception:
                break
