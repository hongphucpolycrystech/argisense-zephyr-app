# ArgiSense Methane + Pressure Firmware Overview

This document describes the proposed firmware architecture for the ArgiSense
methane + pressure product profile on the `argisense_mp_u575rg` board, based on
STM32U575RGT6 and Zephyr 4.4.0.

The firmware reads methane gas from a Dynament Platinum Hydrocarbon infrared
sensor, pressure from an MS580305BA01-00 sensor, and humidity/ambient
temperature from an HTU21D sensor. It publishes measurements over RS485 and
drives two 4-20 mA analog outputs through two GP8302 current DAC devices. The
design is power-first, while keeping the Dynament methane sensor powered
continuously by default.

## Product Board Split

This document applies to the methane + pressure product board:

```text
argisense_mp_u575rg
```

The pH product uses a separate board target because its PCB and pinout are
different:

```text
argisense_ph_u575rg
```

Recommended firmware split:

| Product | Board target | Firmware profile | Notes |
| --- | --- | --- | --- |
| Methane + pressure | `argisense_mp_u575rg` | Current document | Two sensors, RS485, DAC0 methane, DAC1 pressure |
| pH | `argisense_ph_u575rg` | Future pH profile | pH front-end, pH compensation, pH-specific DAC mapping |

Shared services such as MCUboot, RS485 framing, calibration storage, power
sequencing, and 4-20 mA scaling should stay common. Sensor measurement logic and
DAC source mapping should be product-specific.

## Firmware Goals

- Periodically measure methane gas concentration.
- Periodically measure pressure.
- Periodically measure relative humidity and ambient temperature with HTU21D.
- Export live values and status over RS485.
- Drive two 4-20 mA outputs: GP8302 DAC0 for methane and GP8302 DAC1 for
  pressure.
- Keep high-current rails off whenever the board is idle.
- Preserve a clean path for calibration, diagnostics, and future OTA/storage.

## Methane + Pressure Board Interfaces

The `argisense_mp_u575rg` board DTS currently exposes these hardware interfaces:

| Function | Zephyr device | MCU pins | Schematic role |
| --- | --- | --- | --- |
| Debug console | `usart1` | `PA9`, `PA10` | Console/debug UART |
| RS485 | `usart2` | `PA2`, `PA3`, `PA1 DE` | Half-duplex RS485 port |
| Methane sensor | `uart4` | `PC10`, `PC11` | Dynament Platinum methane UART, 38400 baud |
| DAC0 and EEPROM bus | `i2c1` | `PB8`, `PB9` | Methane GP8302 at `0x58` and EEPROM |
| DAC1/helper bus | `i2c2` | `PB10`, `PB14` | Pressure GP8302 at `0x58`, analog monitor, and HTU21D |
| Humidity sensor | `i2c2` | `PB10`, `PB14` | HTU21D at `0x40`, Zephyr `sensirion,sht21` compatible |
| Pressure sensor | `spi1` | `PA5`, `PA6`, `PA7`, `PA4 CS` | MS580305BA01-00 / MS5803-05BA pressure sensor SPI |
| Thermistor/ADC | `adc1` | `PC0 / ADC1_IN1` | Analog temperature or board sensing |

The board-specific GPIOs are exposed through `/zephyr,user`:

| Property | Purpose | Default firmware state |
| --- | --- | --- |
| `dac-channel-count` | Declares the firmware DAC output count | `2` |
| `dac0-output-source` | Documents GP8302 DAC0 source on `i2c1` | `methane` |
| `dac1-output-source` | Documents GP8302 DAC1 source on `i2c2` | `pressure` |
| `pressure-part-number` | Documents the selected pressure sensor | `MS580305BA01-00` |
| `pressure-interface` | Documents the selected pressure sensor bus | `spi` |
| `pressure-ps-active-state` | Documents protocol-select polarity for the selected pressure sensor | `low` |
| `humidity-part-number` | Documents the selected humidity sensor | `HTU21D` |
| `humidity-interface` | Documents the selected humidity sensor bus | `i2c2` |
| `pre-power-gpios` | Enables `+3V3_PRE` sensor rail | Off |
| `analog-power-gpios` | Enables analog positive/negative rail section | Off |
| `dac-power-gpios` | Enables the shared DAC/4-20 mA boost supply for both GP8302 devices | Off |
| `dac-alarm-gpios` | Reads DAC alarm/status pin if populated as a shared alarm | Input |
| `rs485-termination-gpios` | Enables optional 120 ohm termination | Off |
| `pressure-ps-gpios` | MS5803 protocol select, low = SPI and high = I2C | Low during SPI measurements |
| `pressure-cs-gpios` | Pressure sensor SPI chip select | Inactive |

## Proposed Runtime Model

The firmware should run as a small event-driven application with one primary
measurement loop and optional communication work items.

Recommended thread/work split:

| Component | Responsibility | Suggested context |
| --- | --- | --- |
| Power manager | Controls external rails and settle delays | Main thread or service module |
| Sensor manager | Coordinates methane, pressure, and auxiliary reads | Main measurement loop |
| Methane driver | UART protocol parser for methane module | Work queue or blocking read with timeout |
| Pressure driver | SPI transactions and compensation | Main measurement loop |
| Humidity driver | HTU21D relative humidity and ambient temperature reads | Main measurement loop |
| DAC driver | Converts methane and pressure values to two GP8302 4-20 mA commands | Measurement loop or output service |
| RS485 service | Modbus RTU or framed protocol slave | Dedicated thread or UART async callbacks |
| Data model | Holds latest measurements, status flags, timestamps | Shared module with locking |

The initial implementation can stay simple and run from `main.c`. Once sensor
drivers grow, split the application into these files:

```text
app/src/main.c
app/src/power_manager.c
app/src/power_manager.h
app/src/sensor_manager.c
app/src/sensor_manager.h
app/src/methane_sensor.c
app/src/methane_sensor.h
app/src/pressure_sensor.c
app/src/pressure_sensor.h
app/src/humidity_sensor.c
app/src/humidity_sensor.h
app/src/current_loop_dac.c
app/src/current_loop_dac.h
app/src/current_loop_output.c
app/src/current_loop_output.h
app/src/rs485_service.c
app/src/rs485_service.h
app/src/measurement_data.c
app/src/measurement_data.h
```

## Measurement Cycle

The proposed measurement sequence is:

1. Wake from Zephyr idle.
2. Keep RS485 termination disabled unless configured by the host.
3. Enable `+3V3_PRE`.
4. Wait `CONFIG_ARGISENSE_PRE_RAIL_SETTLE_MS`.
5. Configure pressure `PS` mode.
6. Enable analog rails.
7. Wait `CONFIG_ARGISENSE_ANALOG_RAIL_SETTLE_MS`.
8. Read pressure over SPI.
9. Read Dynament methane live-data-simple over UART.
10. Read HTU21D humidity and ambient temperature over I2C.
11. Validate and timestamp all readings.
12. Update the internal data model.
13. Update GP8302 DAC0 from methane and GP8302 DAC1 from pressure if enabled.
14. Turn off DAC power when the current loops do not need continuous output.
15. Turn off analog rails.
16. Keep `+3V3_PRE` on by default so the Dynament methane sensor is not
    repeatedly power-cycled.
17. Sleep until the next measurement period.

Current Kconfig duty-cycle controls:

```text
CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS
CONFIG_ARGISENSE_PRE_RAIL_ALWAYS_ON
CONFIG_ARGISENSE_PRE_RAIL_SETTLE_MS
CONFIG_ARGISENSE_ANALOG_RAIL_SETTLE_MS
CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS
CONFIG_ARGISENSE_PRESSURE_PS_ACTIVE
CONFIG_ARGISENSE_DAC_POWER_DURING_MEASUREMENT
CONFIG_ARGISENSE_DAC_MIN_CURRENT_UA
CONFIG_ARGISENSE_DAC_MAX_CURRENT_UA
CONFIG_ARGISENSE_DAC_FAULT_CURRENT_UA
CONFIG_ARGISENSE_METHANE_DAC_RANGE_LOW_PPM
CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM
CONFIG_ARGISENSE_METHANE_SENSOR_WARMUP_SECONDS
CONFIG_ARGISENSE_METHANE_SENSOR_READ_PERIOD_MS
CONFIG_ARGISENSE_HUMIDITY_SENSOR_READ_PERIOD_SECONDS
CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_LOW_PA
CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_HIGH_PA
```

## Methane Sensor Handling

The selected methane sensor family is the Dynament Platinum Series Hydrocarbon
infrared gas sensor. It is connected to `uart4` at 38400 baud using 8 data bits,
1 stop bit, and no parity. The board DTS declares it as
`argisense,dynament-platinum-hydrocarbon`.

Recommended behavior:

- Keep the sensor powered continuously by default. Dynament documents the
  Platinum sensor as intended for continuous powered operation, and repeated
  short-interval power cycling is outside the intended use of the product.
- Allow up to 60 seconds after first power before trusting gas readings. During
  warm-up or fault conditions, the digital live data can report `-250`.
- Send the live-data-simple request from AN0007:

```text
10 13 06 10 1F 00 58
```

- Parse the compact live-data-simple response:
  - `10 1A` means DLE + DAT.
  - byte 3 is data length.
  - bytes 4-5 are version, little-endian.
  - bytes 6-7 are status flags, little-endian.
  - bytes 8-11 are the gas reading as little-endian IEEE-754 float.
  - `10 1F` means DLE + EOF.
  - final two bytes are a high/low additive checksum.
- Treat the gas reading as percent volume and convert it to `ppm_x100` with:

```text
ppm_x100 = percent_volume * 1,000,000
```

- Handle Dynament byte-stuffed DLE/control characters in the final UART stream
  parser before decoding the compact frame.
- Use a read timeout so the measurement loop cannot hang.
- Track these fields in the data model:
  - methane concentration
  - methane percent volume
  - methane unit
  - Dynament status flags
  - warm-up state
  - last successful read timestamp
  - communication error counter

The methane sensor should be treated as invalid during warm-up or after a UART
timeout. The RS485 status map and 4-20 mA fault behavior must expose this state.
The detailed integration note is in `docs/dynament-platinum-methane.md`.

## Humidity Sensor Handling

The selected humidity sensor is HTU21D. It is connected to `i2c2` at address
`0x40`. Zephyr 4.4 does not need an out-of-tree HTU21D driver for this board:
HTU21D is handled by the built-in SHT21-compatible driver path, with DTS
compatible `sensirion,sht21` and Kconfig `CONFIG_SENSOR=y`.

Recommended behavior:

- Fetch `SENSOR_CHAN_HUMIDITY` and `SENSOR_CHAN_AMBIENT_TEMP` after the I2C bus
  is ready.
- Use `CONFIG_ARGISENSE_HUMIDITY_SENSOR_READ_PERIOD_SECONDS` to avoid reading
  humidity more often than needed.
- Store relative humidity as `%RH x100` and ambient temperature as millidegrees
  Celsius in the central data model.
- Expose humidity sensor communication errors in the RS485 status map.
- Treat humidity as auxiliary process data; it does not drive either 4-20 mA
  output by default.

## Pressure Sensor Handling

The selected pressure sensor is TE Connectivity `MS580305BA01-00`
(`MS5803-05BA01`). It is a 5 bar / 500 kPa absolute digital pressure sensor
with a 24-bit ADC and SPI/I2C digital interface. The `argisense_mp_u575rg`
board uses SPI on `spi1` with GPIO chip select on `PA4`, so the firmware keeps
the sensor `PS` pin low during measurement windows.

Recommended behavior:

- Keep `pressure-ps-gpios` low before enabling SPI transactions.
- Send the MS5803 reset command before the first calibration read.
- Read PROM calibration coefficients and verify the PROM CRC.
- Start D1 pressure conversion and D2 temperature conversion with the selected
  oversampling ratio.
- Wait the datasheet conversion time before reading the 24-bit ADC result.
- Apply the MS5803 first-order and second-order compensation equations before
  publishing pressure.
- Use pascals in the firmware data model and RS485 register map.
- Track these fields:
  - raw pressure sample
  - compensated pressure
  - pressure unit
  - sensor temperature if available
  - sensor status
  - CRC or data-valid flag if supported by the sensor

Pressure sensor chip select should stay inactive while the sensor rail is off.

## Dual GP8302 4-20 mA Outputs

The board uses two single-output GP8302 current DAC devices. They share the
same DAC/current-loop power enable but are mapped to different I2C buses so both
devices can keep the GP8302 default address.

Default hardware mapping:

| Output | GP8302 alias/node | I2C bus | I2C address | Source value | Default low point | Default high point |
| --- | --- | --- | --- | --- | --- | --- |
| DAC0 | `current-loop-dac0` / `dac0_gp8302` | `i2c1` | `0x58` | Methane concentration | `CONFIG_ARGISENSE_METHANE_DAC_RANGE_LOW_PPM` | `CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM` |
| DAC1 | `current-loop-dac1` / `dac1_gp8302` | `i2c2` | `0x58` | Pressure | `CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_LOW_PA` | `CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_HIGH_PA` |

The default methane DAC range is 0 ppm to 1000000 ppm, matching a 0-100%
volume Dynament methane variant. For a 0-5% volume ordered variant, set the high
point to 50000 ppm.

For the selected MS580305BA01-00 pressure sensor, the default pressure DAC
range is 0 Pa to 500000 Pa, matching the nominal 5 bar absolute sensor range.

The default `0x58` address follows the common GP8302 module/library default.
Using separate buses avoids an address conflict without adding an I2C mux or
changing hardware addresses.

Recommended behavior:

- Keep `dac-power-gpios` off by default.
- Power the shared DAC block only when outputs must be refreshed, unless the product
  requirement says the 4-20 mA loops must remain continuously driven.
- Map each process value to its own current command:

```text
span = clamp((value - range_low) / (range_high - range_low), 0.0, 1.0)
current_mA = 4.0 + (span * 16.0)
```

- Convert each `current_mA` to the DAC register value using the final DAC
  resolution and calibration constants from the DAC datasheet.
- Support fault current policy:
  - 3.6 mA for sensor fault or under-range
  - 21.0 mA for over-range or configured alarm
  - hold-last-value for short transient faults, if required

Recommended Kconfig or settings fields:

```text
dac0_i2c_bus = i2c1
dac0_i2c_address = 0x58
dac0_output_source = methane
dac0_output_range_low
dac0_output_range_high
dac1_i2c_bus = i2c2
dac1_i2c_address = 0x58
dac1_output_source = pressure
dac1_output_range_low
dac1_output_range_high
fault_current_mode = low | high | hold
dac_update_period_seconds
```

## RS485 Communication

The RS485 interface is connected to `usart2` with hardware DE on `PA1`.

Recommended protocol: Modbus RTU slave.

Reasons:

- Common for industrial sensors.
- Easy to test with existing RS485 tools.
- Natural fit for measured values, status words, calibration, and output
  configuration.

Default electrical and UART setup:

| Setting | Proposed value |
| --- | --- |
| Interface | RS485 half-duplex |
| UART | `usart2` |
| Baud rate | 115200 for bring-up, configurable later |
| Data format | 8N1 |
| DE control | STM32 USART hardware DE |
| Termination | Disabled by default, host configurable |

Proposed register map:

| Register | Type | Description |
| --- | --- | --- |
| 0x0000 | Input | Device status bitfield |
| 0x0001 | Input | Firmware version major/minor |
| 0x0002 | Input | Uptime low word |
| 0x0003 | Input | Uptime high word |
| 0x0010 | Input | Methane value, scaled integer |
| 0x0011 | Input | Methane status |
| 0x0020 | Input | Pressure value, scaled integer |
| 0x0021 | Input | Pressure status |
| 0x0028 | Input | Relative humidity, `%RH x100` |
| 0x0029 | Input | HTU21D ambient temperature, millidegrees Celsius |
| 0x002A | Input | Humidity sensor status |
| 0x0030 | Input | DAC0 methane current command in microamps |
| 0x0031 | Input | DAC0 status |
| 0x0032 | Input | DAC1 pressure current command in microamps |
| 0x0033 | Input | DAC1 status |
| 0x0100 | Holding | Modbus slave address |
| 0x0101 | Holding | RS485 baud preset |
| 0x0110 | Holding | Measurement period seconds |
| 0x0120 | Holding | DAC0 range low |
| 0x0121 | Holding | DAC0 range high |
| 0x0122 | Holding | DAC1 range low |
| 0x0123 | Holding | DAC1 range high |
| 0x0124 | Holding | Fault current mode |
| 0x0200 | Coil | Force measurement |
| 0x0201 | Coil | Enable RS485 termination |
| 0x0202 | Coil | Enable continuous DAC power |

All multi-register values should use a documented endianness. For bring-up,
prefer signed 32-bit scaled integers split into two 16-bit registers.

## Data Model

Use one central measurement data structure so RS485 and DAC output read from the
same validated state.

Example fields:

```c
struct argisense_measurement {
	int32_t methane_ppm_x100;
	int32_t pressure_pa;
	int32_t humidity_rh_x100;
	int32_t board_temp_mc;
	uint32_t status_flags;
	uint32_t uptime_ms;
	int32_t dac_current_ua[2];
};
```

Recommended status flags:

```text
BIT(0) methane_valid
BIT(1) pressure_valid
BIT(2) dac0_valid
BIT(3) dac1_valid
BIT(4) methane_warmup
BIT(5) methane_comm_error
BIT(6) pressure_comm_error
BIT(7) dac0_alarm
BIT(8) dac1_alarm
BIT(9) rail_fault
BIT(10) calibration_missing
BIT(11) humidity_valid
BIT(12) humidity_comm_error
```

## Power Strategy

The board should keep only the MCU, always-on support circuitry, and the
Dynament methane sensor rail alive during idle by default.

Idle state:

- `+3V3_PRE` on by default for the Dynament methane sensor.
- Analog rails off.
- Shared DAC boost off unless continuous 4-20 mA output is required.
- RS485 termination off unless the installation requires this node to terminate
  the bus.
- Pressure CS inactive.
- Zephyr PM allowed to enter STM32U575 STOP idle states.

Measurement state:

- Ensure the pre-sensor rail is already on or enable it if
  `CONFIG_ARGISENSE_PRE_RAIL_ALWAYS_ON` is disabled.
- Wait for rails to settle.
- Read sensors.
- Update output and communication data.
- Return rails to off state.

Product decision to confirm: whether either 4-20 mA loop must remain driven
while the MCU sleeps. If yes, the shared DAC power strategy must change from
duty-cycled to continuous or latched-output mode.

## Fault Handling

Minimum recommended fault handling:

- If a sensor read fails, keep the last valid value and mark the corresponding
  status bit invalid.
- If the same sensor fails for N consecutive cycles, drive that sensor's DAC
  channel to the configured fault current.
- If either DAC alarm is asserted, expose it over RS485 immediately.
- If rail GPIO setup fails at boot, stop the measurement loop and report a fatal
  error over console.
- If RS485 receives unsupported commands, respond with a protocol error rather
  than silently ignoring them.

## Calibration and Persistent Settings

The schematic includes I2C storage support, so firmware should eventually store:

- Methane offset and span calibration.
- Pressure offset and span calibration.
- Per-channel DAC current trim at 4 mA and 20 mA.
- RS485 address and baud rate.
- Per-channel output range.
- Measurement period.
- Fault-current policy.

Recommended storage approach:

- Use Zephyr settings/NVS for generic firmware settings.
- Add an EEPROM backend only if the product must store settings outside MCU
  internal flash.
- Keep a settings version and CRC so invalid calibration can be detected.

## Suggested Implementation Phases

Phase 1 - Board bring-up:

- Keep the current power manager.
- Add GPIO self-test logs for each rail.
- Add I2C scan for `i2c1` and `i2c2`.
- Add Dynament live-data-simple UART request and parser smoke test.
- Add HTU21D humidity/temperature mapping check on `i2c2`.
- Add MS5803 reset, PROM read, CRC check, and ADC conversion read over SPI.

Phase 2 - Sensor acquisition:

- Implement Dynament UART stream parser with DLE byte-stuff handling.
- Implement pressure sensor driver.
- Implement HTU21D sample fetch and data-model update.
- Add measurement data model and status flags.
- Replace the placeholder measurement window with real sensor reads.

Phase 3 - Outputs:

- Implement the two-device GP8302 current DAC driver: DAC0 on `i2c1`, DAC1 on
  `i2c2`.
- Add per-channel 4-20 mA scaling and fault current policy.
- Add RS485 Modbus RTU slave register map.

Phase 4 - Product behavior:

- Add calibration storage.
- Add configurable measurement period.
- Add watchdog strategy.
- Add low-power validation on real hardware.
- Add hardware-in-loop tests for sensor read, RS485, and DAC output.

## Open Hardware Items

These details should be confirmed before final firmware lock:

- Exact Dynament methane ordered range: 0-5% volume, 0-100% volume, or
  0-5-100% volume.
- Whether HTU21D is powered from `+3V3_PRE` or another rail in the final PCB.
- Exact pressure oversampling ratio and conversion timing policy.
- GP8302 register format, resolution, and alarm polarity.
- Whether `dac-alarm-gpios` is a shared alarm, only DAC0 alarm, or should become
  two separate alarm GPIOs.
- Whether either 4-20 mA output must be continuously driven while sleeping.
- Whether RS485 default baud should be 9600, 19200, or 115200.
- Whether RS485 termination should be controlled by firmware or fixed by
  installer configuration.
