# ArgiSense Zephyr Application

Standalone Zephyr 4.4.0 application repository for ArgiSense.

This repository follows the Zephyr application repository pattern used by the official `zephyrproject-rtos/example-application` project. It is intended to live outside the Zephyr source tree and to be stored in its own Git repository.

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

The application starts by logging:

```text
ArgiSense Zephyr 4.4 application started
```
