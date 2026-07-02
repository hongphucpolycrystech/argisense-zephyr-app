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
boards/argisense/argisense_ph_u575rg  - pH product board skeleton with mapped pH front-end
```

Build the methane + pressure STM32U575RGT6 board with:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg argisense-zephyr-app
```

Build the pH STM32U575RGT6 board skeleton with:

```bat
py -3.12 -m west build -p always -b argisense_ph_u575rg argisense-zephyr-app
```

Both STM32U575 board definitions use a 25 MHz HSE crystal driving PLL1 to produce a 160 MHz system clock.

To build from the internal HSI 16 MHz oscillator instead, use the shared `argisense-u575-hsi` snippet:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi argisense-zephyr-app
```

The pH board is separated at the hardware layer because it has a different
PCB/pinout. `argisense_ph_u575rg` maps the Sensor_Platform_V1 `pH_V1_0`
front-end metadata for ADS124S08, AD5675, LT3582, PT1000, and ISFET/REFET
channel labels. See `docs/firmware-overview.md` for the document index,
`docs/methane-pressure-firmware-overview.md` for methane + pressure firmware,
and `docs/ph-firmware-overview.md` for pH firmware.
