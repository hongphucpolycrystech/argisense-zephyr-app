#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#

"""ArgiSense RS485 service GUI.

This tool monitors ArgiSense measurements, edits runtime configuration, and
uploads an MCUboot signed binary through the external RS485 Modbus RTU port.
"""

from __future__ import annotations

import hashlib
import queue
import struct
import threading
import time
import zlib
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - allows py_compile without pyserial.
    serial = None
    list_ports = None


DFU_REG_CONTROL = 1000
DFU_REG_STATUS = 1001
DFU_REG_ERROR = 1002
DFU_REG_IMAGE_SIZE_HI = 1003
DFU_REG_IMAGE_SIZE_LO = 1004
DFU_REG_IMAGE_CRC32_HI = 1005
DFU_REG_IMAGE_CRC32_LO = 1006
DFU_REG_BYTES_WRITTEN_HI = 1007
DFU_REG_BYTES_WRITTEN_LO = 1008
DFU_REG_CHUNK_OFFSET_HI = 1009
DFU_REG_CHUNK_OFFSET_LO = 1010
DFU_REG_CHUNK_LENGTH = 1011
DFU_REG_CHUNK_CRC32_HI = 1012
DFU_REG_CHUNK_CRC32_LO = 1013
DFU_REG_MAX_CHUNK_BYTES = 1014
DFU_REG_LAST_COMMAND = 1015
DFU_REG_UNLOCK_KEY = 1016
DFU_REG_UNLOCK_REMAINING_S = 1017
DFU_REG_SHA256_BASE = 1020
DFU_REG_SHA256_COUNT = 16
DFU_REG_DATA_BASE = 1100

REG_DEVICE_ID = 0
REG_MAP_VERSION = 1
REG_STATUS_FLAGS = 2
REG_MODBUS_ADDRESS = 3
REG_BAUDRATE_HI = 4
REG_BAUDRATE_LO = 5
REG_MEASUREMENT_PERIOD_SECONDS = 6
REG_MEASUREMENT_WINDOW_MS = 7
REG_BAUD_PRESET = 8
REG_TERMINATION_ENABLED = 9
REG_METHANE_PPM_X100_HI = 10
REG_PRESSURE_PA_HI = 12
REG_DAC0_CURRENT_UA = 14
REG_DAC1_CURRENT_UA = 15
REG_SAMPLE_SEQUENCE_HI = 16
REG_SAMPLE_UPTIME_SECONDS_HI = 18
REG_REBOOT_REQUIRED = 27
REG_LAST_COMMAND = 28
REG_LAST_COMMAND_RESULT = 29
REG_DAC_MIN_CURRENT_UA = 30
REG_DAC_MAX_CURRENT_UA = 31
REG_DAC_FAULT_CURRENT_UA = 32
REG_COMMAND = 33
REG_RS485_PARITY = 34
REG_RS485_STOP_BITS = 35
REG_RS485_DATA_BITS = 36
REG_PRESSURE_TEMP_CENTI_C = 74
REG_HUMIDITY_RH_X100 = 80
REG_AMBIENT_TEMP_CENTI_C = 81
REG_HUMIDITY_LAST_ERROR = 82

COMMAND_REBOOT = 0xA551
COMMAND_CONFIRM_IMAGE = 0xA553

DFU_CMD_NONE = 0x0000
DFU_CMD_BEGIN = 0xD001
DFU_CMD_WRITE = 0xD002
DFU_CMD_VERIFY = 0xD003
DFU_CMD_TEST = 0xD004
DFU_CMD_ABORT = 0xD005
DFU_CMD_TEST_REBOOT = 0xD006

DFU_STATUS_IDLE = 0
DFU_STATUS_RECEIVING = 1
DFU_STATUS_VERIFYING = 2
DFU_STATUS_VERIFIED = 3
DFU_STATUS_PENDING = 4
DFU_STATUS_ERROR = 5

STATUS_NAMES = {
    DFU_STATUS_IDLE: "idle",
    DFU_STATUS_RECEIVING: "receiving",
    DFU_STATUS_VERIFYING: "verifying",
    DFU_STATUS_VERIFIED: "verified",
    DFU_STATUS_PENDING: "pending",
    DFU_STATUS_ERROR: "error",
}

DEFAULT_CHUNK_BYTES = 96
DEFAULT_DFU_UNLOCK_KEY = "0xA65D"
ARGISENSE_MP_DEVICE_ID = 0xA651
ARGISENSE_PH_DEVICE_ID = 0xA652
ARGISENSE_DEVICE_PRODUCTS = {
    ARGISENSE_MP_DEVICE_ID: "methane-pressure",
    ARGISENSE_PH_DEVICE_ID: "pH",
}
DISCOVERY_BAUDS = ("115200", "57600", "38400", "19200", "9600")
DISCOVERY_PARITY_LABELS = ("None (N)", "Even (E)", "Odd (O)")
DISCOVERY_STOP_BITS = ("2", "1")
DISCOVERY_TIMEOUT_S = 0.20
PARITY_OPTIONS = {
    "None (N)": "N",
    "Odd (O)": "O",
    "Even (E)": "E",
}
STOP_BITS_OPTIONS = {
    "1": 1,
    "2": 2,
}
DATA_BITS_OPTIONS = {
    "8": 8,
}
BAUD_PRESET_OPTIONS = {
    "9600": 0,
    "19200": 1,
    "38400": 2,
    "57600": 3,
    "115200": 4,
}
BAUD_PRESET_BY_VALUE = {value: key for key, value in BAUD_PRESET_OPTIONS.items()}
PARITY_CODE_OPTIONS = {
    "None (0)": 0,
    "Odd (1)": 1,
    "Even (2)": 2,
}
PARITY_LABEL_BY_CODE = {value: key for key, value in PARITY_CODE_OPTIONS.items()}
MAX_HISTORY_SAMPLES = 1000
DEFAULT_GRAPH_WINDOW_SAMPLES = 60


class ModbusError(RuntimeError):
    """Base Modbus error."""


class ModbusExceptionResponse(ModbusError):
    """Modbus exception response from slave."""

    def __init__(self, function: int, code: int) -> None:
        super().__init__(
            f"Modbus exception: function=0x{function:02X} code=0x{code:02X}"
        )
        self.function = function
        self.code = code


def crc16_modbus(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 0x0001:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc & 0xFFFF


def product_name(device_id: int) -> str:
    return ARGISENSE_DEVICE_PRODUCTS.get(device_id, f"unknown-0x{device_id:04X}")


def is_ph_device(device_id: int) -> bool:
    return device_id == ARGISENSE_PH_DEVICE_ID


def append_crc(frame: bytes) -> bytes:
    crc = crc16_modbus(frame)
    return frame + bytes((crc & 0xFF, crc >> 8))


def check_crc(frame: bytes) -> None:
    if len(frame) < 4:
        raise ModbusError("short Modbus response")
    expected = frame[-2] | (frame[-1] << 8)
    got = crc16_modbus(frame[:-2])
    if got != expected:
        raise ModbusError(f"CRC mismatch: got=0x{got:04X} expected=0x{expected:04X}")


def words_from_u32(value: int) -> tuple[int, int]:
    return ((value >> 16) & 0xFFFF, value & 0xFFFF)


def u32_from_words(high: int, low: int) -> int:
    return ((high & 0xFFFF) << 16) | (low & 0xFFFF)


def signed32_from_words(high: int, low: int) -> int:
    value = u32_from_words(high, low)
    if value & 0x80000000:
        value -= 0x100000000
    return value


def words_from_bytes(data: bytes) -> list[int]:
    if len(data) % 2:
        data += b"\x00"
    return [int.from_bytes(data[i : i + 2], "big") for i in range(0, len(data), 2)]


class ModbusRtuClient:
    def __init__(
        self,
        port: str,
        baudrate: int,
        unit_id: int,
        timeout_s: float,
        bytesize: int,
        parity: str,
        stopbits: int,
    ) -> None:
        if serial is None:
            raise RuntimeError(
                "pyserial is not installed. Run: py -3.12 -m pip install -r tools\\rs485_dfu\\requirements.txt"
            )
        self.unit_id = unit_id
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=bytesize,
            parity=parity,
            stopbits=stopbits,
            timeout=timeout_s,
            write_timeout=timeout_s,
        )
        self.ser.reset_input_buffer()
        self.ser.reset_output_buffer()

    def close(self) -> None:
        self.ser.close()

    def _read_exact(self, count: int) -> bytes:
        data = self.ser.read(count)
        if len(data) != count:
            raise ModbusError(f"timeout waiting for {count} bytes, got {len(data)}")
        return data

    def _request(self, function: int, payload: bytes, response_data_len: int | None) -> bytes:
        request = append_crc(bytes((self.unit_id, function)) + payload)
        self.ser.reset_input_buffer()
        self.ser.write(request)
        self.ser.flush()

        header = self._read_exact(3)
        if header[0] != self.unit_id:
            raise ModbusError(f"unexpected slave id: {header[0]}")

        response_function = header[1]
        if response_function & 0x80:
            tail = self._read_exact(2)
            frame = header + tail
            check_crc(frame)
            raise ModbusExceptionResponse(response_function & 0x7F, header[2])

        if response_function != function:
            raise ModbusError(f"unexpected function: 0x{response_function:02X}")

        if function == 0x03:
            byte_count = header[2]
            tail = self._read_exact(byte_count + 2)
            frame = header + tail
            check_crc(frame)
            return frame[3:-2]

        if response_data_len is None:
            raise ModbusError("internal error: response length not provided")

        tail = self._read_exact(response_data_len - 3)
        frame = header + tail
        check_crc(frame)
        return frame[2:-2]

    def read_holding(self, address: int, count: int) -> list[int]:
        if not 1 <= count <= 125:
            raise ValueError("holding-register read count must be 1..125")
        payload = struct.pack(">HH", address, count)
        data = self._request(0x03, payload, None)
        if len(data) != count * 2:
            raise ModbusError(f"unexpected read length: {len(data)}")
        return list(struct.unpack(f">{count}H", data))

    def write_single(self, address: int, value: int) -> None:
        payload = struct.pack(">HH", address, value & 0xFFFF)
        response = self._request(0x06, payload, 8)
        echoed_addr, echoed_value = struct.unpack(">HH", response)
        if echoed_addr != address or echoed_value != (value & 0xFFFF):
            raise ModbusError("write-single echo mismatch")

    def write_multiple(self, address: int, values: list[int]) -> None:
        if not 1 <= len(values) <= 123:
            raise ValueError("holding-register write count must be 1..123")
        regs = struct.pack(f">{len(values)}H", *[value & 0xFFFF for value in values])
        payload = struct.pack(">HHB", address, len(values), len(regs)) + regs
        response = self._request(0x10, payload, 8)
        echoed_addr, echoed_count = struct.unpack(">HH", response)
        if echoed_addr != address or echoed_count != len(values):
            raise ModbusError("write-multiple echo mismatch")


class Rs485DfuUploader:
    def __init__(
        self,
        client: ModbusRtuClient,
        log,
        progress,
        chunk_bytes: int,
        reboot_after_upload: bool,
        unlock_key: int,
    ) -> None:
        self.client = client
        self.log = log
        self.progress = progress
        self.chunk_bytes = chunk_bytes
        self.reboot_after_upload = reboot_after_upload
        self.unlock_key = unlock_key

    def read_status(self) -> tuple[int, int]:
        status, error = self.client.read_holding(DFU_REG_STATUS, 2)
        return status, error

    def read_bytes_written(self) -> int:
        high, low = self.client.read_holding(DFU_REG_BYTES_WRITTEN_HI, 2)
        return u32_from_words(high, low)

    def command(self, value: int, retries: int = 1) -> None:
        last_error: Exception | None = None
        for _ in range(retries + 1):
            try:
                self.client.write_single(DFU_REG_CONTROL, value)
                return
            except Exception as exc:  # noqa: BLE001 - logged and retried.
                last_error = exc
                time.sleep(0.1)
        assert last_error is not None
        raise last_error

    def wait_for_status(self, wanted: set[int], timeout_s: float) -> int:
        deadline = time.monotonic() + timeout_s
        while time.monotonic() < deadline:
            status, error = self.read_status()
            if status in wanted:
                return status
            if status == DFU_STATUS_ERROR:
                raise RuntimeError(f"device entered DFU error state: {signed16(error)}")
            time.sleep(0.2)
        raise TimeoutError("timed out waiting for DFU status")

    def upload(self, image_path: Path) -> None:
        image = image_path.read_bytes()
        if not image:
            raise RuntimeError("firmware image is empty")

        image_size = len(image)
        image_crc = zlib.crc32(image) & 0xFFFFFFFF
        image_sha = hashlib.sha256(image).digest()

        self.log(f"Image: {image_path}")
        self.log(f"Size : {image_size} bytes")
        self.log(f"CRC32: 0x{image_crc:08X}")
        self.log(f"SHA256: {image_sha.hex()}")

        self.log("Unlocking RS485 DFU service window...")
        self.client.write_single(DFU_REG_UNLOCK_KEY, self.unlock_key)
        remaining_s = self.client.read_holding(DFU_REG_UNLOCK_REMAINING_S, 1)[0]
        if remaining_s == 0:
            raise RuntimeError("device did not open the DFU service window")
        self.log(f"DFU service window open for {remaining_s} seconds")

        device_max_chunk = self.client.read_holding(DFU_REG_MAX_CHUNK_BYTES, 1)[0]
        chunk_bytes = min(self.chunk_bytes, device_max_chunk)
        chunk_bytes -= chunk_bytes % 2
        if chunk_bytes < 16:
            raise RuntimeError(f"invalid chunk size negotiated with device: {chunk_bytes}")

        self.log(f"Device max chunk: {device_max_chunk} bytes")
        self.log(f"Using chunk: {chunk_bytes} bytes")

        self.log("Aborting any previous DFU session...")
        self.command(DFU_CMD_ABORT, retries=2)

        size_hi, size_lo = words_from_u32(image_size)
        crc_hi, crc_lo = words_from_u32(image_crc)
        self.client.write_multiple(
            DFU_REG_IMAGE_SIZE_HI, [size_hi, size_lo, crc_hi, crc_lo]
        )
        self.client.write_multiple(DFU_REG_SHA256_BASE, words_from_bytes(image_sha))

        self.log("Starting DFU session...")
        self.command(DFU_CMD_BEGIN, retries=2)
        self.wait_for_status({DFU_STATUS_RECEIVING}, timeout_s=5.0)

        offset = 0
        while offset < image_size:
            chunk = image[offset : offset + chunk_bytes]
            self._write_chunk(offset, chunk)
            offset += len(chunk)
            self.progress(offset, image_size)
            self.log(f"Uploaded {offset}/{image_size} bytes")

        self.log("Verifying image in secondary slot...")
        self.command(DFU_CMD_VERIFY, retries=1)
        self.wait_for_status({DFU_STATUS_VERIFIED}, timeout_s=15.0)
        self.log("Image verified by device.")

        if self.reboot_after_upload:
            self.log("Marking image for MCUboot test swap and rebooting...")
            self.command(DFU_CMD_TEST_REBOOT, retries=1)
            self.log("Device should reboot. Reopen the port after enumeration.")
        else:
            self.log("Upload complete. Image is verified but not marked for boot.")

    def _write_chunk(self, offset: int, chunk: bytes) -> None:
        chunk_crc = zlib.crc32(chunk) & 0xFFFFFFFF
        offset_hi, offset_lo = words_from_u32(offset)
        crc_hi, crc_lo = words_from_u32(chunk_crc)
        data_words = words_from_bytes(chunk)
        expected_written = offset + len(chunk)

        for attempt in range(1, 4):
            try:
                self.client.write_multiple(DFU_REG_DATA_BASE, data_words)
                self.client.write_multiple(
                    DFU_REG_CHUNK_OFFSET_HI,
                    [offset_hi, offset_lo, len(chunk), crc_hi, crc_lo],
                )
                self.command(DFU_CMD_WRITE, retries=0)
                written = self.read_bytes_written()
                if written != expected_written:
                    raise ModbusError(
                        f"device wrote {written} bytes, expected {expected_written}"
                    )
                return
            except Exception as exc:  # noqa: BLE001 - upload should recover from line noise.
                try:
                    written = self.read_bytes_written()
                    if written == expected_written:
                        self.log(
                            f"Chunk at {offset} accepted after ambiguous response."
                        )
                        return
                except Exception:
                    pass

                if attempt >= 3:
                    raise

                self.log(f"Retry chunk offset {offset}, attempt {attempt + 1}: {exc}")
                time.sleep(0.2)


def signed16(value: int) -> int:
    value &= 0xFFFF
    if value & 0x8000:
        return value - 0x10000
    return value


def format_scaled(value: int | float | None, scale: float, unit: str, decimals: int = 2) -> str:
    if value is None:
        return "-"
    return f"{float(value) / scale:.{decimals}f} {unit}"


def parse_unit_ids(text: str) -> list[int]:
    result: list[int] = []
    seen: set[int] = set()
    for item in text.split(","):
        item = item.strip()
        if not item:
            continue
        if "-" in item:
            start_text, end_text = item.split("-", 1)
            start = int(start_text.strip())
            end = int(end_text.strip())
            if start > end:
                raise ValueError(f"Invalid Unit ID range: {item}")
            values = range(start, end + 1)
        else:
            values = (int(item),)

        for unit_id in values:
            if not 1 <= unit_id <= 247:
                raise ValueError("Scan Unit IDs must be in the Modbus range 1..247.")
            if unit_id not in seen:
                seen.add(unit_id)
                result.append(unit_id)

    if not result:
        raise ValueError("Enter at least one Unit ID to scan.")
    return result


def parse_u16_text(text: str, field_name: str) -> int:
    try:
        value = int(text.strip(), 0)
    except ValueError as exc:
        raise ValueError(f"{field_name} must be a decimal or 0x-prefixed value.") from exc

    if not 0 <= value <= 0xFFFF:
        raise ValueError(f"{field_name} must be 0..0xFFFF.")

    return value


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ArgiSense RS485 Service Tool")
        self.geometry("1060x760")
        self.minsize(920, 680)

        self.messages: queue.Queue[tuple[str, object]] = queue.Queue()
        self.worker: threading.Thread | None = None
        self.action_buttons: list[ttk.Button] = []
        self.monitoring = False
        self.sample_history: list[dict[str, float | int | bool | str | None]] = []
        self.detected_devices: list[dict[str, int | str]] = []
        self.scan_cancel_event = threading.Event()
        self.current_product = "methane-pressure"

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.data_bits_var = tk.StringVar(value="8")
        self.parity_var = tk.StringVar(value="None (N)")
        self.stop_bits_var = tk.StringVar(value="2")
        self.unit_var = tk.StringVar(value="1")
        self.scan_units_var = tk.StringVar(value="1-10,247")
        self.scan_all_ports_var = tk.BooleanVar(value=False)
        self.timeout_var = tk.StringVar(value="1.0")
        self.chunk_var = tk.StringVar(value=str(DEFAULT_CHUNK_BYTES))
        self.unlock_key_var = tk.StringVar(value=DEFAULT_DFU_UNLOCK_KEY)
        self.file_var = tk.StringVar()
        self.reboot_var = tk.BooleanVar(value=True)
        self.status_var = tk.StringVar(value="Idle")
        self.poll_interval_var = tk.StringVar(value="2.0")
        self.graph_window_var = tk.StringVar(value=str(DEFAULT_GRAPH_WINDOW_SAMPLES))
        self.sensor_vars = {
            "methane": tk.StringVar(value="-"),
            "pressure": tk.StringVar(value="-"),
            "humidity": tk.StringVar(value="-"),
            "ambient_temp": tk.StringVar(value="-"),
            "pressure_temp": tk.StringVar(value="-"),
            "dac0": tk.StringVar(value="-"),
            "dac1": tk.StringVar(value="-"),
            "sequence": tk.StringVar(value="-"),
            "uptime": tk.StringVar(value="-"),
            "status": tk.StringVar(value="-"),
        }
        self.sensor_label_vars = {
            "methane": tk.StringVar(value="Methane"),
            "pressure": tk.StringVar(value="Pressure"),
            "humidity": tk.StringVar(value="Humidity"),
            "ambient_temp": tk.StringVar(value="Ambient temp"),
            "pressure_temp": tk.StringVar(value="Pressure temp"),
            "dac0": tk.StringVar(value="DAC0 methane"),
            "dac1": tk.StringVar(value="DAC1 pressure"),
            "sequence": tk.StringVar(value="Sample seq"),
            "uptime": tk.StringVar(value="Uptime"),
            "status": tk.StringVar(value="Status"),
        }
        self.sensor_value_rows: dict[str, tuple[ttk.Label, ttk.Label]] = {}
        self.config_vars = {
            "unit_id": tk.StringVar(value="1"),
            "baud_preset": tk.StringVar(value="115200"),
            "data_bits": tk.StringVar(value="8"),
            "parity": tk.StringVar(value="None (0)"),
            "stop_bits": tk.StringVar(value="2"),
            "termination": tk.BooleanVar(value=False),
            "period_s": tk.StringVar(value="60"),
            "window_ms": tk.StringVar(value="1000"),
            "dac_min": tk.StringVar(value="4000"),
            "dac_max": tk.StringVar(value="20000"),
            "dac_fault": tk.StringVar(value="3600"),
            "map_version": tk.StringVar(value="-"),
            "reboot_required": tk.StringVar(value="-"),
        }

        self._build_ui()
        self.refresh_ports()
        self.after(100, self._poll_messages)

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=12)
        root.pack(fill=tk.BOTH, expand=True)

        settings = ttk.LabelFrame(root, text="Connection")
        settings.pack(fill=tk.X)

        ttk.Label(settings, text="Port").grid(row=0, column=0, sticky=tk.W, padx=6, pady=6)
        self.port_combo = ttk.Combobox(settings, textvariable=self.port_var, width=24)
        self.port_combo.grid(row=0, column=1, sticky=tk.EW, padx=6, pady=6)
        ttk.Button(settings, text="Refresh", command=self.refresh_ports).grid(
            row=0, column=2, sticky=tk.W, padx=6, pady=6
        )
        self.auto_detect_button = ttk.Button(
            settings, text="Auto Detect", command=self.auto_detect
        )
        self.auto_detect_button.grid(row=0, column=7, sticky=tk.E, padx=6, pady=6)
        self.action_buttons.append(self.auto_detect_button)

        ttk.Label(settings, text="Baud").grid(row=0, column=3, sticky=tk.W, padx=6, pady=6)
        ttk.Entry(settings, textvariable=self.baud_var, width=10).grid(
            row=0, column=4, sticky=tk.W, padx=6, pady=6
        )

        ttk.Label(settings, text="Unit ID").grid(row=0, column=5, sticky=tk.W, padx=6, pady=6)
        ttk.Entry(settings, textvariable=self.unit_var, width=6).grid(
            row=0, column=6, sticky=tk.W, padx=6, pady=6
        )

        ttk.Label(settings, text="Timeout").grid(row=1, column=0, sticky=tk.W, padx=6, pady=6)
        ttk.Entry(settings, textvariable=self.timeout_var, width=8).grid(
            row=1, column=1, sticky=tk.W, padx=6, pady=6
        )
        ttk.Label(settings, text="seconds").grid(row=1, column=2, sticky=tk.W, padx=0, pady=6)

        ttk.Label(settings, text="Chunk").grid(row=1, column=3, sticky=tk.W, padx=6, pady=6)
        ttk.Entry(settings, textvariable=self.chunk_var, width=8).grid(
            row=1, column=4, sticky=tk.W, padx=6, pady=6
        )
        ttk.Label(settings, text="bytes").grid(row=1, column=5, sticky=tk.W, padx=0, pady=6)

        ttk.Label(settings, text="Scan IDs").grid(row=1, column=6, sticky=tk.W, padx=6, pady=6)
        ttk.Entry(settings, textvariable=self.scan_units_var, width=14).grid(
            row=1, column=7, sticky=tk.W, padx=6, pady=6
        )

        ttk.Label(settings, text="Data bits").grid(row=2, column=0, sticky=tk.W, padx=6, pady=6)
        self.data_bits_combo = ttk.Combobox(
            settings,
            textvariable=self.data_bits_var,
            values=list(DATA_BITS_OPTIONS.keys()),
            state="readonly",
            width=8,
        )
        self.data_bits_combo.grid(row=2, column=1, sticky=tk.W, padx=6, pady=6)

        ttk.Label(settings, text="Parity").grid(row=2, column=3, sticky=tk.W, padx=6, pady=6)
        self.parity_combo = ttk.Combobox(
            settings,
            textvariable=self.parity_var,
            values=list(PARITY_OPTIONS.keys()),
            state="readonly",
            width=10,
        )
        self.parity_combo.grid(row=2, column=4, sticky=tk.W, padx=6, pady=6)

        ttk.Label(settings, text="Stop bits").grid(row=2, column=5, sticky=tk.W, padx=6, pady=6)
        self.stop_bits_combo = ttk.Combobox(
            settings,
            textvariable=self.stop_bits_var,
            values=list(STOP_BITS_OPTIONS.keys()),
            state="readonly",
            width=8,
        )
        self.stop_bits_combo.grid(row=2, column=6, sticky=tk.W, padx=6, pady=6)

        ttk.Checkbutton(
            settings,
            text="All COM ports",
            variable=self.scan_all_ports_var,
        ).grid(row=2, column=7, sticky=tk.W, padx=6, pady=6)

        settings.columnconfigure(1, weight=1)

        detected = ttk.LabelFrame(root, text="Detected devices")
        detected.pack(fill=tk.X, pady=(10, 0))
        columns = ("port", "product", "unit", "baud", "framing", "map")
        self.detected_tree = ttk.Treeview(
            detected,
            columns=columns,
            show="headings",
            height=4,
            selectmode="browse",
        )
        headings = {
            "port": ("Port", 120),
            "product": ("Product", 120),
            "unit": ("Unit ID", 70),
            "baud": ("Baud", 90),
            "framing": ("Framing", 80),
            "map": ("Map", 60),
        }
        for column, (label, width) in headings.items():
            self.detected_tree.heading(column, text=label)
            self.detected_tree.column(column, width=width, anchor=tk.CENTER)
        self.detected_tree.column("port", anchor=tk.W)
        self.detected_tree.pack(side=tk.LEFT, fill=tk.X, expand=True, padx=6, pady=6)
        self.detected_tree.bind("<Double-1>", lambda _event: self.use_selected_detected_device())

        detected_actions = ttk.Frame(detected)
        detected_actions.pack(side=tk.RIGHT, fill=tk.Y, padx=(0, 6), pady=6)
        self.use_detected_button = ttk.Button(
            detected_actions,
            text="Use Selected",
            command=self.use_selected_detected_device,
        )
        self.use_detected_button.pack(anchor=tk.N)
        self.action_buttons.append(self.use_detected_button)
        self.stop_scan_button = ttk.Button(
            detected_actions,
            text="Stop Scan",
            command=self.stop_scan,
            state=tk.DISABLED,
        )
        self.stop_scan_button.pack(anchor=tk.N, pady=(8, 0))

        content = ttk.PanedWindow(root, orient=tk.VERTICAL)
        content.pack(fill=tk.BOTH, expand=True, pady=(10, 0))

        notebook_frame = ttk.Frame(content)
        log_panel = ttk.Frame(content)
        content.add(notebook_frame, weight=4)
        content.add(log_panel, weight=1)

        self.notebook = ttk.Notebook(notebook_frame)
        self.notebook.pack(fill=tk.BOTH, expand=True)

        firmware_tab = ttk.Frame(self.notebook, padding=8)
        sensors_tab = ttk.Frame(self.notebook, padding=8)
        config_tab = ttk.Frame(self.notebook, padding=8)
        self.notebook.add(firmware_tab, text="Firmware Update")
        self.notebook.add(sensors_tab, text="Sensors")
        self.notebook.add(config_tab, text="Device Config")

        self._build_firmware_tab(firmware_tab)
        self._build_sensors_tab(sensors_tab)
        self._build_config_tab(config_tab)

        status = ttk.Frame(log_panel)
        status.pack(fill=tk.X)
        ttk.Label(status, text="Status").pack(side=tk.LEFT)
        ttk.Label(status, textvariable=self.status_var).pack(side=tk.LEFT, padx=(8, 0))

        log_frame = ttk.LabelFrame(log_panel, text="Log")
        log_frame.pack(fill=tk.BOTH, expand=True, pady=(6, 0))
        self.log_text = scrolledtext.ScrolledText(log_frame, height=7, wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)
        self.log_text.configure(state=tk.DISABLED)

    def _build_firmware_tab(self, tab: ttk.Frame) -> None:
        firmware = ttk.LabelFrame(tab, text="Firmware image")
        firmware.pack(fill=tk.X)

        ttk.Entry(firmware, textvariable=self.file_var).grid(
            row=0, column=0, sticky=tk.EW, padx=6, pady=6
        )
        ttk.Button(firmware, text="Browse", command=self.browse_file).grid(
            row=0, column=1, sticky=tk.W, padx=6, pady=6
        )
        ttk.Checkbutton(
            firmware,
            text="Mark test image and reboot after verify",
            variable=self.reboot_var,
        ).grid(row=1, column=0, columnspan=2, sticky=tk.W, padx=6, pady=(0, 6))
        ttk.Label(firmware, text="DFU unlock key").grid(
            row=2, column=0, sticky=tk.W, padx=6, pady=(0, 6)
        )
        ttk.Entry(firmware, textvariable=self.unlock_key_var, width=12).grid(
            row=2, column=0, sticky=tk.W, padx=(110, 6), pady=(0, 6)
        )

        firmware.columnconfigure(0, weight=1)

        actions = ttk.Frame(tab)
        actions.pack(fill=tk.X, pady=(10, 0))
        self.probe_button = ttk.Button(actions, text="Probe", command=self.probe)
        self.probe_button.pack(side=tk.LEFT)
        self.upload_button = ttk.Button(actions, text="Upload Firmware", command=self.upload)
        self.upload_button.pack(side=tk.LEFT, padx=(8, 0))
        self.confirm_image_button = ttk.Button(
            actions, text="Confirm Image", command=self.confirm_image
        )
        self.confirm_image_button.pack(side=tk.LEFT, padx=(8, 0))
        self.action_buttons.extend(
            [self.probe_button, self.upload_button, self.confirm_image_button]
        )

        self.progress_bar = ttk.Progressbar(tab, mode="determinate", maximum=100)
        self.progress_bar.pack(fill=tk.X, pady=(10, 0))

    def _build_sensors_tab(self, tab: ttk.Frame) -> None:
        toolbar = ttk.Frame(tab)
        toolbar.pack(fill=tk.X)
        self.read_sensors_button = ttk.Button(
            toolbar, text="Read Once", command=self.read_sensors_once
        )
        self.read_sensors_button.pack(side=tk.LEFT)
        self.start_monitor_button = ttk.Button(
            toolbar, text="Start Graph", command=self.start_monitor
        )
        self.start_monitor_button.pack(side=tk.LEFT, padx=(8, 0))
        self.stop_monitor_button = ttk.Button(
            toolbar, text="Stop", command=self.stop_monitor
        )
        self.stop_monitor_button.pack(side=tk.LEFT, padx=(8, 0))
        ttk.Label(toolbar, text="Poll").pack(side=tk.LEFT, padx=(16, 4))
        ttk.Entry(toolbar, textvariable=self.poll_interval_var, width=7).pack(side=tk.LEFT)
        ttk.Label(toolbar, text="s").pack(side=tk.LEFT, padx=(4, 0))
        ttk.Label(toolbar, text="Window").pack(side=tk.LEFT, padx=(16, 4))
        self.graph_window_entry = ttk.Entry(
            toolbar, textvariable=self.graph_window_var, width=7
        )
        self.graph_window_entry.pack(side=tk.LEFT)
        self.graph_window_entry.bind("<Return>", lambda _event: self._redraw_graph())
        self.graph_window_entry.bind("<FocusOut>", lambda _event: self._redraw_graph())
        ttk.Label(toolbar, text="samples").pack(side=tk.LEFT, padx=(4, 0))
        self.action_buttons.extend(
            [self.read_sensors_button, self.start_monitor_button]
        )

        values = ttk.LabelFrame(tab, text="Latest values")
        values.pack(fill=tk.X, pady=(8, 0))
        rows = [
            ("Methane", "methane"),
            ("Pressure", "pressure"),
            ("Humidity", "humidity"),
            ("Ambient temp", "ambient_temp"),
            ("Pressure temp", "pressure_temp"),
            ("DAC0 methane", "dac0"),
            ("DAC1 pressure", "dac1"),
            ("Sample seq", "sequence"),
            ("Uptime", "uptime"),
            ("Status", "status"),
        ]
        for index, (label, key) in enumerate(rows):
            if key == "status":
                row = 3
                col = 0
                value_columnspan = 5
            else:
                row = index // 3
                col = (index % 3) * 2
                value_columnspan = 1
            label_widget = ttk.Label(
                values, textvariable=self.sensor_label_vars[key]
            )
            value_widget = ttk.Label(values, textvariable=self.sensor_vars[key])
            label_widget.grid(
                row=row, column=col, sticky=tk.W, padx=(6, 4), pady=2
            )
            value_widget.grid(
                row=row,
                column=col + 1,
                columnspan=value_columnspan,
                sticky=tk.W,
                padx=(4, 18),
                pady=2,
            )
            self.sensor_value_rows[key] = (label_widget, value_widget)
        values.columnconfigure(1, weight=1)
        values.columnconfigure(3, weight=1)
        values.columnconfigure(5, weight=1)

        graph_frame = ttk.LabelFrame(tab, text="Trend graph")
        graph_frame.pack(fill=tk.BOTH, expand=True, pady=(6, 0))
        self.graph_canvas = tk.Canvas(
            graph_frame,
            height=360,
            background="white",
            highlightthickness=1,
            highlightbackground="#b8c0cc",
        )
        self.graph_canvas.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)
        self.graph_canvas.bind("<Configure>", lambda _event: self._redraw_graph())

    def _build_config_tab(self, tab: ttk.Frame) -> None:
        actions = ttk.Frame(tab)
        actions.pack(fill=tk.X)
        self.read_config_button = ttk.Button(
            actions, text="Read Config", command=self.read_config
        )
        self.read_config_button.pack(side=tk.LEFT)
        self.apply_config_button = ttk.Button(
            actions, text="Apply Config", command=self.apply_config
        )
        self.apply_config_button.pack(side=tk.LEFT, padx=(8, 0))
        self.reboot_button = ttk.Button(
            actions, text="Reboot Device", command=self.reboot_device
        )
        self.reboot_button.pack(side=tk.LEFT, padx=(8, 0))
        self.action_buttons.extend(
            [self.read_config_button, self.apply_config_button, self.reboot_button]
        )

        transport = ttk.LabelFrame(tab, text="RS485 transport")
        transport.pack(fill=tk.X, pady=(10, 0))
        ttk.Label(transport, text="Unit ID").grid(row=0, column=0, sticky=tk.W, padx=6, pady=6)
        ttk.Entry(transport, textvariable=self.config_vars["unit_id"], width=8).grid(
            row=0, column=1, sticky=tk.W, padx=6, pady=6
        )
        ttk.Label(transport, text="Baud").grid(row=0, column=2, sticky=tk.W, padx=6, pady=6)
        ttk.Combobox(
            transport,
            textvariable=self.config_vars["baud_preset"],
            values=list(BAUD_PRESET_OPTIONS.keys()),
            state="readonly",
            width=10,
        ).grid(row=0, column=3, sticky=tk.W, padx=6, pady=6)
        ttk.Label(transport, text="Data bits").grid(row=0, column=4, sticky=tk.W, padx=6, pady=6)
        ttk.Combobox(
            transport,
            textvariable=self.config_vars["data_bits"],
            values=list(DATA_BITS_OPTIONS.keys()),
            state="readonly",
            width=8,
        ).grid(row=0, column=5, sticky=tk.W, padx=6, pady=6)
        ttk.Label(transport, text="Parity").grid(row=1, column=0, sticky=tk.W, padx=6, pady=6)
        ttk.Combobox(
            transport,
            textvariable=self.config_vars["parity"],
            values=list(PARITY_CODE_OPTIONS.keys()),
            state="readonly",
            width=10,
        ).grid(row=1, column=1, sticky=tk.W, padx=6, pady=6)
        ttk.Label(transport, text="Stop bits").grid(row=1, column=2, sticky=tk.W, padx=6, pady=6)
        ttk.Combobox(
            transport,
            textvariable=self.config_vars["stop_bits"],
            values=list(STOP_BITS_OPTIONS.keys()),
            state="readonly",
            width=8,
        ).grid(row=1, column=3, sticky=tk.W, padx=6, pady=6)
        ttk.Checkbutton(
            transport,
            text="RS485 termination",
            variable=self.config_vars["termination"],
        ).grid(row=1, column=4, columnspan=2, sticky=tk.W, padx=6, pady=6)

        runtime = ttk.LabelFrame(tab, text="Runtime")
        runtime.pack(fill=tk.X, pady=(10, 0))
        runtime_rows = [
            ("Measurement period", "period_s", "s"),
            ("Measurement window", "window_ms", "ms"),
            ("DAC min current", "dac_min", "uA"),
            ("DAC max current", "dac_max", "uA"),
            ("DAC fault current", "dac_fault", "uA"),
        ]
        for index, (label, key, unit) in enumerate(runtime_rows):
            ttk.Label(runtime, text=label).grid(
                row=index, column=0, sticky=tk.W, padx=6, pady=4
            )
            ttk.Entry(runtime, textvariable=self.config_vars[key], width=12).grid(
                row=index, column=1, sticky=tk.W, padx=6, pady=4
            )
            ttk.Label(runtime, text=unit).grid(
                row=index, column=2, sticky=tk.W, padx=0, pady=4
            )

        info = ttk.LabelFrame(tab, text="Device state")
        info.pack(fill=tk.X, pady=(10, 0))
        ttk.Label(info, text="Register map").grid(row=0, column=0, sticky=tk.W, padx=6, pady=4)
        ttk.Label(info, textvariable=self.config_vars["map_version"]).grid(
            row=0, column=1, sticky=tk.W, padx=6, pady=4
        )
        ttk.Label(info, text="Reboot required").grid(row=0, column=2, sticky=tk.W, padx=6, pady=4)
        ttk.Label(info, textvariable=self.config_vars["reboot_required"]).grid(
            row=0, column=3, sticky=tk.W, padx=6, pady=4
        )

    def refresh_ports(self) -> None:
        if list_ports is None:
            self.port_combo["values"] = []
            self.log("pyserial is not installed.")
            return
        ports = [port.device for port in list_ports.comports()]
        self.port_combo["values"] = ports
        if ports and not self.port_var.get():
            self.port_var.set(ports[0])

    def browse_file(self) -> None:
        initial = Path.cwd() / "build" / "argisense-zephyr-app" / "zephyr"
        filename = filedialog.askopenfilename(
            title="Select MCUboot signed binary",
            initialdir=str(initial if initial.exists() else Path.cwd()),
            filetypes=(("Signed binary", "*.bin"), ("All files", "*.*")),
        )
        if filename:
            self.file_var.set(filename)

    def log(self, message: str) -> None:
        self.messages.put(("log", message))

    def progress(self, done: int, total: int) -> None:
        self.messages.put(("progress", (done, total)))

    def set_busy(self, busy: bool) -> None:
        state = tk.DISABLED if busy else tk.NORMAL
        for button in self.action_buttons:
            button.configure(state=state)
        self.stop_monitor_button.configure(state=tk.NORMAL)

    def auto_detect(self) -> None:
        if self.worker and self.worker.is_alive():
            messagebox.showinfo("ArgiSense RS485", "An operation is already running.")
            return
        if self.scan_all_ports_var.get():
            if not messagebox.askyesno(
                "ArgiSense RS485",
                "Auto Detect will open every COM port it can find and send "
                "Modbus probe frames. Continue?",
            ):
                return
        try:
            parse_unit_ids(self.scan_units_var.get())
            timeout = float(self.timeout_var.get())
            if timeout <= 0:
                raise ValueError("Timeout must be positive.")
        except Exception as exc:  # noqa: BLE001 - presented to operator.
            messagebox.showerror("ArgiSense RS485", str(exc))
            return

        self.monitoring = False
        self.progress_bar["value"] = 0
        self._clear_detected_devices_ui()
        self.scan_cancel_event.clear()
        self.set_busy(True)
        self.stop_scan_button.configure(state=tk.NORMAL)
        self.status_var.set("Scanning")
        self.worker = threading.Thread(
            target=lambda: self._auto_detect_worker(reschedule_monitor=False),
            daemon=True,
        )
        self.worker.start()

    def probe(self) -> None:
        self._start_worker(self._probe_worker)

    def upload(self) -> None:
        self._start_worker(self._upload_worker, require_chunk=True)

    def confirm_image(self) -> None:
        if not messagebox.askyesno(
            "ArgiSense RS485",
            "Confirm the currently running MCUboot image? This disables "
            "automatic rollback for this image.",
        ):
            return
        self._start_worker(self._confirm_image_worker)

    def read_sensors_once(self) -> None:
        self._start_worker(self._read_sensors_worker)

    def start_monitor(self) -> None:
        try:
            interval = float(self.poll_interval_var.get())
        except ValueError:
            messagebox.showerror("ArgiSense RS485", "Poll interval must be numeric.")
            return
        if interval <= 0:
            messagebox.showerror("ArgiSense RS485", "Poll interval must be positive.")
            return
        try:
            window = int(self.graph_window_var.get())
        except ValueError:
            messagebox.showerror("ArgiSense RS485", "Graph window must be numeric.")
            return
        if window <= 1:
            messagebox.showerror("ArgiSense RS485", "Graph window must be at least 2 samples.")
            return
        self.monitoring = True
        self.status_var.set("Graph polling")
        self._monitor_tick()

    def stop_monitor(self) -> None:
        self.monitoring = False
        self.status_var.set("Graph stopped")

    def read_config(self) -> None:
        self._start_worker(self._read_config_worker)

    def apply_config(self) -> None:
        self._start_worker(self._apply_config_worker)

    def reboot_device(self) -> None:
        if not messagebox.askyesno(
            "ArgiSense RS485",
            "Reboot the device now? The RS485 port may disconnect briefly.",
        ):
            return
        self._start_worker(self._reboot_worker)

    def use_selected_detected_device(self) -> None:
        selection = self.detected_tree.selection()
        if not selection:
            messagebox.showinfo("ArgiSense RS485", "Select a detected device first.")
            return

        index = int(selection[0])
        if index < 0 or index >= len(self.detected_devices):
            messagebox.showerror("ArgiSense RS485", "Selected device is no longer valid.")
            return

        connection = self.detected_devices[index]
        self._update_connection_ui(connection)
        summary = self._format_connection_summary(connection)
        self.status_var.set(f"Selected {summary}")
        self._append_log(f"Selected detected device: {summary}")

    def stop_scan(self) -> None:
        if not (self.worker and self.worker.is_alive()):
            self.stop_scan_button.configure(state=tk.DISABLED)
            return

        self.scan_cancel_event.set()
        self.stop_scan_button.configure(state=tk.DISABLED)
        self.status_var.set("Stopping scan")
        self._append_log("Stop scan requested")

    def _monitor_tick(self) -> None:
        if not self.monitoring:
            return
        if self.worker and self.worker.is_alive():
            self.after(250, self._monitor_tick)
            return
        self._start_worker(self._read_sensors_worker, reschedule_monitor=True)

    def _start_worker(
        self,
        target,
        *,
        require_chunk: bool = False,
        reschedule_monitor: bool = False,
    ) -> None:
        if self.worker and self.worker.is_alive():
            if not reschedule_monitor:
                messagebox.showinfo("ArgiSense RS485", "An operation is already running.")
            return
        try:
            self._validate_common(require_chunk=require_chunk)
        except Exception as exc:  # noqa: BLE001 - presented to operator.
            self.monitoring = False
            messagebox.showerror("ArgiSense RS485", str(exc))
            return
        self.set_busy(True)
        self.status_var.set("Working")
        self.worker = threading.Thread(
            target=lambda: target(reschedule_monitor=reschedule_monitor),
            daemon=True,
        )
        self.worker.start()

    def _validate_common(self, *, require_chunk: bool = False) -> None:
        if not self.port_var.get():
            raise ValueError("Select a serial port.")
        baud = int(self.baud_var.get())
        unit_id = int(self.unit_var.get())
        timeout = float(self.timeout_var.get())
        if self.data_bits_var.get() not in DATA_BITS_OPTIONS:
            raise ValueError("Select a valid data-bits value.")
        if self.parity_var.get() not in PARITY_OPTIONS:
            raise ValueError("Select a valid parity value.")
        if self.stop_bits_var.get() not in STOP_BITS_OPTIONS:
            raise ValueError("Select 1 or 2 stop bits.")
        if not 1 <= unit_id <= 247:
            raise ValueError("Unit ID must be 1..247.")
        if baud <= 0:
            raise ValueError("Baudrate must be positive.")
        if timeout <= 0:
            raise ValueError("Timeout must be positive.")
        if require_chunk:
            chunk = int(self.chunk_var.get())
            if chunk < 16 or chunk > 128 or chunk % 2:
                raise ValueError("Chunk must be an even value from 16 to 128 bytes.")

    def _make_client(self) -> ModbusRtuClient:
        return self._make_client_for(
            port=self.port_var.get(),
            baudrate=int(self.baud_var.get()),
            unit_id=int(self.unit_var.get()),
            timeout_s=float(self.timeout_var.get()),
            bytesize=DATA_BITS_OPTIONS[self.data_bits_var.get()],
            parity=PARITY_OPTIONS[self.parity_var.get()],
            stopbits=STOP_BITS_OPTIONS[self.stop_bits_var.get()],
        )

    def _make_client_for(
        self,
        *,
        port: str,
        baudrate: int,
        unit_id: int,
        timeout_s: float,
        bytesize: int,
        parity: str,
        stopbits: int,
    ) -> ModbusRtuClient:
        return ModbusRtuClient(
            port=port,
            baudrate=baudrate,
            unit_id=unit_id,
            timeout_s=timeout_s,
            bytesize=bytesize,
            parity=parity,
            stopbits=stopbits,
        )

    def _scan_port_candidates(self) -> list[str]:
        selected = self.port_var.get().strip()
        if self.scan_all_ports_var.get():
            if list_ports is None:
                if selected:
                    return [selected]
                raise RuntimeError(
                    "pyserial is not installed. Run: py -3.12 -m pip install -r tools\\rs485_dfu\\requirements.txt"
                )
            ports = [port.device for port in list_ports.comports()]
            if selected and selected not in ports:
                ports.insert(0, selected)
            if not ports:
                raise RuntimeError("No serial ports found.")
            return ports

        if not selected:
            raise RuntimeError("Select a serial port or enable All COM ports.")
        return [selected]

    def _auto_detect_worker(self, *, reschedule_monitor: bool = False) -> None:
        ARG_UNUSED = reschedule_monitor
        del ARG_UNUSED
        try:
            if serial is None:
                raise RuntimeError(
                    "pyserial is not installed. Run: py -3.12 -m pip install -r tools\\rs485_dfu\\requirements.txt"
                )

            ports = self._scan_port_candidates()
            unit_ids = parse_unit_ids(self.scan_units_var.get())
            timeout_s = min(max(float(self.timeout_var.get()), 0.05), DISCOVERY_TIMEOUT_S)
            data_label = "8"
            data_bits = DATA_BITS_OPTIONS[data_label]
            total = (
                len(ports)
                * len(DISCOVERY_BAUDS)
                * len(DISCOVERY_PARITY_LABELS)
                * len(DISCOVERY_STOP_BITS)
                * len(unit_ids)
            )
            attempts = 0
            last_error: Exception | None = None
            found_devices: list[dict[str, int | str]] = []
            found_keys: set[tuple[str, int, int, int]] = set()

            self.log(
                "Auto detect started: "
                f"ports={','.join(ports)} ids={self.scan_units_var.get()} "
                f"timeout={timeout_s:.2f}s"
            )

            for port in ports:
                if self.scan_cancel_event.is_set():
                    break
                self.log(f"Scanning {port}")
                for baud_label in DISCOVERY_BAUDS:
                    if self.scan_cancel_event.is_set():
                        break
                    baudrate = int(baud_label)
                    for parity_label in DISCOVERY_PARITY_LABELS:
                        if self.scan_cancel_event.is_set():
                            break
                        parity = PARITY_OPTIONS[parity_label]
                        for stop_label in DISCOVERY_STOP_BITS:
                            if self.scan_cancel_event.is_set():
                                break
                            stopbits = STOP_BITS_OPTIONS[stop_label]
                            self.log(
                                f"Trying {port}: {baudrate} baud, "
                                f"{data_label}{parity}{stop_label}"
                            )
                            client: ModbusRtuClient | None = None
                            try:
                                client = self._make_client_for(
                                    port=port,
                                    baudrate=baudrate,
                                    unit_id=unit_ids[0],
                                    timeout_s=timeout_s,
                                    bytesize=data_bits,
                                    parity=parity,
                                    stopbits=stopbits,
                                )
                                time.sleep(0.02)

                                for unit_id in unit_ids:
                                    if self.scan_cancel_event.is_set():
                                        break
                                    client.unit_id = unit_id
                                    try:
                                        regs = client.read_holding(REG_DEVICE_ID, 2)
                                        device_id = regs[REG_DEVICE_ID]
                                        if device_id in ARGISENSE_DEVICE_PRODUCTS:
                                            found = {
                                                "port": port,
                                                "device_id": device_id,
                                                "product": product_name(device_id),
                                                "baudrate": baudrate,
                                                "unit_id": unit_id,
                                                "data_bits_label": data_label,
                                                "parity_label": parity_label,
                                                "stop_bits_label": stop_label,
                                                "map_version": regs[REG_MAP_VERSION],
                                            }
                                            key = (port, baudrate, unit_id, device_id)
                                            if key not in found_keys:
                                                found_keys.add(key)
                                                found_devices.append(found)
                                                self.messages.put(("detected_device", found))
                                                self.log(
                                                    "Found ArgiSense device: "
                                                    f"{self._format_connection_summary(found)}"
                                                )
                                            else:
                                                self.log(
                                                    "Duplicate response ignored for "
                                                    f"{port} unit={unit_id} {baudrate}; "
                                                    f"also answered as {data_label}{parity}{stop_label}"
                                                )
                                        else:
                                            self.log(
                                                f"{port} unit={unit_id} responded with "
                                                f"device_id=0x{device_id:04X}"
                                            )
                                    except Exception as exc:  # noqa: BLE001 - line noise/empty IDs are expected.
                                        last_error = exc
                                    finally:
                                        attempts += 1
                                        self.messages.put(("scan_progress", (attempts, total)))
                            except Exception as exc:  # noqa: BLE001 - try next serial format.
                                last_error = exc
                                self.log(
                                    f"Skip {port} {baudrate} {data_label}{parity}{stop_label}: {exc}"
                                )
                                attempts += len(unit_ids)
                                self.messages.put(("scan_progress", (attempts, total)))
                            finally:
                                if client is not None:
                                    client.close()

            scan_stopped = self.scan_cancel_event.is_set()
            if found_devices:
                selected = found_devices[0]
                self.messages.put(("connection", selected))
                client = None
                try:
                    client = self._make_client_for(
                        port=str(selected["port"]),
                        baudrate=int(selected["baudrate"]),
                        unit_id=int(selected["unit_id"]),
                        timeout_s=timeout_s,
                        bytesize=DATA_BITS_OPTIONS[str(selected["data_bits_label"])],
                        parity=PARITY_OPTIONS[str(selected["parity_label"])],
                        stopbits=STOP_BITS_OPTIONS[str(selected["stop_bits_label"])],
                    )
                    config = self._read_config_snapshot(client)
                    self.messages.put(("config", config))
                    try:
                        sample = self._read_sample(client)
                        self.messages.put(("sample", sample))
                    except Exception as exc:  # noqa: BLE001 - optional after discovery.
                        self.log(f"Sensor read after detect skipped: {exc}")
                except Exception as exc:  # noqa: BLE001 - connection details are still usable.
                    self.log(f"Post-scan config read skipped: {exc}")
                finally:
                    if client is not None:
                        client.close()

                self.messages.put(
                    (
                        "done",
                        f"Scan {'stopped' if scan_stopped else 'complete'}: "
                        f"found {len(found_devices)} device(s). "
                        f"Connected to first result: {self._format_connection_summary(selected)}. "
                        "Select another detected row and use Use Selected to switch.",
                    )
                )
                return

            if scan_stopped:
                self.messages.put(("done", "Scan stopped: no ArgiSense devices found."))
                return

            suffix = f" Last error: {last_error}" if last_error else ""
            raise RuntimeError(f"No ArgiSense device found.{suffix}")
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _read_sample(self, client: ModbusRtuClient) -> dict[str, float | int | bool | str | None]:
        live = client.read_holding(REG_DEVICE_ID, 20)
        device_id = live[REG_DEVICE_ID]
        status_flags = live[REG_STATUS_FLAGS]
        if is_ph_device(device_id):
            diagnostics = client.read_holding(70, 7)
            ph_x1000 = signed32_from_words(live[10], live[11])
            temperature_centi_c = signed32_from_words(live[12], live[13])
            ph_error = signed16(diagnostics[0])
            temperature_error = signed16(diagnostics[1])
            humidity_rh = None
            ambient_temp_c = None
            humidity_error_code = 0
            primary_value = ph_x1000 / 1000.0
            secondary_value = temperature_centi_c / 100.0
            pressure_temp_c = temperature_centi_c / 100.0
        else:
            diagnostics = client.read_holding(70, 13)
            methane_ppm_x100 = signed32_from_words(live[10], live[11])
            pressure_pa = signed32_from_words(live[12], live[13])
            humidity_rh_x100 = signed16(diagnostics[10])
            ambient_temp_centi_c = signed16(diagnostics[11])
            pressure_temp_centi_c = signed16(diagnostics[4])
            ph_error = signed16(diagnostics[2])
            temperature_error = signed16(diagnostics[3])
            humidity_error_code = signed16(diagnostics[12])
            humidity_rh = humidity_rh_x100 / 100.0
            ambient_temp_c = ambient_temp_centi_c / 100.0
            primary_value = methane_ppm_x100 / 100.0
            secondary_value = pressure_pa
            pressure_temp_c = pressure_temp_centi_c / 100.0
        return {
            "timestamp": time.time(),
            "device_id": device_id,
            "product": product_name(device_id),
            "map_version": live[REG_MAP_VERSION],
            "status_flags": status_flags,
            "methane_valid": bool(status_flags & 0x0001),
            "pressure_valid": bool(status_flags & 0x0002),
            "sample_ready": bool(status_flags & 0x0004),
            "humidity_valid": bool(status_flags & 0x0010),
            "humidity_error": bool(status_flags & 0x0020),
            "methane_ppm": primary_value,
            "pressure_pa": secondary_value,
            "humidity_rh": humidity_rh,
            "ambient_temp_c": ambient_temp_c,
            "pressure_temp_c": pressure_temp_c,
            "dac0_ua": live[REG_DAC0_CURRENT_UA],
            "dac1_ua": live[REG_DAC1_CURRENT_UA],
            "sequence": u32_from_words(live[REG_SAMPLE_SEQUENCE_HI], live[REG_SAMPLE_SEQUENCE_HI + 1]),
            "uptime_s": u32_from_words(
                live[REG_SAMPLE_UPTIME_SECONDS_HI],
                live[REG_SAMPLE_UPTIME_SECONDS_HI + 1],
            ),
            "methane_error": ph_error,
            "pressure_error": temperature_error,
            "humidity_error_code": humidity_error_code,
        }

    def _read_config_snapshot(self, client: ModbusRtuClient) -> dict[str, int]:
        regs = client.read_holding(REG_DEVICE_ID, 34)
        device_id = regs[REG_DEVICE_ID]
        map_version = regs[REG_MAP_VERSION]
        parity = 0
        stop_bits = 2
        data_bits = 8
        if is_ph_device(device_id) or map_version >= 6:
            parity, stop_bits, data_bits = client.read_holding(REG_RS485_PARITY, 3)
        elif map_version >= 5:
            parity, stop_bits = client.read_holding(REG_RS485_PARITY, 2)
        return {
            "device_id": device_id,
            "map_version": map_version,
            "unit_id": regs[REG_MODBUS_ADDRESS],
            "baudrate": u32_from_words(regs[REG_BAUDRATE_HI], regs[REG_BAUDRATE_LO]),
            "period_s": regs[REG_MEASUREMENT_PERIOD_SECONDS],
            "window_ms": regs[REG_MEASUREMENT_WINDOW_MS],
            "baud_preset": regs[REG_BAUD_PRESET],
            "termination": regs[REG_TERMINATION_ENABLED],
            "reboot_required": regs[REG_REBOOT_REQUIRED],
            "dac_min": regs[REG_DAC_MIN_CURRENT_UA],
            "dac_max": regs[REG_DAC_MAX_CURRENT_UA],
            "dac_fault": regs[REG_DAC_FAULT_CURRENT_UA],
            "parity": parity,
            "stop_bits": stop_bits,
            "data_bits": data_bits,
        }

    def _probe_worker(self, *, reschedule_monitor: bool = False) -> None:
        ARG_UNUSED = reschedule_monitor
        del ARG_UNUSED
        try:
            client = self._make_client()
            try:
                self.log("Probe started")
                identity = client.read_holding(REG_DEVICE_ID, 2)
                device_id = identity[REG_DEVICE_ID]
                map_version = identity[REG_MAP_VERSION]
                max_chunk = client.read_holding(DFU_REG_MAX_CHUNK_BYTES, 1)[0]
                status, error = client.read_holding(DFU_REG_STATUS, 2)
                self.log(
                    f"Device: {product_name(device_id)} "
                    f"device_id=0x{device_id:04X} map=v{map_version}"
                )
                self.log(f"DFU max chunk: {max_chunk} bytes")
                if is_ph_device(device_id) or map_version >= 7:
                    remaining_s = client.read_holding(
                        DFU_REG_UNLOCK_REMAINING_S, 1
                    )[0]
                    self.log(f"DFU unlock remaining: {remaining_s} seconds")
                self.log(
                    f"DFU status: {STATUS_NAMES.get(status, status)} error={signed16(error)}"
                )
                if is_ph_device(device_id) or map_version >= 6:
                    parity, stop_bits, data_bits = client.read_holding(
                        REG_RS485_PARITY, 3
                    )
                    self.log(
                        f"RS485 settings: data_bits={data_bits} parity={parity} stop_bits={stop_bits}"
                    )
                elif map_version >= 5:
                    parity, stop_bits = client.read_holding(REG_RS485_PARITY, 2)
                    self.log(f"RS485 settings: parity={parity} stop_bits={stop_bits}")
                self.messages.put(("done", "Probe complete"))
                self.messages.put(
                    (
                        "info",
                        "Probe complete. See the Log panel for register map, "
                        "DFU status, unlock window, and RS485 settings.",
                    )
                )
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _read_sensors_worker(self, *, reschedule_monitor: bool = False) -> None:
        try:
            client = self._make_client()
            try:
                sample = self._read_sample(client)
                self.messages.put(("sample", sample))
                self.messages.put(("done", "Sensor read complete"))
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.monitoring = False
            self.messages.put(("error", str(exc)))
        finally:
            if reschedule_monitor:
                self.messages.put(("monitor_reschedule", None))

    def _read_config_worker(self, *, reschedule_monitor: bool = False) -> None:
        ARG_UNUSED = reschedule_monitor
        del ARG_UNUSED
        try:
            client = self._make_client()
            try:
                config = self._read_config_snapshot(client)
                self.messages.put(("config", config))
                self.messages.put(("done", "Config read complete"))
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _apply_config_worker(self, *, reschedule_monitor: bool = False) -> None:
        ARG_UNUSED = reschedule_monitor
        del ARG_UNUSED
        try:
            values = self._collect_config_values()
            client = self._make_client()
            try:
                current = self._read_config_snapshot(client)
                client.write_single(REG_MODBUS_ADDRESS, values["unit_id"])
                client.write_single(REG_BAUD_PRESET, values["baud_preset"])
                client.write_single(REG_RS485_DATA_BITS, values["data_bits"])
                client.write_single(REG_RS485_PARITY, values["parity"])
                client.write_single(REG_RS485_STOP_BITS, values["stop_bits"])
                client.write_single(REG_TERMINATION_ENABLED, values["termination"])
                client.write_single(REG_MEASUREMENT_PERIOD_SECONDS, values["period_s"])
                client.write_single(REG_MEASUREMENT_WINDOW_MS, values["window_ms"])
                if values["dac_min"] > current["dac_max"]:
                    client.write_single(REG_DAC_MAX_CURRENT_UA, values["dac_max"])
                    client.write_single(REG_DAC_MIN_CURRENT_UA, values["dac_min"])
                elif values["dac_max"] < current["dac_min"]:
                    client.write_single(REG_DAC_MIN_CURRENT_UA, values["dac_min"])
                    client.write_single(REG_DAC_MAX_CURRENT_UA, values["dac_max"])
                else:
                    client.write_single(REG_DAC_MIN_CURRENT_UA, values["dac_min"])
                    client.write_single(REG_DAC_MAX_CURRENT_UA, values["dac_max"])
                client.write_single(REG_DAC_FAULT_CURRENT_UA, values["dac_fault"])
                config = self._read_config_snapshot(client)
                self.messages.put(("config", config))
                self.messages.put(("done", "Config saved; reboot required for transport changes"))
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _reboot_worker(self, *, reschedule_monitor: bool = False) -> None:
        ARG_UNUSED = reschedule_monitor
        del ARG_UNUSED
        try:
            client = self._make_client()
            try:
                client.write_single(REG_COMMAND, COMMAND_REBOOT)
                self.messages.put(("done", "Reboot command sent"))
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _confirm_image_worker(self, *, reschedule_monitor: bool = False) -> None:
        ARG_UNUSED = reschedule_monitor
        del ARG_UNUSED
        try:
            client = self._make_client()
            try:
                self.log("Confirm image command started")
                client.write_single(REG_COMMAND, COMMAND_CONFIRM_IMAGE)
                last_command, result = client.read_holding(REG_LAST_COMMAND, 2)
                result = signed16(result)
                if last_command != COMMAND_CONFIRM_IMAGE:
                    self.log(
                        "Warning: command echo changed: "
                        f"0x{last_command:04X}"
                    )
                if result < 0:
                    raise RuntimeError(f"confirm image failed: {result}")
                self.log("MCUboot image confirm command accepted by device")
                self.messages.put(("done", "MCUboot image confirmed"))
                self.messages.put(("info", "MCUboot image confirmed."))
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _upload_worker(self, *, reschedule_monitor: bool = False) -> None:
        ARG_UNUSED = reschedule_monitor
        del ARG_UNUSED
        try:
            image_path = Path(self.file_var.get())
            if not image_path.is_file():
                raise FileNotFoundError("Select a valid firmware .bin file.")
            unlock_key = parse_u16_text(self.unlock_key_var.get(), "DFU unlock key")

            client = self._make_client()
            try:
                uploader = Rs485DfuUploader(
                    client=client,
                    log=self.log,
                    progress=self.progress,
                    chunk_bytes=int(self.chunk_var.get()),
                    reboot_after_upload=self.reboot_var.get(),
                    unlock_key=unlock_key,
                )
                uploader.upload(image_path)
                self.messages.put(("done", "Upload complete"))
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _collect_config_values(self) -> dict[str, int]:
        def parse_range(key: str, minimum: int, maximum: int) -> int:
            value = int(self.config_vars[key].get())
            if value < minimum or value > maximum:
                raise ValueError(f"{key} must be {minimum}..{maximum}")
            return value

        baud_label = self.config_vars["baud_preset"].get()
        parity_label = self.config_vars["parity"].get()
        stop_label = self.config_vars["stop_bits"].get()
        data_label = self.config_vars["data_bits"].get()
        if baud_label not in BAUD_PRESET_OPTIONS:
            raise ValueError("Select a supported baud preset.")
        if parity_label not in PARITY_CODE_OPTIONS:
            raise ValueError("Select a supported parity setting.")
        if stop_label not in STOP_BITS_OPTIONS:
            raise ValueError("Select 1 or 2 stop bits.")
        if data_label not in DATA_BITS_OPTIONS:
            raise ValueError("Select 8 data bits.")

        values = {
            "unit_id": parse_range("unit_id", 1, 247),
            "baud_preset": BAUD_PRESET_OPTIONS[baud_label],
            "data_bits": DATA_BITS_OPTIONS[data_label],
            "parity": PARITY_CODE_OPTIONS[parity_label],
            "stop_bits": STOP_BITS_OPTIONS[stop_label],
            "termination": 1 if self.config_vars["termination"].get() else 0,
            "period_s": parse_range("period_s", 1, 65535),
            "window_ms": parse_range("window_ms", 1, 60000),
            "dac_min": parse_range("dac_min", 0, 25000),
            "dac_max": parse_range("dac_max", 0, 25000),
            "dac_fault": parse_range("dac_fault", 0, 25000),
        }
        if values["dac_min"] >= values["dac_max"]:
            raise ValueError("dac_min must be lower than dac_max")
        return values

    def _set_sensor_labels_mp(self) -> None:
        labels = {
            "methane": "Methane",
            "pressure": "Pressure",
            "humidity": "Humidity",
            "ambient_temp": "Ambient temp",
            "pressure_temp": "Pressure temp",
            "dac0": "DAC0 methane",
            "dac1": "DAC1 pressure",
            "sequence": "Sample seq",
            "uptime": "Uptime",
            "status": "Status",
        }
        for key, label in labels.items():
            self.sensor_label_vars[key].set(label)
        for key in ("humidity", "ambient_temp", "pressure_temp"):
            self._set_sensor_row_visible(key, True)

    def _set_sensor_labels_ph(self) -> None:
        labels = {
            "methane": "pH",
            "pressure": "PT1000 temp",
            "humidity": "Humidity",
            "ambient_temp": "Ambient temp",
            "pressure_temp": "Sensor temp",
            "dac0": "DAC0 pH",
            "dac1": "DAC1 temp",
            "sequence": "Sample seq",
            "uptime": "Uptime",
            "status": "Status",
        }
        for key, label in labels.items():
            self.sensor_label_vars[key].set(label)
        for key in ("humidity", "ambient_temp", "pressure_temp"):
            self._set_sensor_row_visible(key, False)

    def _set_sensor_row_visible(self, key: str, visible: bool) -> None:
        widgets = self.sensor_value_rows.get(key)
        if widgets is None:
            return
        for widget in widgets:
            if visible:
                widget.grid()
            else:
                widget.grid_remove()

    def _set_sensor_labels_for_product(self, product: str) -> None:
        if product == "pH":
            self._set_sensor_labels_ph()
        else:
            self._set_sensor_labels_mp()

    def _update_sample_ui(self, sample: dict[str, float | int | bool | str | None]) -> None:
        device_id = int(sample.get("device_id") or 0)
        self.current_product = product_name(device_id)
        if is_ph_device(device_id):
            self._set_sensor_labels_ph()
            self.sensor_vars["methane"].set(
                format_scaled(sample.get("methane_ppm"), 1.0, "pH", 3)
            )
            self.sensor_vars["pressure"].set(
                format_scaled(sample.get("pressure_pa"), 1.0, "degC", 2)
            )
            self.sensor_vars["humidity"].set("-")
            self.sensor_vars["ambient_temp"].set("-")
            self.sensor_vars["pressure_temp"].set("-")
        else:
            self._set_sensor_labels_mp()
            self.sensor_vars["methane"].set(
                format_scaled(sample.get("methane_ppm"), 1.0, "ppm", 2)
            )
            self.sensor_vars["pressure"].set(
                format_scaled(sample.get("pressure_pa"), 1.0, "Pa", 0)
            )
            self.sensor_vars["humidity"].set(
                format_scaled(sample.get("humidity_rh"), 1.0, "%RH", 2)
            )
            self.sensor_vars["ambient_temp"].set(
                format_scaled(sample.get("ambient_temp_c"), 1.0, "degC", 2)
            )
            self.sensor_vars["pressure_temp"].set(
                format_scaled(sample.get("pressure_temp_c"), 1.0, "degC", 2)
            )
        self.sensor_vars["dac0"].set(format_scaled(sample.get("dac0_ua"), 1.0, "uA", 0))
        self.sensor_vars["dac1"].set(format_scaled(sample.get("dac1_ua"), 1.0, "uA", 0))
        self.sensor_vars["sequence"].set(str(sample.get("sequence", "-")))
        self.sensor_vars["uptime"].set(format_scaled(sample.get("uptime_s"), 1.0, "s", 0))
        status = []
        status_fields = (
            (("pH", "methane_valid"), ("temperature", "pressure_valid"))
            if is_ph_device(device_id)
            else (("methane", "methane_valid"), ("pressure", "pressure_valid"), ("humidity", "humidity_valid"))
        )
        for label, key in status_fields:
            status.append(f"{label}={'ok' if sample.get(key) else 'bad'}")
        self.sensor_vars["status"].set(", ".join(status))

        self.sample_history.append(sample)
        if len(self.sample_history) > MAX_HISTORY_SAMPLES:
            self.sample_history = self.sample_history[-MAX_HISTORY_SAMPLES:]
        self._redraw_graph()

    def _update_config_ui(self, config: dict[str, int]) -> None:
        self.current_product = product_name(config.get("device_id", 0))
        self._set_sensor_labels_for_product(self.current_product)
        self.config_vars["map_version"].set(str(config["map_version"]))
        self.config_vars["unit_id"].set(str(config["unit_id"]))
        self.config_vars["period_s"].set(str(config["period_s"]))
        self.config_vars["window_ms"].set(str(config["window_ms"]))
        self.config_vars["dac_min"].set(str(config["dac_min"]))
        self.config_vars["dac_max"].set(str(config["dac_max"]))
        self.config_vars["dac_fault"].set(str(config["dac_fault"]))
        self.config_vars["termination"].set(config["termination"] != 0)
        self.config_vars["reboot_required"].set("yes" if config["reboot_required"] else "no")

        baud_label = BAUD_PRESET_BY_VALUE.get(config["baud_preset"])
        if baud_label is None:
            baud_label = str(config["baudrate"])
            if baud_label not in BAUD_PRESET_OPTIONS:
                baud_label = "115200"
                self.log(
                    f"Device reports custom baud {config['baudrate']}; GUI apply uses presets only."
                )
        self.config_vars["baud_preset"].set(baud_label)
        self.config_vars["data_bits"].set(str(config["data_bits"]))
        self.config_vars["stop_bits"].set(str(config["stop_bits"]))
        self.config_vars["parity"].set(
            PARITY_LABEL_BY_CODE.get(config["parity"], "None (0)")
        )

    def _update_connection_ui(self, connection: dict[str, int | str]) -> None:
        port = str(connection["port"])
        current_ports = list(self.port_combo["values"])
        if port not in current_ports:
            self.port_combo["values"] = [port, *current_ports]
        self.port_var.set(port)
        self.baud_var.set(str(connection["baudrate"]))
        self.unit_var.set(str(connection["unit_id"]))
        self.data_bits_var.set(str(connection["data_bits_label"]))
        self.parity_var.set(str(connection["parity_label"]))
        self.stop_bits_var.set(str(connection["stop_bits_label"]))
        self.current_product = str(connection.get("product", "methane-pressure"))
        self._set_sensor_labels_for_product(self.current_product)

    def _format_connection_summary(self, connection: dict[str, int | str]) -> str:
        parity = PARITY_OPTIONS[str(connection["parity_label"])]
        framing = (
            f"{connection['data_bits_label']}"
            f"{parity}"
            f"{connection['stop_bits_label']}"
        )
        return (
            f"{connection.get('product', 'ArgiSense')} {connection['port']} "
            f"unit={connection['unit_id']} "
            f"{connection['baudrate']} {framing} "
            f"map=v{connection['map_version']}"
        )

    def _clear_detected_devices_ui(self) -> None:
        self.detected_devices.clear()
        for item in self.detected_tree.get_children():
            self.detected_tree.delete(item)

    def _add_detected_device_ui(self, connection: dict[str, int | str]) -> None:
        index = len(self.detected_devices)
        self.detected_devices.append(dict(connection))
        parity = PARITY_OPTIONS[str(connection["parity_label"])]
        framing = (
            f"{connection['data_bits_label']}"
            f"{parity}"
            f"{connection['stop_bits_label']}"
        )
        iid = str(index)
        self.detected_tree.insert(
            "",
            tk.END,
            iid=iid,
            values=(
                str(connection["port"]),
                str(connection.get("product", "-")),
                str(connection["unit_id"]),
                str(connection["baudrate"]),
                framing,
                f"v{connection['map_version']}",
            ),
        )
        if index == 0:
            self.detected_tree.selection_set(iid)
            self.detected_tree.focus(iid)

    def _graph_window_samples(self) -> int:
        try:
            window = int(self.graph_window_var.get())
        except ValueError:
            return DEFAULT_GRAPH_WINDOW_SAMPLES
        return max(2, min(window, MAX_HISTORY_SAMPLES))

    def _visible_graph_history(self) -> list[dict[str, float | int | bool | str | None]]:
        window = self._graph_window_samples()
        return self.sample_history[-window:]

    def _redraw_graph(self) -> None:
        canvas = self.graph_canvas
        canvas.delete("all")
        visible_history = self._visible_graph_history()
        width = max(canvas.winfo_width(), 200)
        height = max(canvas.winfo_height(), 160)
        left = 52
        right = width - 16
        top = 30
        bottom = height - 32
        canvas.create_rectangle(left, top, right, bottom, outline="#cbd5e1")
        for grid_index in range(1, 4):
            y = top + ((bottom - top) * grid_index / 4)
            canvas.create_line(left, y, right, y, fill="#e2e8f0")
        for grid_index in range(1, 5):
            x = left + ((right - left) * grid_index / 5)
            canvas.create_line(x, top, x, bottom, fill="#f1f5f9")
        latest_device_id = int(visible_history[-1].get("device_id") or 0) if visible_history else 0
        if is_ph_device(latest_device_id):
            graph_title = "Auto-scaled trend: pH, PT1000 temperature degC"
            series = [
                ("methane_ppm", "pH", "#15803d"),
                ("pressure_pa", "PT1000 temperature degC", "#2563eb"),
            ]
        else:
            graph_title = "Auto-scaled trend: methane ppm, pressure Pa, humidity %RH"
            series = [
                ("methane_ppm", "Methane ppm", "#15803d"),
                ("pressure_pa", "Pressure Pa", "#2563eb"),
                ("humidity_rh", "Humidity %RH", "#d97706"),
            ]
        canvas.create_text(
            left,
            10,
            anchor=tk.W,
            text=graph_title,
            fill="#334155",
        )
        canvas.create_text(
            right,
            10,
            anchor=tk.E,
            text=f"last {len(visible_history)}/{len(self.sample_history)} samples",
            fill="#64748b",
        )

        if len(visible_history) < 2:
            canvas.create_text(
                (left + right) // 2,
                (top + bottom) // 2,
                text="Read sensor values to start graph",
                fill="#64748b",
            )
            return

        legend_x = max(left + 120, right - 178)
        canvas.create_rectangle(
            legend_x - 8,
            top + 8,
            right - 4,
            top + 74,
            fill="white",
            outline="#e2e8f0",
        )
        x_span = max(len(visible_history) - 1, 1)
        for s_index, (key, label, color) in enumerate(series):
            values = [
                float(sample[key])
                for sample in visible_history
                if sample.get(key) is not None
            ]
            if len(values) < 2:
                continue
            min_v = min(values)
            max_v = max(values)
            if min_v == max_v:
                min_v -= 1.0
                max_v += 1.0
            points: list[float] = []
            for index, sample in enumerate(visible_history):
                value = sample.get(key)
                if value is None:
                    continue
                x = left + ((right - left) * index / x_span)
                y = bottom - ((float(value) - min_v) * (bottom - top) / (max_v - min_v))
                points.extend([x, y])
            if len(points) >= 4:
                canvas.create_line(*points, fill=color, width=2, smooth=True)
            legend_y = top + 18 + s_index * 18
            canvas.create_line(legend_x, legend_y, legend_x + 25, legend_y, fill=color, width=3)
            canvas.create_text(
                legend_x + 30,
                legend_y,
                anchor=tk.W,
                text=f"{label} ({min_v:.1f}..{max_v:.1f})",
                fill="#0f172a",
            )

    def _poll_messages(self) -> None:
        try:
            while True:
                kind, payload = self.messages.get_nowait()
                if kind == "log":
                    self._append_log(str(payload))
                elif kind == "connection":
                    self._update_connection_ui(payload)  # type: ignore[arg-type]
                elif kind == "sample":
                    self._update_sample_ui(payload)  # type: ignore[arg-type]
                elif kind == "config":
                    self._update_config_ui(payload)  # type: ignore[arg-type]
                elif kind == "detected_device":
                    self._add_detected_device_ui(payload)  # type: ignore[arg-type]
                elif kind == "progress":
                    done, total = payload  # type: ignore[misc]
                    percent = 0 if total == 0 else int((done * 100) / total)
                    self.progress_bar["value"] = percent
                    self.status_var.set(f"Uploading {percent}%")
                elif kind == "scan_progress":
                    done, total = payload  # type: ignore[misc]
                    percent = 0 if total == 0 else int((done * 100) / total)
                    self.progress_bar["value"] = min(percent, 100)
                    self.status_var.set(f"Scanning {min(percent, 100)}%")
                elif kind == "monitor_reschedule":
                    self.set_busy(False)
                    if self.monitoring:
                        delay_ms = max(100, int(float(self.poll_interval_var.get()) * 1000))
                        self.after(delay_ms, self._monitor_tick)
                elif kind == "done":
                    self.status_var.set(str(payload))
                    self.set_busy(False)
                    self.stop_scan_button.configure(state=tk.DISABLED)
                    self._append_log(str(payload))
                elif kind == "info":
                    messagebox.showinfo("ArgiSense RS485", str(payload))
                elif kind == "error":
                    self.status_var.set("Error")
                    self.set_busy(False)
                    self.stop_scan_button.configure(state=tk.DISABLED)
                    self._append_log(f"ERROR: {payload}")
                    messagebox.showerror("ArgiSense RS485", str(payload))
        except queue.Empty:
            pass
        self.after(100, self._poll_messages)

    def _append_log(self, message: str) -> None:
        timestamp = time.strftime("%H:%M:%S")
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, f"[{timestamp}] {message}\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)


def main() -> int:
    app = App()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
