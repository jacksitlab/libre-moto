/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * TourPreviewTest — DISABLED by default (@Ignore): renders MANY frames
 * across the whole map, not just one point.
 *
 * For every stop: a PNG through the COMPLETE production chain
 * (OsmPbfReader → LmmBuilder → LmmMap → query → FrameBuilder →
 * MapFrame.encode() → C++ render_frame with the REAL firmware
 * pipeline). Output: build/map-preview/tour/tour-<n>.png + tour.txt
 * with a per-stop street report (types in window, in frame, dropped).
 *
 * stops are chosen to answer "how does it look at different points":
 *  - a grid over the map's populated tiles (systematic coverage)
 *  - the densest spots (most streets in a 500 m window — city centres)
 *  - the emptiest spots (few streets — rural roads, worst case)
 *
 * Enable:  ./gradlew testDebugUnitTest --tests '*TourPreview*' \
 *            -DrunTour=true --rerun-tasks
 * (Gradle's -D doesn't reach the test JVM by default → the build.gradle
 * maps it: test { systemProperty 'runTour', ... } — see build.gradle.kts.)
 */
package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.MapFrame
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

class TourPreviewTest {

    private val pbf = File("src/test/resources/liechtenstein.osm.pbf")

    /** Enable switch: -DrunTour=true (mapped through build.gradle.kts). */
    private val enabled = System.getProperty("runTour", "false").toBoolean()

    /** Max stops — keep the output manageable (runtime: ~1 s per stop). */
    private val maxstops = 12

    /** VIEWER MODE: explicit stops via -Dtourstops="lat,lon[;lat,lon…]".
     *  Renders the EXACT production frames at your coordinates (any zoom
     *  via -DtourScale=48) so you can verify the display offline:
     *    ./gradlew testDebugUnitTest --tests '*TourPreview*' \
     *      -DrunTour=true -Dtourstops="52.51933,13.34229" --rerun-tasks
     *  → build/map-preview/tour/tour-00.png (+ .bin) at that point.
     *  Empty/unset → automatic density-based stop selection (as before). */
    private val explicitstops: List<LatLon>? =
        System.getProperty("tourstops")?.split(';')
            ?.mapNotNull { s ->
                val p = s.split(',').map { it.trim() }
                if (p.size == 2) p[0].toDoubleOrNull()?.let { la ->
                    p[1].toDoubleOrNull()?.let { lo -> LatLon(la, lo) } } else null
            }?.takeIf { it.isNotEmpty() }

    /** Explicit zoom override for viewer mode (px/m × 100; default 48). */
    private val viewerScale = System.getProperty("tourScale")?.toIntOrNull() ?: 48

    @Test
    fun renderTourAcrossMap() {
        // Disabled by default: enable with -DrunTour=true (mapped through
        // build.gradle.kts). JUnit4 @Ignore would skip unconditionally, so
        // the switch is assumeTrue — a skipped assumption is not a failure.
        assumeTrue("Tour-Test disabled (-DrunTour=true zum Aktivieren)", enabled)

        assertTrue("Fixture fehlt: ${pbf.absolutePath}", pbf.exists())
        val tourDir = File("build/map-preview/tour").apply { mkdirs() }
        val report = StringBuilder()

        // ---- full production map pipeline (once) ----
        val progress = File(tourDir, "progress.txt")
        fun prog(line: String) {
            println(line)
            progress.writeText("${System.currentTimeMillis() / 1000}  $line\n")
        }
        prog("Pass 1/2: pbf scannen (Ways) …")
        val res = OsmPbfReader.scan(pbf)
        val built = LmmBuilder.build(res)
        val map = LmmMap.load(ByteArrayInputStream(built.bytes))
        assertTrue("keine roads im pbf", map.roadCount > 0)
        prog("map: ${map.roadCount} roads, ${built.tileCount} tiles, ${built.bytes.size / 1024} KB")

        // ---- pick stops: explicit viewer coordinates or automatic ----
        prog(if (explicitstops != null) "viewer mode: ${explicitstops.size} stops provided" else "choosing stops …")
        val stops = explicitstops ?: pickstops(map)
        prog("stops chosen: ${stops.size}")

        // ---- render every stop through the full chain ----
        val renderTool = buildRenderTool()
        stops.forEachIndexed { n, stop ->
            prog("Stop %02d/%d: %.5f,%.5f …".format(n, stops.size, stop.latitude, stop.longitude))
            val frame = buildFrameAt(map, stop)
            val bin = File(tourDir, "tour-%02d.bin".format(n))
            bin.writeBytes(frame.encode())
            val png = File(tourDir, "tour-%02d.png".format(n))
            val proc = ProcessBuilder(renderTool.absolutePath, bin.absolutePath, png.absolutePath)
                .redirectErrorStream(true).start()
            val out = proc.inputStream.bufferedReader().readText()
            proc.waitFor()
            val streets = frame.segments.count { it.type == de.jacksitlab.libremoto.protocol.SegmentType.Road }
            val line = "Stop %02d: %.5f,%.5f  roads in frame=%d  PNG=%s".format(
                n, stop.latitude, stop.longitude, streets, png.name)
            prog(line)
            report.appendLine(line)
            report.appendLine(out.lineSequence().filter {
                it.startsWith("Zeilen:") || it.startsWith("Massstab")
            }.joinToString("\n"))
        }

        File(tourDir, "tour.txt").writeText(report.toString())
        progress.delete()   // done — no stale progress file
        println("\nTour fertig: ${tourDir.absolutePath}")
    }

    /** Stop selection: quantiles by street density + both extremes.
     *  Density via TILE road counts (O(roads)), NOT per-street queries —
     *  a per-street queryBbox over 100k streets ran for hours on
     *  Brandenburg-sized regions. */
    private fun pickstops(map: LmmMap): List<LatLon> {
        val roads = map.allRoads()

        // density proxy: number of roads in the street's own tile
        // (one pass over the road table; tile counts precomputed)
        data class Cand(val pos: LatLon, val tileRoads: Int)
        val cands = ArrayList<Cand>(roads.size)
        for (r in roads) {
            val mid = r.npts / 2
            val lat = r.latE7[mid] / 1e7
            val lon = r.lonE7[mid] / 1e7
            cands.add(Cand(LatLon(lat, lon), map.tileRoadCount(lat, lon)))
        }

        cands.sortBy { it.tileRoads }
        val picked = ArrayList<LatLon>()
        val step = (cands.size / (maxstops - 2)).coerceAtLeast(1)
        var i = 0
        while (picked.size < maxstops - 2 && i < cands.size) {
            picked.add(cands[i].pos)
            i += step
        }
        cands.firstOrNull()?.let { picked.add(0, it.pos) }
        cands.lastOrNull()?.let { picked.add(it.pos) }
        return picked.take(maxstops)
    }

    /** The production frame build at a stop (same code as on the road).
     *  In viewer mode the query radius mirrors the app: the visible window
     *  (WINDOW_PX at the scale) + 10% margin — not a fixed 500 m. */
    private fun buildFrameAt(map: LmmMap, stop: LatLon): MapFrame {
        val scale100 = viewerScale
        val windowM = MapProjector.AHEAD_PX / (scale100 / 100.0) * 1.1
        val dLat = windowM / MapProjector.METERS_PER_DEG_LAT
        val dLon = windowM / (MapProjector.METERS_PER_DEG_LAT *
                kotlin.math.cos(Math.toRadians(stop.latitude)))
        val queried = map.queryBbox(stop.latitude - dLat, stop.longitude - dLon,
                                    stop.latitude + dLat, stop.longitude + dLon)
        val streets = queried.map { r ->
            StreetRoad(r.highway, (0 until r.npts).map { i ->
                LatLon(r.latE7[i] / 1e7, r.lonE7[i] / 1e7)
            })
        }
        // route: a REAL street through the vehicle position — the street
        // whose nearest point is closest to the stop, walked end-to-end
        // through the stop. Real crossings exercise the corridor filter
        // exactly like on the road. Fallback: straight north 600 m.
        val all = map.allRoads()
        var routeStreet: LmmRoad? = null
        var bestStreetD = Double.MAX_VALUE
        for (r in all) {
            if (r.npts < 3) continue
            for (i in 0 until r.npts) {
                val la = r.latE7[i] / 1e7 - stop.latitude
                val lo = r.lonE7[i] / 1e7 - stop.longitude
                val d = la * la + lo * lo
                if (d < bestStreetD) { bestStreetD = d; routeStreet = r }
            }
        }
        val route: List<LatLon> = if (routeStreet != null) {
            val rs = routeStreet!!
            // nearest point index on that street
            var bestI = 0; var bestD = Double.MAX_VALUE
            for (i in 0 until rs.npts) {
                val la = rs.latE7[i] / 1e7 - stop.latitude
                val lo = rs.lonE7[i] / 1e7 - stop.longitude
                val d = la * la + lo * lo
                if (d < bestD) { bestD = d; bestI = i }
            }
            val rp = ArrayList<LatLon>()
            rp.add(stop)
            for (i in 0 until rs.npts) rp.add(LatLon(rs.latE7[i] / 1e7, rs.lonE7[i] / 1e7))
            rp
        } else {
            listOf(stop,
                LatLon(stop.latitude + 600.0 / MapProjector.METERS_PER_DEG_LAT, stop.longitude))
        }
        return FrameBuilder.buildFrame(1, 0, scale100, route, streets)
    }

    /** Compile render_frame.cpp once (host g++ + zlib). */
    private fun buildRenderTool(): File {
        val tool = File("build/render_frame_tour")
        val src = File("../firmware/test/render_frame.cpp")
        assertTrue("render_frame.cpp fehlt: ${src.absolutePath}", src.exists())
        val p = ProcessBuilder("g++", "-std=c++11", "-O2",
                "-I../firmware/src/map", src.absolutePath,
                "-o", tool.absolutePath, "-lz")
            .redirectErrorStream(true).start()
        val out = p.inputStream.bufferedReader().readText()
        p.waitFor()
        assertTrue("render_frame.cpp Kompilierung fehlgeschlagen: $out", p.exitValue() == 0)
        return tool
    }
}
