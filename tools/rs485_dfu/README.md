# ArgiSense RS485 Service GUI

This folder contains a Python/Tkinter PC service tool for ArgiSense devices on
the external RS485 Modbus RTU port.

The tool provides three operator views:

- `Firmware Update`: upload an MCUboot signed binary to the secondary image
  slot after writing the DFU service unlock key, verify SHA-256 on the device,
  then optionally mark the image for an MCUboot test swap and reboot. The same
  tab also has `Confirm Image` to manually confirm the currently running
  MCUboot image after field validation.
- `Sensors`: read methane, pressure, humidity, temperature, DAC current, status,
  sequence, and uptime registers. The tab can poll continuously and draw an
  auto-scaled trend graph.
- `Device Config`: read and write common runtime settings including unit ID,
  baud preset, data bits, parity, stop bits, RS485 termination, measurement
  timing, and DAC current limits.

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

Use `Auto Detect` for normal service work. The tool scans the selected COM port
or all available COM ports, tries the supported baud presets, parity modes, stop
bits, and Unit IDs from `Scan IDs`, then connects when register `0` reports the
ArgiSense device ID `0xA651`. The default `Scan IDs` value is `1-10,247`; enter
`1-247` when the full RS485 network must be searched.

For manual connection, select the USB-to-RS485 adapter COM port, the current
Modbus baudrate, data bits, parity, stop bits, and slave address.

For firmware update, open the `Firmware Update` tab and choose:

```text
build\argisense-zephyr-app\zephyr\zephyr.signed.bin
```

Use the default 96-byte chunk size unless the firmware reports a lower device
maximum during `Probe`. The default unlock key is `0xA65D`; enter the key that
matches `CONFIG_ARGISENSE_RS485_DFU_UNLOCK_KEY` for the target firmware.

Use `Confirm Image` only after the running firmware has passed the required
sensor, output, and communication checks. Confirming an image disables MCUboot's
automatic rollback for that image.

The default firmware transport setting is 8 data bits, no parity, and 2 stop
bits, matching Modbus RTU no-parity framing. If the device is configured for
8N1, choose `8` data bits, `None (N)`, and `1` stop bit in the GUI.

Use the `Sensors` tab for live polling and graphing. Use the `Device Config`
tab to read the current device configuration before applying changes. Transport
changes such as unit ID, baud, data bits, parity, and stop bits are saved
immediately but take effect after device reboot.

`Auto Detect` stops at the first valid ArgiSense response. If multiple
ArgiSense devices are on the same RS485 bus, set `Scan IDs` to a narrow range.
The GUI scans only the selected COM port by default. Enable `All COM ports` only
when you deliberately want the tool to open every detected serial port and send
Modbus probe frames; the GUI asks for confirmation before doing this.

## Service Registers

The monitor and configuration tabs use these holding registers:

```text
0       device_id
1       register_map_version
2       status_flags
3       modbus_address
4..5    rs485_baudrate
6       measurement_period_seconds
7       measurement_window_ms
8       baud_preset
9       rs485_termination_enabled
10..11  methane_ppm_x100
12..13  pressure_pa
14      dac0_current_ua
15      dac1_current_ua
16..17  sample_sequence
18..19  sample_uptime_seconds
27      reboot_required
30      dac_min_current_ua
31      dac_max_current_ua
32      dac_fault_current_ua
33      command
34      rs485_parity
35      rs485_stop_bits
36      rs485_data_bits
74      pressure_temperature_centi_c
80      humidity_rh_x100
81      ambient_temperature_centi_c
82      humidity_last_error
```

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
1016  dfu_unlock_key
1017  dfu_unlock_remaining_s
1020..1035  dfu_sha256
1100..      dfu_chunk_data
```

Write the configured service key to `1016` before writing image metadata,
SHA-256, chunk payload, or DFU control commands. Register `1017` reports the
remaining unlock window in seconds.

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
