# CrowPanel 2.1" — hardware reference

Sources: Elecrow product datasheet + official wiki + official ESPHome example (as of 2026-08-30).
**Before the first flash, verify the exact values from the bundled demo Arduino sketch against this file** (pinmap can vary between batches).

## System

| Item | Value |
|---|---|
| MCU | **ESP32-S3R8** (Xtensa LX7, 240 MHz, 512 KB SRAM + **8 MB PSRAM**, 16 MB flash) |
| Display | 2.1" IPS 480×480, **ST7701S**, RGB parallel interface |
| Touch | **CST816** (CST8x family), I2C, address **0x15** |
| I/O expander | **PCF8574** @ I2C, address **0x21** |
| Power | 5 V / 1 A |
| Knob/encoder | **not present** on this board revision — all UI input is touch |

## Arduino_GFX initialization (ST7701 + RGB)

```cpp
// RGB databus + SPI init bus in one object (Arduino_GFX 1.6.7)
Arduino_ESP32RGBPanel *bus = new Arduino_ESP32RGBPanel(
    16 /* CS  */, 2 /* SCK */, 1 /* SDA (MOSI) */,
    40 /* DE  */, 7 /* VSYNC */, 15 /* HSYNC */, 41 /* PCLK */,
    46, 3, 8, 18, 17,   // R0..R4
    14,13,12,11,10, 9,  // G0..G5
     5,45,48,47,21      // B0..B4
);

Arduino_ST7701_RGBPanel *gfx = new Arduino_ST7701_RGBPanel(
    bus, GFX_NOT_DEFINED /* RST — via PCF8574 P4, see below */,
      0 /* rotation */, false /* IPS */,
    480 /* width */, 480 /* height */,
    st7701_type5_init_operations,
    sizeof(st7701_type5_init_operations),
     true /* BGR */,
    10 /* hsync_front_porch  */,
     4 /* hsync_pulse_width  */,
    20 /* hsync_back_porch   */,
    10 /* vsync_front_porch  */,
     4 /* vsync_pulse_width  */,
    20 /* vsync_back_porch   */
);
// Fallback timings (from the official ESPHome example) if banding/artifacts appear:
// hfp=20, hpw=10, hbp=10, vfp=8, vpw=10, vbp=10, pclk inverted, 18 MHz
```

## Pinmap (direct GPIOs)

| Function | GPIO | Notes |
|---|---|---|
| I2C SDA | **38** | PCF8574 + CST816 (+ optional SSD1306 OLED) |
| I2C SCL | **39** | same bus |
| Backlight | **6** | LEDC PWM, ~20 kHz |
| BOOT | 0 | (board default) |

## PCF8574 expander (I2C 0x21)

| PCF pin | Function | Type |
|---|---|---|
| **P0** | touch (CST816) reset | output |
| **P2** | touch (CST816) IRQ | input (pull-up) |
| **P3** | LCD power | output |
| **P4** | LCD reset (ST7701) | output, **inverted** |
| **P5** | (was encoder press on older boards) | **unused** |

### Boot sequence (official ESPHome example, ~800 ms)

```
P3 (LCD power)  → ON
P4 (LCD reset)  → ON → 100 ms → OFF → 100 ms → OFF
P0 (TP reset)   → ON → 100 ms → OFF → 120 ms → ON → 120 ms → ON
P2 (TP IRQ)     → ON  (keep pull-up)
```

> P4 is `inverted: true` in the ESPHome example — in Arduino terms: `digitalWrite(pcf4, LOW)` = reset active.

## Touch (CST816 / CST8x)

- I2C: SDA 38, SCL 39, address **0x15**, `skip_probe: true` (ESPHome) — i.e. **do not query the device ID**, just attach to the I2C bus.
- IRQ: via PCF8574 P2 (not a direct GPIO).
- Reset: via PCF8574 P0.
- `Adafruit_CST8XX` (or `Adafruit_TouchScreen`): constructor with `irqPin = -1, resetPin = -1` → read proc by polling (20–25 ms) + IRQ via P2 as wake trigger.
- Coordinates: 480×480, rotation 0, **round display** → clamp corners to a circle of radius 240 px.

## Rotary encoder — not used

Therotary knob has been removed. UI input is touch only:
**tap** (screen cycle), **hold ≥ 500 ms** (back to home), **horiz. swipe** (brightness).
Pins 42/4 (old encoder A/B) and PCF8574 P5 (old knob press) are not connected in software.

## Libraries (Arduino IDE / PlatformIO)

| Package | Version | Purpose |
|---|---|---|
| `arduino` | ESP32-S3 core | MCU, PSRAM (8 MB, octal, 80 MHz) |
| `Arduino_GFX` | **1.6.7** | ST7701-S RGB panel + RGB bus |
| `Adafruit_CST8XX` | latest | touch controller |
| `Wire` (PCF8574) | — | I/O expander (own mini module `pcf8574.h/.cpp`) |
| `NimBLE-Arduino` | 2.x | BLE GATT server |
| `LVGL` | **9.1.0** | UI |
| `ArduinoJson` | 7.x | JSON parser (NavData) |

## Power

- 5 V / 1 A (USB-C or FPC-12P)
- ESP32-S3 + display + backlight: ~200–400 mW active, ~100 mW deep sleep (with BLE active sleep)
- Backlight PWM: 20 kHz, 0–100 %
