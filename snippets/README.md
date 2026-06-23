# Snippets

Place Zephyr snippets here.

This directory is registered through `zephyr/module.yml` as:

```yaml
settings:
  snippet_root: .
```

Available snippets:

```text
argisense-u575-hsi
```

Build an STM32U575RGT6 ArgiSense board with the internal HSI 16 MHz oscillator instead of the external 25 MHz HSE crystal:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi argisense-zephyr-app
```
