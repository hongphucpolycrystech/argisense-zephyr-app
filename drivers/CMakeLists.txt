# SPDX-License-Identifier: Apache-2.0

# Add out-of-tree driver subdirectories here as they are implemented.
# Examples:
# add_subdirectory_ifdef(CONFIG_SIM7600 sim7600)
# add_subdirectory_ifdef(CONFIG_MLX90640 mlx90640)
add_subdirectory_ifdef(CONFIG_DAC_GP8302 dac)
add_subdirectory_ifdef(CONFIG_SENSOR sensor)
