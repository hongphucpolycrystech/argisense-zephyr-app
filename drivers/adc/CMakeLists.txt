# SPDX-License-Identifier: Apache-2.0

zephyr_library()
zephyr_library_sources_ifdef(CONFIG_ARGISENSE_ADS124S08 ads124s08.c)
