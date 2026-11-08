/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * MapProjectorTest — renders what the hardware display would show, without
 * any hardware. Uses exactly the production projection (MapProjector), the
 * firmware's edge-offset for roads (map_render_impl.h pass 1a) and its render
 * styles (libre-moto.ino):
 *
 *   ROAD  → two 2 px white edge lines, ±width/2 px around the centre line
 *           (dark middle) — "black line with white border"
 *   ROUTE → fat white stroke drawn ON TOP of the roads, slightly narrower
 *           than a road's edge distance — "white line lying on the road"
 *
 * Scenario: a real-world-style crossing — the route runs north on a
 * residential street, a parallel residential street to the east, a
 * secondary cross street ahead, a service turn candidate, a dead-end
 * alley behind, and a far motorway link outside the window.
 *
 * Output per zoom level: console segment listing + PNG previews under
 * build/map-preview/ (pure JVM, hand-rolled PNG writer).
 *
 * Run:   ./gradlew testDebugUnitTest --tests '*MapProjector*'
 * Find:  app/build/map-preview/map-*.png
 */
package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.MapFrame
import de.jacksitlab.libremoto.protocol.Point
import de.jacksitlab.libremoto.protocol.Segment
import de.jacksitlab.libremoto.protocol.SegmentType
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.io.FileOutputStream
import java.util.zip.CRC32

data class Road(val name: String, val highway: String, val points: List<LatLon>)

class MapProjectorTest {

    // ---- scenario "Innenstadt-Kreuzung" (near Berlin, WGS84) -------------
    private val vehicle = LatLon(52.52000, 13.40500)

    /** Metres north/east relative to the vehicle → lat/lon. */
    private fun at(northM: Double, eastM: Double): LatLon {
        val lat = vehicle.latitude + northM / MapProjector.METERS_PER_DEG_LAT
        val lon = vehicle.longitude + eastM / (MapProjector.METERS_PER_DEG_LAT *
                kotlin.math.cos(Math.toRadians(vehicle.latitude)))
        return LatLon(lat, lon)
    }

    private fun line(name: String, highway: String, vararg pts: Pair<Double, Double>) =
        Road(name, highway, pts.map { at(it.first, it.second) })

    private val scenario: List<Road> = listOf(
        // THE ROUTE: straight north on a residential street, then a right bend
        Road("Route (main street)", "residential", listOf(
            at(0.0, 0.0), at(100.0, 0.0), at(200.0, 0.0), at(300.0, 0.0), at(400.0, 0.0),
            at(450.0, 40.0), at(500.0, 120.0), at(550.0, 220.0), at(600.0, 300.0),
        )),
        // parallel residential street, 120 m east
        line("side street east", "residential", 600.0 to 120.0, 300.0 to 120.0, 0.0 to 120.0, -300.0 to 120.0),
        // secondary cross street 200 m ahead
        line("cross street", "secondary", 200.0 to -400.0, 200.0 to -150.0, 200.0 to 0.0, 200.0 to 200.0, 200.0 to 400.0),
        // service road turn candidate, 80 m ahead
        line("Abbiegung Ost", "service", 80.0 to 0.0, 80.0 to 60.0, 80.0 to 150.0),
        // dead-end alley behind-left
        line("Sackgasse West", "living_street", -60.0 to -20.0, -60.0 to -80.0),
        // motorway link — mostly OUTSIDE the ±260 px window at city zoom → clamped
        line("Autobahnzufahrt", "motorway_link", 700.0 to 500.0, 1000.0 to 800.0, 1300.0 to 1200.0),
    )

    private val zooms = listOf(
        48 to "Stadt",
        30 to "Uebergang",
        24 to "Landstrasse",
    )

    @Test
    fun projectAndRenderScenario() {
        val outDir = File("build/map-preview")
        outDir.mkdirs()

        for ((scale100, label) in zooms) {
            println("\n=== Zoom scale=$scale100 (0.${scale100 / 10} px/m) — $label ===")
            val pxPerM = scale100 / 100.0
            println("Fenster: ±260 px → ±${"%.0f".format(260 / pxPerM)} m um das Fahrzeug")

            data class Seg(val type: SegmentType, val highway: String, val pts: List<Point>, val label: String)

            val segs = ArrayList<Seg>()
            for ((i, road) in scenario.withIndex()) {
                val isRoute = i == 0
                val pts = MapProjector.projectPolyline(vehicle, road.points, scale100)
                segs.add(Seg(if (isRoute) SegmentType.Route else SegmentType.Road, road.highway, pts, road.name))

                val widthPx = if (isRoute) MapProjector.routeWidthPx(scale100)
                              else MapProjector.roadWidthPx(road.highway, scale100)
                val start = pts.first(); val end = pts.last()
                println("%-24s %-14s %-6s %2d pts w=%2dpx  px:(%+4d,%+4d)-(%+4d,%+4d)".format(
                    road.name, road.highway,
                    if (isRoute) "ROUTE" else "ROAD", pts.size, widthPx,
                    start.x, start.y, end.x, end.y))
            }

            // Frame must stay within the protocol budget
            val frameSegs = segs.map {
                Segment(it.type,
                        if (it.type == SegmentType.Route) MapProjector.routeWidthPx(scale100)
                        else MapProjector.roadWidthPx(it.highway, scale100),
                        it.pts)
            }
            val frame = MapFrame(0, MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
                                 0, scale100, frameSegs)
            val bytes = frame.encode()
            println("Frame: ${frameSegs.size} Segmente, ${bytes.size} B (Limit ${MapFrame.MAX_FRAME} B)")
            assertTrue("frame too large: ${bytes.size}", bytes.size <= MapFrame.MAX_FRAME)

            // vehicle-relative: first route point IS the vehicle
            assertEquals(Point(0, 0), segs[0].pts[0])

            renderPng(File(outDir, "map-scale-$scale100.png"),
                      segs.map { Seg2(it.type, it.highway, it.pts, it.label) },
                      scale100, label)
            println("PNG: ${File(outDir, "map-scale-$scale100.png").absolutePath}")
        }
    }

    @Test
    fun projectionMath() {
        // 100 m north at scale 24 (0.24 px/m) → y = -24 px, x = 0
        val north = MapProjector.projectPolyline(vehicle, listOf(at(100.0, 0.0)), 24)
        assertEquals(-24, north[0].y)
        assertEquals(0, north[0].x)
        // 100 m east → x = +24 px
        val east = MapProjector.projectPolyline(vehicle, listOf(at(0.0, 100.0)), 24)
        assertEquals(24, east[0].x)
        // clamping: 10 km north at scale 48 → 4800 px → clamped to -AHEAD_PX
        val far = MapProjector.projectPolyline(vehicle, listOf(at(10_000.0, 0.0)), 48)
        assertEquals(-480, far[0].y)
        // rotation 0° north: point north of vehicle maps to vehicle pos - y
        val rot = MapProjector.rotateForDisplay(listOf(Point(0, -100)), 0.0)
        assertEquals(240, rot[0].x); assertEquals(220, rot[0].y)   // 320-100
    }

    @Test
    fun roadWidthsFollowHighwayType() {
        // motorway wider than residential at the same zoom
        assertTrue(MapProjector.roadWidthPx("motorway", 48) > MapProjector.roadWidthPx("residential", 48))
        // residential at city zoom: 6 m × 0.48 px/m × 1.8 = 5 px fill width
        assertEquals(5, MapProjector.roadWidthPx("residential", 48))
        // route stroke WIDER than the road it lies on (dominant line)
        assertTrue(MapProjector.routeWidthPx(48) > MapProjector.roadWidthPx("residential", 48))
        // width byte ceiling: protocol max is 16, casing adds +2 in the firmware
        assertTrue(MapProjector.roadWidthPx("motorway", 48) <= 13)
    }

    // ---- PNG writer (no dependencies: 480×480, firmware palette) -----------
    private data class Seg2(val type: SegmentType, val highway: String, val pts: List<Point>, val label: String)

    private fun renderPng(file: File, segs: List<Seg2>, scale100: Int, label: String) {
        val W = 480; val H = 480
        val img = Array(H) { IntArray(W) { 0x0D1117 } }

        fun plot(x: Int, y: Int, color: Int) {
            if (x in 0 until W && y in 0 until H) img[y][x] = color
        }

        fun strokeLine(x0: Int, y0: Int, x1: Int, y1: Int, width: Int, color: Int) {
            val steps = maxOf(kotlin.math.abs(x1 - x0), kotlin.math.abs(y1 - y0)) * 2 + 1
            for (s in 0..steps) {
                val t = s.toDouble() / steps
                val x = (x0 + (x1 - x0) * t).toInt()
                val y = (y0 + (y1 - y0) * t).toInt()
                val r = width / 2
                for (dy in -r..r) for (dx in -r..r)
                    if (dx * dx + dy * dy <= r * r) plot(x + dx, y + dy, color)
            }
        }

        // firmware draw order (map_render_impl.h): ALL road casings first
        // (white), then ALL road fills (black body), route LAST —
        // "black line with white border", crossings join seamlessly.
        val roads = segs.filter { it.type == SegmentType.Road }
        for (seg in roads) {   // casing pass: white border
            val screen = MapProjector.rotateForDisplay(seg.pts, 0.0)
            val widthPx = MapProjector.roadWidthPx(seg.highway, scale100)
            for (i in 0 until screen.size - 1)
                strokeLine(screen[i].x, screen[i].y, screen[i + 1].x, screen[i + 1].y, widthPx + 2, 0xE6EDF3)
        }
        for (seg in roads) {   // fill pass: black body
            val screen = MapProjector.rotateForDisplay(seg.pts, 0.0)
            val widthPx = MapProjector.roadWidthPx(seg.highway, scale100)
            for (i in 0 until screen.size - 1)
                strokeLine(screen[i].x, screen[i].y, screen[i + 1].x, screen[i + 1].y, widthPx, 0x0D1117)
        }
        for (seg in segs.filter { it.type == SegmentType.Route }) {
            val screen = MapProjector.rotateForDisplay(seg.pts, 0.0)
            val w = MapProjector.routeWidthPx(scale100)
            for (i in 0 until screen.size - 1)
                strokeLine(screen[i].x, screen[i].y, screen[i + 1].x, screen[i + 1].y, w, 0xE6EDF3)
        }

        // vehicle dot at centre (as on the display)
        for (dy in -6..6) for (dx in -6..6)
            if (dx * dx + dy * dy <= 36) plot(240 + dx, 320 + dy, 0x3FB950)

        // scale bar: 100 px = 10000/scale100 metres (firmware caption logic)
        val scaleM = 100 * 100 / scale100
        for (x in 20..119) { plot(x, 455, 0xE6EDF3); plot(x, 458, 0xE6EDF3) }
        repeat(30) { i -> plot(20 + i * 4, 456, 0) }

        writePng(file, W, H, img)
    }

    /** Minimal PNG writer: 8-bit RGB, no interlace. Good enough for previews. */
    private fun writePng(file: File, w: Int, h: Int, rgb: Array<IntArray>) {
        fun chunk(type: String, data: ByteArray): ByteArray {
            val out = ByteArray(12 + data.size)
            val len = data.size
            out[0] = ((len shr 24) and 0xFF).toByte(); out[1] = ((len shr 16) and 0xFF).toByte()
            out[2] = ((len shr 8) and 0xFF).toByte(); out[3] = (len and 0xFF).toByte()
            for (i in 0 until 4) out[4 + i] = type[i].code.toByte()
            System.arraycopy(data, 0, out, 8, data.size)
            val crc = CRC32()
            crc.update(type.toByteArray())
            crc.update(data)
            val v = crc.value.toInt()
            out[8 + data.size] = ((v shr 24) and 0xFF).toByte()
            out[9 + data.size] = ((v shr 16) and 0xFF).toByte()
            out[10 + data.size] = ((v shr 8) and 0xFF).toByte()
            out[11 + data.size] = (v and 0xFF).toByte()
            return out
        }

        val sig = byteArrayOf(0x89.toByte(), 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A)
        val ihdr = ByteArray(13)
        ihdr[0] = ((w shr 24) and 0xFF).toByte(); ihdr[1] = ((w shr 16) and 0xFF).toByte()
        ihdr[2] = ((w shr 8) and 0xFF).toByte(); ihdr[3] = (w and 0xFF).toByte()
        ihdr[4] = ((h shr 24) and 0xFF).toByte(); ihdr[5] = ((h shr 16) and 0xFF).toByte()
        ihdr[6] = ((h shr 8) and 0xFF).toByte(); ihdr[7] = (h and 0xFF).toByte()
        ihdr[8] = 8.toByte(); ihdr[9] = 2.toByte()   // 8-bit, truecolour RGB
        val raw = ByteArray(h * (1 + w * 3))
        var o = 0
        for (y in 0 until h) {
            raw[o++] = 0
            for (x in 0 until w) {
                val c = rgb[y][x]
                raw[o++] = ((c shr 16) and 0xFF).toByte()
                raw[o++] = ((c shr 8) and 0xFF).toByte()
                raw[o++] = (c and 0xFF).toByte()
            }
        }
        val deflater = java.util.zip.Deflater(java.util.zip.Deflater.BEST_COMPRESSION)
        deflater.setInput(raw)
        deflater.finish()
        val idat = ByteArray(raw.size)
        val idatLen = deflater.deflate(idat)
        deflater.end()

        val png = sig + chunk("IHDR", ihdr) +
                chunk("IDAT", idat.copyOf(idatLen)) + chunk("IEND", ByteArray(0))
        FileOutputStream(file).use { it.write(png) }
    }
}
