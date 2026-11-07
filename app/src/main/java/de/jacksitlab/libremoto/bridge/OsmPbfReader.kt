/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * OsmPbfReader — minimal .osm.pbf reader for LibreMoto map building.
 *
 * Reads ONLY what we need: ways tagged with a vehicular `highway=*` and
 * the coordinates of their nodes. Pure stdlib: hand-rolled varints +
 * protobuf wire parsing + zlib.
 *
 * TWO-PASS, PRIMITIVE-ARRAYS memory design (the naive HashMap approach
 * OOMed at the 256 MB tablet heap on region downloads):
 *
 *   PASS 1: stream all blocks, collect highway ways (refs as LongArray)
 *           and every referenced node id into one growable LongArray;
 *           sort + dedup it.
 *   PASS 2: re-stream the DenseNode blocks; for each node whose id is in
 *           the sorted table (binary search), store its coords in two
 *           parallel IntArrays aligned with the id table.
 *
 * Peak memory for a Bundesland (~5 M referenced nodes, ~500 k ways):
 *   ways ≈ 30 MB + id table 40 MB + coords 40 MB ≈ 110 MB — fits the
 *   256 MB app heap where a boxed HashMap needed 4-6× that.
 *
 * PBF details verified against the committed Liechtenstein extract:
 *   - BlobHeader length is a 4-byte BIG-ENDIAN int (not a varint)
 *   - DenseNodes id/lat/lon deltas AND Way.refs are sint64 ZIGZAG
 *   - latOffset/lonOffset are zigzag too
 *   - coords: 1e-9 * (offset + granularity * delta) → /100 for 1e-7
 */
package de.jacksitlab.libremoto.bridge

import java.io.BufferedInputStream
import java.io.ByteArrayInputStream
import java.io.DataInputStream
import java.io.EOFException
import java.io.InputStream
import java.util.Arrays
import java.util.zip.InflaterInputStream

/** One OSM way: highway tag + node ids (+ maxspeed km/h, 0 = none/unknown). */
class OsmWay(val id: Long, val highway: String, val refs: LongArray, val maxspeedKmh: Int = 0)

/** Scan result: ways + a primitive id→coord table (binary-searched).
 *  After LmmBuilder.build() consumed the ways, call releaseWays() — on
 *  Bundesland-sized regions the way list alone is ~65 MB and the 256 MB
 *  tablet heap cannot hold scan data AND build result at once. */
class OsmPbfResult(val ways: java.util.ArrayList<OsmWay?>, private val ids: LongArray,
                   private val lats: IntArray, private val lons: IntArray) {
    /** latE7/lonE7 of a node, or null if the pbf lacked it. */
    fun node(id: Long): Pair<Int, Int>? {
        val i = Arrays.binarySearch(ids, id)
        if (i < 0) return null
        return lats[i] to lons[i]
    }

    fun nodeCount(): Int = ids.size

    /** Free the way list (coords are resolved into the builder already).
     *  The id→coord table stays (32 MB primitives, needed until built). */
    fun releaseWays() { ways.clear() }
}

/** Growable primitive long array (no boxing). */
private class LongBag(initial: Int = 1 shl 16) {
    var data = LongArray(initial)
    var size = 0
        private set

    fun add(v: Long) {
        if (size == data.size) data = data.copyOf(data.size * 2)
        data[size++] = v
    }

    /** sort + dedup in place; returns a trimmed copy. */
    fun toSortedUnique(): LongArray {
        Arrays.sort(data, 0, size)
        var w = 0
        for (r in 0 until size) {
            if (w == 0 || data[w - 1] != data[r]) data[w++] = data[r]
        }
        return data.copyOf(w)
    }
}

object OsmPbfReader {

    /** Highway values LibreMoto renders (must match LmmMap/MapProjector). */
    val HIGHWAYS = setOf(
        "motorway", "motorway_link", "trunk", "trunk_link",
        "primary", "primary_link", "secondary", "secondary_link",
        "tertiary", "tertiary_link", "residential", "unclassified",
        "living_street", "service", "road",
    )

    /** Two-pass scan of a pbf FILE (see class doc for the memory design).
     *  A File (not a stream): pass 2 re-opens it. */
    fun scan(file: java.io.File,
             progress: ((bytesRead: Long, totalBytes: Long, ways: Int) -> Unit)? = null): OsmPbfResult =
        scan({ file.inputStream() }, progress)

    /** Two-pass scan from a stream FACTORY (called twice — must return a
     *  fresh stream over the same data each time). */
    fun scan(open: () -> InputStream,
             progress: ((bytesRead: Long, totalBytes: Long, ways: Int) -> Unit)? = null): OsmPbfResult {
        // ---- PASS 1: ways + referenced node ids ----
        val ways = ArrayList<OsmWay?>(64 * 1024)
        val refBag = LongBag(1 shl 20)
        scanBlocks(open()) { data ->
            parseBlockWays(data, ways, refBag)
            progress?.invoke(0L, 0L, ways.size)
        }

        val ids = refBag.toSortedUnique()

        // ---- PASS 2: coords for the referenced ids ----
        val lats = IntArray(ids.size)
        val lons = IntArray(ids.size)
        scanBlocks(open()) { data ->
            parseBlockNodes(data, ids, lats, lons)
        }

        return OsmPbfResult(ways, ids, lats, lons)
    }

    /** Stream all OSMData blocks, hand each decompressed block to `sink`.
     *  The caller passes a REOPENABLE stream (file), not a socket. */
    private fun scanBlocks(input: InputStream, sink: (ByteArray) -> Unit) {
        val din = DataInputStream(BufferedInputStream(input, 1 shl 16))
        while (true) {
            val hdrLen: Int
            try { hdrLen = din.readInt() } catch (e: EOFException) { break }   // 4 B big-endian
            if (hdrLen <= 0 || hdrLen > 64 * 1024) throw IllegalArgumentException("bad BlobHeader len $hdrLen")
            val hdr = ByteArray(hdrLen)
            din.readFully(hdr)
            val bh = parseBlobHeader(hdr)
            if (bh.dataSize <= 0 || bh.dataSize > 1024 * 1024 * 1024) throw IllegalArgumentException("bad Blob size")
            val blob = ByteArray(bh.dataSize)
            din.readFully(blob)
            if (bh.type == "OSMData") {
                sink(decodeBlob(blob))
            }
        }
    }

    // ---- fileformat.proto: BlobHeader ----
    private class BlobHeaderInfo(val type: String, val dataSize: Int)

    private fun parseBlobHeader(b: ByteArray): BlobHeaderInfo {
        var type = ""
        var datasize = 0
        var i = 0
        while (i < b.size) {
            val (field, wire) = readField(b, i)
            when (field) {
                1 -> { type = stringAt(b, i); i = skipField(b, i, wire) }
                3 -> { datasize = varintValue(b, i).toInt(); i = skipField(b, i, wire) }
                else -> i = skipField(b, i, wire)
            }
        }
        return BlobHeaderInfo(type, datasize)
    }

    /** fileformat.proto: Blob → raw bytes (zlib-decompressed if needed). */
    private fun decodeBlob(b: ByteArray): ByteArray {
        var raw: ByteArray? = null
        var zlib: ByteArray? = null
        var i = 0
        while (i < b.size) {
            val (field, wire) = readField(b, i)
            when (field) {
                1 -> { raw = bytesAt(b, i); i = skipField(b, i, wire) }
                3 -> { zlib = bytesAt(b, i); i = skipField(b, i, wire) }
                else -> i = skipField(b, i, wire)
            }
        }
        zlib?.let {
            return InflaterInputStream(ByteArrayInputStream(it)).readBytes()
        }
        return raw ?: ByteArray(0)
    }

    // ---- osmformat.proto: PrimitiveBlock — PASS 1 (ways) -----------------
    private fun parseBlockWays(data: ByteArray, ways: MutableList<OsmWay?>, refBag: LongBag) {
        var i = 0
        var stringTable: List<String> = emptyList()
        while (i < data.size) {
            val (field, wire) = readField(data, i)
            when (field) {
                1 -> { stringTable = parseStringTable(bytesAt(data, i)); i = skipField(data, i, wire) }
                2 -> {
                    val g = bytesAt(data, i)
                    parseGroupWays(g, stringTable, ways, refBag)
                    i = skipField(data, i, wire)
                }
                else -> i = skipField(data, i, wire)
            }
        }
    }

    private fun parseGroupWays(g: ByteArray, strings: List<String>,
                              ways: MutableList<OsmWay?>, refBag: LongBag) {
        var i = 0
        while (i < g.size) {
            val (field, wire) = readField(g, i)
            if (field == 3) {
                parseWay(bytesAt(g, i), strings, ways, refBag)
            }
            i = skipField(g, i, wire)
        }
    }

    /** Ways: collect only those with a vehicular highway tag. */
    private fun parseWay(b: ByteArray, strings: List<String>, ways: MutableList<OsmWay?>, refBag: LongBag) {
        var id = 0L
        var keys: LongArray? = null
        var vals: LongArray? = null
        var refs: LongArray? = null
        var i = 0
        while (i < b.size) {
            val (field, wire) = readField(b, i)
            when (field) {
                1 -> { id = varintValue(b, i); i = skipField(b, i, wire) }
                2 -> { keys = packedVarints(bytesAt(b, i)); i = skipField(b, i, wire) }
                3 -> { vals = packedVarints(bytesAt(b, i)); i = skipField(b, i, wire) }
                8 -> { refs = packedVarints(bytesAt(b, i)); i = skipField(b, i, wire) }
                else -> i = skipField(b, i, wire)
            }
        }
        val k = keys ?: return
        val v = vals ?: return
        var highway: String? = null
        var maxspeed: Int = 0
        for (j in k.indices) {
            if (j < v.size) {
                val key = strings.getOrNull(k[j].toInt()) ?: continue
                val value = strings.getOrNull(v[j].toInt()) ?: continue
                if (key == "highway") {
                    highway = value
                } else if (key == "maxspeed" && maxspeed == 0) {
                    /* "50", "50 kph", "zone:30" → take the plain number;
                       anything else (walk, signals, "none") = unknown (0). */
                    maxspeed = value.takeWhile { it.isDigit() }.toIntOrNull() ?: 0
                }
            }
        }
        if (highway == null || highway !in HIGHWAYS) return
        val r = refs ?: return
        if (r.size < 2) return
        // refs are ZIGZAG delta-coded (sint64)
        var ref = 0L
        val out = LongArray(r.size)
        for (j in r.indices) {
            ref += zigzag(r[j])
            out[j] = ref
            refBag.add(ref)
        }
        ways.add(OsmWay(id, highway, out, maxspeed.coerceAtMost(255)))
    }

    // ---- osmformat.proto: PrimitiveBlock — PASS 2 (node coords) -----------
    private fun parseBlockNodes(data: ByteArray, ids: LongArray,
                                lats: IntArray, lons: IntArray) {
        var i = 0
        var granularity = 100
        var latOffset = 0L
        var lonOffset = 0L
        while (i < data.size) {
            val (field, wire) = readField(data, i)
            when (field) {
                2 -> {
                    val g = bytesAt(data, i)
                    parseGroupNodes(g, granularity, latOffset, lonOffset, ids, lats, lons)
                    i = skipField(data, i, wire)
                }
                17 -> { granularity = varintValue(data, i).toInt(); i = skipField(data, i, wire) }
                19 -> { latOffset = zigzag(varintValue(data, i)); i = skipField(data, i, wire) }
                20 -> { lonOffset = zigzag(varintValue(data, i)); i = skipField(data, i, wire) }
                else -> i = skipField(data, i, wire)
            }
        }
    }

    private fun parseGroupNodes(g: ByteArray, granularity: Int, latOffset: Long, lonOffset: Long,
                                ids: LongArray, lats: IntArray, lons: IntArray) {
        var i = 0
        while (i < g.size) {
            val (field, wire) = readField(g, i)
            if (field == 2) {
                parseDenseNodes(bytesAt(g, i), granularity, latOffset, lonOffset, ids, lats, lons)
            }
            i = skipField(g, i, wire)
        }
    }

    /** DenseNodes: id/lat/lon ZIGZAG delta streams; keep only referenced ids. */
    private fun parseDenseNodes(b: ByteArray, granularity: Int, latOffset: Long, lonOffset: Long,
                                ids: LongArray, lats: IntArray, lons: IntArray) {
        var idBuf: LongArray? = null
        var latBuf: LongArray? = null
        var lonBuf: LongArray? = null
        var i = 0
        while (i < b.size) {
            val (field, wire) = readField(b, i)
            when (field) {
                1 -> { idBuf = packedVarints(bytesAt(b, i)); i = skipField(b, i, wire) }
                8 -> { latBuf = packedVarints(bytesAt(b, i)); i = skipField(b, i, wire) }
                9 -> { lonBuf = packedVarints(bytesAt(b, i)); i = skipField(b, i, wire) }
                else -> i = skipField(b, i, wire)
            }
        }
        val idArr = idBuf ?: return
        val latArr = latBuf ?: return
        val lonArr = lonBuf ?: return
        var id = 0L; var lat = 0L; var lon = 0L
        for (k in idArr.indices) {
            id += zigzag(idArr[k])
            if (k < latArr.size && k < lonArr.size) {
                lat += zigzag(latArr[k]); lon += zigzag(lonArr[k])
                val idx = Arrays.binarySearch(ids, id)
                if (idx >= 0) {
                    // OSM-PBF: coord = 1e-9 * (offset + granularity * delta)
                    // we store 1e-7 fixed point → divide by 100
                    lats[idx] = ((latOffset + granularity * lat) / 100).toInt()
                    lons[idx] = ((lonOffset + granularity * lon) / 100).toInt()
                }
            }
        }
    }

    // ---- protobuf wire helpers ------------------------------------------------
    private fun parseStringTable(b: ByteArray): List<String> {
        val out = ArrayList<String>(256)
        var i = 0
        while (i < b.size) {
            val (field, wire) = readField(b, i)
            if (field == 1) out.add(stringAt(b, i))
            i = skipField(b, i, wire)
        }
        return out
    }

    private fun readField(b: ByteArray, o: Int): Pair<Int, Int> {
        val key = varintAt(b, o)
        return (key shr 3).toInt() to (key and 0x7).toInt()
    }

    /** Value of a varint FIELD whose KEY sits at `o` (key is skipped). */
    private fun varintValue(b: ByteArray, o: Int): Long {
        val afterKey = o + varintLen(b, o)
        return varintAt(b, afterKey)
    }

    /** protobuf zigzag (sint32/sint64): (n >> 1) ^ -(n & 1). */
    private fun zigzag(v: Long): Long = (v shr 1) xor -(v and 1L)

    private fun varintAt(b: ByteArray, o: Int): Long {
        var shift = 0; var result = 0L; var p = o
        while (true) {
            val byte = b[p++].toInt() and 0xFF
            result = result or ((byte and 0x7F).toLong() shl shift)
            if (byte and 0x80 == 0) return result
            shift += 7
            if (shift > 63) throw IllegalArgumentException("varint zu lang")
        }
    }

    private fun varintLen(b: ByteArray, o: Int): Int {
        var p = o
        while (true) {
            val byte = b[p++].toInt() and 0xFF
            if (byte and 0x80 == 0) return p - o
        }
    }

    /** Payload of a length-delimited field. `o` points at the KEY varint
     *  (field<<3 | wire); the length varint follows it. */
    private fun bytesAt(b: ByteArray, o: Int): ByteArray {
        val afterKey = o + varintLen(b, o)
        val len = varintAt(b, afterKey).toInt()
        return b.copyOfRange(afterKey + varintLen(b, afterKey), afterKey + varintLen(b, afterKey) + len)
    }

    private fun stringAt(b: ByteArray, o: Int): String {
        val raw = bytesAt(b, o)
        return raw.decodeToString()
    }

    private fun skipField(b: ByteArray, o: Int, wire: Int): Int {
        val afterKey = o + varintLen(b, o)
        return when (wire) {
            0 -> afterKey + varintLen(b, afterKey)
            1 -> afterKey + 8
            2 -> afterKey + varintLen(b, afterKey) + varintAt(b, afterKey).toInt()
            5 -> afterKey + 4
            else -> throw IllegalArgumentException("wire $wire")
        }
    }

    private fun packedVarints(b: ByteArray): LongArray {
        val out = ArrayList<Long>(b.size / 2 + 8)
        var i = 0
        while (i < b.size) {
            out.add(varintAt(b, i))
            i += varintLen(b, i)
        }
        return out.toLongArray()
    }
}
