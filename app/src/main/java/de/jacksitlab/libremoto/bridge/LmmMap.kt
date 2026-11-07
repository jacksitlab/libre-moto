/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * LmmMap — LibreMoto minimalist vector map, loaded from a prebuilt .lmm file.
 *
 * WHY: the AIDL NearbyRoads path (OsmAnd route-index scan + 50 m proximity
 * filter + maxRoads cap) delivered incomplete and erratic side streets.
 * A prebuilt map contains exactly what WE want (vehicular roads, with
 * highway types), and a bbox query is a memory scan — no binder call,
 * no OsmAnd CPU load, deterministic results.
 *
 * FORMAT (.lmm, little-endian, version 1):
 *   header:
 *     4 B  magic "LMM1"
 *     4 B  u32 tileCount
 *     4 B  u32 roadCount
 *     tileCount × 16 B tile entries: (u32 tileX, u32 tileY,
 *                                     u32 firstRoadIdx, u32 endRoadIdx)
 *       tileX/tileY at zoom 14 (LmmMap.ZOOM); the roads of a tile are
 *       road table indices firstRoadIdx .. endRoadIdx-1
 *   road table (roadCount entries, variable size):
 *     1 B  highwayType (index into TYPES)
 *     1 B  npts (2..255)
 *     npts × 8 B: i32 latE7, i32 lonE7
 *
 * A road is assigned to EVERY tile its bbox overlaps → a tile query
 * returns all roads intersecting the tile, no dedup needed at load time.
 * The loader dedups by identity when a road spans several queried tiles.
 */
package de.jacksitlab.libremoto.bridge

import java.io.EOFException
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/** One street: highway type + polyline (WGS84, 1e7 fixed point). */
class LmmRoad(val highway: String, val latE7: IntArray, val lonE7: IntArray) {
    val npts: Int get() = latE7.size
}

/** Loaded .lmm map: tile index + road table. Thread-safe (immutable). */
class LmmMap private constructor(
    private val tiles: HashMap<Long, IntRange>,   // tileId → road indices
    private val roads: List<LmmRoad>,
) {
    val roadCount: Int get() = roads.size

    /** All roads whose polyline intersects the bbox (lat/lon in degrees).
     *  Tile lookup first (coarse), then an exact bbox-vs-road-bbox filter —
     *  tiles are ~1 km wide, so without the filter the query would return
     *  every road in the touched tiles. */
    fun queryBbox(minLat: Double, minLon: Double, maxLat: Double, maxLon: Double): List<LmmRoad> {
        val minTx = lonToTileX(minLon)
        val maxTx = lonToTileX(maxLon)
        val minTy = latToTileY(maxLat)   // tile Y grows south
        val maxTy = latToTileY(minLat)
        val seen = HashSet<Int>()
        val out = ArrayList<LmmRoad>(32)
        for (ty in minTy..maxTy) {
            for (tx in minTx..maxTx) {
                val range = tiles[tileId(tx, ty)] ?: continue
                for (i in range) {
                    if (!seen.add(i)) continue
                    val r = roads[i]
                    // exact filter: road's own bbox must intersect the query
                    var rMinLat = Int.MAX_VALUE; var rMaxLat = Int.MIN_VALUE
                    var rMinLon = Int.MAX_VALUE; var rMaxLon = Int.MIN_VALUE
                    for (k in 0 until r.npts) {
                        val la = r.latE7[k]; val lo = r.lonE7[k]
                        if (la < rMinLat) rMinLat = la; if (la > rMaxLat) rMaxLat = la
                        if (lo < rMinLon) rMinLon = lo; if (lo > rMaxLon) rMaxLon = lo
                    }
                    val qMinLatE7 = (minLat * 1e7).toInt(); val qMaxLatE7 = (maxLat * 1e7).toInt()
                    val qMinLonE7 = (minLon * 1e7).toInt(); val qMaxLonE7 = (maxLon * 1e7).toInt()
                    if (rMaxLat < qMinLatE7 || rMinLat > qMaxLatE7) continue
                    if (rMaxLon < qMinLonE7 || rMinLon > qMaxLonE7) continue
                    out.add(r)
                }
            }
        }
        return out
    }

    /** Does this map contain the given tile? (coverage checks) */
    fun hasTile(tx: Int, ty: Int): Boolean = tiles.containsKey(tileId(tx, ty))

    /** Coarse point-in-map check via tile coverage (coverage checks). */
    fun containsPoint(lat: Double, lon: Double): Boolean =
        hasTile(lonToTileX(lon), latToTileY(lat))

    /** Number of roads indexed in the tile containing this point — a cheap
     *  density proxy (O(1) map lookup, no bbox scan). */
    fun tileRoadCount(lat: Double, lon: Double): Int =
        tiles[tileId(lonToTileX(lon), latToTileY(lat))]?.count() ?: 0

    /** The road table (for tests/inspection — e.g. picking a test vehicle
     *  position from the map itself). */
    fun allRoads(): List<LmmRoad> = roads

    companion object {
        const val ZOOM = 14   // ~1 km × ~1 km tiles at mid latitudes
        private val TYPES = listOf(
            "motorway", "motorway_link", "trunk", "trunk_link",
            "primary", "primary_link", "secondary", "secondary_link",
            "tertiary", "tertiary_link", "residential", "unclassified",
            "living_street", "service", "road",
        )

        /** 38-bit key: both tile coords need 14 bits at zoom 14 → Long. */
        fun tileId(tx: Int, ty: Int): Long = (ty.toLong() shl 24) or tx.toLong()

        fun lonToTileX(lon: Double): Int {
            val n = 1 shl ZOOM
            return ((lon + 180.0) / 360.0 * n).toInt().coerceIn(0, n - 1)
        }

        fun latToTileY(lat: Double): Int {
            val n = 1 shl ZOOM
            val clamped = lat.coerceIn(-85.05, 85.05)
            val rad = Math.toRadians(clamped)
            val y = (1.0 - Math.log(Math.tan(rad) + 1.0 / Math.cos(rad)) / Math.PI) / 2.0
            return (y * n).toInt().coerceIn(0, n - 1)
        }

        /** Parse a .lmm stream (see format header above). */
        fun load(input: InputStream): LmmMap {
            val buf = input.readBytes()
            if (buf.size < 12) throw EOFException("lmm zu kurz")
            val bb = ByteBuffer.wrap(buf).order(ByteOrder.LITTLE_ENDIAN)
            val magic = ByteArray(4).also { bb.get(it) }.decodeToString()
            if (magic != "LMM1") throw IllegalArgumentException("kein LMM-File: $magic")
            val tileCount = bb.int
            val roadCount = bb.int
            if (tileCount < 0 || roadCount < 0) throw IllegalArgumentException("bad counts")

            // road table starts right after the tile entries
            val roadStart = 12 + tileCount * 16
            if (buf.size < roadStart) throw EOFException("lmm zu kurz (Tiles)")

            val tiles = HashMap<Long, IntRange>(tileCount)
            for (i in 0 until tileCount) {
                val off = 12 + i * 16
                val tx = readU32(buf, off)
                val ty = readU32(buf, off + 4)
                val first = readU32(buf, off + 8)
                val end = readU32(buf, off + 12)
                tiles[tileId(tx, ty)] = first until end
            }

            // road table
            val roads = ArrayList<LmmRoad>(roadCount)
            var pos = roadStart
            while (pos < buf.size) {
                val typeIdx = buf[pos].toInt() and 0xFF
                val npts = buf[pos + 1].toInt() and 0xFF
                if (typeIdx >= TYPES.size) throw IllegalArgumentException("bad type $typeIdx")
                if (pos + 2 + npts * 8 > buf.size) throw EOFException("lmm zu kurz (Roads)")
                val lat = IntArray(npts)
                val lon = IntArray(npts)
                var p = pos + 2
                for (i in 0 until npts) {
                    lat[i] = readI32(buf, p)
                    lon[i] = readI32(buf, p + 4)
                    p += 8
                }
                roads.add(LmmRoad(TYPES[typeIdx], lat, lon))
                pos = p
            }
            if (roads.size != roadCount) throw IllegalArgumentException(
                "roadCount ${roads.size} != header $roadCount")
            return LmmMap(tiles, roads)
        }

        private fun readU32(b: ByteArray, o: Int): Int =
            (b[o].toInt() and 0xFF) or ((b[o + 1].toInt() and 0xFF) shl 8) or
            ((b[o + 2].toInt() and 0xFF) shl 16) or (b[o + 3].toInt() shl 24)

        private fun readI32(b: ByteArray, o: Int): Int = readU32(b, o)
    }
}

