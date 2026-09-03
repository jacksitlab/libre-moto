# Libre-Moto

Open-source alternative to [Beeline Moto II](https://beeline.co/pages/beeline-moto) — a navigation/map display for motorcycle handlebars, built on the **ESP32-S3 + round touch display** hardware (CrowPanel 2.1" 480×480, ST7701S, CST816 touch). All input is **touch** (tap / hold / swipe) — this board revision has no rotary knob.

The display receives navigation data (turn instructions + a minimalistic vector map) via **Bluetooth Low Energy** from a smartphone (osmAnd + companion app) and shows them **glove-friendly** and **without distraction** — in the style of Beeline Moto II.

The board ships with a **serial self-test** (`mt0`–`mt3`) that drives the full BLE pipeline from a terminal, so you can verify the map renderer, state machine, and touch UI without a phone. See `firmware/README_TESTS.md`.

## Status: Phases A + B.1 done. Next: Android companion app (C.2)

- [x] **Phase 0** — Protocol & hardware specs, repo structure, starter firmware
- [x] **Phase A** — Display & UI (Arduino_GFX + LVGL 9.5, touch gestures, state machine)
- [x] **Phase B** — BLE GATT server (NavData, Status, MapData, Control) + heartbeat
- [x] **C.1** — MapData binary parser + LVGL map renderer (roads as twin-edge lines, route as filled white line)
- [ ] **C.2** — **Android companion app** (Kotlin, XML, osmAnd provider reader + BLE client + map projection)  ← **next**
- [ ] **B.2** — Robustness & stress tests (100 writes/5s, reconnect <5s)
- [ ] **Phase D** — UX polish (auto-dim, auto-off, transitions)
- [ ] **Phase E** — v0.1.0 release (firmware + APK)

## Architecture decision

```
┌──────────────────┐        BLE GATT (JSON + binary)         ┌────────────────┐
│  Smartphone      │  ───  C1 NavData (JSON, 1-2 Hz) ───►   │     ESP32      │
│                  │  ───  C4 MapData (binary, 2-4 KB) ─►   │  S3 + display  │
│  osmAnd (nav)    │  ◄──  C2 Status (JSON) ─────────────   │ (ST7701 480²)  │
│ + Companion app  │  ◄──  C5 Control (JSON) ────────────   │  LVGL + BLE    │
└──────────────────┘                                        └────────────────┘
```

**Principles:**
- **Phone = brain.** The phone knows the route (osmAnd), projects the map into screen coordinates, and sends finished vector data. The display only renders.
- **No map tiles, no routing computation** on the ESP32 — minimal RAM/flash budget, clean BLE transfers.
- **Open source (GPLv3)** — firmware and phone app are copyleft (deliberate: no closed product may be built from Libre-Moto code).

## Hardware

- **MCU:** ESP32-S3R8 (dual-core LX7, 240 MHz, 8 MB PSRAM, 16 MB flash)
- **Display:** 2.1" RGB 480×480 IPS, **ST7701S**, **round**, ~460 nits (backlight LED)
- **Touch:** **CST816** (CST8x family) over I2C — sole input device (tap / hold / swipe)
- **Knob/encoder:** none on this board revision (PCF8574 P5 unused)
- **I/O expander:** PCF8574 (I2C, 0x21) for reset/IRQ signals
- **Power:** 5 V USB-C

Details in [`docs/hardware.md`](docs/hardware.md).

## Repository layout

```
├── LICENSE                  # GPLv3
├── README.md
├── docs/
│   ├── hardware.md          # CrowPanel pinmap, boot sequence, libraries
│   ├── protocol.md          # BLE + JSON + binary protocol (canonical)
│   ├── design.md            # UI screens, state machine, colors
│   └── partition_table/     # elecrow_s3 partition scheme (boards.txt + csv)
├── firmware/
│   ├── src/libre-moto/
│   │   ├── libre-moto.ino   # main sketch (Arduino IDE project = this folder)
│   │   └── config.h         # pins, display init, settings
│   ├── src/nav/             # NavState struct + hand-rolled JSON parser + tests
│   ├── src/ble/             # GATT server (NavData, Status, MapData, Control)
│   ├── src/map/             # MapData binary frame (protocol.md §4)
│   ├── test/                # native C++ unit tests (g++ — no Arduino needed)
│   ├── libraries/           # vendored Arduino libraries (GFX_Library_for_Arduino,
│   │                        #   lvgl 9.5.0 + lv_conf.h, Adafruit_CST8XX_Library,
│   │                        #   Adafruit_BusIO, PCF8574_library)
│   └── README_TESTS.md      # how to build & run the native unit tests
└── phone/                   # (Phase C) Android companion app (Kotlin, XML layout)
```

## Setup Dev Env

- Install Arduino IDE 2.X
- Install Arduino Partition Scheme from `docs/partition_table/`
- Setup IDE:
    - Board: ESP32S3 Dev Module
    - CPU Frequency: 240MHz (WiFi)
    - Flash Mode: QIO 80MHz
    - Flash Size: 16MB (128Mb)
    - PSRAM: OPI PSRAM
    - USB CDC On Boot: Enabled
    - USB Mode: Hardware CDC and JTAG
    - Upload Mode: UART0 / Hardware CDC
    - Upload Speed: 921600
    - Partition Scheme: elecrow_s3

The Arduino IDE project is the `firmware/src/libre-moto` folder (the `.ino` file defines the project); libraries are picked up from `firmware/libraries/`.

## Roadmap / milestones

| Phase | Goal | Gate |
|---|---|---|
| **0** ✅ | Docs, protocol, repo structure, starter sketch | These files complete + basic display init works |
| **A** ✅ | Display, UI, map renderer, state machine **without** phone | Serial mock shows all screens |
| **B** ✅ (B.1) | BLE server, heartbeat, reconnect, stress test | GATT server verified end-to-end with nRF Connect (B.2 stress pending) |
| **C** 🚧 | Android companion (osmAnd + BLE) | C.1: map renderer done. C.2: app (Kotlin, XML, Android 10+) — **next** |
| **D** | UX polish (brightness, auto-dim, auto-off) | Brightness via swipe (done as debug), auto-dim after 30 s |
| **E** | **v0.1.0** release | Firmware binary + APK, GitHub release |

## Development

### Firmware

- **Toolchain:** Arduino IDE 2.X (or `arduino-cli`). Project folder = `firmware/src/libre-moto/`.
- **Core:** [esp32](https://github.com/espressif/arduino-esp32) ≥ 3.3 (BLE via classic Bluedroid API — see `firmware/src/ble/`).
- **Display:** [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) + [LVGL 9.5](https://lvgl.io) (vendored, `LV_USE_FLOAT=0` → all coordinates are `int32_t`).
- **Touch:** [Adafruit_CST8XX_Library](https://github.com/adafruit/Adafruit_CST8XX) (CST816).
- **I/O expander (LCD touch reset/IRQ):** PCF8574 (I²C 0x21, [PCF8574_library](https://github.com/robotics-community/PCF8574_library)).
- **JSON parsing:** hand-rolled (no ArduinoJson) — see `firmware/src/nav/nav_message_parse_impl.h`. This keeps the firmware build free of template bloat.
- **Native unit tests (no hardware needed):**
  ```bash
  cd firmware/test
  g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/nav test_nav_message.cpp -o /tmp/tnm && /tmp/tnm
  g++ -std=c++11 -Wall -Wextra -Werror -O2 -I../src/map test_map_frame.cpp  -o /tmp/tmf && /tmp/tmf
  ```
  Full guide in [`firmware/README_TESTS.md`](firmware/README_TESTS.md).

### Phone app (Phase C, next)

- **Language:** Kotlin, classic XML layouts (no Jetpack Compose).
- **Android:** `minSdk 29` (Android 10), `targetSdk 34` (Android 14).
- **Navigation source:** osmAnd (read route + nav data from osmAnd's routing state; fallback to a generic "NavProvider" interface so other apps can be added later).
- **Build system:** Gradle Kotlin DSL, single module, BLE via Android platform `BluetoothLeScanner`.
- **License:** GPLv3 (firmware **and** phone app). Copyleft deliberately chosen.

## License

[GNU GPLv3](LICENSE) — all source code is open source; redistribution/forks are only allowed under GPLv3 (intentional: no closed product).

## Credits & references

- Hardware: [Elecrow CrowPanel 2.1"](https://www.elecrow.com/crowpanel-2-1inch-hmi-esp32-rotary-display-480-480-ips-round-touch-knob-screen.html)
- Display stack: [Arduino_GFX](https://github.com/moononournation/Arduino_GFX), [LVGL 9](https://lvgl.io)
- BLE: classic **Bluedroid API** shipped with [esp32-arduino](https://github.com/espressif/arduino-esp32) core ≥ 3.3 (see `firmware/src/ble/ble_link.h`)
- Touch: [Adafruit CST8XX](https://github.com/adafruit/Adafruit_CST8XX)
- Navigation engine: [osmAnd](https://osmand.net)
- Design reference: [Beeline Moto II](https://beeline.co/pages/beeline-moto)

## Issues & contributing

File issues [on this repo](https://github.com/jacksitlab/libre-moto/issues). PRs are welcome, especially for Phases A–C.
