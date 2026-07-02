# SPDX-License-Identifier: Apache-2.0

# Add out-of-tree driver subdirectories here as they are implemented.
# Examples:
# add_subdirectory_ifdef(CONFIG_SIM7600 sim7600)
# add_subdirectory_ifdef(CONFIG_MLX90640 mlx90640)
add_subdirectory_ifdef(CONFIG_ARGISENSE_ADS124S08 adc)
add_subdirectory_ifdef(CONFIG_DAC dac)
add_subdirectory_ifdef(CONFIG_SENSOR sensor)
