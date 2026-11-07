/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * EndToEndPreviewTest — pbf IN → hardware display OUT, using the FULL
 * production chain:
 *
 *   OsmPbfReader.scan (two-pass) → LmmBuilder → LmmMap → BBox-Query
 *   → FrameBuilder.buildFrame (the same code OsmAndNavSource calls:
 *     road ranking, budget, decimation, widths)
 *   → MapFrame.encode() → build/map-preview/frame.bin
 *
 * A C++ host tool (firmware/test/render_frame.cpp) then runs the REAL
 * firmware pipeline on frame.bin — parse_map_frame + prepare_map_frame
 * (map_render_impl.h, the exact geometry the ESP32 renders) — and draws
 * the PNG with the firmware's styles. What you see is what the display
 * shows.
 *
 * The console report lists which streets made it into the frame (and
 * which were dropped by filter/budget) — the tool to debug the
 * "wrong side streets" suspicion.
 *
 * Route: a straight line through Vaduz; window = ±500 m (city zoom 48).
 */
package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.MapFrame
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.io.FileOutputStream

class EndToEndPreviewTest {

    private val pbf = File("src/test/resources/liechtenstein.osm.pbf")

    @Test
    fun pbfToDisplayFrame() {
        assertTrue("Fixture fehlt: ${pbf.absolutePath}", pbf.exists())
        val outDir = File("build/map-preview").apply { mkdirs() }

        // ---- 1. full production map pipeline ----
        val res = OsmPbfReader.scan(pbf)
        val built = LmmBuilder.build(res)
        val map = LmmMap.load(java.io.ByteArrayInputStream(built.bytes))
        println("Karte: ${map.roadCount} Straßen, ${built.tileCount} Kacheln, ${built.bytes.size / 1024} KB")
        assertTrue("keine Straßen im pbf — Filter/Region prüfen", map.roadCount > 0)

        // ---- 2. scenario: vehicle ON A REAL STREET from the map itself ----
        // No hardcoded coordinates: take the longest primary/secondary/
        // residential street's midpoint so the frame is guaranteed to
        // contain the street the vehicle is on — works with ANY pbf.
        val all = map.allRoads()
        val street = all.filter { it.highway in listOf("primary", "secondary", "residential") }
            .maxByOrNull { it.npts }
            ?: all.maxByOrNull { it.npts }
        assertTrue("keine Straße in der Karte", street != null)
        street!!
        val mid = street.npts / 2
        val vehicle = LatLon(street.latE7[mid] / 1e7, street.lonE7[mid] / 1e7)
        println("Fahrzeug: auf ${street.highway}-Straße bei " +
            "${"%.5f".format(vehicle.latitude)}, ${"%.5f".format(vehicle.longitude)}")

        // route: continue along the street's direction (its next point)
        val nextIdx = (mid + 1).coerceAtMost(street.npts - 1)
        val dirLat = street.latE7[nextIdx] / 1e7 - vehicle.latitude
        val dirLon = street.lonE7[nextIdx] / 1e7 - vehicle.longitude
        val route = ArrayList<LatLon>()
        route.add(vehicle)
        for (k in 1..3) {
            route.add(LatLon(vehicle.latitude + dirLat * k, vehicle.longitude + dirLon * k))
        }

        val scale100 = 48   // city zoom, 500 m Voraussicht
        val lookaheadM = 500.0
        val dLat = lookaheadM / MapProjector.METERS_PER_DEG_LAT
        val dLon = lookaheadM / (MapProjector.METERS_PER_DEG_LAT *
                kotlin.math.cos(Math.toRadians(vehicle.latitude)))

        // the query the app performs per tick
        val queried = map.queryBbox(vehicle.latitude - dLat, vehicle.longitude - dLon,
                                     vehicle.latitude + dLat, vehicle.longitude + dLon)
        println("\n=== Query ±${lookaheadM.toInt()} m um das Fahrzeug: ${queried.size} Straßen ===")
        val byType = queried.groupBy { it.highway }.mapValues { it.value.size }
        println("Typen im Fenster: ${byType.entries.sortedByDescending { it.value }}")

        // ---- 3. THE production frame build (same code as on the road) ----
        val streets = queried.map { r ->
            StreetRoad(r.highway, (0 until r.npts).map { i ->
                LatLon(r.latE7[i] / 1e7, r.lonE7[i] / 1e7)
            })
        }
        val frame = FrameBuilder.buildFrame(1, 0, scale100, route, streets) { line ->
            println("FrameBuilder: $line")
        }

        // ---- 4. wire format + report ----
        val bytes = frame.encode()
        println("\nFrame: ${frame.segments.size} Segmente, ${bytes.size} B (Limit ${MapFrame.MAX_FRAME} B)")
        frame.segments.forEach { seg ->
            val kind = if (seg.type == de.jacksitlab.libremoto.protocol.SegmentType.Route) "ROUTE" else "ROAD "
            println("  $kind w=${seg.width} pts=${seg.points.size} " +
                "erst(${seg.points.first().x},${seg.points.first().y}) " +
                "letzt(${seg.points.last().x},${seg.points.last().y})")
        }
        assertTrue("Frame zu groß", bytes.size <= MapFrame.MAX_FRAME)
        assertTrue("mindestens ein Straßensegment erwartet",
            frame.segments.any { it.type == de.jacksitlab.libremoto.protocol.SegmentType.Road })

        // ---- 5. frame.bin for the C++ firmware-pipeline renderer ----
        File(outDir, "frame.bin").writeBytes(bytes)
        println("\nframe.bin geschrieben: ${File(outDir, "frame.bin").absolutePath}")
        println("Nächster Schritt (C++ Renderer):")
        println("  g++ -std=c++11 -O2 -I../src/map -I../src/ble firmware/test/render_frame.cpp " +
            "-o build/render_frame && ./build/render_frame build/map-preview/frame.bin")
    }
}
