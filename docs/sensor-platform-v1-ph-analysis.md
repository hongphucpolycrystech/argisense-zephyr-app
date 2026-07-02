# Sensor Platform V1 pH Schematic Analysis

This note captures the firmware impact of the updated
`Sensor_Platform_V1 (1).pdf` schematic. The schematic title block identifies
the hardware variant as `pH_V1_0`.

## Board Split

Keep the existing methane + pressure product on `argisense_mp_u575rg`.
Use `argisense_ph_u575rg` for the pH PCB because the schematic adds a
different analog front-end and a separate firmware profile.

## Existing Firmware Coverage

The current firmware already has reusable support for these shared blocks:

| Schematic block | Existing firmware support |
| --- | --- |
| RS485 THVD1420 | UART2, RS485 DE, Modbus registers, RS485 DFU |
| GP8302 4-20 mA outputs | `drivers/dac/gp8302` and dual current-loop model |
| HTU21D humidity/temperature | Reusable driver for the methane + pressure product only; the pH product does not use this IC |
| MS5803-05BA pressure | `drivers/sensor/ms5803_05ba` |
| Dynament methane UART | `drivers/sensor/dynament_platinum` |
| AT24C512 EEPROM | Zephyr `atmel,at24` EEPROM node |

## New pH Hardware

The pH schematic adds these firmware-facing devices:

| Device | Role | Firmware status |
| --- | --- | --- |
| ADS124S08IPBSR | 24-bit ADC for ISFET/REFET/PT1000 measurements | Driver added under `drivers/adc/ads124s08.c`; mapped to `spi1` on `PA4..PA7` |
| AD5675BCPZ-REEL7 | 8-channel I2C DAC for ISFET/REFET bias outputs | Driver added under `drivers/dac/ad5675_i2c.c`; mapped to `i2c3` on `PC0/PC1` |
| REF3025AIDBZR | 2.5 V DAC/ADC reference | Model as reference metadata |
| LT3582EUD-5 | Generates `+5V_AN` and `-5V_AN` analog rails | Enable mapped to `PC9`; I2C node disabled until address/programming is verified |
| PT1000 | Temperature input measured through ADS124S08 IDAC path | Measurement path added in `app/products/ph/src/ph_measurement.c` |
| Electrode zero switch | `ELECTRODE_ZERO_CTRL` controls electrode short/open | Mapped to `PC4` |

## Selected MCU Pin Map

| Signal group | MCU mapping |
| --- | --- |
| ADS124S08 SPI | `PA4` CS, `PA5` SCLK, `PA6` DOUT/MISO, `PA7` DIN/MOSI |
| ADS124S08 control | `PB12` reset, `PB13` start/sync, `PB15` data-ready |
| AD5675 I2C/reset | `PC0` SCL, `PC1` SDA, `PC2` reset |
| AD5675 LDAC* | Not routed to MCU on pH_V1_0; R73 0R ties it low and TP17 is a test point |
| pH power/control | `PC3` ADC 3V3 enable, `PC4` electrode zero, `PC9` analog rail enable |
| Service buses kept free | `PA11/PA12` USB, `PA13/PA14` SWD, `PA9/PA10` debug UART, `PA1/PA2/PA3` RS485 |

## pH ADC Channel Map

The schematic names these ADS124S08 measurement inputs:

| Logical signal | Meaning |
| --- | --- |
| `AIN0_Vs_ISFET` | ISFET source sense path |
| `AIN1_Vs_REFET` | REFET source sense path |
| `AIN2_PT1000_P` | PT1000 positive input |
| `AIN3_PT1000_N` | PT1000 negative input |
| `ISFET_BULK_ADC` | ISFET bulk monitor |
| `ISFET_SUBS_ADC` | ISFET substrate monitor |
| `REFET_BULK_ADC` | REFET bulk monitor |
| `REFET_SUBS_ADC` | REFET substrate monitor |
| `ISFET_DRAIN_ADC` | ISFET drain monitor |
| `REFET_DRAIN_ADC` | REFET drain monitor |
| `GATE_BUFF_ADC` | ISFET/REFET gate buffer output |

The PT1000 note in the schematic says the ADS124S08 current source is set to
250 uA for PT1000 measurement.

## pH Bias DAC Channel Map

The AD5675 outputs are used as pH bias setpoints:

| DAC output | Function |
| --- | --- |
| `VOUT0_ID_SET_ISFET` | ISFET current-source setpoint, nominal 448 mV |
| `VOUT1_VDS_SET_ISFET` | ISFET VDS setpoint, nominal 500 mV |
| `VOUT2_ID_SET_REFET` | REFET current-source setpoint, nominal 448 mV |
| `VOUT3_VDS_SET_REFET` | REFET VDS setpoint, nominal 500 mV |
| `VOUT4_BULK_ISFET` | ISFET bulk bias, tracks `Vs_ISFET` reading |
| `VOUT5_SUBS_ISFET` | ISFET substrate bias, tracks `Vs_ISFET` reading |
| `VOUT6_BULK_REFET` | REFET bulk bias, tracks `Vs_REFET` reading |
| `VOUT7_SUBS_REFET` | REFET substrate bias, tracks `Vs_REFET` reading |

The schematic note on the AD5675 sheet states `Vout = Vs reading` for the
bulk/substrate tracking outputs. Firmware therefore preloads VOUT4..VOUT7 to
0 mV, reads the ISFET/REFET source voltages, then rewrites VOUT4/VOUT5 from
`Vs_ISFET` and VOUT6/VOUT7 from `Vs_REFET`.

## pH Measurement Flow

1. Enable `+3V3_PRE`, ADC clean 3.3 V, and the analog rail enable, then wait
   for analog settling.
2. Configure AD5675 channels and write safe bring-up defaults: 448 mV on the
   ISFET/REFET current-source setpoints and 500 mV on the ISFET/REFET VDS
   setpoints.
3. Read the ISFET/REFET source and gate channels, then update AD5675
   VOUT4..VOUT7 from the measured source voltages.
4. Read ADS124S08 drain/bulk/substrate diagnostics for ISFET and REFET. The
   firmware now records raw pH differential voltage,
   `VGS_ISFET`, `VGS_REFET`, source, gate, drain, bulk, and substrate values.
5. Read ADS124S08 AIN2/AIN3 differentially with 250 uA IDAC on AIN2 for
   PT1000 temperature.
6. Use the electrode-zero switch as a calibration/service state to capture
   offset data.
7. Calculate pH only after calibration mode and calibration-valid are enabled.
   The stored calibration includes pH7 raw offset, uV/pH slope, temperature
   reference, and temperature coefficient.

## Remaining Hardware Checks

- Verify ADS124S08 SPI mode, `DRDY` polarity/timing, reset timing, and the
  external REF3025 reference path on hardware.
- Verify AD5675 address `0x0c`, reset polarity, LDAC hardware option, output
  scaling, and the 448 mV / 500 mV nominal bias points on a scope or DMM before
  connecting electrodes.
- Verify whether LT3582 requires runtime I2C programming or can be treated as a
  GPIO-enabled analog rail during bring-up.
- Confirm the placeholder LT3582 I2C address before enabling the
  `ph_analog_rail` node.
- Confirm final product behavior for keeping `+3V3_PRE` on continuously for
  RS485/service access versus duty-cycling it during measurement windows.

## Firmware Implementation Status

1. Shell commands are available for direct ADS124S08 raw reads, AD5675 voltage
   writes, electrode-zero switching, and forced pH sampling.
2. Validate the ADS124S08/PT1000 temperature conversion against known
   resistance points.
3. Validate pH7 raw offset, slope sign, VDS tolerance, and temperature
   compensation with real buffer solutions before enabling `ph_cal_valid`.
4. Add GUI plotting fields for pH, PT1000 temperature, gate voltage,
   ISFET/REFET diagnostics, and pH output current.
