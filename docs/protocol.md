# Libre-Moto — BLE/data protocol (v1)

Canonical definition of all data exchanged between phone and display.
Firmware and phone app both implement **against this file** — never guess the other side.
Changes: bump the `v` version + add an entry to "Changelog".

**Changelog:**
- v1 (2026-08-30): initial protocol.

---

## 1. BLE/GATT topology

- Roles: **ESP32 = GATT server**, phone = GATT client.
- BLE name: `LibreMoto`
- MTU: negotiate 512 (fallback 23).

| UUID (128-bit) | Name | Properties | Direction |
|---|---|---|---|
| `0x6f1c4d52-5b66-4a3e-9c8a-3d2e7f1a4401` | Service | — | — |
| `0x6f1c4d52-5b66-4a3e-9c8a-3d2e7f1a4402` | `NavData` | Write Without Response, Notify | Phone → Device |
| `0x6f1c4d52-5b66-4a3e-9c8a-3d2e7f1a4403` | `Status` | Read, Notify | Device → Phone |
| `0x6f1c4d52-5b66-4a3e-9c8a-3d2e7f1a4404` | `MapData` | Write Without Response | Phone → Device |
| `0x6f1c4d52-5b66-4a3e-9c8a-3d2e7f1a4405` | `Control` | Write Without Response | Phone → Device |

> UUIDs are a provisional fixed set (no known clash with BASE UUIDs).
> Final 16-bit alternatives only if a BLE-stack problem arises.

---

## 2. `NavData` (JSON, UTF-8, one object per write)

```
{
  "v": 1,
  "t": "nav",              // nav | idle | reroute | arrived | beep
  "maneuver": "left",      // left | right | straight | uturn | merge | slight_left | slight_right  (only when t=nav)
  "text": "Main St.",      // optional short label, max 24 chars
  "dist_m": 350,           // meters to maneuver (only when t=nav)
  "limit_kmh": 50,         // optional speed limit
  "batt_pct": 87,          // optional phone battery
  "map_on": true,          // map view on/off (true/false) — driven by phone
  "ts": 1719792000000      // phone timestamp, ms (epoch)
}
```

### Semantics

| `t`       | Firmware behavior |
|---|---|
| `beep`    | heartbeat only (once every 5 s from phone), updates internal status |
| `nav`     | take over nav data (maneuver/text/dist_m), show nav or map screen (per `map_on`) |
| `reroute` | nav screen blinks 3× (200 ms), stays in NAV |
| `arrived` | status screen "Arrived ✓", after 10 s → IDLE |
| `idle`    | → STATE_IDLE (home screen) |

### Heartbeat & timeout

- Phone sends at least `{"v":1,"t":"beep"}` every **5 s**.
- Firmware: **>15 s** without any NavData write → STATE_IDLE + "Link lost" (5 s) → home.

---

## 3. `Status` (device → phone, JSON, Read or Notify)

```
{ "v": 1, "state": "nav", "ble_rx_count": 1234, "brightness_pct": 75,
  "uptime_s": 3600, "seq_last": 42, "map_drop_count": 0 }
```

Field meaning:
- `state`: `boot | idle | nav | arrived`
- `ble_rx_count`: number of received NavData writes (since reset)
- `brightness_pct`: current brightness, 0–100
- `seq_last`: last successfully reassembled MapFrame `seq`
- `map_drop_count`: dropped/incomplete MapFrames (since reset)

---

## 4. `MapData` (binary, little-endian, one frame per logical write)

A frame is split into **512 B chunks** (Write Without Response + re-write).
A complete frame must arrive within 500 ms, otherwise the frame is dropped (`map_drop_count++`).

```
Offset  Size   Field        Type    Description
------  -----  -----------  ------  ----------------------------------------------
0       2      magic        u16     0x4D4C (little-endian: "LM")
2       1      ver          u8      protocol version (1)
3       1      seq          u8      monotonic frame counter (0..255, wrap)
4       2      flags        u16     bit0: rotate with heading, bit1: scale bar,
                                    bit2: show vehicle arrow (default 1)
6       2      heading      i16     degrees × 10   (e.g. 45.5° = 455)
8       2      scale        u16     px_per_meter × 100  (e.g. 4 px/m = 400)
10      1      seg_count    u8      number of segments (1..30)
11      ...    segments     —       (seg_count × Segment, see below)

SEGMENT:
  rel offset   Size   Field    Type    Description
  0              1      type   u8      1=route, 2=road, 3=branch, 4=destination
  1              1      width  u8      road width 1..16 px (route: line width;
                                      road/branch: distance between the two
                                      rendered edge lines)
  2              1      npts   u8      point count (2..120)
  3              2×npts px[]  i16     dx, dy — delta-encoded (1st point absolute,
                                       subsequent: dx/dy relative to previous point)
```

**Origin:** `dx=0, dy=0` = vehicle position = center of the 480×480 display.
Points outside the visible window (typically > ±260 px) must not be sent (viewport culling on the phone side).

**Typical size estimate:**
- route: ~60 points × 2 segments
- 2–4 roads: ~30 points/segment
- total: ~2 KB (max 4 KB = upper bound for one frame)

**Rate limits:**
- while moving: max 2 Hz
- while stationary (velocity < 1 km/h): max 1 Hz
- the phone must not write faster than the firmware can reassemble (~1 frame/200 ms)

---

## 5. `Control` (phone → device, JSON, optional fields)

```
{ "brightness_pct": 60 }        // 0..100, persistent (Preferences)
{ "state": "idle" }             // force home screen (e.g. test button in the app)
{ "reset_map": true }           // clear the map frame buffer + reset seq to 0
```

---

## 6. Errors & edge cases

| Case | Behavior |
|---|---|
| NavData JSON parse error | frame is ignored, `ble_rx_count` still increments |
| MapFrame magic ≠ 0x4D4C | dropped, `map_drop_count++` |
| MapFrame seq == seq_last (duplicate) | ignored (no drop count) |
| MapFrame seq < seq_last − 64 (stale) | dropped, `map_drop_count++` |
| MapFrame > 4 KB | dropped, `map_drop_count++` |
| connection timeout (>15 s) | STATE_IDLE, "Link lost" for 5 s, then home |
| phone disconnects | advertising restarted immediately (3 s jitter) |
