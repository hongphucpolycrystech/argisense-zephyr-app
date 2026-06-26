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
argisense-usb-console
argisense-usb-update
argisense-usb-console-test
```

Build an STM32U575RGT6 ArgiSense board with the internal HSI 16 MHz oscillator instead of the external 25 MHz HSE crystal:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi argisense-zephyr-app
```

Build the ArgiSense application with the USB-C connector as the Zephyr console,
log output, and shell:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-usb-console argisense-zephyr-app
```

The USB console snippet enables STM32U575 USB FS on PA11/PA12 and routes
`zephyr,console` and `zephyr,shell-uart` to one CDC ACM UART.

Build the ArgiSense application with USB-C console plus a second CDC ACM port
for MCUmgr firmware update:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-usb-update argisense-zephyr-app
```

The USB update snippet exposes:

```text
CDC ACM 0: Zephyr console, log output, and shell
CDC ACM 1: MCUmgr SMP firmware update transport
```

The HSI and USB snippets can be combined:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi -S argisense-usb-console argisense-zephyr-app
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi -S argisense-usb-update argisense-zephyr-app
```

To build Zephyr's upstream USB console sample with this out-of-tree board, pass
the board root and either use the application `argisense-usb-console` snippet
or the minimal test overlay below. The test overlay is kept so the upstream
sample can be exercised without pulling in the application's extra USB debug
configuration:

```powershell
cd C:\Users\zephyr44_workspace\zephyr\samples\subsys\usb\console
py -3.12 -m west build -b argisense_mp_u575rg -p always -- "-DBOARD_ROOT=C:\Users\zephyr44_workspace\argisense-zephyr-app" "-DDTS_ROOT=C:\Users\zephyr44_workspace\argisense-zephyr-app" "-DDTC_OVERLAY_FILE=C:/Users/zephyr44_workspace/argisense-zephyr-app/snippets/argisense-usb-console-test/argisense_usb_console_test.overlay"
```
