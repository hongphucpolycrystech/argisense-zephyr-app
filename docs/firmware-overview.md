# ArgiSense Firmware Overview

This document describes the proposed firmware architecture for the ArgiSense
low-power sensor board based on STM32U575RGT6 and Zephyr 4.4.0.

The firmware reads methane gas and pressure sensors, publishes measurements over
RS485, and drives a 4-20 mA analog output through the external DAC/current-output
circuit. The design is power-first: external sensor and analog rails are off by
default and are enabled only during measurement or output update windows.

## Firmware Goals

- Periodically measure methane gas concentration.
- Periodically measure pressure.
- Optionally read auxiliary environmental data if populated on the board.
- Export live values and status over RS485.
- Drive a 4-20 mA output mapped from a selected process value.
- Keep high-current rails off whenever the board is idle.
- Preserve a clean path for calibration, diagnostics, and future OTA/storage.

## Current Board Interfaces

The custom board DTS currently exposes these hardware interfaces:

| Function | Zephyr device | MCU pins | Schematic role |
| --- | --- | --- | --- |
| Debug console | `usart1` | `PA9`, `PA10` | Console/debug UART |
| RS485 | `usart2` | `PA2`, `PA3`, `PA1 DE` | Half-duplex RS485 port |
| Methane sensor | `uart4` | `PC10`, `PC11` | Methane sensor UART |
| DAC and EEPROM bus | `i2c1` | `PB8`, `PB9` | GP8302 DAC and EEPROM |
| Analog/helper bus | `i2c2` | `PB10`, `PB14` | Analog supply monitor and HTU21D |
| Pressure sensor | `spi1` | `PA5`, `PA6`, `PA7`, `PA4 CS` | Pressure sensor SPI |
| Thermistor/ADC | `adc1` | `PC0 / ADC1_IN1` | Analog temperature or board sensing |

The board-specific GPIOs are exposed through `/zephyr,user`:

| Property | Purpose | Default firmware state |
| --- | --- | --- |
| `pre-power-gpios` | Enables `+3V3_PRE` sensor rail | Off |
| `analog-power-gpios` | Enables analog positive/negative rail section | Off |
| `dac-power-gpios` | Enables DAC/4-20 mA boost supply | Off |
| `dac-alarm-gpios` | Reads DAC alarm/status pin | Input |
| `rs485-termination-gpios` | Enables optional 120 ohm termination | Off |
| `pressure-ps-gpios` | Selects pressure sensor protocol/mode | Configurable |
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
| DAC driver | Converts engineering value to 4-20 mA command | Measurement loop or output service |
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
app/src/current_loop_dac.c
app/src/current_loop_dac.h
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
9. Read methane sensor over UART.
10. Read optional ADC or I2C environmental values.
11. Validate and timestamp all readings.
12. Update the internal data model.
13. Update the 4-20 mA DAC if enabled.
14. Turn off DAC power when the current loop does not need continuous output.
15. Turn off analog rails.
16. Turn off `+3V3_PRE`.
17. Sleep until the next measurement period.

Current Kconfig duty-cycle controls:

```text
CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS
CONFIG_ARGISENSE_PRE_RAIL_SETTLE_MS
CONFIG_ARGISENSE_ANALOG_RAIL_SETTLE_MS
CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS
CONFIG_ARGISENSE_PRESSURE_PS_ACTIVE
CONFIG_ARGISENSE_DAC_POWER_DURING_MEASUREMENT
```

## Methane Sensor Handling

The methane sensor is connected to `uart4`.

Recommended behavior:

- Default to the sensor module's required baud rate. The current DTS default is
  9600 baud.
- Add a frame parser specific to the selected methane module.
- Use a read timeout so the measurement loop cannot hang.
- Track these fields in the data model:
  - methane concentration
  - methane unit
  - sensor status
  - warm-up state
  - last successful read timestamp
  - communication error counter

The methane sensor should be treated as invalid during warm-up or after a UART
timeout. The RS485 status map and 4-20 mA fault behavior must expose this state.

## Pressure Sensor Handling

The pressure sensor is connected to `spi1` with GPIO chip select on `PA4`.

Recommended behavior:

- Use SPI mode unless the selected pressure sensor datasheet requires I2C.
- Keep `pressure-ps-gpios` deterministic before enabling transactions.
- Add sensor-specific compensation in the pressure driver.
- Track these fields:
  - raw pressure sample
  - compensated pressure
  - pressure unit
  - sensor temperature if available
  - sensor status
  - CRC or data-valid flag if supported by the sensor

Pressure sensor chip select should stay inactive while the sensor rail is off.

## 4-20 mA DAC Output

The DAC/current-output device is on `i2c1`. The schematic uses a GP8302-family
current DAC circuit with a 0-25 mA capable output stage.

Recommended behavior:

- Keep `dac-power-gpios` off by default.
- Power the DAC only when output must be refreshed, unless the product
  requirement says the 4-20 mA loop must remain continuously driven.
- Map one selected process value to current:

```text
span = clamp((value - range_low) / (range_high - range_low), 0.0, 1.0)
current_mA = 4.0 + (span * 16.0)
```

- Convert `current_mA` to the DAC register value using the final DAC resolution
  and calibration constants from the DAC datasheet.
- Support fault current policy:
  - 3.6 mA for sensor fault or under-range
  - 21.0 mA for over-range or configured alarm
  - hold-last-value for short transient faults, if required

Recommended Kconfig or settings fields:

```text
output_source = methane | pressure
output_range_low
output_range_high
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
| 0x0030 | Input | DAC current command in microamps |
| 0x0031 | Input | DAC/alarm status |
| 0x0100 | Holding | Modbus slave address |
| 0x0101 | Holding | RS485 baud preset |
| 0x0110 | Holding | Measurement period seconds |
| 0x0120 | Holding | DAC output source |
| 0x0121 | Holding | DAC range low |
| 0x0122 | Holding | DAC range high |
| 0x0123 | Holding | Fault current mode |
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
	int32_t board_temp_mc;
	uint32_t status_flags;
	uint32_t uptime_ms;
	int32_t dac_current_ua;
};
```

Recommended status flags:

```text
BIT(0) methane_valid
BIT(1) pressure_valid
BIT(2) dac_valid
BIT(3) methane_warmup
BIT(4) methane_comm_error
BIT(5) pressure_comm_error
BIT(6) dac_alarm
BIT(7) rail_fault
BIT(8) calibration_missing
```

## Power Strategy

The board should keep only the MCU and always-on support circuitry alive during
idle.

Idle state:

- `+3V3_PRE` off.
- Analog rails off.
- DAC boost off unless continuous 4-20 mA output is required.
- RS485 termination off unless the installation requires this node to terminate
  the bus.
- Pressure CS inactive.
- Zephyr PM allowed to enter STM32U575 STOP idle states.

Measurement state:

- Enable sensor rails.
- Wait for rails to settle.
- Read sensors.
- Update output and communication data.
- Return rails to off state.

Product decision to confirm: whether the 4-20 mA loop must remain driven while
the MCU sleeps. If yes, the DAC power strategy must change from duty-cycled to
continuous or latched-output mode.

## Fault Handling

Minimum recommended fault handling:

- If a sensor read fails, keep the last valid value and mark the corresponding
  status bit invalid.
- If the same sensor fails for N consecutive cycles, drive the DAC to configured
  fault current.
- If DAC alarm is asserted, expose it over RS485 immediately.
- If rail GPIO setup fails at boot, stop the measurement loop and report a fatal
  error over console.
- If RS485 receives unsupported commands, respond with a protocol error rather
  than silently ignoring them.

## Calibration and Persistent Settings

The schematic includes I2C storage support, so firmware should eventually store:

- Methane offset and span calibration.
- Pressure offset and span calibration.
- DAC current trim at 4 mA and 20 mA.
- RS485 address and baud rate.
- Output source and range.
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
- Add simple UART receive test for methane sensor.
- Add pressure SPI device ID read.

Phase 2 - Sensor acquisition:

- Implement methane UART protocol driver.
- Implement pressure sensor driver.
- Add measurement data model and status flags.
- Replace the placeholder measurement window with real sensor reads.

Phase 3 - Outputs:

- Implement GP8302 current DAC driver.
- Add 4-20 mA scaling and fault current policy.
- Add RS485 Modbus RTU slave register map.

Phase 4 - Product behavior:

- Add calibration storage.
- Add configurable measurement period.
- Add watchdog strategy.
- Add low-power validation on real hardware.
- Add hardware-in-loop tests for sensor read, RS485, and DAC output.

## Open Hardware Items

These details should be confirmed before final firmware lock:

- Exact methane sensor module and UART protocol.
- Exact pressure sensor part number, SPI mode, and `PS` polarity.
- GP8302 I2C address, resolution, and alarm polarity.
- Whether 4-20 mA output must be continuously driven while sleeping.
- Whether RS485 default baud should be 9600, 19200, or 115200.
- Whether RS485 termination should be controlled by firmware or fixed by
  installer configuration.
