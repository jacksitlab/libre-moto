# Libre-Moto

Open-source alternative to [Beeline Moto II](https://beeline.co/pages/beeline-moto) — a navigation/map display for motorcycle handlebars, built on the **ESP32-S3 + round touch display** hardware (CrowPanel 2.1" 480×480, ST7701S, rotary knob, CST816 touch).

The display receives navigation data (turn instructions + a minimalistic vector map) via **Bluetooth Low Energy** from a smartphone (osmAnd + companion app) and shows them **glove-friendly** and **without distraction** — in the style of Beeline Moto II.

## Status: Early stage (Phase 0 — Documentation)

- [x] **Phase 0** — Protocol & hardware specs, repo structure  *(this repo)*
- [ ] **Phase A** — Display & UI bring-up (Arduino_GFX + LVGL, map renderer, state machine)
- [ ] **Phase B** — BLE GATT server (NimBLE) + robustness
- [ ] **Phase C** — Android companion app (osmAnd provider reader + BLE client + map projection)
- [ ] **Phase D** — UX polish (brightness, auto-dim, auto-off)
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
- **Touch:** **CST816** (CST8x family) over I2C
- **Rotary knob:** A/B encoder + press switch
- **I/O expander:** PCF8574 (I2C, 0x21) for reset/IRQ signals
- **Power:** 5 V USB-C

Details in [`docs/hardware.md`](docs/hardware.md).

## Repository layout

```
├── LICENSE              # GPLv3
├── README.md
├── docs/
│   ├── hardware.md      # CrowPanel pinmap, boot sequence, libraries
│   ├── protocol.md      # BLE + JSON + binary protocol (canonical)
│   └── design.md        # UI screens, state machine, colors
├── firmware/            # (Phase A) ESP32-S3 firmware
│   ├── src/
│   │   ├── main.cpp
│   │   ├── config.h
│   │   ├── nav/         # JSON parser, MapFrame parser (host-testable)
│   │   ├── ble/         # NimBLE GATT server
│   │   ├── ui/          # LVGL app (screens, map renderer, state machine)
│   │   └── display/     # Arduino_GFX/LVGL port, PCF8574, touch
│   └── test/            # host-side unit tests
└── phone/               # (Phase C) Android companion app (Kotlin)
```

## Setup Dev Env

  - Install Arduino IDE 2.X
  - Install Arduino Partion Scheme
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
  


## Roadmap / milestones

| Phase | Goal | Gate |
|---|---|---|
| **0** ✅ | Docs, protocol, repo structure | These files complete |
| **A** | Display, UI, map renderer, state machine **without** phone | Serial mock shows all screens |
| **B** | BLE server, heartbeat, reconnect, stress test | 100 BLE writes / 5 s, reconnect <5 s |
| **C** | Android companion (osmAnd + BLE) | E2E: osmAnd live → display + map |
| **D** | UX polish (brightness, auto-dim, auto-off) | Brightness via knob, auto-dim after 30 s |
| **E** | **v0.1.0** release | Firmware binary + APK, GitHub release |

## Development

- Firmware: **Arduino IDE / PlatformIO** (both work; PlatformIO preferred for unit tests).
- Libraries: `Arduino_GFX 1.6.7`, `LVGL 9.1.0`, `NimBLE-Arduino`, `Adafruit_CST8XX`, `Wire` (PCF8574), `ArduinoJson`.
- Phone app: Kotlin, minSdk 28, targetSdk 34, Gradle.
- License: **GPLv3** (firmware **and** phone app). Copyleft deliberately chosen.

## License

[GNU GPLv3](LICENSE) — all source code is open source; redistribution/forks are only allowed under GPLv3 (intentional: no closed product).

## Credits & references

- Hardware: [Elecrow CrowPanel 2.1"](https://www.elecrow.com/crowpanel-2-1inch-hmi-esp32-rotary-display-480-480-ips-round-touch-knob-screen.html)
- Display stack: [Arduino_GFX](https://github.com/moononournation/Arduino_GFX), [LVGL 9](https://lvgl.io)
- BLE: [NimBLE-Arduino](https://github.com/h2zero/NimBLE-Arduino)
- Touch: [Adafruit_CST8XX](https://github.com/adafruit/Adafruit_CST8XX)
- Navigation engine: [osmAnd](https://osmand.net)
- Design reference: [Beeline Moto II](https://beeline.co/pages/beeline-moto)

## Issues & contributing

File issues [on this repo](https://github.com/jacksitlab/libre-moto/issues). PRs are welcome, especially for Phases A–C.

