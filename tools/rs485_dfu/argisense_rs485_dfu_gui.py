#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#

"""ArgiSense RS485 firmware update GUI.

This tool uploads an MCUboot signed binary to the ArgiSense secondary image
slot through the custom Modbus holding-register DFU window.
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
DFU_REG_SHA256_BASE = 1020
DFU_REG_SHA256_COUNT = 16
DFU_REG_DATA_BASE = 1100

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


def words_from_bytes(data: bytes) -> list[int]:
    if len(data) % 2:
        data += b"\x00"
    return [int.from_bytes(data[i : i + 2], "big") for i in range(0, len(data), 2)]


class ModbusRtuClient:
    def __init__(self, port: str, baudrate: int, unit_id: int, timeout_s: float) -> None:
        if serial is None:
            raise RuntimeError(
                "pyserial is not installed. Run: py -3.12 -m pip install -r tools\\rs485_dfu\\requirements.txt"
            )
        self.unit_id = unit_id
        self.ser = serial.Serial(
            port=port,
            baudrate=baudrate,
            bytesize=8,
            parity="N",
            stopbits=1,
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
    ) -> None:
        self.client = client
        self.log = log
        self.progress = progress
        self.chunk_bytes = chunk_bytes
        self.reboot_after_upload = reboot_after_upload

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


class App(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("ArgiSense RS485 Firmware Update")
        self.geometry("820x620")
        self.minsize(720, 520)

        self.messages: queue.Queue[tuple[str, object]] = queue.Queue()
        self.worker: threading.Thread | None = None

        self.port_var = tk.StringVar()
        self.baud_var = tk.StringVar(value="115200")
        self.unit_var = tk.StringVar(value="1")
        self.timeout_var = tk.StringVar(value="1.0")
        self.chunk_var = tk.StringVar(value=str(DEFAULT_CHUNK_BYTES))
        self.file_var = tk.StringVar()
        self.reboot_var = tk.BooleanVar(value=True)
        self.status_var = tk.StringVar(value="Idle")

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

        settings.columnconfigure(1, weight=1)

        firmware = ttk.LabelFrame(root, text="Firmware image")
        firmware.pack(fill=tk.X, pady=(10, 0))

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

        firmware.columnconfigure(0, weight=1)

        actions = ttk.Frame(root)
        actions.pack(fill=tk.X, pady=(10, 0))
        self.probe_button = ttk.Button(actions, text="Probe", command=self.probe)
        self.probe_button.pack(side=tk.LEFT)
        self.upload_button = ttk.Button(actions, text="Upload Firmware", command=self.upload)
        self.upload_button.pack(side=tk.LEFT, padx=(8, 0))
        ttk.Label(actions, textvariable=self.status_var).pack(side=tk.LEFT, padx=(16, 0))

        self.progress_bar = ttk.Progressbar(root, mode="determinate", maximum=100)
        self.progress_bar.pack(fill=tk.X, pady=(10, 0))

        self.log_text = scrolledtext.ScrolledText(root, height=22, wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=True, pady=(10, 0))
        self.log_text.configure(state=tk.DISABLED)

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
        self.probe_button.configure(state=state)
        self.upload_button.configure(state=state)

    def probe(self) -> None:
        self._start_worker(self._probe_worker)

    def upload(self) -> None:
        self._start_worker(self._upload_worker)

    def _start_worker(self, target) -> None:
        if self.worker and self.worker.is_alive():
            messagebox.showinfo("ArgiSense RS485 DFU", "An operation is already running.")
            return
        try:
            self._validate_common()
        except Exception as exc:  # noqa: BLE001 - presented to operator.
            messagebox.showerror("ArgiSense RS485 DFU", str(exc))
            return
        self.set_busy(True)
        self.status_var.set("Working")
        self.worker = threading.Thread(target=target, daemon=True)
        self.worker.start()

    def _validate_common(self) -> None:
        if not self.port_var.get():
            raise ValueError("Select a serial port.")
        baud = int(self.baud_var.get())
        unit_id = int(self.unit_var.get())
        timeout = float(self.timeout_var.get())
        chunk = int(self.chunk_var.get())
        if not 1 <= unit_id <= 247:
            raise ValueError("Unit ID must be 1..247.")
        if baud <= 0:
            raise ValueError("Baudrate must be positive.")
        if timeout <= 0:
            raise ValueError("Timeout must be positive.")
        if chunk < 16 or chunk > 128 or chunk % 2:
            raise ValueError("Chunk must be an even value from 16 to 128 bytes.")

    def _make_client(self) -> ModbusRtuClient:
        return ModbusRtuClient(
            port=self.port_var.get(),
            baudrate=int(self.baud_var.get()),
            unit_id=int(self.unit_var.get()),
            timeout_s=float(self.timeout_var.get()),
        )

    def _probe_worker(self) -> None:
        try:
            client = self._make_client()
            try:
                map_version = client.read_holding(1, 1)[0]
                max_chunk = client.read_holding(DFU_REG_MAX_CHUNK_BYTES, 1)[0]
                status, error = client.read_holding(DFU_REG_STATUS, 2)
                self.log(f"Register map version: {map_version}")
                self.log(f"DFU max chunk: {max_chunk} bytes")
                self.log(
                    f"DFU status: {STATUS_NAMES.get(status, status)} error={signed16(error)}"
                )
                self.messages.put(("done", "Probe complete"))
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _upload_worker(self) -> None:
        try:
            image_path = Path(self.file_var.get())
            if not image_path.is_file():
                raise FileNotFoundError("Select a valid firmware .bin file.")

            client = self._make_client()
            try:
                uploader = Rs485DfuUploader(
                    client=client,
                    log=self.log,
                    progress=self.progress,
                    chunk_bytes=int(self.chunk_var.get()),
                    reboot_after_upload=self.reboot_var.get(),
                )
                uploader.upload(image_path)
                self.messages.put(("done", "Upload complete"))
            finally:
                client.close()
        except Exception as exc:  # noqa: BLE001 - displayed in GUI.
            self.messages.put(("error", str(exc)))

    def _poll_messages(self) -> None:
        try:
            while True:
                kind, payload = self.messages.get_nowait()
                if kind == "log":
                    self._append_log(str(payload))
                elif kind == "progress":
                    done, total = payload  # type: ignore[misc]
                    percent = 0 if total == 0 else int((done * 100) / total)
                    self.progress_bar["value"] = percent
                    self.status_var.set(f"Uploading {percent}%")
                elif kind == "done":
                    self.status_var.set(str(payload))
                    self.set_busy(False)
                    self._append_log(str(payload))
                elif kind == "error":
                    self.status_var.set("Error")
                    self.set_busy(False)
                    self._append_log(f"ERROR: {payload}")
                    messagebox.showerror("ArgiSense RS485 DFU", str(payload))
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
