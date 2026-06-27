#!/usr/bin/env python3
#
# SPDX-License-Identifier: Apache-2.0
#

"""ArgiSense USB-C MCUmgr firmware update GUI."""

from __future__ import annotations

import json
import queue
import re
import struct
import subprocess
import threading
import time
from dataclasses import dataclass
from pathlib import Path
import tkinter as tk
from tkinter import filedialog, messagebox, scrolledtext, ttk

try:
    from serial.tools import list_ports
except ImportError:  # pragma: no cover - pyserial is optional.
    list_ports = None


DEFAULT_BAUD = 115200
DEFAULT_MTU = 128
DEFAULT_COMMAND_TIMEOUT_S = 60
DEFAULT_UPLOAD_TIMEOUT_S = 900
DEFAULT_RESET_WAIT_S = 8


@dataclass
class ImageInfo:
    image: int
    slot: int
    version: str = ""
    flags: str = ""
    hash: str = ""


def repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def workspace_root() -> Path:
    return repo_root().parent


def default_image_path() -> Path:
    return (
        workspace_root()
        / "build"
        / "argisense-zephyr-app"
        / "zephyr"
        / "zephyr.signed.bin"
    )


def parse_mcuboot_header(path: Path) -> str:
    data = path.read_bytes()[:32]
    if len(data) < 32:
        return "file too small"

    magic, _load_addr, hdr_size, _prot_tlv_size, img_size, flags = struct.unpack_from(
        "<IIHHII", data, 0
    )
    major, minor, revision, build_num = struct.unpack_from("<BBHI", data, 20)
    if magic != 0x96F3B83D:
        return f"not an MCUboot image, magic=0x{magic:08X}"

    file_size = path.stat().st_size
    return (
        f"version {major}.{minor}.{revision}+{build_num}, "
        f"image {img_size} bytes, header {hdr_size} bytes, "
        f"file {file_size} bytes, flags=0x{flags:08X}"
    )


def enumerate_serial_ports() -> list[tuple[str, str]]:
    if list_ports is not None:
        ports = []
        for port in list_ports.comports():
            detail = f"{port.description}"
            if port.hwid:
                detail = f"{detail} [{port.hwid}]"
            ports.append((port.device, detail))
        return ports

    command = [
        "powershell",
        "-NoProfile",
        "-Command",
        "Get-CimInstance Win32_SerialPort | "
        "Select-Object DeviceID,Description,PNPDeviceID | "
        "ConvertTo-Json -Compress",
    ]
    try:
        result = subprocess.run(
            command,
            check=True,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=10,
        )
        if not result.stdout.strip():
            return []
        payload = json.loads(result.stdout)
        if isinstance(payload, dict):
            payload = [payload]
        return [
            (
                item.get("DeviceID", ""),
                f"{item.get('Description', '')} [{item.get('PNPDeviceID', '')}]",
            )
            for item in payload
            if item.get("DeviceID")
        ]
    except Exception:
        return []


def parse_image_list(output: str) -> list[ImageInfo]:
    images: list[ImageInfo] = []
    current: ImageInfo | None = None

    for raw_line in output.splitlines():
        line = raw_line.strip()
        match = re.match(r"image=(\d+)\s+slot=(\d+)", line)
        if match:
            if current is not None:
                images.append(current)
            current = ImageInfo(image=int(match.group(1)), slot=int(match.group(2)))
            continue

        if current is None:
            continue

        if line.startswith("version:"):
            current.version = line.split(":", 1)[1].strip()
        elif line.startswith("flags:"):
            current.flags = line.split(":", 1)[1].strip()
        elif line.startswith("hash:"):
            current.hash = line.split(":", 1)[1].strip()

    if current is not None:
        images.append(current)

    return images


def parse_progress_percent(text: str) -> float | None:
    matches = re.findall(r"(\d+(?:\.\d+)?)\s*%", text)
    if not matches:
        return None
    return max(0.0, min(100.0, float(matches[-1])))


class McumgrError(RuntimeError):
    pass


class UsbMcumgrClient:
    def __init__(
        self,
        mcumgr_path: str,
        port: str,
        baud: int,
        mtu: int,
        logger,
        progress_callback=None,
    ) -> None:
        self.mcumgr_path = mcumgr_path
        self.port = port
        self.baud = baud
        self.mtu = mtu
        self.logger = logger
        self.progress_callback = progress_callback

    @property
    def connstring(self) -> str:
        return f"dev={self.port},baud={self.baud},mtu={self.mtu}"

    def command(self, args: list[str]) -> list[str]:
        return [
            self.mcumgr_path,
            "--conntype",
            "serial",
            "--connstring",
            self.connstring,
            *args,
        ]

    def run(self, args: list[str], timeout_s: int) -> str:
        command = self.command(args)
        self.logger("> " + " ".join(command))
        try:
            result = subprocess.run(
                command,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=timeout_s,
            )
        except FileNotFoundError as exc:
            raise McumgrError(
                "mcumgr was not found. Install mcumgr and add it to PATH, "
                "or set the full mcumgr.exe path in the GUI."
            ) from exc
        except subprocess.TimeoutExpired as exc:
            raise McumgrError(f"mcumgr timed out after {timeout_s} seconds") from exc

        output = (result.stdout or "") + (result.stderr or "")
        for line in output.splitlines():
            if line.strip():
                self.logger(line)

        if result.returncode != 0:
            if "Access is denied" in output:
                raise McumgrError(
                    f"{self.port} is busy or unavailable. Close any serial "
                    "terminal, shell, or other MCUmgr session using this COM "
                    "port, then try again."
                )
            raise McumgrError(f"mcumgr failed with exit code {result.returncode}")
        return output

    def run_streaming_upload(
        self,
        image_path: Path,
        timeout_s: int,
        erase_before_upload: bool,
    ) -> str:
        args = ["image", "upload", str(image_path)]
        if erase_before_upload:
            args.append("--noerase=false")

        command = self.command(args)
        self.logger("> " + " ".join(command))

        try:
            process = subprocess.Popen(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                encoding="utf-8",
                errors="replace",
            )
        except FileNotFoundError as exc:
            raise McumgrError(
                "mcumgr was not found. Install mcumgr and add it to PATH, "
                "or set the full mcumgr.exe path in the GUI."
            ) from exc

        output_parts: list[str] = []
        start_time = time.monotonic()

        assert process.stdout is not None
        for raw_line in process.stdout:
            output_parts.append(raw_line)
            line = raw_line.strip()
            if line:
                self.logger(line)

            percent = parse_progress_percent(raw_line)
            if percent is not None and self.progress_callback is not None:
                self.progress_callback(percent)

            if time.monotonic() - start_time > timeout_s:
                process.kill()
                raise McumgrError(f"mcumgr upload timed out after {timeout_s} seconds")

        returncode = process.wait()
        output = "".join(output_parts)

        if returncode != 0:
            if "Access is denied" in output:
                raise McumgrError(
                    f"{self.port} is busy or unavailable. Close any serial "
                    "terminal, shell, or other MCUmgr session using this COM "
                    "port, then try again."
                )
            raise McumgrError(f"mcumgr upload failed with exit code {returncode}")

        if self.progress_callback is not None:
            self.progress_callback(100.0)
        return output

    def echo(self) -> str:
        return self.run(["echo", "hello"], DEFAULT_COMMAND_TIMEOUT_S)

    def image_list(self) -> tuple[str, list[ImageInfo]]:
        output = self.run(["image", "list"], DEFAULT_COMMAND_TIMEOUT_S)
        return output, parse_image_list(output)

    def upload(
        self,
        image_path: Path,
        timeout_s: int,
        erase_before_upload: bool,
    ) -> None:
        self.run_streaming_upload(image_path, timeout_s, erase_before_upload)

    def test(self, image_hash: str) -> None:
        self.run(["image", "test", image_hash], DEFAULT_COMMAND_TIMEOUT_S)

    def confirm(self, image_hash: str) -> None:
        self.run(["image", "confirm", image_hash], DEFAULT_COMMAND_TIMEOUT_S)

    def reset(self) -> None:
        self.run(["reset"], DEFAULT_COMMAND_TIMEOUT_S)


class UsbMcumgrGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("ArgiSense USB-C Firmware Update")
        self.root.geometry("900x700")

        self.ui_queue: queue.Queue[tuple[str, object]] = queue.Queue()
        self.worker: threading.Thread | None = None

        self.port_var = tk.StringVar()
        self.port_detail_var = tk.StringVar(value="No port selected")
        self.mcumgr_var = tk.StringVar(value="mcumgr")
        self.baud_var = tk.StringVar(value=str(DEFAULT_BAUD))
        self.mtu_var = tk.StringVar(value=str(DEFAULT_MTU))
        self.timeout_var = tk.StringVar(value=str(DEFAULT_UPLOAD_TIMEOUT_S))
        self.reset_wait_var = tk.StringVar(value=str(DEFAULT_RESET_WAIT_S))
        self.confirm_var = tk.BooleanVar(value=True)
        self.erase_var = tk.BooleanVar(value=True)
        self.file_var = tk.StringVar(value=str(default_image_path()))
        self.status_var = tk.StringVar(value="Ready")
        self.progress_var = tk.DoubleVar(value=0.0)
        self.progress_text_var = tk.StringVar(value="")

        self.port_details: dict[str, str] = {}
        self.buttons: list[ttk.Button] = []

        self._build_ui()
        self.refresh_ports()
        self.update_image_info()
        self.root.after(100, self._drain_queue)

    def _build_ui(self) -> None:
        main = ttk.Frame(self.root, padding=12)
        main.pack(fill=tk.BOTH, expand=True)

        connection = ttk.LabelFrame(main, text="USB-C MCUmgr connection")
        connection.pack(fill=tk.X)
        connection.columnconfigure(1, weight=1)
        connection.columnconfigure(4, weight=1)

        ttk.Label(connection, text="COM port").grid(row=0, column=0, sticky=tk.W, padx=(8, 4), pady=8)
        self.port_combo = ttk.Combobox(connection, textvariable=self.port_var, width=18)
        self.port_combo.grid(row=0, column=1, sticky=tk.EW, padx=4, pady=8)
        self.port_combo.bind("<<ComboboxSelected>>", lambda _event: self._update_port_detail())
        self._add_button(connection, "Refresh", self.refresh_ports).grid(row=0, column=2, padx=4, pady=8)

        ttk.Label(connection, text="mcumgr").grid(row=0, column=3, sticky=tk.W, padx=(16, 4), pady=8)
        ttk.Entry(connection, textvariable=self.mcumgr_var).grid(row=0, column=4, sticky=tk.EW, padx=4, pady=8)

        ttk.Label(connection, text="Baud").grid(row=1, column=0, sticky=tk.W, padx=(8, 4), pady=(0, 8))
        ttk.Entry(connection, textvariable=self.baud_var, width=12).grid(row=1, column=1, sticky=tk.W, padx=4, pady=(0, 8))
        ttk.Label(connection, text="MTU").grid(row=1, column=2, sticky=tk.E, padx=4, pady=(0, 8))
        ttk.Entry(connection, textvariable=self.mtu_var, width=10).grid(row=1, column=3, sticky=tk.W, padx=4, pady=(0, 8))

        detail = ttk.Label(connection, textvariable=self.port_detail_var, foreground="#555555")
        detail.grid(row=2, column=0, columnspan=5, sticky=tk.EW, padx=8, pady=(0, 8))

        firmware = ttk.LabelFrame(main, text="Firmware image")
        firmware.pack(fill=tk.X, pady=(10, 0))
        firmware.columnconfigure(0, weight=1)

        ttk.Entry(firmware, textvariable=self.file_var).grid(row=0, column=0, sticky=tk.EW, padx=8, pady=8)
        self._add_button(firmware, "Browse", self.browse_file).grid(row=0, column=1, padx=(0, 8), pady=8)
        self.image_info_var = tk.StringVar(value="")
        ttk.Label(firmware, textvariable=self.image_info_var, foreground="#555555").grid(
            row=1, column=0, columnspan=2, sticky=tk.EW, padx=8, pady=(0, 8)
        )

        options = ttk.LabelFrame(main, text="Update options")
        options.pack(fill=tk.X, pady=(10, 0))
        ttk.Label(options, text="Upload timeout (s)").grid(row=0, column=0, sticky=tk.W, padx=(8, 4), pady=8)
        ttk.Entry(options, textvariable=self.timeout_var, width=10).grid(row=0, column=1, sticky=tk.W, padx=4, pady=8)
        ttk.Label(options, text="Reset wait (s)").grid(row=0, column=2, sticky=tk.W, padx=(16, 4), pady=8)
        ttk.Entry(options, textvariable=self.reset_wait_var, width=10).grid(row=0, column=3, sticky=tk.W, padx=4, pady=8)
        ttk.Checkbutton(
            options,
            text="Confirm active image after reset",
            variable=self.confirm_var,
        ).grid(row=0, column=4, sticky=tk.W, padx=(16, 8), pady=8)
        ttk.Checkbutton(
            options,
            text="Erase secondary slot before upload",
            variable=self.erase_var,
        ).grid(row=1, column=0, columnspan=5, sticky=tk.W, padx=8, pady=(0, 8))

        actions = ttk.Frame(main)
        actions.pack(fill=tk.X, pady=(10, 0))
        self._add_button(actions, "Probe", self.probe).pack(side=tk.LEFT)
        self._add_button(actions, "Image List", self.image_list).pack(side=tk.LEFT, padx=(8, 0))
        self._add_button(actions, "Upload Only", self.upload_only).pack(side=tk.LEFT, padx=(8, 0))
        self._add_button(actions, "Upload + Test + Reset", self.upload_test_reset).pack(side=tk.LEFT, padx=(8, 0))
        self._add_button(actions, "Confirm Active", self.confirm_active).pack(side=tk.LEFT, padx=(8, 0))

        status = ttk.Frame(main)
        status.pack(fill=tk.X, pady=(10, 0))
        self.progress = ttk.Progressbar(
            status,
            mode="indeterminate",
            maximum=100.0,
            variable=self.progress_var,
        )
        self.progress.pack(side=tk.LEFT, fill=tk.X, expand=True)
        ttk.Label(status, textvariable=self.progress_text_var, width=8).pack(side=tk.LEFT, padx=(8, 0))
        ttk.Label(status, textvariable=self.status_var, width=32).pack(side=tk.LEFT, padx=(8, 0))

        log_frame = ttk.LabelFrame(main, text="Log")
        log_frame.pack(fill=tk.BOTH, expand=True, pady=(10, 0))
        self.log_text = scrolledtext.ScrolledText(log_frame, height=18, wrap=tk.WORD)
        self.log_text.pack(fill=tk.BOTH, expand=True, padx=8, pady=8)
        self.log_text.configure(state=tk.DISABLED)

    def _add_button(self, parent, text: str, command) -> ttk.Button:
        button = ttk.Button(parent, text=text, command=command)
        self.buttons.append(button)
        return button

    def refresh_ports(self) -> None:
        ports = enumerate_serial_ports()
        self.port_details = {port: detail for port, detail in ports}
        self.port_combo["values"] = [port for port, _detail in ports]

        current = self.port_var.get()
        if current not in self.port_details:
            preferred = self._preferred_port(ports)
            self.port_var.set(preferred)

        self._update_port_detail()

    def _preferred_port(self, ports: list[tuple[str, str]]) -> str:
        for port, detail in ports:
            detail_upper = detail.upper()
            if (
                "2FE3" in detail_upper
                and ("MI_02" in detail_upper or "X.2" in detail_upper)
            ):
                return port
        for port, detail in ports:
            if "2FE3" in detail.upper():
                return port
        for port, _detail in ports:
            if port.upper() == "COM24":
                return port
        return ports[0][0] if ports else ""

    def _update_port_detail(self) -> None:
        port = self.port_var.get()
        if port:
            self.port_detail_var.set(self.port_details.get(port, "Manual COM port"))
        else:
            self.port_detail_var.set("No COM port found. Type the MCUmgr port manually.")

    def browse_file(self) -> None:
        filename = filedialog.askopenfilename(
            title="Select MCUboot signed firmware image",
            filetypes=[("MCUboot signed binary", "*.bin"), ("All files", "*.*")],
            initialdir=str(default_image_path().parent),
        )
        if filename:
            self.file_var.set(filename)
            self.update_image_info()

    def update_image_info(self) -> None:
        path = Path(self.file_var.get())
        if not path.exists():
            self.image_info_var.set("Image file not found.")
            return
        try:
            self.image_info_var.set(parse_mcuboot_header(path))
        except Exception as exc:
            self.image_info_var.set(f"Image info unavailable: {exc}")

    def probe(self) -> None:
        self._start_worker(self._probe_worker)

    def image_list(self) -> None:
        self._start_worker(self._image_list_worker)

    def upload_only(self) -> None:
        self._start_worker(lambda: self._upload_worker(test_after_upload=False, reset_after_test=False))

    def upload_test_reset(self) -> None:
        self._start_worker(lambda: self._upload_worker(test_after_upload=True, reset_after_test=True))

    def confirm_active(self) -> None:
        self._start_worker(self._confirm_active_worker)

    def _client(self) -> UsbMcumgrClient:
        port = self.port_var.get().strip()
        if not port:
            raise ValueError("Select or enter the MCUmgr COM port.")

        return UsbMcumgrClient(
            mcumgr_path=self.mcumgr_var.get().strip() or "mcumgr",
            port=port,
            baud=int(self.baud_var.get()),
            mtu=int(self.mtu_var.get()),
            logger=self.log,
            progress_callback=self.progress_percent,
        )

    def _start_worker(self, target) -> None:
        if self.worker is not None and self.worker.is_alive():
            messagebox.showinfo("ArgiSense USB-C update", "An operation is already running.")
            return

        self.update_image_info()
        self._set_progress(None)
        self._set_busy(True)
        self.worker = threading.Thread(target=self._worker_guard, args=(target,), daemon=True)
        self.worker.start()

    def _worker_guard(self, target) -> None:
        try:
            target()
            self._queue("status", "Ready")
        except Exception as exc:  # noqa: BLE001 - surface all tool errors to user.
            self.log(f"ERROR: {exc}")
            self._queue("status", "Failed")
            self._queue("error", str(exc))
        finally:
            self._queue("busy", False)

    def _probe_worker(self) -> None:
        self._queue("status", "Probing...")
        client = self._client()
        output = client.echo()
        if "hello" not in output:
            raise McumgrError("MCUmgr echo did not return hello.")
        _output, images = client.image_list()
        self._log_images(images)
        self.log("Probe OK.")

    def _image_list_worker(self) -> None:
        self._queue("status", "Reading images...")
        _output, images = self._client().image_list()
        self._log_images(images)

    def _upload_worker(self, test_after_upload: bool, reset_after_test: bool) -> None:
        image_path = Path(self.file_var.get())
        if not image_path.exists():
            raise FileNotFoundError("Select a valid zephyr.signed.bin file.")

        upload_timeout_s = int(self.timeout_var.get())
        client = self._client()

        self._queue("status", "Uploading...")
        self._queue("progress", 0.0)
        self.log(f"Image: {image_path}")
        self.log(parse_mcuboot_header(image_path))
        if self.erase_var.get():
            self.log("Erase before upload: enabled (--noerase=false)")
        else:
            self.log("Erase before upload: disabled, mcumgr may resume an old upload")
        client.upload(image_path, upload_timeout_s, self.erase_var.get())
        self._queue("progress", 100.0)

        self._queue("status", "Reading uploaded image...")
        _output, images = client.image_list()
        self._log_images(images)

        if not test_after_upload:
            self.log("Upload complete. The image is in the secondary slot.")
            return

        slot1 = self._slot1_image(images)
        if slot1 is None or not slot1.hash:
            raise McumgrError("Could not find uploaded slot 1 image hash.")

        active = self._active_image(images)
        if active is not None and active.hash == slot1.hash:
            self.log(
                "Uploaded image hash matches the active image; "
                "skipping test/reset because there is no new image to swap."
            )
            return

        self._queue("status", "Marking image test...")
        client.test(slot1.hash)

        if not reset_after_test:
            self.log("Image marked pending. Reset the board to boot it.")
            return

        self._queue("status", "Resetting...")
        client.reset()

        wait_s = int(self.reset_wait_var.get())
        self.log(f"Waiting {wait_s} seconds for USB re-enumeration...")
        time.sleep(wait_s)

        self._queue("status", "Checking active image...")
        _output, images = client.image_list()
        self._log_images(images)

        active = self._active_image(images)
        if active is None:
            raise McumgrError("Could not determine active image after reset.")

        if self.confirm_var.get() and "confirmed" not in active.flags and active.hash:
            self._queue("status", "Confirming active image...")
            client.confirm(active.hash)
            _output, images = client.image_list()
            self._log_images(images)
            self.log("Active image confirmed.")
        else:
            self.log("Active image is already confirmed or auto-confirm is disabled.")

    def _confirm_active_worker(self) -> None:
        self._queue("status", "Confirming active image...")
        client = self._client()
        _output, images = client.image_list()
        active = self._active_image(images)
        if active is None or not active.hash:
            raise McumgrError("Could not find active image hash.")
        client.confirm(active.hash)
        _output, images = client.image_list()
        self._log_images(images)
        self.log("Active image confirmed.")

    def _slot1_image(self, images: list[ImageInfo]) -> ImageInfo | None:
        for image in images:
            if image.slot == 1:
                return image
        return None

    def _active_image(self, images: list[ImageInfo]) -> ImageInfo | None:
        for image in images:
            if "active" in image.flags:
                return image
        return None

    def _log_images(self, images: list[ImageInfo]) -> None:
        if not images:
            self.log("No image entries parsed from mcumgr output.")
            return
        self.log("Parsed image table:")
        for image in images:
            self.log(
                f"  image={image.image} slot={image.slot} "
                f"version={image.version} flags='{image.flags}' hash={image.hash}"
            )

    def log(self, text: str) -> None:
        self._queue("log", text)

    def progress_percent(self, percent: float) -> None:
        self._queue("progress", percent)

    def _queue(self, kind: str, payload: object) -> None:
        self.ui_queue.put((kind, payload))

    def _drain_queue(self) -> None:
        try:
            while True:
                kind, payload = self.ui_queue.get_nowait()
                if kind == "log":
                    self._append_log(str(payload))
                elif kind == "status":
                    self.status_var.set(str(payload))
                elif kind == "busy":
                    self._set_busy(bool(payload))
                elif kind == "progress":
                    self._set_progress(float(payload))
                elif kind == "error":
                    messagebox.showerror("ArgiSense USB-C update", str(payload))
        except queue.Empty:
            pass
        self.root.after(100, self._drain_queue)

    def _append_log(self, text: str) -> None:
        self.log_text.configure(state=tk.NORMAL)
        self.log_text.insert(tk.END, text + "\n")
        self.log_text.see(tk.END)
        self.log_text.configure(state=tk.DISABLED)

    def _set_busy(self, busy: bool) -> None:
        state = tk.DISABLED if busy else tk.NORMAL
        for button in self.buttons:
            button.configure(state=state)
        if busy:
            self.progress.start(12)
        else:
            self.progress.stop()

    def _set_progress(self, percent: float | None) -> None:
        if percent is None:
            self.progress.stop()
            self.progress.configure(mode="indeterminate")
            self.progress_var.set(0.0)
            self.progress_text_var.set("")
            return

        self.progress.stop()
        self.progress.configure(mode="determinate")
        value = max(0.0, min(100.0, percent))
        self.progress_var.set(value)
        self.progress_text_var.set(f"{value:5.1f}%")
        self.status_var.set(f"Uploading {value:.1f}%")


def main() -> None:
    root = tk.Tk()
    UsbMcumgrGui(root)
    root.mainloop()


if __name__ == "__main__":
    main()
