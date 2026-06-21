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
boards/argisense/argisense_u575rg
```

Build the STM32U575RGT6 custom board with:

```bat
py -3.12 -m west build -p always -b argisense_u575rg argisense-zephyr-app
```
