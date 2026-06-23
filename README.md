# ArgiSense Zephyr Application

Standalone Zephyr 4.4.0 application repository for ArgiSense.

This repository follows the Zephyr application repository pattern used by the official `zephyrproject-rtos/example-application` project. It is intended to live outside the Zephyr source tree and to be stored in its own Git repository.

## Firmware overview

The proposed sensor firmware architecture is documented in:

```text
docs/firmware-overview.md
docs/dynament-platinum-methane.md
```

## Repository role

This repository is the west manifest repository and is also configured as a Zephyr module through `zephyr/module.yml`.

The module configuration exposes these roots for future out-of-tree additions:

- `boards/`
- `dts/`
- `snippets/`
- `drivers/`
- `lib/`

This layout supports later additions such as:

- `boards/stm32h745_custom`
- `drivers/sim7600`
- `drivers/mlx90640`
- `lib/mqtt_backend`
- `lib/updatehub_helpers`

## Workspace setup

Workspace location:

```bat
C:\Users\zephyr44_workspace
```

Initialization:

```bat
cd C:\Users\zephyr44_workspace
west init -l argisense-zephyr-app
west update
west zephyr-export
py -3.12 -m pip install -r zephyr\scripts\requirements.txt
```

## Build

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
py -3.12 -m west build -p always -b nucleo_h745zi_q/stm32h745xx/m7 argisense-zephyr-app
```

Zephyr 4.4 requires the fully qualified board target for this board. Use `nucleo_h745zi_q/stm32h745xx/m4` instead if building the Cortex-M4 target.

## Custom STM32U575RGT6 Board

This repository includes out-of-tree board definitions based on `nucleo_u575zi_q`, adapted for STM32U575RGT6:

```text
boards/argisense/argisense_mp_u575rg
boards/argisense/argisense_ph_u575rg
```

`argisense_mp_u575rg` is the methane + pressure board currently used by the application. `argisense_ph_u575rg` is separated for the pH PCB/pinout and is ready for a future pH firmware profile.

The methane + pressure board uses the TE Connectivity `MS580305BA01-00`
(`MS5803-05BA01`) pressure sensor on `spi1`. The sensor `PS` protocol-select
pin must be low for SPI mode and high for I2C mode; this board keeps the
default firmware setting low for SPI.

The methane sensor is a Dynament Platinum Series Hydrocarbon infrared sensor on
`uart4` at 38400 baud. The board DTS describes it as
`argisense,dynament-platinum-hydrocarbon`, using the Dynament/Premier
live-data-simple request from AN0007.

The humidity sensor is an HTU21D on `i2c2` at address `0x40`. Zephyr 4.4 uses
the SHT21-compatible humidity/temperature driver path for this part, so the DTS
node uses compatible `sensirion,sht21`.

Both boards use a 25 MHz HSE crystal and PLL1 to generate a 160 MHz system clock.

Recommended product split:

```text
argisense_mp_u575rg  -> methane + pressure firmware profile
argisense_ph_u575rg  -> pH firmware profile
```

Keep shared services such as MCUboot, RS485 framing, calibration storage, power sequencing, and 4-20 mA scaling in common code. Keep sensor-specific measurement logic and DAC mapping in product-specific files or snippets.

Build the methane + pressure application with:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
py -3.12 -m west build -p always -b argisense_mp_u575rg argisense-zephyr-app
```

To use the internal HSI 16 MHz oscillator instead of the external 25 MHz HSE crystal:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi argisense-zephyr-app
```

You can also build from the helper script:

```bat
argisense-zephyr-app\app\compile.bat
argisense-zephyr-app\app\compile.bat hsi
```

## MCUboot build

The custom `argisense_mp_u575rg` board DTS already reserves flash partitions for MCUboot:

- `mcuboot`
- `image-0`
- `image-1`
- `storage`

This repository enables MCUboot through Zephyr sysbuild:

```text
sysbuild.conf
sysbuild/argisense-zephyr-app.conf
sysbuild/mcuboot.conf
```

The selected boot mode is `SWAP_USING_OFFSET`. It uses the primary and secondary image slots without requiring a `scratch_partition`, and still supports MCUboot test/confirm rollback behavior.

Build the application together with MCUboot:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
set ARGISENSE_APP_ROOT=C:\Users\zephyr44_workspace\argisense-zephyr-app
py -3.12 -m west build -p always --sysbuild -b argisense_mp_u575rg "%ARGISENSE_APP_ROOT%" -- "-DBOARD_ROOT=%ARGISENSE_APP_ROOT%" "-DDTS_ROOT=%ARGISENSE_APP_ROOT%" "-DSNIPPET_ROOT=%ARGISENSE_APP_ROOT%"
```

Or use the helper script:

```bat
argisense-zephyr-app\app\compile.bat mcuboot
argisense-zephyr-app\app\compile.bat mcuboot hsi
```

Typical sysbuild outputs are:

```text
build\mcuboot\zephyr\zephyr.hex
build\argisense-zephyr-app\zephyr\zephyr.signed.bin
build\argisense-zephyr-app\zephyr\zephyr.signed.hex
```

The application calls `boot_write_img_confirmed()` after the basic board bring-up checks pass. That means a test update is only confirmed after GPIO power setup and both GP8302 I2C mappings are valid. As the firmware grows, move this confirmation later so it happens after methane, pressure, RS485, and DAC output self-tests also pass.

For initial bring-up, Zephyr/MCUboot may use the default development signing key. Do not use that key for production. Store production signing keys outside this repository, then configure the key path through the build environment or a private, ignored config fragment.

## Power Management

The `argisense_mp_u575rg` application is configured for low-power sensor duty cycling:

- Zephyr system PM is enabled with system-managed device PM and tickless idle.
- Switched external rails default to off at boot.
- `+3V3_PRE` is kept enabled by default while idle because the Dynament Platinum methane sensor is intended for continuous powered operation.
- `+3V3_PRE`, analog power, shared DAC/current-loop power, RS485 termination, pressure `PS`, and pressure chip select are controlled from `zephyr,user` GPIOs in the board DTS.
- The firmware model exposes two 4-20 mA GP8302 outputs: DAC0 for methane on `i2c1`, and DAC1 for pressure on `i2c2`.
- HTU21D humidity and ambient temperature sensing is mapped on `i2c2` at `0x40`.
- The default methane DAC span is 0 ppm to 1000000 ppm to match a 0-100% volume Dynament methane sensor. Use 50000 ppm for a 0-5% volume ordered variant.
- The default pressure DAC span is 0 Pa to 500000 Pa to match the selected 5 bar absolute pressure sensor.
- The application periodically powers the sensor rails, waits for settling, keeps a short measurement window, powers external rails off again, then sleeps so STM32U575 can enter low-power idle states.

Duty-cycle settings are exposed through Kconfig:

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

The application starts by logging:

```text
ArgiSense Zephyr 4.4 application started
```
