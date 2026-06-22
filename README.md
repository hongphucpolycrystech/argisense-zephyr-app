# ArgiSense Zephyr Application

Standalone Zephyr 4.4.0 application repository for ArgiSense.

This repository follows the Zephyr application repository pattern used by the official `zephyrproject-rtos/example-application` project. It is intended to live outside the Zephyr source tree and to be stored in its own Git repository.

## Firmware overview

The proposed sensor firmware architecture is documented in:

```text
docs/firmware-overview.md
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

This repository includes an out-of-tree board definition based on `nucleo_u575zi_q`, adapted for STM32U575RGT6:

```text
boards/argisense/argisense_u575rg
```

The custom board uses a 25 MHz HSE crystal and PLL1 to generate a 160 MHz system clock.

Build it with:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
py -3.12 -m west build -p always -b argisense_u575rg argisense-zephyr-app
```

To use the internal HSI 16 MHz oscillator instead of the external 25 MHz HSE crystal:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
py -3.12 -m west build -p always -b argisense_u575rg -S argisense-u575rg-hsi argisense-zephyr-app
```

You can also build from the helper script:

```bat
argisense-zephyr-app\app\compile.bat
argisense-zephyr-app\app\compile.bat hsi
```

## Power Management

The `argisense_u575rg` application is configured for low-power sensor duty cycling:

- Zephyr system PM is enabled with system-managed device PM and tickless idle.
- External rails default to off at boot.
- `+3V3_PRE`, analog power, DAC power, RS485 termination, pressure `PS`, and pressure chip select are controlled from `zephyr,user` GPIOs in the board DTS.
- The application periodically powers the sensor rails, waits for settling, keeps a short measurement window, powers external rails off again, then sleeps so STM32U575 can enter low-power idle states.

Duty-cycle settings are exposed through Kconfig:

```text
CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS
CONFIG_ARGISENSE_PRE_RAIL_SETTLE_MS
CONFIG_ARGISENSE_ANALOG_RAIL_SETTLE_MS
CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS
CONFIG_ARGISENSE_PRESSURE_PS_ACTIVE
CONFIG_ARGISENSE_DAC_POWER_DURING_MEASUREMENT
```

The application starts by logging:

```text
ArgiSense Zephyr 4.4 application started
```
