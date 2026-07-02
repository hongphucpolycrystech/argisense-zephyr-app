# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_DAC_GP8302 gp8302.c)
zephyr_library_sources_ifdef(CONFIG_DAC_AD5675_I2C ad5675_i2c.c)
