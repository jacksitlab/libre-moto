/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * LmmBuilder — converts an OsmPbfReader result into a .lmm map file.
 *
 * Same format LmmMap.load() reads (see LmmMap.kt header): tile-indexed at
 * zoom 14, 1e7 fixed-point polylines, highway types as byte indices.
 *
 * Ways whose nodes are missing from the block-local node map are dropped
 * (cross-block references are rare in Geofabrik extracts; dropping keeps
 * the builder streaming and memory-bounded).
 */
package de.jacksitlab.libremoto.bridge

import java.io.ByteArrayOutputStream

object LmmBuilder {

    /** Max points per street in the .lmm format (npts is 1 byte). */
    private const val MAX_NPTS = 255

    private val TYPES = listOf(
        "motorway", "motorway_link", "trunk", "trunk_link",
        "primary", "primary_link", "secondary", "secondary_link",
        "tertiary", "tertiary_link", "residential", "unclassified",
        "living_street", "service", "road",
    )
    private val TYPE_IDX = TYPES.withIndex().associate { (i, t) -> t to i }

    /** Types WRITTEN into new .lmm maps. "service" is deliberately NOT
     *  built: service roads (driveways, parking aisles) are 76% of the
     *  candidates at urban spots but never win a render slot — they only
     *  bloat map size, query and selection. The TYPE INDEX stays stable
     *  (service keeps slot 13 in the format) so OLD maps with service
     *  roads still decode correctly; they just hold data the display
     *  would never show. */
    private val BUILD_TYPES = TYPES - "service"

    /** Merge way chains: ways sharing an endpoint (same road type) become one
     *  continuous street. A shared endpoint used by 3+ ways is a junction —
     *  no merging there (the ways really do end at the intersection).
     *  Direction-insensitive: [A..B] + [B..C] → [A..B..C]; [A..B] + [C..B]
     *  → [A..B..C] reversed as needed.
     *
     *  Memory: PRIMITIVE arrays only (LongArray endpoint keys, index sort,
     *  no boxing) — a HashMap-based version OOMed on Brandenburg (~400k
     *  ways) under the 256 MB tablet heap.
     *  Chain length is capped by POINTS (≤ MAX_NPTS) so no merged street
     *  ever exceeds the format's npts byte — truncation would silently
     *  drop the far end of long streets. */
    private fun mergeWays(roads: List<RoadT>): List<RoadT> {
        val n = roads.size
        if (n < 2) return roads

        // endpoint key: 1e7 fixed point coords packed into a Long
        fun key(lat: Int, lon: Int): Long = (lat.toLong() shl 32) or (lon.toLong() and 0xFFFFFFFFL)

        // 2n endpoint keys: slot 2*i = start of road i, slot 2*i+1 = end
        val keys = LongArray(n * 2)
        for (i in 0 until n) {
            val r = roads[i]
            keys[2 * i] = key(r.lat.first(), r.lon.first())
            keys[2 * i + 1] = key(r.lat.last(), r.lon.last())
        }

        // sort slot indices by key (index quicksort, no boxing)
        val order = IntArray(n * 2) { it }
        sortByKey(order, keys)

        // link partners: a key occurring on EXACTLY two slots of two DIFFERENT
        // roads connects them; 1 occurrence = dead end, 3+ = junction,
        // both slots on the same road = closed loop → no link
        val partner = IntArray(n * 2) { -1 }
        var i = 0
        while (i < order.size) {
            var j = i
            while (j < order.size && keys[order[j]] == keys[order[i]]) j++
            if (j - i == 2) {
                val s1 = order[i]; val s2 = order[i + 1]
                if (s1 ushr 1 != s2 ushr 1) {   // different roads
                    partner[s1] = s2; partner[s2] = s1
                }
            }
            i = j
        }

        // walk chains via partner links
        val used = BooleanArray(n)
        val out = ArrayList<RoadT>(n)
        val chainIdx = ArrayList<Int>()      // road indices, chain order
        val chainRev = ArrayList<Boolean>()  // orientation of each road
        val chainLat = ArrayList<Int>()
        val chainLon = ArrayList<Int>()

        for (startRoad in 0 until n) {
            if (used[startRoad]) continue
            used[startRoad] = true
            chainIdx.clear(); chainRev.clear()
            chainIdx.add(startRoad); chainRev.add(false)
            var pts = roads[startRoad].lat.size

            // extend at the END: slot 2*r+1 is the open end of the chain head
            var curSlot = 2 * startRoad + 1
            while (pts < MAX_NPTS) {
                val p = partner[curSlot]
                if (p < 0) break   // dead end, junction or loop
                val ri = p ushr 1
                if (used[ri]) break
                if (roads[ri].typeIdx != roads[chainIdx.last()].typeIdx) break
                if (pts - 1 + roads[ri].lat.size > MAX_NPTS) break   // keep it whole
                used[ri] = true
                chainIdx.add(ri)
                chainRev.add(p and 1 == 1)   // arrived at ri's END slot → reversed
                pts += roads[ri].lat.size - 1
                curSlot = if (p and 1 == 0) 2 * ri + 1 else 2 * ri
            }
            // extend at the START: slot 2*r is the open start of the chain head
            curSlot = 2 * startRoad
            while (pts < MAX_NPTS) {
                val p = partner[curSlot]
                if (p < 0) break
                val ri = p ushr 1
                if (used[ri]) break
                if (roads[ri].typeIdx != roads[chainIdx.first()].typeIdx) break
                if (pts - 1 + roads[ri].lat.size > MAX_NPTS) break
                used[ri] = true
                chainIdx.add(0, ri)
                chainRev.add(0, p and 1 == 0)   // arrived at ri's START slot → reversed
                pts += roads[ri].lat.size - 1
                curSlot = if (p and 1 == 0) 2 * ri + 1 else 2 * ri
            }

            // materialize the chain
            chainLat.clear(); chainLon.clear()
            for (ci in chainIdx.indices) {
                val r = roads[chainIdx[ci]]
                if (chainRev[ci]) {
                    for (k in r.lat.indices.reversed()) { chainLat.add(r.lat[k]); chainLon.add(r.lon[k]) }
                } else {
                    for (k in r.lat.indices) { chainLat.add(r.lat[k]); chainLon.add(r.lon[k]) }
                }
            }
            // drop duplicated joint points (each link point appears twice)
            var w = 1
            for (k in 1 until chainLat.size) {
                if (chainLat[k] == chainLat[w - 1] && chainLon[k] == chainLon[w - 1]) continue
                chainLat[w] = chainLat[k]; chainLon[w] = chainLon[k]; w++
            }
            out.add(RoadT(roads[startRoad].typeIdx,
                chainLat.toIntArray().copyOf(w), chainLon.toIntArray().copyOf(w)))
        }
        return out
    }

    /** Index quicksort: order[] sorted by keys[order[]] — no boxing. */
    private fun sortByKey(order: IntArray, keys: LongArray) {
        val stack = IntArray(64)
        var sp = 0
        stack[sp++] = 0; stack[sp++] = order.size - 1
        while (sp > 0) {
            val hi = stack[--sp]; val lo = stack[--sp]
            if (lo >= hi) continue
            // median-of-three pivot
            val mid = (lo + hi) ushr 1
            if (keys[order[mid]] < keys[order[lo]]) { val t = order[mid]; order[mid] = order[lo]; order[lo] = t }
            if (keys[order[hi]] < keys[order[mid]]) { val t = order[hi]; order[hi] = order[mid]; order[mid] = t }
            if (keys[order[mid]] < keys[order[lo]]) { val t = order[mid]; order[mid] = order[lo]; order[lo] = t }
            val pivot = keys[order[mid]]
            var a = lo; var b = hi
            while (a <= b) {
                while (keys[order[a]] < pivot) a++
                while (keys[order[b]] > pivot) b--
                if (a <= b) {
                    val t = order[a]; order[a] = order[b]; order[b] = t
                    a++; b--
                }
            }
            if (lo < b) { stack[sp++] = lo; stack[sp++] = b }
            if (a < hi) { stack[sp++] = a; stack[sp++] = hi }
        }
    }

    /** Internal road type for merging (typeIdx + coords). */
    data class RoadT(val typeIdx: Int, val lat: IntArray, val lon: IntArray)

    data class Built(val bytes: ByteArray, val roadCount: Int, val tileCount: Int, val droppedIncomplete: Int)

    /** Build a .lmm byte blob from a pbf scan result. Frees the scan data
     *  (ways) as it goes: on Bundesland-sized regions scan data AND build
     *  result together exceed the 256 MB tablet heap. */
    fun build(res: OsmPbfResult): Built {
        // resolve way → polyline; drop ways with missing node coords.
        // Consume res.ways destructively (null as we go) so the heap
        // doesn't hold ways AND coords at once.
        val rawRoads = ArrayList<RoadT>(res.ways.size)
        var dropped = 0
        val ways = res.ways
        var wi = 0
        while (wi < ways.size) {
            val w = ways[wi] ?: break   // null = already consumed (defensive)
            ways.set(wi, null)   // release early — the RoadT holds the coords now
            wi++
            if (w.highway !in BUILD_TYPES) continue   // e.g. service — never rendered
            val typeIdx = TYPE_IDX[w.highway] ?: continue
            val lat = IntArray(w.refs.size)
            val lon = IntArray(w.refs.size)
            var ok = true
            for (i in w.refs.indices) {
                val n = res.node(w.refs[i])
                if (n == null) { ok = false; break }
                lat[i] = n.first; lon[i] = n.second
            }
            if (!ok) { dropped++; continue }
            if (w.refs.size < 2) continue
            rawRoads.add(RoadT(typeIdx, lat, lon))
        }
        res.releaseWays()

        // ---- WAY-MERGING: OSM splits streets into short ways between
        // intersections (Berlin: avg 4 points/way). A chain of ways sharing
        // an endpoint (and road type) is ONE street — merge it so the map
        // (and the display) shows continuous streets.
        // Only merge chains (each endpoint used at most twice overall);
        // junction nodes (3+ ways) stay split — they ARE intersections.
        val roads = mergeWays(rawRoads)
        rawRoads.clear()   // merged roads hold copies; free the originals

        // road table size (exact, for the final blob allocation)
        var roadBytes = 0
        for (r in roads) roadBytes += 2 + r.lat.size * 8

        // tile index: every road into every tile its bbox overlaps.
        // Primitive arrays (tile key packed Long → index sort) — the
        // HashMap<Pair, MutableList> version OOMed on Brandenburg.
        // The road table KEEPS its original order; each tile stores
        // [minRoadIdx, maxRoadIdx+1) — the loader dedups via a seen-set
        // (LmmMap.load/queryBbox), so ranges may overlap.
        val n = roads.size
        // count tile entries first (a road can span several tiles)
        var tc = 0
        for (r in roads) {
            val tx0 = LmmMap.lonToTileX(r.lon.min() / 1e7)
            val tx1 = LmmMap.lonToTileX(r.lon.max() / 1e7)
            val ty0 = LmmMap.latToTileY(r.lat.max() / 1e7)
            val ty1 = LmmMap.latToTileY(r.lat.min() / 1e7)
            tc += (tx1 - tx0 + 1) * (ty1 - ty0 + 1)
        }
        val tKeys = LongArray(tc)
        val tRoad = IntArray(tc)
        var w2 = 0
        for (ri in 0 until n) {
            val r = roads[ri]
            val tx0 = LmmMap.lonToTileX(r.lon.min() / 1e7)
            val tx1 = LmmMap.lonToTileX(r.lon.max() / 1e7)
            val ty0 = LmmMap.latToTileY(r.lat.max() / 1e7)
            val ty1 = LmmMap.latToTileY(r.lat.min() / 1e7)
            for (ty in ty0..ty1) for (tx in tx0..tx1) {
                tKeys[w2] = (tx.toLong() shl 32) or ty.toLong()
                tRoad[w2] = ri
                w2++
            }
        }
        // sort entries by tile key — one tile's entries become adjacent
        val order = IntArray(tc) { it }
        sortByKey(order, tKeys)

        // final blob: header + tile table + road table
        var tileCount = 0
        var i2 = 0
        while (i2 < tc) {
            var j = i2
            while (j < tc && tKeys[order[j]] == tKeys[order[i2]]) j++
            tileCount++
            i2 = j
        }
        val out = ByteArray(12 + tileCount * 16 + roadBytes)
        var op = 0
        fun putU32(v: Int) {
            out[op++] = (v and 0xFF).toByte(); out[op++] = ((v shr 8) and 0xFF).toByte()
            out[op++] = ((v shr 16) and 0xFF).toByte(); out[op++] = ((v shr 24) and 0xFF).toByte()
        }
        "LMM1".toByteArray().forEach { out[op++] = it }
        putU32(tileCount)
        putU32(n)
        // tile table: (tx, ty, firstRoadIdx, endRoadIdx) with min/max of the
        // tile's road indices — the loader dedups overlapping ranges
        i2 = 0
        while (i2 < tc) {
            var j = i2
            while (j < tc && tKeys[order[j]] == tKeys[order[i2]]) j++
            var mn = Int.MAX_VALUE; var mx = Int.MIN_VALUE
            for (e in i2 until j) {
                val ri = tRoad[order[e]]
                if (ri < mn) mn = ri
                if (ri > mx) mx = ri
            }
            putU32((tKeys[order[i2]] shr 32).toInt())   // tx
            putU32(tKeys[order[i2]].toInt())            // ty
            putU32(mn)                                 // firstRoadIdx
            putU32(mx + 1)                             // endRoadIdx (exclusive)
            i2 = j
        }
        // road table in ORIGINAL order (indices match the ranges above)
        var rp = op
        for (ri in 0 until n) {
            val r = roads[ri]
            out[rp++] = r.typeIdx.toByte()
            val npts = r.lat.size.coerceAtMost(MAX_NPTS)
            out[rp++] = npts.toByte()
            for (i in 0 until npts) {
                writeI32(out, rp, r.lat[i]); rp += 4
                writeI32(out, rp, r.lon[i]); rp += 4
            }
        }
        return Built(out, n, tileCount, dropped)
    }

    private fun writeI32(buf: ByteArray, off: Int, v: Int) {
        buf[off] = (v and 0xFF).toByte(); buf[off + 1] = ((v shr 8) and 0xFF).toByte()
        buf[off + 2] = ((v shr 16) and 0xFF).toByte(); buf[off + 3] = ((v shr 24) and 0xFF).toByte()
    }
}
