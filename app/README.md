# ArgiSense Application Source Layout

This application repository supports multiple product firmware profiles.
Each product has its own board target and its own application entry point.

```text
app/
  common/
    src/                         reusable services shared by product firmware
  products/
    methane_pressure/
      src/                       firmware for argisense_mp_u575rg
    ph/
      src/                       firmware for argisense_ph_u575rg
```

## Product Selection

The top-level `CMakeLists.txt` selects the product source set from the Zephyr
board Kconfig symbol:

| Board | Product source |
| --- | --- |
| `argisense_mp_u575rg` | `app/products/methane_pressure/src` |
| `argisense_ph_u575rg` | `app/products/ph/src` |

Shared service code that is not sensor-profile-specific belongs in
`app/common/src`. Examples include reusable firmware update and USB-C service
code. Product-specific measurement loops, register maps, settings, shell
commands, and calibration policy belong under the matching product directory.

Both product profiles can use the common USB-C service:

- `usbconsole` routes Zephyr console, logs, and shell to USB CDC ACM.
- `usbupdate` exposes a USB composite device with one CDC ACM interface for
  shell/console and one CDC ACM interface for MCUmgr firmware update.

Both product profiles can also use RS485 Modbus for service access. The shared
RS485 DFU engine lives in `app/common/src/argisense_dfu.c`, while each product
owns its own register map and persistent settings namespace.

## Build Shortcuts

```bat
app\compile.bat mp
app\compile.bat ph
app\compile.bat mcuboot mp
app\compile.bat mcuboot ph
app\compile.bat hsi ph
app\compile.bat ph usbconsole
app\compile.bat mcuboot ph usbupdate
```

The full Zephyr board names are still accepted:

```bat
app\compile.bat argisense_mp_u575rg
app\compile.bat argisense_ph_u575rg
```
