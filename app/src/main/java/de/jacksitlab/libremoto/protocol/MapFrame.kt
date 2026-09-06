/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * MapData — binary frame encoder, protocol.md §4 (little-endian).
 * Byte-exact mirror of the firmware writer (map_put_seg / map_send_demo
 * in libre-moto.ino): first point absolute, the rest delta-encoded.
 */
package de.jacksitlab.libremoto.protocol

/** Segment kinds (protocol.md §4). */
enum class SegmentType(val id: Byte) {
    Route(1),
    Road(2),
    Branch(3),
    Destination(4),
}

/** One segment: `type`, `width` px, and `npts` screen-relative points.
 * Points are vehicle-relative in the firmware viewport (±260 px window). */
data class Segment(
    val type: SegmentType,
    val width: Int,           // 1..16 px (route: line width, road: edge distance)
    val points: List<Point>,// >= 2, <= 120; absolute screen coords, first point at vehicle (0,0) expected
)

data class MapFrame(
    val seq: Int,             // 0..255, monotonic, wrap
    val flags: Int,           // bit0 rotate, bit1 scale bar, bit2 vehicle arrow
    val headingDeg10: Int,    // degrees × 10, i16 range
    val scalePxPerMeter100: Int, // px_per_m × 100
    val segments: List<Segment>, // 1..30
) {
    companion object {
        const val MAGIC = 0x4D4C
        const val VER = 1
        const val MAX_SEGS = 30
        const val MAX_PTS = 120
        const val MAX_WIDTH = 16
        const val MAX_FRAME = 4096
        const val FLAG_ROTATE = 1
        const val FLAG_SCALE_BAR = 2
        const val FLAG_VEHICLE = 4
    }

    fun encode(): ByteArray {
        require(segments.size in 1..MAX_SEGS) { "seg_count=${segments.size}" }
        require(seq in 0..255) { "seq=$seq" }
        val out = ByteArray(11 + segments.sumOf { 3 + it.points.size * 4 })
        var o = 0
        out[o++] = (MAGIC and 0xFF).toByte(); out[o++] = ((MAGIC shr 8) and 0xFF).toByte()
        out[o++] = VER.toByte()
        out[o++] = seq.toByte()
        out[o++] = (flags and 0xFF).toByte(); out[o++] = ((flags shr 8) and 0xFF).toByte()
        appendI16(out, o, headingDeg10); o += 2
        out[o++] = (scalePxPerMeter100 and 0xFF).toByte(); out[o++] = ((scalePxPerMeter100 shr 8) and 0xFF).toByte()
        out[o++] = segments.size.toByte()
        var px = 0; var py = 0
        for (seg in segments) {
            require(seg.type.id in 1..4) { "bad seg type" }
            require(seg.width in 1..MAX_WIDTH) { "width=${seg.width}" }
            require(seg.points.size in 2..MAX_PTS) { "npts=${seg.points.size}" }
            out[o++] = seg.type.id
            out[o++] = seg.width.toByte()
            out[o++] = seg.points.size.toByte()
            seg.points.forEachIndexed { i, p ->
                require(p.x in -32768..32767 && p.y in -32768..32767) { "coord out of i16" }
                val dx = if (i == 0) p.x else p.x - px
                val dy = if (i == 0) p.y else p.y - py
                require(dx in -32768..32767 && dy in -32768..32767) { "delta wraps i16: ($dx,$dy)" }
                px = p.x; py = p.y
                appendI16(out, o, dx); o += 2
                appendI16(out, o, dy); o += 2
            }
        }
        require(o == out.size) { "encode length mismatch" }
        require(out.size <= MAX_FRAME) { "frame ${out.size} B > ${MAX_FRAME} B" }
        return out
    }

    private fun appendI16(buf: ByteArray, o: Int, v: Int) {
        buf[o] = (v and 0xFF).toByte()
        buf[o + 1] = ((v shr 8) and 0xFF).toByte()
    }
}

/** Split a frame for BLE Write-Without-Response (protocol.md §4):
 * successive chunks of at most `chunk` bytes; the firmware reassembles
 * within 500 ms. */
fun MapFrame.chunks(chunk: Int): List<ByteArray> {
    val b = encode()
    require(chunk >= 20) { "chunk too small for BLE ($chunk)" }
    return (0 until b.size step chunk).map { b.copyOfRange(it, minOf(it + chunk, b.size)) }
}
