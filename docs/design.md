# Libre-Moto — UI & state design

Display: 480×480 px, **round** → all layouts inside a circle of radius ~238 px (12 px margin).

## Screens (v0.1)

### S1 — Boot
- 2 s: `LibreMoto` (large, centered) + "BLE ready" as a subline.
- Then automatically → S2 (home).

### S2 — Home (STATE_IDLE)
```
        ┌─────────────────────┐
        │        LibreMoto    │   title, 30 px, top
        │  ───────────────    │
        │                     │
        │      ● BLE ✓        │   status dot (green/amber/red)
        │      Bat 87%        │   phone battery (last known)
        │                     │
        │   [  Show map  ]    │   touch button (or tap = cycle screens)
        │                     │
        │  ▭ 350m  ⛽ 50      │   last nav instruction (small, gray)
        └─────────────────────┘
```
- All input is **touch** (no rotary knob on this board revision): tap = next screen, hold ≥ 500 ms = back to home, horizontal swipe on home = brightness ±10 % (toast feedback).

### S3 — Nav instruction (STATE_NAV, map_on=false)
```
        ┌─────────────────────┐
        │                     │
        │     ╭───────╮       │
        │     │   ←     │ 350m│  maneuver arrow (large, 120 px) + distance
        │     ╰───────╯       │
        │                     │
        │     Main St.        │   road name (24 px, max 1 line)
        │     ─────────────   │
        │     Limit 50 km/h   │   (only when limit_kmh is set)
        └─────────────────────┘
```
- Reroute: 3× blink (200 ms) + "Recalculating…" for 2 s.
- Arrived: large ✓ + "Arrived" for 10 s → home.

### S4 — Map (STATE_NAV, map_on=true) — **main feature**
```
        ┌─────────────────────┐
        │  ╱──╲                │
        │  ╱    ╲   route (white, 5 px)  │
        │ ◄──────────── ●     │  roads gray 2-3 px        │
        │   ╲    ╱      ╱    │  vehicle arrow (white,      │
        │   ╲  ● (here) │    │  center, ~24 px)            │
        │    ╲  ╰────╯  ╱    │
        └─────────────────────┘
      header (top, 24 px):  [→ 350m]   [Limit 50]   [Bat 87%]
```
- Map content from C4 MapFrames: route, 2–4 adjacent roads, destination circle.
- Vehicle arrow always dead center, rotates with `heading` (map fixed, only arrow turns) — OR map rotates (flags bit0) + arrow points up. **Default: map rotates, arrow points up** (like Beeline/Android Auto).
- Header overlays (not a separate screen).
- Screen switching: tap (map ↔ nav instruction), hold (map → home).
- Refresh: 1–2 Hz per `MapData`.

### S5 — Status/Connection (stateless overlay)
- "BLE connected · 1234 msgs"
- "Link lost" (amber) for 5 s on timeout → S2.
- "Arrived ✓" (green) for 10 s.

## State machine (firmware)

```
            BLE ready
   BOOT ───────────────► IDLE ◄──────────┐
    │                  ▲    │            │
    │                  │    │ map_on +   │ arrived 10s /
    │                  │    │ nav        │ idle 20s / lost
    │                  │    ▼            │
    │                  └── NAV ─────────┘
    │                     ▲  │
    │                     │  │ reroute (3× blink, stays NAV)
    │                     └──┘
    │
    └── (display init / BLE init, 3 s timeout → crash screen "E")
```

State details:
- **BOOT**: display init, BLE init, home screen. (≤3 s)
- **IDLE**: home screen, advertising active, waiting for `t=nav` or `t=beep`.
- **NAV**: nav or map screen (per `map_on`), waiting for the next NavData.
- **ARRIVED**: overlay for 10 s, then IDLE.
- **LOST**: "Link lost" overlay for 5 s, then IDLE (advertising continues).

## Interaction (touch only — no knob/encoder on this board)

| Action | Result |
|---|---|
| Tap (any screen) | cycle: home → nav instruction → map → home |
| Hold ≥ 500 ms (any screen) | → S2 (home), toast "Home" |
| Horizontal swipe on home (≥ 40 px) | brightness ∓ 10 %, persisted, toast "Brightness NN %" |
| Tap header (NAV, v0.2) | toggle header items (limit/battery show/hide) |

## Colors (v0.1, dark theme)

| Element | Color |
|---|---|
| Background | `#0D1117` (dark gray) |
| Route | `#FFFFFF` 5 px |
| Roads | `#5C6773` 2–3 px |
| Text primary | `#E6EDF3` |
| Text secondary | `#8B949E` |
| Accent (BLE ok) | `#3FB950` |
| Accent (BLE warning) | `#D29922` |
| Accent (BLE lost) | `#F85149` |
| Reroute warning | `#D29922` blinking |

## Fonts
- **Bebas Neue** / **Inter** (or `lv_font_montserrat` from LVGL, 12/20/28 px)
- Large numbers (distance): 48 px
- Road name: 24 px, 1 line, max 24 chars (phone sends it pre-truncated)

## Icons
- Arrows: `lv_image` from 4 PNGs (left/right/straight/uturn) @ 128×128 (v0.1: draw with `lv_arc`/`lv_line` — no image files, saves flash)
- ✓ / BLE / battery: Unicode glyphs (✓ ✔ ⚡ 📶) — LVGL montserrat glyph set; if unavailable: draw

## Animations (optional, Phase D)
- Heading animation (map rotates smoothly, 90 ms lerp)
- Fade in/out between screens (100 ms)
- Reroute blink (3× 200 ms)
