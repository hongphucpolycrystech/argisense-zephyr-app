# HTU21D Humidity Sensor Integration

This note documents the HTU21D humidity and ambient temperature integration for
the ArgiSense methane + pressure firmware profile.

## Hardware Mapping

The HTU21D is connected on the `argisense_mp_u575rg` board as:

| Signal | Mapping |
| --- | --- |
| I2C bus | `i2c2` |
| I2C address | `0x40` |
| Zephyr node label | `humidity_htu21d` |
| Compatible | `argisense,htu21d` |
| Product role | Auxiliary humidity and ambient temperature compensation data |

Board DTS example:

```dts
humidity_htu21d: humidity-sensor@40 {
	compatible = "argisense,htu21d";
	reg = <0x40>;
	part-number = "HTU21D";
	status = "okay";
};
```

The DTS binding is:

```text
dts/bindings/sensor/argisense,htu21d.yaml
```

## Driver

The driver is implemented out of tree:

```text
drivers/sensor/htu21d/htu21d.c
drivers/sensor/htu21d/Kconfig
drivers/sensor/htu21d/CMakeLists.txt
```

Enable it with:

```text
CONFIG_ARGISENSE_HTU21D=y
```

The driver exposes the standard Zephyr Sensor API:

| Zephyr channel | Firmware unit |
| --- | --- |
| `SENSOR_CHAN_HUMIDITY` | `%RH x100` in the application data model |
| `SENSOR_CHAN_AMBIENT_TEMP` | centi-degrees Celsius in the application data model |

Driver behavior:

- Sends HTU21D soft reset command `0xfe` at initialization.
- Uses no-hold humidity command `0xf5`.
- Uses no-hold temperature command `0xf3`.
- Waits for conversion completion before reading the 3-byte response.
- Verifies the HTU21D CRC-8 byte with polynomial `0x31`.
- Clears the raw status bits before applying the datasheet conversion formulas.
- Clamps relative humidity to `0..10000` (`0.00..100.00 %RH`).

## Runtime Flow

The main measurement loop reads HTU21D through `argisense_read_humidity_sample()`
in `app/src/main.c`.

The read period is controlled by persistent runtime settings, with the default
coming from:

```text
CONFIG_ARGISENSE_HUMIDITY_SENSOR_READ_PERIOD_SECONDS
```

The application caches the most recent humidity sample so the sensor is not
polled more often than needed. Each measurement sample carries:

```c
int32_t humidity_rh_x100;
int32_t ambient_temperature_centi_c;
int32_t humidity_temperature_centi_c;
int32_t humidity_last_error;
bool humidity_valid;
```

`ambient_temperature_centi_c` is the primary firmware field for HTU21D
temperature. `humidity_temperature_centi_c` is kept as a compatibility alias for
older code and tools.

Humidity is auxiliary data. It does not drive either GP8302 4-20 mA output by
default. DAC0 remains mapped to methane and DAC1 remains mapped to pressure.

## RS485 Registers

The register map version is `8`.

Register `2` exposes HTU21D state through these status bits:

| Bit | Meaning |
| --- | --- |
| `bit4` | Latest HTU21D sample is valid |
| `bit5` | Latest HTU21D read returned an error |

HTU21D diagnostics are exposed in the diagnostic block:

| Address | Access | Description |
| --- | --- | --- |
| `80` | R | HTU21D relative humidity, `%RH x100` |
| `81` | R | HTU21D ambient temperature in centi-degrees Celsius |
| `82` | R | Latest HTU21D read result, signed errno-style code |

Useful shell checks:

```text
argisense drivers
argisense ambient
argisense sensors
argisense rs485 80 3
```

## Troubleshooting

- If `argisense drivers` reports the humidity device is not ready, verify that
  `i2c2` is enabled and the node has `status = "okay"`.
- If reads return `-EIO`, check pull-ups, address `0x40`, rail power, and CRC
  integrity on the I2C bus.
- If humidity values stay stale, check the configured
  `humidity_read_period_seconds` setting.
- If `argisense sensors` works but Modbus values do not change, read
  `sample_sequence` registers `16..17` to confirm that a new measurement cycle
  completed.
