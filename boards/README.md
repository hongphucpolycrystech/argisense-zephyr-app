# Boards

Place out-of-tree board definitions here.

This directory is registered through `zephyr/module.yml` as:

```yaml
settings:
  board_root: .
```

Future board ports can be added without changing the repository layout, for example:

```text
boards/stm32h745_custom
```

Current custom boards:

```text
boards/argisense/argisense_mp_u575rg  - methane + pressure product board
boards/argisense/argisense_ph_u575rg  - pH product board skeleton
```

Build the methane + pressure STM32U575RGT6 board with:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg argisense-zephyr-app
```

Both STM32U575 board definitions use a 25 MHz HSE crystal driving PLL1 to produce a 160 MHz system clock.

To build from the internal HSI 16 MHz oscillator instead, use the shared `argisense-u575-hsi` snippet:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi argisense-zephyr-app
```

The pH board is separated at the hardware layer because it has a different PCB/pinout. The current application profile is still methane + pressure; add a pH product profile before building the main application for `argisense_ph_u575rg`.
