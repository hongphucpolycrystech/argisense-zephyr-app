# ArgiSense Zephyr Application

Standalone Zephyr 4.4.0 application repository for ArgiSense.

This repository follows the Zephyr application repository pattern used by the official `zephyrproject-rtos/example-application` project. It is intended to live outside the Zephyr source tree and to be stored in its own Git repository.

## Firmware overview

Firmware architecture and sensor integration notes are documented in:

```text
docs/firmware-overview.md
docs/firmware-overview.docx
docs/firmware-overview.pdf
docs/dynament-platinum-methane.md
docs/htu21d-humidity.md
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

This repository includes out-of-tree board definitions based on `nucleo_u575zi_q`, adapted for STM32U575RGT6:

```text
boards/argisense/argisense_mp_u575rg
boards/argisense/argisense_ph_u575rg
```

`argisense_mp_u575rg` is the methane + pressure board currently used by the application. `argisense_ph_u575rg` is separated for the pH PCB/pinout and is ready for a future pH firmware profile.

The methane + pressure board uses the TE Connectivity `MS580305BA01-00`
(`MS5803-05BA01`) pressure sensor on `spi1`. The sensor `PS` protocol-select
pin must be low for SPI mode and high for I2C mode; this board keeps the
default firmware setting low for SPI.

The methane sensor is a Dynament Platinum Series Hydrocarbon infrared sensor on
`uart4` at 38400 baud. The board DTS describes it as
`argisense,dynament-platinum-hydrocarbon`, using the Dynament/Premier
live-data-simple request from AN0007.

The humidity sensor is an HTU21D on `i2c2` at address `0x40`. This repository
implements it as the out-of-tree `argisense,htu21d` Zephyr Sensor API driver.

The external EEPROM is a Microchip `AT24C512C-SSHD-T` on `i2c1` at address
`0x50`. It is mapped with Zephyr's standard `atmel,at24` EEPROM driver as a
64 KiB device with 128-byte pages and 16-bit memory addressing.

Both boards use a 25 MHz HSE crystal and PLL1 to generate a 160 MHz system clock.

Recommended product split:

```text
argisense_mp_u575rg  -> methane + pressure firmware profile
argisense_ph_u575rg  -> pH firmware profile
```

Keep shared services such as MCUboot, RS485 framing, calibration storage, power sequencing, and 4-20 mA scaling in common code. Keep sensor-specific measurement logic and DAC mapping in product-specific files or snippets.

Build the methane + pressure application with:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
py -3.12 -m west build -p always -b argisense_mp_u575rg argisense-zephyr-app
```

To use the internal HSI 16 MHz oscillator instead of the external 25 MHz HSE crystal:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-u575-hsi argisense-zephyr-app
```

You can also build from the helper script:

```bat
argisense-zephyr-app\app\compile.bat
argisense-zephyr-app\app\compile.bat hsi
argisense-zephyr-app\app\compile.bat usbconsole
argisense-zephyr-app\app\compile.bat hsi usbconsole
argisense-zephyr-app\app\compile.bat hsi flash
argisense-zephyr-app\app\compile.bat flash-only
argisense-zephyr-app\app\compile.bat flash-all-only
```

To use the board USB-C connector as the Zephyr shell console, build with the
`argisense-usb-console` snippet:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
py -3.12 -m west build -p always -b argisense_mp_u575rg -S argisense-usb-console argisense-zephyr-app
```

Or use the helper script:

```bat
argisense-zephyr-app\app\compile.bat usbconsole
argisense-zephyr-app\app\compile.bat mcuboot usbconsole flash
argisense-zephyr-app\app\compile.bat mcuboot hsi usbconsole flash
```

The USB console appears on the host PC as a CDC ACM virtual COM port after the
application boots and the USB device enumerates. Keep the USART1 debug header
available during bring-up because MCUboot and very early boot logs are still
best captured over UART. The `usbconsole` snippet is a debug profile: it uses
the STM32U575 USB FS peripheral and disables Zephyr PM/deep idle so the PC does
not lose the CDC ACM device while the firmware is sleeping.

Set the firmware image version with the helper script:

```bat
argisense-zephyr-app\app\compile.bat version 1.2.0+7
argisense-zephyr-app\app\compile.bat mcuboot hsi version 1.2.0+7
argisense-zephyr-app\app\compile.bat mcuboot hsi version 1.2.0+7 flash
```

The version format is `major.minor.patch+build`. The script writes the root
`VERSION` file before building so MCUboot/imgtool can place the version in the
signed image header. At runtime, the firmware reads the version back from the
MCUboot image header in flash instead of printing an application-created string.

Add the `flash` argument to program only the signed application image after a
successful build. In a sysbuild MCUboot build, this uses
`west flash --no-rebuild -d build --domain argisense-zephyr-app`, so MCUboot is
not reflashed every time. Use `flash-only` when the project has already been
built and you only want to program the existing signed application image again.
The target board must be connected through the configured Zephyr runner, such as
ST-LINK/OpenOCD or STM32CubeProgrammer.

Use `flash-all` or `flash-all-only` only when MCUboot itself changed or the
board is blank:

```bat
argisense-zephyr-app\app\compile.bat mcuboot hsi version 1.2.0+7 flash-all
argisense-zephyr-app\app\compile.bat flash-all-only
```

When more than one ST-LINK is connected, select the intended debugger before
flashing. The helper script defaults to the ArgiSense ST-LINK serial
`003600253234510237333934`, so a normal flash-only command should select that
probe automatically:

```bat
argisense-zephyr-app\app\compile.bat flash-only
```

You can also pass the ST-LINK serial directly on the command line:

```bat
argisense-zephyr-app\app\compile.bat stlink 003600253234510237333934 flash-only
```

PowerShell environment-variable syntax:

```powershell
$env:FLASH_DEV_ID="003600253234510237333934"
argisense-zephyr-app\app\compile.bat flash-only
```

cmd.exe environment-variable syntax:

```bat
set FLASH_DEV_ID=003600253234510237333934
argisense-zephyr-app\app\compile.bat flash-only
```

`STLINK_SN` can be used instead of `FLASH_DEV_ID`. In PowerShell, do not use
`set FLASH_DEV_ID=...` for this workflow; that does not export an environment
variable to the `.bat` process.

When a `version` argument matches the existing root `VERSION` file, the helper
script does not rewrite the file. This avoids unnecessary rebuilds caused only
by touching the version metadata. The command still invokes `west build`; use
`flash-only` when you want to skip build completely.

## MCUboot build

The custom `argisense_mp_u575rg` board DTS already reserves flash partitions for MCUboot:

- `mcuboot`
- `image-0`
- `image-1`
- `storage`

This repository enables MCUboot through Zephyr sysbuild:

```text
sysbuild.conf
sysbuild/argisense-zephyr-app.conf
sysbuild/mcuboot.conf
```

The selected boot mode is `SWAP_USING_OFFSET`. It uses the primary and secondary image slots without requiring a `scratch_partition`, and still supports MCUboot test/confirm rollback behavior.

Build the application together with MCUboot:

```bat
set ZEPHYR_SDK_INSTALL_DIR=C:\Users\USER\zephyr-sdk-1.0.1
set ARGISENSE_APP_ROOT=C:\Users\zephyr44_workspace\argisense-zephyr-app
py -3.12 -m west build -p always --sysbuild -b argisense_mp_u575rg "%ARGISENSE_APP_ROOT%" -- "-DBOARD_ROOT=%ARGISENSE_APP_ROOT%" "-DDTS_ROOT=%ARGISENSE_APP_ROOT%" "-DSNIPPET_ROOT=%ARGISENSE_APP_ROOT%"
```

Or use the helper script:

```bat
argisense-zephyr-app\app\compile.bat mcuboot
argisense-zephyr-app\app\compile.bat mcuboot hsi
argisense-zephyr-app\app\compile.bat mcuboot usbconsole
argisense-zephyr-app\app\compile.bat mcuboot hsi usbconsole
argisense-zephyr-app\app\compile.bat mcuboot hsi flash
argisense-zephyr-app\app\compile.bat flash-only
argisense-zephyr-app\app\compile.bat flash-all-only
```

Typical sysbuild outputs are:

```text
build\mcuboot\zephyr\zephyr.hex
build\argisense-zephyr-app\zephyr\zephyr.signed.bin
build\argisense-zephyr-app\zephyr\zephyr.signed.hex
```

The application calls `boot_write_img_confirmed()` after the basic board bring-up checks pass. That means a test update is only confirmed after GPIO power setup and both GP8302 I2C mappings are valid. As the firmware grows, move this confirmation later so it happens after methane, pressure, RS485, and DAC output self-tests also pass.

For initial bring-up, Zephyr/MCUboot may use the default development signing key. Do not use that key for production. Store production signing keys outside this repository, then configure the key path through the build environment or a private, ignored config fragment.

## Power Management

The `argisense_mp_u575rg` application is configured for low-power sensor duty cycling:

- Zephyr system PM is enabled with system-managed device PM and tickless idle.
- Switched external rails default to off at boot.
- `+3V3_PRE` is kept enabled by default while idle because the Dynament Platinum methane sensor is intended for continuous powered operation.
- `+3V3_PRE`, analog power, shared DAC/current-loop power, RS485 termination, pressure `PS`, and pressure chip select are controlled from `zephyr,user` GPIOs in the board DTS.
- The firmware model exposes two 4-20 mA GP8302 outputs: DAC0 for methane on `i2c1`, and DAC1 for pressure on `i2c2`.
- HTU21D humidity and ambient temperature sensing is mapped on `i2c2` at `0x40`.
- The default methane DAC span is 0 ppm to 1000000 ppm to match a 0-100% volume Dynament methane sensor. Use 50000 ppm for a 0-5% volume ordered variant.
- The default pressure DAC span is 0 Pa to 500000 Pa to match the selected 5 bar absolute pressure sensor.
- The application periodically powers the sensor rails, waits for settling, keeps a short measurement window, powers external rails off again, then sleeps so STM32U575 can enter low-power idle states.

Duty-cycle settings are exposed through Kconfig:

```text
CONFIG_ARGISENSE_MEASUREMENT_PERIOD_SECONDS
CONFIG_ARGISENSE_PRE_RAIL_ALWAYS_ON
CONFIG_ARGISENSE_PRE_RAIL_SETTLE_MS
CONFIG_ARGISENSE_ANALOG_RAIL_SETTLE_MS
CONFIG_ARGISENSE_MEASUREMENT_WINDOW_MS
CONFIG_ARGISENSE_PRESSURE_PS_ACTIVE
CONFIG_ARGISENSE_DAC_POWER_DURING_MEASUREMENT
CONFIG_ARGISENSE_DAC_POWER_IDLE
CONFIG_ARGISENSE_DAC_MIN_CURRENT_UA
CONFIG_ARGISENSE_DAC_MAX_CURRENT_UA
CONFIG_ARGISENSE_DAC_FAULT_CURRENT_UA
CONFIG_ARGISENSE_METHANE_DAC_RANGE_LOW_PPM
CONFIG_ARGISENSE_METHANE_DAC_RANGE_HIGH_PPM
CONFIG_ARGISENSE_METHANE_SENSOR_WARMUP_SECONDS
CONFIG_ARGISENSE_METHANE_SENSOR_READ_PERIOD_MS
CONFIG_ARGISENSE_HUMIDITY_SENSOR_READ_PERIOD_SECONDS
CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_LOW_PA
CONFIG_ARGISENSE_PRESSURE_DAC_RANGE_HIGH_PA
```

## Persistent Settings

The `argisense_mp_u575rg` flash layout reserves the final 64 KiB as
`storage_partition`. The board selects this partition as
`zephyr,settings-partition`, and the application uses Zephyr Settings with the
NVS backend to persist runtime configuration under the key:

```text
argisense/config
```

The stored record currently contains:

- Measurement period and measurement window.
- Methane warm-up, methane polling period, and HTU21D read period.
- Methane and pressure DAC scaling ranges.
- DAC 4-20 mA current limits and fault current.
- Methane and pressure calibration offsets.
- DAC channel trim placeholders.
- RS485 baudrate, Modbus address, and software-controlled termination.

On a fresh board, the firmware loads compile-time defaults from Kconfig, writes
one default `argisense/config` record into NVS, then uses that runtime
configuration. On later boots, the firmware reads the record from flash and uses
it for the measurement period and DAC scaling. If the stored record has an
unsupported schema or invalid values, the firmware ignores it and falls back to
defaults.

The storage partition is intentionally separate from MCUboot image slots:

```text
0x08000000 - 0x0800FFFF  MCUboot      64 KiB
0x08010000 - 0x0807FFFF  image-0     448 KiB
0x08080000 - 0x080EFFFF  image-1     448 KiB
0x080F0000 - 0x080FFFFF  storage      64 KiB
```

## GP8302 Current-Loop DAC

The two GP8302 devices are implemented as out-of-tree Zephyr DAC drivers under
`drivers/dac`. The driver binds to `compatible = "argisense,gp8302"` and exposes
one DAC channel through the standard Zephyr DAC API:

```c
dac_channel_setup(dev, &(struct dac_channel_cfg){ .channel_id = 0, .resolution = 12 });
dac_write_value(dev, 0, raw);
```

The GP8302 output command is a 12-bit raw value:

```text
raw = current_uA * 0x0FFF / max-current-microamp
```

With the board default `max-current-microamp = <25000>`, typical loop values are
approximately:

| Current | Raw code |
| --- | --- |
| 4 mA | `0x28F` |
| 20 mA | `0xCCC` |
| 25 mA | `0xFFF` |

At runtime, `main.c` maps methane and pressure process values to current-loop
commands, converts those currents to GP8302 raw codes, and writes:

- DAC0 on `i2c1`, address `0x58`, methane output.
- DAC1 on `i2c2`, address `0x58`, pressure output.

`CONFIG_ARGISENSE_DAC_POWER_IDLE=y` keeps the GP8302 rail enabled after the
measurement window so the 4-20 mA outputs remain active. Disable it only for a
product mode that intentionally duty-cycles analog output.

## Shell Diagnostics

The firmware enables Zephyr shell on the selected console backend. The default
backend is USART1. Build with `argisense-usb-console` or the helper script
`usbconsole` option to move the shell to the USB-C CDC ACM virtual COM port.
Useful diagnostic commands:

```text
argisense drivers
argisense eeprom
argisense eeprom 0 32
argisense sensors
argisense rs485
argisense rs485 30 3
argisense settings
```

- `argisense drivers` prints driver and bus readiness for methane, pressure,
  humidity, external EEPROM, DAC0, DAC1, and RS485.
- `argisense eeprom` reads the first 16 bytes from the AT24C512C external
  EEPROM.
- `argisense eeprom <offset> <length>` reads up to 64 bytes from the external
  EEPROM for bring-up diagnostics.
- `argisense sensors` performs a one-shot read of Dynament methane, MS5803
  pressure/temperature, and HTU21D humidity/temperature.
- `argisense rs485` dumps live/control registers `0..33`, configuration
  registers `40..59`, and diagnostics `70..82`.
- `argisense rs485 <start> <count>` dumps a custom holding-register range.
- `argisense settings` prints the active runtime settings loaded from NVS or
  defaults.

`argisense sensors` temporarily powers the measurement rails before reading, so
the MS5803 pressure sensor can be checked from the shell even while the product
is otherwise in idle. The command returns the rails to the normal idle policy
after the one-shot read.

## RS485 Modbus RTU

The methane + pressure board exposes RS485 on `usart2` with hardware DE on
`PA1`. The board DTS defines an enabled `zephyr,modbus-serial` child node named
`rs485_modbus`, and the application starts a Zephyr Modbus RTU server from the
persistent runtime settings.

The Modbus transport and register ownership are intentionally separated:

- `app/src/argisense_rs485.c` starts the Zephyr Modbus RTU server and forwards
  holding-register reads/writes to the register module.
- `app/src/argisense_registers.c` owns the holding-register map, live
  measurement snapshot, validation, and persistent settings writes.
- `app/src/argisense_shell.c` uses the same register module for the
  `argisense rs485` diagnostic command, so shell output and Modbus reads share
  one source of truth.

Default serial settings:

- Unit ID: `1`
- Baudrate: `115200`
- Parity: none
- Stop bits: Modbus-compliant setting selected by Zephyr for RTU/no parity

Holding register map v3, high word first for 32-bit values. Signed values and
error codes use two's-complement representation.

| Address | Access | Description |
| --- | --- | --- |
| `0` | R | Device ID, fixed `0xA651` |
| `1` | R | Register map version, currently `3` |
| `2` | R | Status flags: bit0 methane valid, bit1 pressure valid, bit2 sample ready, bit3 RS485 termination enabled, bit4 humidity valid, bit5 humidity error |
| `3` | R/W | Modbus unit address, `1..247`; takes effect after reboot |
| `4` | R | RS485 baudrate high word |
| `5` | R | RS485 baudrate low word |
| `6` | R/W | Measurement period in seconds, `1..65535`; takes effect immediately for the next sleep cycle |
| `7` | R/W | Measurement window in milliseconds, `1..60000` |
| `8` | R/W | Baud preset: `0=9600`, `1=19200`, `2=38400`, `3=57600`, `4=115200`; takes effect after reboot |
| `9` | R/W | RS485 termination control: `0=off`, `1=on` |
| `10..11` | R | Methane concentration, signed `ppm x 100` |
| `12..13` | R | Pressure, signed pascals |
| `14` | R | DAC0 methane loop current in microamps |
| `15` | R | DAC1 pressure loop current in microamps |
| `16..17` | R | Sample sequence counter |
| `18..19` | R | Sample uptime in seconds |
| `20` | R | Firmware major version from MCUboot image header |
| `21` | R | Firmware minor version from MCUboot image header |
| `22` | R | Firmware patch version from MCUboot image header |
| `23..24` | R | Firmware build number from MCUboot image header |
| `25` | R | Boot flags: bit0 MCUboot enabled, bit1 image confirmed, bit2 image header valid, bit3 reboot required |
| `26` | R | Active MCUboot slot, or `0xFFFF` if unknown |
| `27` | R | Reboot-required flag, set after address/baud/default reset changes |
| `28` | R | Last command value written to register `33` |
| `29` | R | Last command result as signed errno-style code |
| `30` | R/W | DAC normal minimum current in microamps |
| `31` | R/W | DAC normal maximum current in microamps |
| `32` | R/W | DAC fault current in microamps |
| `33` | W | Command register: `0xA551=reboot`, `0xA552=reset settings defaults`, `0xA553=confirm MCUboot image` |
| `40..41` | R/W | Methane DAC low range, signed ppm |
| `42..43` | R/W | Methane DAC high range, signed ppm |
| `44..45` | R/W | Pressure DAC low range, signed Pa |
| `46..47` | R/W | Pressure DAC high range, signed Pa |
| `48..49` | R/W | Methane zero offset, signed `ppm x 100` |
| `50..51` | R/W | Pressure offset, signed Pa |
| `52..53` | R/W | DAC0 4 mA trim placeholder, signed microamps |
| `54..55` | R/W | DAC0 20 mA trim placeholder, signed microamps |
| `56..57` | R/W | DAC1 4 mA trim placeholder, signed microamps |
| `58..59` | R/W | DAC1 20 mA trim placeholder, signed microamps |
| `70` | R | Dynament methane status flags from the latest parsed frame |
| `71` | R | Dynament methane protocol version |
| `72` | R | Latest methane read result, signed errno-style code |
| `73` | R | Latest pressure read result, signed errno-style code |
| `74` | R | MS5803 temperature in centi-degrees Celsius |
| `75..76` | R | MS5803 raw D1 pressure ADC conversion |
| `77..78` | R | MS5803 raw D2 temperature ADC conversion |
| `79` | R | MS5803 PROM CRC, high byte read CRC and low byte calculated CRC |
| `80` | R | HTU21D relative humidity, `%RH x100` |
| `81` | R | HTU21D ambient temperature in centi-degrees Celsius |
| `82` | R | Latest HTU21D read result, signed errno-style code |

Writable registers are stored through Zephyr Settings/NVS in the
`storage_partition`. Unsupported addresses or invalid values return a Modbus
exception.

Changing the Modbus unit address or baud preset updates NVS immediately, but the
running Modbus server continues on the old address/baud until reboot. Read
register `27` or boot flag bit3 to know when a reboot is required.

For coherent multi-word values, read both 16-bit words in one Modbus request.
The `sample_sequence` registers can also be read before and after a larger block
to detect whether a measurement update occurred while the master was polling.

The application starts by logging:

```text
ArgiSense Zephyr 4.4 application started
```
