# Snippets

Place Zephyr snippets here.

This directory is registered through `zephyr/module.yml` as:

```yaml
settings:
  snippet_root: .
```

Available snippets:

```text
argisense-u575rg-hsi
```

Build the STM32U575RGT6 board with the internal HSI 16 MHz oscillator instead of the external 25 MHz HSE crystal:

```bat
py -3.12 -m west build -p always -b argisense_u575rg -S argisense-u575rg-hsi argisense-zephyr-app
```
