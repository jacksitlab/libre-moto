#!/usr/bin/env bash
# render_preview.sh — END-TO-END display preview from a .osm.pbf
#
# Runs the FULL production chain and shows what the hardware display
# would render — including every filter you want to debug:
#
#   pbf → OsmPbfReader → LmmBuilder → LmmMap → BBox-Query
#       → FrameBuilder (ranking, budget, decimation, widths)
#       → MapFrame.encode() → frame.bin
#       → FIRMWARE pipeline: parse_map_frame + prepare_map_frame
#       → PNG (firmware styles)
#
# Usage:
#   ./render_preview.sh <region.osm.pbf> [lat lon] [out.png]
#
# Defaults: the committed Liechtenstein extract, Vaduz Städtle.
set -euo pipefail

PBF="${1:-}"
LAT="${2:-47.1172}"
LON="${3:-9.5226}"
OUT="${4:-frame-preview.png}"

DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" >/dev/null 2>&1 && pwd )"
ROOT="$DIR/.."
APP="$ROOT/app"
FW="$ROOT/firmware"

if [ -n "$PBF" ]; then
  cp "$PBF" "$APP/src/test/resources/liechtenstein.osm.pbf"
  echo "Test-Fixture ersetzt durch: $PBF"
fi

echo "== 1. Kotlin: pbf → frame.bin (Produktionscode) =="
( cd "$APP" && ../scripts/gradle-test.sh testDebugUnitTest --tests '*EndToEndPreview*' ) || {
  echo "Fehler: gradle nicht gefunden — nutze ./gradlew oder passe gradle-test.sh an" >&2
  exit 1
}

echo "== 2. C++: frame.bin → PNG (echte Firmware-Geometrie) =="
g++ -std=c++11 -Wall -O2 -I"$FW/src/map" "$FW/test/render_frame.cpp" -o "$APP/build/render_frame" -lz
"$APP/build/render_frame" "$APP/build/map-preview/frame.bin" "$APP/build/map-preview/$OUT"

echo ""
echo "Fertig: $APP/build/map-preview/$OUT"
echo "Konsolen-Report der Straßenfilterung: gradle-Output oben."
