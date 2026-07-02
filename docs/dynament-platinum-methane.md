# Dynament Platinum Methane Sensor Integration

This note records the firmware assumptions for using a Dynament Platinum Series
Hydrocarbon infrared sensor as the ArgiSense methane sensor.

The full methane + pressure firmware overview is kept separately in
`docs/methane-pressure-firmware-overview.md`.

## Selected Sensor Family

The selected methane sensor family is:

```text
Dynament Hydrocarbon Infrared Gas Sensors - Platinum Series
```

Firmware target gas:

```text
methane
```

The Platinum hydrocarbon sensor is an NDIR sensor module with integrated optics,
electronics, firmware, temperature compensation, and a linearized gas output.
Dynament offers methane ranges including 0-5 percent volume, 0-100 percent
volume, and a 0-5-100 percent dual-range option.

## Electrical And Power Notes

Important hardware-facing requirements:

| Item | Firmware assumption |
| --- | --- |
| Supply | 3.0 V to 5.0 V |
| Supply rise time | Less than 50 ms |
| UART logic | Nominal 2.8 V logic |
| UART format | 8 data bits, 1 stop bit, no parity |
| Default firmware baud | 38400 |
| Other supported baud rates | 19200, 9600, 4800 |
| Warm-up | Default 45 s; allow up to 60 s before trusting gas data |
| T90 response | Less than 30 s |

The Dynament user manual states that the sensor is designed for continuous
powered operation. Repeated short-interval power cycling is outside the intended
use of the product. Because of that, the ArgiSense methane + pressure firmware
keeps `+3V3_PRE` enabled by default while idle:

```text
CONFIG_ARGISENSE_PRE_RAIL_ALWAYS_ON=y
```

The pressure sensor PS/CS, analog rails, DAC power, and RS485 termination can
still be duty-cycled. This keeps the power-saving structure without repeatedly
power-cycling the methane sensor.

## Zephyr Board Mapping

The firmware maps the sensor on `uart4`:

| Signal | MCU pin |
| --- | --- |
| Methane TX from MCU | `PC10 / UART4_TX` |
| Methane RX to MCU | `PC11 / UART4_RX` |

Schematic review note:

The updated `Sensor_Platform_V1 (1).pdf` sheet 9 shows `METH_UART_TXD` on
`PC8` and `METH_UART_RXD` on `PC7`, and sheet 14 routes those nets through
0 ohm options to the methane sensor connector `SENSOR_COMM1..4`. That is a
hardware/firmware mismatch: the Zephyr STM32U575RGT6 pinctrl data does not
provide UART TX/RX alternate functions on `PC8`/`PC7`.

Keep the firmware mapping on a valid hardware UART pair and update the PCB
route to `PC10`/`PC11`, or choose another valid UART TX/RX pair and update both
the schematic and DTS together. Do not remap DTS to `PC8`/`PC7` as a hardware
UART unless the MCU pinout is changed or a software UART solution is explicitly
designed and validated.

`SENSOR_COMM1..4` are connector-side nets, not independent MCU GPIOs. They do
not need separate DTS entries when they are populated as the Dynament UART path.
For the consolidated IO mapping and conflict list across both product variants,
see `docs/sensor-io-mapping.md`.

DTS node:

```dts
&uart4 {
	current-speed = <38400>;

	methane_dynament: methane-sensor {
		compatible = "argisense,dynament-platinum-hydrocarbon";
		gas-type = "methane";
		protocol = "dynament-premier-live-data-simple";
		full-scale-percent-x100 = <10000>;
		live-data-variable-id = <0x06>;
		warmup-time-ms = <60000>;
		read-period-ms = <5000>;
		logic-level-mv = <2800>;
		status = "okay";
	};
};
```

`full-scale-percent-x100 = <10000>` means 100.00 percent volume. If the ordered
sensor is the 0-5 percent volume methane variant, change this property to
`<500>` and set:

```text
CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM=50000
```

## Live Data Simple Protocol

The attached AN0007 Arduino example uses the Dynament/Premier protocol to read a
simplified live data structure over UART.

Request bytes:

```text
10 13 06 10 1F 00 58
```

Request breakdown:

| Byte | Value | Meaning |
| --- | --- | --- |
| 1 | `0x10` | DLE |
| 2 | `0x13` | RD |
| 3 | `0x06` | Live data simple variable ID |
| 4 | `0x10` | DLE |
| 5 | `0x1F` | EOF |
| 6 | `0x00` | Checksum high |
| 7 | `0x58` | Checksum low |

Example response from AN0007:

```text
10 1A 08 04 00 04 00 00 00 7A C3 10 1F 01 A6
```

Response breakdown:

| Bytes | Meaning |
| --- | --- |
| `10 1A` | DLE + DAT |
| `08` | Data length |
| `04 00` | Version, little-endian |
| `04 00` | Status flags, little-endian |
| `00 00 7A C3` | Gas reading, IEEE-754 little-endian float |
| `10 1F` | DLE + EOF |
| `01 A6` | Additive checksum high/low |

From the AN0007 examples, the checksum is the 16-bit additive sum of all bytes
before the checksum field, transmitted high byte first.

The gas value is a decimal percentage value, not an integer. The example
`00 00 7A C3` decodes to `-250.0`, which Dynament uses for warm-up or fault
conditions.

The protocol can byte-stuff DLE/control characters. The driver reads the UART
stream as `DLE + DAT + length + payload + DLE + EOF + checksum`, unescapes
stuffed payload bytes, then passes a compact frame to the live-data-simple
decoder.

## Firmware Data Mapping

The ArgiSense firmware stores methane internally as:

```text
ppm_x100
```

Conversion from Dynament percent volume:

```text
ppm_x100 = percent_volume * 1,000,000
```

Examples:

| Dynament value | Methane ppm | Firmware `ppm_x100` |
| --- | ---: | ---: |
| `1.00` percent volume | `10000` | `1000000` |
| `5.00` percent volume | `50000` | `5000000` |
| `100.00` percent volume | `1000000` | `100000000` |

Negative values should be treated as invalid for DAC output and RS485 process
data. The DAC channel should use the configured fault current while the methane
sensor reports warm-up or fault data.

## Implementation Status

Implemented now:

- DTS binding for `argisense,dynament-platinum-hydrocarbon`.
- `uart4` board node for the Dynament methane sensor, pending PCB routing to a
  valid UART TX/RX pin pair.
- Out-of-tree Zephyr sensor driver in `drivers/sensor/dynament_platinum`.
- Startup mapping check through the methane sensor device.
- AN0007 live-data-simple UART request, stream parser, byte-stuff handling, and
  compact-frame decoder in the driver.
- Default idle policy that keeps `+3V3_PRE` powered.

Still required before production:

- Retry policy around the current blocking read timeout.
- Exact interpretation of Dynament status flags from TDS0045.
- Optional Modbus RTU mode if the ordered sensor is configured for Modbus
  instead of the Dynament/Premier protocol.
- Calibration workflow and gas range lock based on the exact ordered part code.
