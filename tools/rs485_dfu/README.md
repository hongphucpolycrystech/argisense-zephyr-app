# ArgiSense RS485 Firmware Update GUI

This folder contains a small Python/Tkinter PC tool for updating ArgiSense
firmware through the external RS485 Modbus RTU port.

The tool uploads an MCUboot signed binary to the secondary image slot through
the ArgiSense DFU holding-register window, verifies the SHA-256 hash on the
device, then can mark the image for an MCUboot test swap and reboot.

## Install

From the west workspace:

```powershell
cd C:\Users\zephyr44_workspace
py -3.12 -m pip install -r argisense-zephyr-app\tools\rs485_dfu\requirements.txt
```

## Run

```powershell
cd C:\Users\zephyr44_workspace
py -3.12 argisense-zephyr-app\tools\rs485_dfu\argisense_rs485_dfu_gui.py
```

Select the USB-to-RS485 adapter COM port, the current Modbus baudrate and
slave address, then choose:

```text
build\argisense-zephyr-app\zephyr\zephyr.signed.bin
```

Use the default 96-byte chunk size unless the firmware reports a lower device
maximum during `Probe`.

## DFU Register Window

The PC tool uses these holding registers:

```text
1000  dfu_control
1001  dfu_status
1002  dfu_error
1003  dfu_image_size_hi
1004  dfu_image_size_lo
1005  dfu_image_crc32_hi
1006  dfu_image_crc32_lo
1007  dfu_bytes_written_hi
1008  dfu_bytes_written_lo
1009  dfu_chunk_offset_hi
1010  dfu_chunk_offset_lo
1011  dfu_chunk_length
1012  dfu_chunk_crc32_hi
1013  dfu_chunk_crc32_lo
1014  dfu_max_chunk_bytes
1015  dfu_last_command
1020..1035  dfu_sha256
1100..      dfu_chunk_data
```

Control commands:

```text
0xD001  begin upload
0xD002  write current chunk
0xD003  verify uploaded image
0xD004  mark image for MCUboot test swap
0xD005  abort current upload
0xD006  mark image for test swap and reboot
```

Status values:

```text
0  idle
1  receiving
2  verifying
3  verified
4  pending
5  error
```

