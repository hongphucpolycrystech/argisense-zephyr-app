# ArgiSense USB-C Firmware Update GUI

This folder contains a small Python/Tkinter PC tool for updating ArgiSense
firmware through the board USB-C connector.

The firmware must be built with the USB update profile so the board exposes two
CDC ACM virtual COM ports:

```text
CDC ACM 0: Zephyr console, log output, and shell
CDC ACM 1: MCUmgr SMP firmware update transport
```

Use the second COM port for this tool. On the current bring-up machine that port
has appeared as `COM24`.

## Build Firmware

From the west workspace:

```powershell
cd C:\Users\zephyr44_workspace
argisense-zephyr-app\app\compile.bat mcuboot hsi usbupdate version 1.2.1+8
```

The signed image to upload is:

```text
build\argisense-zephyr-app\zephyr\zephyr.signed.bin
```

## Install

The GUI calls the `mcumgr` command-line tool, so `mcumgr` must be available in
`PATH`.

Optional COM-port auto-detection uses `pyserial`:

```powershell
cd C:\Users\zephyr44_workspace
py -3.12 -m pip install -r argisense-zephyr-app\tools\usb_mcumgr\requirements.txt
```

## Run

```powershell
cd C:\Users\zephyr44_workspace
py -3.12 argisense-zephyr-app\tools\usb_mcumgr\argisense_usb_mcumgr_gui.py
```

Typical flow:

1. Select the MCUmgr COM port, for example `COM24`.
2. Keep `baud=115200` and `mtu=128`.
3. Select `build\argisense-zephyr-app\zephyr\zephyr.signed.bin`.
4. Keep `Erase secondary slot before upload` enabled.
5. Click `Probe` to verify the connection.
6. Click `Upload + Test + Reset`.

During upload, the progress bar follows the percentage reported by `mcumgr
image upload`. The log window keeps the raw `mcumgr` output for troubleshooting.
If the selected file is identical to the firmware already running in slot 0, the
GUI skips `image test` and reset because MCUboot has no new image to swap.

After reset, MCUboot swaps the uploaded image into the primary slot. If
`Confirm active image after reset` is enabled, the GUI reads the active hash and
confirms it through MCUmgr so the bootloader keeps it on later resets.

## Troubleshooting

If the GUI reports `Access is denied` for the MCUmgr COM port, close any serial
terminal, Zephyr shell, or previous `mcumgr` process using that port. On Windows,
only one process can open a COM port at a time.

If the device log shows `Uploaded image sha256 hash verification failed`, keep
`Erase secondary slot before upload` enabled. The GUI then passes
`--noerase=false` to `mcumgr image upload`, forcing a clean secondary slot
instead of resuming from stale or partial image data.

The USB update firmware profile also disables the `mcumgr_img_grp` log module.
Zephyr's optional upload `match` check can report a false error on the console
even when MCUboot accepts, swaps, boots, and confirms the uploaded image. The
RS485 DFU path still keeps its own explicit CRC32 and SHA-256 verification.
