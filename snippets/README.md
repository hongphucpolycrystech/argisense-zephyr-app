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
argisense-usb-console-test
```

Build an STM32U575RGT6 ArgiSense board with the internal HSI 16 MHz oscillator instead of the external 25 MHz HSE crystal:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi argisense-zephyr-app
```

Build the ArgiSense application with the USB-C connector as the Zephyr shell
while keeping console/log output on USART1:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-usb-console argisense-zephyr-app
```

The USB console snippet enables STM32U575 USB FS on PA11/PA12 and routes
`zephyr,shell-uart` to the CDC ACM UART. It leaves `zephyr,console` and
`zephyr,log-uart` on USART1 so USB bring-up logs can still be captured when the
PC does not enumerate the board.

The HSI and USB console snippets can be combined:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi -S argisense-usb-console argisense-zephyr-app
```

To build Zephyr's upstream USB console sample with this out-of-tree board, pass
the board root and use the test overlay. The sample requires
`zephyr,console` to be a CDC ACM UART, so it cannot use the application
`argisense-usb-console` snippet:

```powershell
cd C:\Users\zephyr44_workspace\zephyr\samples\subsys\usb\console
py -3.12 -m west build -b argisense_mp_u575rg -p always -- "-DBOARD_ROOT=C:\Users\zephyr44_workspace\argisense-zephyr-app" "-DDTS_ROOT=C:\Users\zephyr44_workspace\argisense-zephyr-app" "-DDTC_OVERLAY_FILE=C:/Users/zephyr44_workspace/argisense-zephyr-app/snippets/argisense-usb-console-test/argisense_usb_console_test.overlay"
```
