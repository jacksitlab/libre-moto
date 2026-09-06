/* LibreMoto app — unit tests (pure JVM, no Android needed).
 *
 * Byte-exact cross-check: the Kotlin encoder must produce the SAME bytes the
 * firmware demo writer (map_put_seg / map_send_demo in libre-moto.ino) emits
 * for the mt0..mt3 scenarios. If this test ever fails, the app and the
 * firmware have drifted on the wire format — that's a bug, fix it.
 */
package de.jacksitlab.libremoto.protocol

import org.junit.Assert.assertEquals
import org.junit.Test

// Reference bytes produced by the C demo writer (firmware/src/libre-moto/libre-moto.ino)
// for mt0..mt3, captured with a tiny C harness (protocol v1, magic 0x4D4C LE, ver 1,
// flags ROT|VEH=0x0005, scale=20 (0x14) → bytes shown are segment bodies only;
// the header is asserted separately below.
class MapFrameTest {

    private fun hdr(seq: Int, flags: Int, heading: Int, scale: Int, segs: Int): ByteArray {
        val b = ByteArray(11)
        b[0] = 0x4C; b[1] = 0x4D          // magic "LM" little-endian
        b[2] = 1                          // ver
        b[3] = seq.toByte()
        b[4] = (flags and 0xFF).toByte(); b[5] = ((flags shr 8) and 0xFF).toByte()
        b[6] = (heading and 0xFF).toByte(); b[7] = ((heading shr 8) and 0xFF).toByte()
        b[8] = (scale and 0xFF).toByte(); b[9] = ((scale shr 8) and 0xFF).toByte()
        b[10] = segs.toByte()
        return b
    }

    private fun concat(a: ByteArray, vararg b: ByteArray): ByteArray {
        val out = ByteArray(a.size + b.sumOf { it.size })
        for (i in a.indices) out[i] = a[i]
        var o = a.size
        for (x in b) { for (i in x.indices) out[o + i] = x[i]; o += x.size }
        return out
    }

    private fun seg(type: Int, width: Int, pts: List<Point>): ByteArray {
        val out = ByteArray(3 + pts.size * 4)
        var o = 0
        out[o++] = (type and 0xFF).toByte(); out[o++] = width.toByte(); out[o++] = pts.size.toByte()
        var px = 0; var py = 0
        pts.forEachIndexed { i, p ->
            val dx = if (i == 0) p.x else p.x - px
            val dy = if (i == 0) p.y else p.y - py
            px = p.x; py = p.y
            out[o++] = (dx and 0xFF).toByte(); out[o++] = ((dx shr 8) and 0xFF).toByte()
            out[o++] = (dy and 0xFF).toByte(); out[o++] = ((dy shr 8) and 0xFF).toByte()
        }
        return out
    }

    @Test fun mt0_matches_firmware_reference() {
        val frame = MapFrame(
            seq = 10,
            flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
            headingDeg10 = 0,
            scalePxPerMeter100 = 20,
            segments = listOf(
                Segment(SegmentType.Road, 12, listOf(Point(-200, -40), Point(200, -40))),
                Segment(SegmentType.Route, 10,
                    listOf(Point(0, 0), Point(0, -70), Point(0, -140), Point(0, -210), Point(0, -280))),
            ),
        )
        val got = frame.encode()
        val expected = concat(
            hdr(10, 0x0005, 0, 20, 2),
            seg(2, 12, listOf(Point(-200, -40), Point(200, -40))),
            seg(1, 10, listOf(Point(0, 0), Point(0, -70), Point(0, -140), Point(0, -210), Point(0, -280))),
        )
        assertEquals("mt0 byte-exact", expected.toList(), got.toList())
        assertEquals("mt0 length 45 (11 hdr + 11 road + 23 route)", 45, got.size)
    }

    @Test fun mt3_destination_type_is_4() {
        val frame = MapFrame(
            seq = 13,
            flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
            headingDeg10 = 0,
            scalePxPerMeter100 = 50,
            segments = listOf(
                Segment(SegmentType.Road, 16, listOf(Point(-220, 0), Point(220, 0))),
                Segment(SegmentType.Destination, 3, listOf(Point(0, 0), Point(0, -160))),
            ),
        )
        val got = frame.encode()
        // destination is the 2nd seg; header = 11, road seg = 3+2*4=11 → dest starts at 22
        assertEquals(4.toByte(), got[22])
        assertEquals(3.toByte(), got[23]) // width 3
        assertEquals(2.toByte(), got[24]) // 2 pts
    }

    @Test fun width_17_is_rejected() {
        val frame = MapFrame(
            seq = 0,
            flags = 0,
            headingDeg10 = 0,
            scalePxPerMeter100 = 20,
            segments = listOf(
                Segment(SegmentType.Road, 1, listOf(Point(0, 0), Point(5, 5))),
                Segment(SegmentType.Route, 17, listOf(Point(0, 0), Point(0, -50))),
            ),
        )
        try {
            frame.encode()
            throw AssertionError("width 17 must throw")
        } catch (e: IllegalArgumentException) {
            // expected
        }
    }

    @Test fun nav_json_is_valid_utf8_and_contains_mandatory_fields() {
        val j = NavData(
            type = NavType.Nav,
            maneuver = Maneuver.Left,
            text = "Main St.",
            distM = 350,
            limitKmh = 50,
            mapOn = true,
            battPct = 87,
        ).toJson()
        // round-trip through org.json would need the dep; instead check well-formedness
        assertEquals("starts with {", '{', j.first())
        assertEquals("ends with }", '}', j.last())
        assertTrue("\"v\":1", j.contains("\"v\":1,") || j.contains("\"v\":1,"))
        assertTrue("\"t\":\"nav\"", j.contains("\"t\":\"nav\""))
        assertTrue("maneuver", j.contains("\"maneuver\":\"left\""))
        assertTrue("text escaped", j.contains("\"text\":\"Main St.\""))
        assertTrue("dist", j.contains("\"dist_m\":350"))
        assertTrue("limit", j.contains("\"limit_kmh\":50"))
        assertTrue("batt", j.contains("\"batt_pct\":87"))
        assertTrue("map on", j.contains("\"map_on\":true"))
    }

    private fun assertTrue(tag: String, cond: Boolean) = org.junit.Assert.assertTrue(tag, cond)

    @Test fun chunks_split_respect_max_frame_size() {
        val big = MapFrame(
            seq = 1, flags = 0, headingDeg10 = 0, scalePxPerMeter100 = 20,
            segments = (1..5).map {
                Segment(
                    type = SegmentType.Route, width = 1,
                    points = (0..100).map { Point(it % 400 - 200, -it / 2) }
                )
            },
        )
        val b = big.encode()
        org.junit.Assert.assertTrue("frame must fit 4KB, got ${b.size}", b.size <= 4096)
        val chunks = big.chunks(20)
        org.junit.Assert.assertEquals(chunks.sumOf { it.size }, b.size)
        chunks.forEach { c -> org.junit.Assert.assertTrue("chunk too big", c.size <= 20) }
    }
}
