# ArgiSense Sensor IO Mapping

This document records the firmware-facing IO mapping for the two ArgiSense
sensor products:

- `argisense_mp_u575rg`: methane + pressure sensor firmware.
- `argisense_ph_u575rg`: pH sensor firmware.

The mapping is based on the current board DTS files and the reviewed
`Sensor_Platform_V1 (1).pdf` schematic. Conflicts and open hardware items are
listed at the end so PCB routing and firmware changes can be tracked together.

## Shared Service IO

Both products keep the service/debug pins consistent where possible.

| Function | DTS node/property | Firmware pin mapping | Notes |
| --- | --- | --- | --- |
| Debug console | `usart1` | `PA9` TX, `PA10` RX | Default console and shell unless a USB snippet overrides it. |
| RS485 Modbus/DFU | `usart2` | `PA2` TX, `PA3` RX, `PA1` DE | `de-enable` is enabled in DTS. |
| RS485 termination | `rs485-termination-gpios` | `PB0` | Runtime setting controls this GPIO. |
| USB-C device | `otgfs` through USB snippets | `PA12` DP, `PA11` DM | Used for shell/console and MCUmgr USB update profiles. |
| SWD debug | hardware debug | `PA13` SWDIO, `PA14` SWCLK | Reserved for debugger/programmer. |
| User LED | `led0` | `PB7` | Board status LED. |
| User button | `sw0` | `PC13` | GPIO key input. |
| Flash storage | `storage_partition` | internal flash offset `0x000f0000` | Zephyr Settings/NVS backend. |

## Methane + Pressure Board

Board target:

```text
argisense_mp_u575rg
```

DTS:

```text
boards/argisense/argisense_mp_u575rg/argisense_mp_u575rg.dts
```

### Methane Sensor

| Signal | Schematic net | DTS node/property | Firmware mapping | Status |
| --- | --- | --- | --- | --- |
| Dynament UART TX from MCU | `METH_UART_TXD` | `uart4` | `PC10 / UART4_TX` | Firmware-valid mapping. |
| Dynament UART RX to MCU | `METH_UART_RXD` | `uart4` | `PC11 / UART4_RX` | Firmware-valid mapping. |
| Methane connector pins | `SENSOR_COMM1..4` | none | connector-side nets | No separate DTS mapping needed when populated as the UART path. |
| Methane power rail | `+3V3_PRE` | `pre-power-gpios` | `PC8` | Kept on by default for Dynament continuous operation. |

Conflict:

The reviewed schematic shows `METH_UART_TXD` on `PC8` and `METH_UART_RXD` on
`PC7`. The STM32U575RGT6 pinctrl data used by Zephyr does not provide UART
TX/RX alternate functions on `PC8`/`PC7`. The current firmware therefore keeps
the Dynament sensor on `UART4 PC10/PC11`. The PCB should route the methane UART
to `PC10/PC11`, or another valid UART TX/RX pair, before relying on the DTS on
real hardware.

### Pressure Sensor

| Signal | Schematic net | DTS node/property | Firmware mapping | Status |
| --- | --- | --- | --- | --- |
| SPI chip select | `PRES_SEN_CSB` | `spi1 cs-gpios`, `pressure-cs-gpios` | `PA4` active-low | Mapped. |
| SPI clock | `PRES_SEN_SCLK` | `spi1` | `PA5 / SPI1_SCK` | Mapped. |
| SPI MISO | `PRES_SEN_SDO` | `spi1` | `PA6 / SPI1_MISO` | Mapped. |
| SPI MOSI | `PRES_SEN_SDI/SDA` | `spi1` | `PA7 / SPI1_MOSI` | Mapped. |
| Pressure power/shutdown | `PRES_SEN_PS` | `pressure-ps-gpios` | `PB1` | Logical active state is handled in firmware. |
| Pressure device | MS580305BA01-00 | `pressure_ms5803` | SPI device CS0 | Driver implemented in `drivers/sensor/ms5803_05ba`. |

### Shared Sensors And Outputs

| Function | Schematic net | DTS node/property | Firmware mapping | Status |
| --- | --- | --- | --- | --- |
| GP8302 DAC0 for methane | `O_D_UC_I2C1_SCL/SDA` | `current-loop-dac0`, `i2c1` | `PB8/PB9`, address `0x58` | Mapped. |
| GP8302 DAC1 for pressure | `O_D_UC_I2C2_SCL/SDA` | `current-loop-dac1`, `i2c2` | `PB10/PB14`, address `0x58` | Mapped. |
| DAC boost enable | `O_D_UC_PWR_DAC` | `dac-power-gpios` | `PC6` | Mapped. |
| DAC alarm | `O_D_DAC_ALARM` / `O_D_DAC_ALARM1` | `dac-alarm-gpios` | `PC7` | One alarm GPIO is mapped. |
| HTU21D humidity/temp | `MCU_I2C2_SCL/SDA` | `humidity-sensor`, `i2c2` | `PB10/PB14`, address `0x40` | Mapped. |
| AT24C512C EEPROM | `MCU_I2C1_SCL/SDA` | `eeprom0`, `i2c1` | `PB8/PB9`, address `0x50` | Mapped. |
| Analog rail enable | `U_O_D_ANA_PWR_EN` | `analog-power-gpios` | `PC9` | Mapped. |
| MCU analog monitor | `MCU_ADC3_INP0` equivalent | `thermistor-adc`, `adc1` | `PC0 / ADC1_IN1` | Mapped as a local analog monitor. |

Open item:

The schematic exposes a second DAC alarm net (`O_D_DAC_ALARM2`) for the second
GP8302 output path. The methane + pressure DTS currently maps only
`dac-alarm-gpios`. If both GP8302 alarm pins must be monitored independently,
add a `dac1-alarm-gpios` property and update the methane + pressure power or
diagnostic code to read it.

## pH Board

Board target:

```text
argisense_ph_u575rg
```

DTS:

```text
boards/argisense/argisense_ph_u575rg/argisense_ph_u575rg.dts
```

### pH ADC: ADS124S08

| Signal | Schematic net | DTS node/property | Firmware mapping | Status |
| --- | --- | --- | --- | --- |
| SPI chip select | `ADC_SPI_CS` | `spi1 cs-gpios` | `PA4` active-low | Mapped. |
| SPI clock | `ADC_SPI_SCLK` | `spi1` | `PA5 / SPI1_SCK` | Mapped. |
| SPI DOUT from ADC | `ADC_SPI_DOUT` | `spi1` | `PA6 / SPI1_MISO` | Mapped. |
| SPI DIN to ADC | `ADC_SPI_DIN` | `spi1` | `PA7 / SPI1_MOSI` | Mapped. |
| ADC reset | `ADC_RESET` | `reset-gpios` | `PB12` active-low | Mapped. |
| ADC start/sync | `ADC_START` | `start-sync-gpios` | `PB13` active-high | Mapped. |
| ADC data ready | `ADC_SRDY` | `drdy-gpios` | `PB15` active-low | Mapped. |
| PT1000 input | `PT1000_P/N` | ADS124S08 inputs | `AIN2/AIN3` | Mapped in ADC channel metadata. |

### pH Bias DAC: AD5675

| Signal | Schematic net | DTS node/property | Firmware mapping | Status |
| --- | --- | --- | --- | --- |
| DAC I2C clock | `DAC_I2C_SCL` | `i2c3` | `PC0` | Mapped. |
| DAC I2C data | `DAC_I2C_SDA` | `i2c3` | `PC1` | Mapped. |
| DAC reset | `DAC_RESET` | `reset-gpios` | `PC2` active-low | Mapped. |
| DAC LDAC | `LDAC*` | optional `ldac-gpios` | not routed to MCU on pH_V1_0 | Hardware option, not a DTS pin. |
| DAC reference | REF3025 2.5 V | `reference-millivolt` | `2500` | Metadata used for output scaling. |

Conflict / hardware note:

`LDAC*` is tied low through `R73 0R` and exposed at `TP17` on the reviewed pH
schematic. The AD5675 driver supports optional `ldac-gpios`, but the current
board DTS intentionally omits it because there is no MCU route. If a later PCB
revision needs MCU-controlled simultaneous DAC updates, route `LDAC*` to a free
GPIO and add `ldac-gpios = <... GPIO_ACTIVE_LOW>;` to the AD5675 node.

### pH Analog Front-End And Outputs

| Function | Schematic net | DTS node/property | Firmware mapping | Status |
| --- | --- | --- | --- | --- |
| ADC clean 3V3 enable | `U_O_D_3V3_EN` | `adc-power-gpios` | `PC3` | Mapped. |
| Electrode zero switch | `ELECTRODE_ZERO_CTRL` | `electrode-zero-gpios` | `PC4` | Mapped. |
| Gate/analog monitor | `GATE_BUFF_ADC` / monitor input | `mcu-adc`, `io-channels` | `PC5 / ADC1_IN14` | Mapped. |
| DAC boost enable | `O_D_UC_PWR_DAC` | `dac-power-gpios` | `PC6` | Mapped. |
| GP8302 DAC0 alarm | `O_D_DAC_ALARM1` | `dac-alarm-gpios` | `PC7` | Mapped. |
| Sensor/pre rail enable | `+3V3_PRE` control | `pre-power-gpios` | `PC8` | Mapped. |
| Analog rail enable | `U_O_D_ANA_PWR_EN` | `analog-power-gpios` | `PC9` | Mapped. |
| GP8302 DAC1 alarm | `O_D_DAC_ALARM2` | `dac1-alarm-gpios` | `PB2` | Mapped. |
| GP8302 DAC0 output | `O_D_UC_I2C1_SCL/SDA` | `current-loop-dac0`, `i2c1` | `PB8/PB9`, address `0x58` | Outputs pH. |
| GP8302 DAC1 output | `O_D_UC_I2C2_SCL/SDA` | `current-loop-dac1`, `i2c2` | `PB10/PB14`, address `0x58` | Outputs PT1000 temperature. |
| HTU21D humidity/temp | n/a on pH_V1_0 | no pH DTS node | n/a | Not used by the pH product; temperature comes from PT1000 through ADS124S08. |
| AT24C512C EEPROM | `MCU_I2C1_SCL/SDA` | `eeprom0`, `i2c1` | `PB8/PB9`, address `0x50` | Mapped. |
| LT3582 analog rail | analog rail I2C block | `ph_analog_rail` | `i2c2`, placeholder `0x2c` | Node disabled until address/programming is verified. |

## Conflict Summary

| Area | Affected board | Conflict / open item | Firmware action | Hardware action |
| --- | --- | --- | --- | --- |
| Dynament methane UART | `argisense_mp_u575rg` | Schematic shows `METH_UART_TXD/RXD` on `PC8/PC7`, but those pins are not valid UART TX/RX pins in Zephyr STM32U575RGT6 pinctrl. | Keep firmware on `UART4 PC10/PC11` until PCB route is corrected. | Route methane UART to `PC10/PC11` or another valid UART pair. |
| PC7/PC8 reuse | `argisense_mp_u575rg` | Current DTS uses `PC7` for DAC alarm and `PC8` for pre-power, while the reviewed schematic labels `PC7/PC8` as methane UART. | Do not move UART to `PC7/PC8`; document as routing conflict. | Resolve schematic pin allocation before PCB release. |
| AD5675 LDAC | `argisense_ph_u575rg` | `LDAC*` is not routed to MCU; it is tied low through `R73 0R` and exposed at `TP17`. | Driver supports optional `ldac-gpios`; DTS omits it now. | Route `LDAC*` to MCU only if firmware-controlled LDAC is required. |
| Second GP8302 alarm | `argisense_mp_u575rg` | Schematic exposes `O_D_DAC_ALARM2`; MP DTS maps only one `dac-alarm-gpios`. | Add `dac1-alarm-gpios` and diagnostics if independent alarm monitoring is required. | Confirm whether both alarm outputs are populated and need MCU monitoring. |
| LT3582 pH rail programming | `argisense_ph_u575rg` | I2C address/programming requirement is not verified. | Keep `ph_analog_rail` disabled and control rail by enable GPIO. | Verify LT3582 address and startup programming on hardware. |

## Recommended Next Checks

1. Decide final methane UART pins in the schematic and update the PCB route
   before hardware bring-up.
2. Decide whether methane + pressure needs independent monitoring of both GP8302
   alarm pins.
3. Decide whether pH needs MCU-controlled AD5675 `LDAC*`; if yes, route the pin
   in the next PCB revision.
4. Validate the pH LT3582 rail IC address and whether it needs runtime I2C
   programming.
5. After schematic updates, rebuild both targets and compare generated
   `zephyr.dts` against the final netlist.
