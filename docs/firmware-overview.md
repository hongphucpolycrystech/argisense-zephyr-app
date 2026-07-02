# ArgiSense Firmware Overview

This repository contains two product-specific firmware profiles. Keep their
architecture notes separate because the boards have different sensor front-ends,
pin maps, measurement flows, calibration data, and DAC output policies.

## Product Firmware Documents

| Product | Board target | Firmware source | Overview |
| --- | --- | --- | --- |
| Methane + pressure | `argisense_mp_u575rg` | `app/products/methane_pressure/src` | `docs/methane-pressure-firmware-overview.md` |
| pH | `argisense_ph_u575rg` | `app/products/ph/src` | `docs/ph-firmware-overview.md` |

## Shared Documents

| Document | Purpose |
| --- | --- |
| `docs/sensor-io-mapping.md` | Consolidated IO mapping for both boards, including schematic conflicts and open hardware items. |
| `docs/dynament-platinum-methane.md` | Dynament Platinum methane sensor protocol and electrical notes. |
| `docs/htu21d-humidity.md` | HTU21D humidity and ambient temperature notes for the methane + pressure product. |
| `docs/sensor-platform-v1-ph-analysis.md` | pH schematic analysis and pH front-end implementation notes. |

## Build Targets

Methane + pressure:

```bat
py -3.12 -m west build -p always -b argisense_mp_u575rg argisense-zephyr-app
```

pH:

```bat
py -3.12 -m west build -p always -b argisense_ph_u575rg argisense-zephyr-app
```

Both product profiles share the common services under `app/common/src`, but
sensor measurement logic and product policy live under the matching
`app/products/<product>/src` directory.
