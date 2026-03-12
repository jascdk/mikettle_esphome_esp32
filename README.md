# Mi Kettle – ESPHome ESP32 Component

An [ESPHome](https://esphome.io) external component for the **Xiaomi Mi Kettle** (product ID 275 / EU model).  
It communicates with the kettle over **Bluetooth Low Energy** using the same protocol as the
[mikettle-ha](https://github.com/drndos/mikettle-ha) /
[mikettle Python library](https://github.com/drndos/mikettle) and the
[protocol documentation by aprosvetova](https://github.com/aprosvetova/xiaomi-kettle).

---

## Features

| Sensor | Type | Description |
|--------|------|-------------|
| `current_temperature` | `sensor` | Current water temperature (°C) |
| `set_temperature` | `sensor` | Keep-warm target temperature (°C) |
| `keep_warm_time` | `sensor` | Minutes elapsed since keep-warm was enabled |
| `action` | `text_sensor` | `idle` / `heating` / `cooling` / `keeping warm` |
| `mode` | `text_sensor` | `none` / `boil` / `keep warm` |
| `keep_warm_type` | `text_sensor` | `boil and cool down` / `heat up` |

---

## Hardware requirements

- Any **ESP32** board (e.g. ESP32-DevKitC, Wemos D32, …)
- [ESPHome](https://esphome.io) ≥ 2023.x

---

## Quick start

### 1 – Find your kettle's Bluetooth MAC address

Use the built-in ESPHome BLE scan or run `bluetoothctl scan on` on any Linux/Mac machine.

### 2 – Copy the component into your ESPHome config directory

```
your-esphome-config/
├── components/
│   └── mikettle/          ← copy this folder from the repo
│       ├── __init__.py
│       ├── mikettle.h
│       └── mikettle.cpp
└── mikettle.yaml          ← adapt the example
```

Or reference this repository directly as an `external_components` source:

```yaml
external_components:
  - source: github://jascdk/mikettle_esphome_esp32@main
    components: [mikettle]
```

### 3 – Create / adapt `mikettle.yaml`

```yaml
esphome:
  name: mikettle
  friendly_name: Mi Kettle

esp32:
  board: esp32dev
  framework:
    type: arduino

logger:
api:
ota:
wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

esp32_ble_tracker:

ble_client:
  - mac_address: "AA:BB:CC:DD:EE:FF"   # ← your kettle's MAC
    id: mi_kettle_ble

external_components:
  - source:
      type: local
      path: components

mikettle:
  ble_client_id: mi_kettle_ble
  product_id: 275          # 275 = EU; use 131 for some CN variants
  # Per-device token (24 hex chars = 12 bytes). Required for authentication.
  # Extract it from Mi Home with a tool like `miio-tokens` or `python-miio`.
  token: "AABBCCDDEEFF001122334455"  # ← replace with your token

  current_temperature:
    name: "Kettle Temperature"
  set_temperature:
    name: "Kettle Set Temperature"
  keep_warm_time:
    name: "Kettle Keep Warm Time"
  action:
    name: "Kettle Action"
  mode:
    name: "Kettle Mode"
  keep_warm_type:
    name: "Kettle Keep Warm Type"
```

### 4 – Flash

```bash
esphome run mikettle.yaml
```

---

## Configuration reference

```yaml
mikettle:
  # Required
  ble_client_id: mi_kettle_ble

  # Optional – product ID; 275 = EU version, 131 = some CN versions
  product_id: 275

  # Per-device authentication token (24 hex chars = 12 bytes).
  # Required for authentication to succeed on most devices.
  # Obtain from Mi Home with a token extractor (e.g. `miio-tokens`, `python-miio`).
  token: "AABBCCDDEEFF001122334455"   # ← replace with your device's token

  # All sensors below are optional – omit any you don't need

  current_temperature:
    name: "..."
    # all standard ESPHome sensor options are accepted

  set_temperature:
    name: "..."

  keep_warm_time:
    name: "..."         # unit: minutes

  action:
    name: "..."         # idle | heating | cooling | keeping warm

  mode:
    name: "..."         # none | boil | keep warm

  keep_warm_type:
    name: "..."         # boil and cool down | heat up
```

---

## BLE protocol overview

Authentication is a 6-step RC4-derived cipher handshake that requires a **per-device token** (12 bytes, obtained from Mi Home):

1. Write `KEY1` (`0x90 0xCA 0x85 0xDE`) to the **auth-init** characteristic.
2. Enable notifications on the **auth** characteristic (write CCCD `0x01 0x00`).
3. Write `cipher(mixA(reversedMAC, productID), token)` to **auth** characteristic.
4. Wait for notification; optionally verify with `mixB`.
5. Write `cipher(token, KEY2)` (`0x92 0xAB 0x54 0xFA`) to **auth** characteristic.
6. Read the **version** characteristic (required to finalise auth).
7. Enable notifications on the **status** characteristic → data flows.

**Status payload** (8+ bytes):

| Byte | Meaning |
|------|---------|
| 0 | Action (0=idle, 1=heating, 2=cooling, 3=keeping warm) |
| 1 | Mode (0xFF=none, 1=boil, 2/3=keep warm) |
| 2–3 | Reserved |
| 4 | Set temperature (°C) |
| 5 | Current temperature (°C) |
| 6 | Keep-warm type (0=boil+cool, 1=heat up) |
| 7–8 | Keep-warm elapsed time (minutes, little-endian) |

**BLE UUIDs:**

| Role | UUID |
|------|------|
| Auth service | `0000fe95-0000-1000-8000-00805f9b34fb` |
| Auth-init char | `00000010-0000-1000-8000-00805f9b34fb` |
| Auth char | `00000001-0000-1000-8000-00805f9b34fb` |
| Version char | `00000004-0000-1000-8000-00805f9b34fb` |
| Data service | `01344736-0000-1000-8000-262837236156` |
| Status char | `0000aa02-0000-1000-8000-262837236156` |
| Setup char | `0000aa01-0000-1000-8000-262837236156` |
| Time char | `0000aa04-0000-1000-8000-262837236156` |
| Boil-mode char | `0000aa05-0000-1000-8000-262837236156` |

---

## Caveats

- Keep-warm mode can only be **started** by pressing the physical button.
- Keep-warm mode has a maximum duration of 12 hours.
- The minimum keep-warm temperature is 40 °C.
- You cannot heat water if keep-warm mode is off.

---

## Credits

Protocol reverse-engineered by
[Anna Prosvetova](https://github.com/aprosvetova/xiaomi-kettle).  
Python Home Assistant integration by
[drndos](https://github.com/drndos/mikettle-ha).  
ESPHome component by this repository.
