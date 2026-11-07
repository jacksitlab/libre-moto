#!/usr/bin/env python3
"""
make_lmm_map.py — LibreMoto minimalist vector map generator.

Lädt Straßen aus der OSM Overpass-API für eine BBox (oder ein Bundesland/
Land via Geofabrik-Extract), filtert die für LibreMoto relevanten
Straßen und schreibt ein kompaktes .lmm-File (siehe LmmMap.kt Format-
Header), das die App in Sekunden lädt und zur Laufzeit ohne OsmAnd
abfragt.

Usage:
    ./make_lmm_map.py --bbox minLon,minLat,maxLon,maxLat --out berlin.lmm
    ./make_lmm_map.py --pbf brandenburg-latest.osm.pbf --out brandenburg.lmm

Ohne --pbf wird die Overpass-API genutzt (kleine BBoxes bis ~50 km Kante;
für mehr Geofabrik .osm.pbf herunterladen und --pbf angeben).
"""
import argparse
import io
import struct
import sys
import urllib.request
import zlib

TYPES = [
    "motorway", "motorway_link", "trunk", "trunk_link",
    "primary", "primary_link", "secondary", "secondary_link",
    "tertiary", "tertiary_link", "residential", "unclassified",
    "living_street", "service", "road",
]
TYPE_IDX = {t: i for i, t in enumerate(TYPES)}
ZOOM = 14  # must match LmmMap.ZOOM

OVERPASS_URL = "https://overpass-api.de/api/interpreter"


def lon_to_tile_x(lon: float) -> int:
    n = 1 << ZOOM
    return max(0, min(n - 1, int((lon + 180.0) / 360.0 * n)))


def lat_to_tile_y(lat: float) -> int:
    import math
    n = 1 << ZOOM
    lat = max(-85.05, min(85.05, lat))
    rad = math.radians(lat)
    y = (1.0 - math.log(math.tan(rad) + 1.0 / math.cos(rad)) / math.pi) / 2.0
    return max(0, min(n - 1, int(y * n)))


def fetch_overpass(bbox):
    min_lon, min_lat, max_lon, max_lat = bbox
    q = f"""
[out:json][timeout:180];
way["highway"~"^({('|').join(TYPES)})$"]({min_lat},{min_lon},{max_lat},{max_lon});
out geom;
"""
    data = urllib.request.urlopen(
        urllib.request.Request(OVERPASS_URL, q.encode(),
                                headers={"User-Agent": "LibreMoto-mapgen/1.0"}),
        timeout=300).read()
    import json
    return json.loads(data).get("elements", [])


def fetch_pbf(path):
    """Minimal .osm.pbf way reader: parses dense geometry via osmium if
    available, else instructs the user. Returns list of dicts like Overpass."""
    try:
        import osmium
    except ImportError:
        sys.exit("--pbf benötigt pyosmium: pip install osmium (oder Overpass mit --bbox nutzen)")
    roads = []
    for way in osmium.FileProcessor(path).with_locations().with_filter(
            osmium.filter.KeyFilter("highway")):
        if way.tags().get("highway") not in TYPE_IDX:
            continue
        pts = [(n.lat, n.lon) for n in way.nodes]
        if len(pts) < 2:
            continue
        roads.append({"type": "way", "tags": {"highway": way.tags().get("highway")},
                      "geometry": [{"lat": la, "lon": lo} for la, lo in pts]})
    return roads


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bbox", help="minLon,minLat,maxLon,maxLat")
    ap.add_argument("--pbf", help="Geofabrik .osm.pbf Extract")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    if args.pbf:
        elements = fetch_pbf(args.pbf)
        src = args.pbf
    elif args.bbox:
        bbox = [float(x) for x in args.bbox.split(",")]
        elements = fetch_overpass(bbox)
        src = f"Overpass {args.bbox}"
    else:
        ap.error("--bbox oder --pbf nötig")

    roads = []
    for el in elements:
        if el.get("type") != "way":
            continue
        hw = el.get("tags", {}).get("highway")
        if hw not in TYPE_IDX:
            continue
        pts = [(g["lat"], g["lon"]) for g in el.get("geometry", [])]
        if len(pts) < 2:
            continue
        roads.append((TYPE_IDX[hw], pts))

    print(f"{src}: {len(roads)} Straßen")
    if not roads:
        sys.exit("keine Straßen — BBox/Filter prüfen")

    # --- road table (variable size) ---
    road_blob = io.BytesIO()
    road_spans = []
    for type_idx, pts in roads:
        start = road_blob.tell()
        npts = len(pts)
        assert 2 <= npts <= 255, f"way mit {npts} Punkten"
        road_blob.write(struct.pack("<BB", type_idx, npts))
        for lat, lon in pts:
            road_blob.write(struct.pack("<ii", int(round(lat * 1e7)), int(round(lon * 1e7))))
        road_spans.append((start, road_blob.tell()))

    # --- tile index: every road into every tile its bbox overlaps ---
    tile_roads = {}
    for ri, (_t, pts) in enumerate(roads):
        lats = [p[0] for p in pts]; lons = [p[1] for p in pts]
        tx0, tx1 = lon_to_tile_x(min(lons)), lon_to_tile_x(max(lons))
        ty0, ty1 = lat_to_tile_y(max(lats)), lat_to_tile_y(min(lats))
        for ty in range(ty0, ty1 + 1):
            for tx in range(tx0, tx1 + 1):
                tile_roads.setdefault((tx, ty), []).append(ri)

    tiles = sorted(tile_roads.items())
    print(f"{len(tiles)} Kacheln (Zoom {ZOOM})")

    out = io.BytesIO()
    out.write(b"LMM1")
    out.write(struct.pack("<II", len(tiles), len(roads)))
    # tile entries: (tileX, tileY, firstRoadIdx, endRoadIdx) — 16 B each
    for (tx, ty), rl in tiles:
        out.write(struct.pack("<IIII", tx, ty, rl[0], rl[-1] + 1))
    out.write(road_blob.getvalue())

    data = out.getvalue()
    with open(args.out, "wb") as f:
        f.write(data)
    print(f"OK: {args.out} ({len(data)/1024:.0f} KB, {len(roads)} Straßen, {len(tiles)} Kacheln)")


if __name__ == "__main__":
    main()
