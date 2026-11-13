package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.MapFrame
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File
import javax.xml.parsers.DocumentBuilderFactory

/**
 * GpxReplayTest — map verification from a REAL drive (user request:
 * "a test that takes a gpx track and renders an image every 50 m in
 * every zoom level of what is visible").
 *
 * Replays a GPX track through the FULL production pipeline:
 *
 *   GPX <trkpt> → sample every 50 m along the track
 *   → LmmMap.queryBbox (the .lmm the app would use)
 *   → FrameBuilder.buildFrame (corridor/contact filter, clipping,
 *      ranking — the exact production code)
 *   → MapFrame.encode → C++ render_frame (the REAL firmware decoder
 *      + geometry) → PNG
 *
 * For every sample point ALL zoom levels (scale 48/64/80/96) are
 * rendered, so a problem spot shows four pictures to compare.
 *
 * Usage (build.gradle.kts maps the system properties):
 *   ./gradlew testDebugUnitTest --tests '*GpxReplay*' --rerun-tasks \
 *       -DrunGpx=true -DgpxFile=/path/to/track.gpx \
 *       -DgpxMap=/path/to/region.lmm -DgpxStep=50
 *
 * Output: build/map-preview/gpx/
 *   gpx-0001_s48.png  … gpx-NNNN_s96.png  (one per sample × zoom)
 *   index.html       (browse all images, sorted by track position)
 *   report.txt       (per sample: streets in frame, route pts)
 *
 * The .lmm can be produced from a Geofabrik extract on the device —
 * or offline via LmmBuilder (see LmmExportTest / the MapsActivity
 * pipeline; the same file the app installs).
 */
class GpxReplayTest {

    private val enabled = System.getProperty("runGpx", "false").toBoolean()
    private val gpxPath = System.getProperty("gpxFile")
    private val lmmPath = System.getProperty("gpxMap")
    private val stepM = System.getProperty("gpxStep")?.toDoubleOrNull() ?: 50.0
    private val zoomLevels = listOf(48, 64, 80, 96)   // 1000/750/600/500 m

    /** 1 deg lat ≈ 111320 m; lon scales with cos(lat). */
    private fun metersPerDegLon(lat: Double) = 111320.0 * kotlin.math.cos(Math.toRadians(lat))

    @Test
    fun replayGpxTrack() {
        assumeTrue("GPX replay disabled (-DrunGpx=true, -DgpxFile=…, -DgpxMap=…)", enabled)
        assertTrue("GPX file missing: $gpxPath", gpxPath != null && File(gpxPath).exists())
        assertTrue(".lmm map missing: $lmmPath", lmmPath != null &&
            lmmPath.split(",").all { File(it.trim()).exists() })

        val outDir = File("build/map-preview/gpx").apply { mkdirs() }
        val report = StringBuilder()
        val html = StringBuilder()

        // ---- 1. load the map(s) — comma-separated paths, like the
        //      device's MapManager holding several regions ----
        val maps = lmmPath!!.split(",").map { p ->
            println("loading map: $p")
            LmmMap.load(ByteArrayInputStream(File(p.trim()).readBytes()))
        }
        assertTrue("maps have no roads", maps.any { it.roadCount > 0 })
        println("maps: " + maps.joinToString { "${it.roadCount} roads" })

        // ---- 2. parse the GPX track ----
        val track = parseGpx(File(gpxPath))
        assertTrue("GPX contains no track points", track.isNotEmpty())
        println("gpx: ${track.size} track points")

        // ---- 3. sample every stepM metres along the track ----
        val samples = sampleAlong(track, stepM)
        println("samples: ${samples.size} (every ${stepM.toInt()} m)")
        report.appendLine("gpx=${gpxPath}")
        report.appendLine("map=${lmmPath}")
        report.appendLine("samples=${samples.size} step=${stepM.toInt()} m")
        html.append("<html><head><title>GPX replay</title></head><body>\n")
        html.append("<h3>${File(gpxPath).name} — ${samples.size} samples, " +
                "every ${stepM.toInt()} m</h3>\n")

        // ---- 4. compile the render tool once ----
        val renderTool = compileRenderTool()
        val maxS = zoomLevels.max()

        var n = 0
        for (s in samples) {
            n++
            val heading = headingDeg10(s.bearingDeg)
            // query radius: the LARGEST window (scale 48) + 10% margin
            val windowM = MapProjector.AHEAD_PX / (maxS / 100.0) * 1.1
            val dLat = windowM / MapProjector.METERS_PER_DEG_LAT
            val dLon = windowM / metersPerDegLon(s.lat)
            val streets = maps.flatMap { m ->
                m.queryBbox(s.lat - dLat, s.lon - dLon,
                            s.lat + dLat, s.lon + dLon).map { r ->
                    StreetRoad(r.highway, (0 until r.npts).map { i ->
                        LatLon(r.latE7[i] / 1e7, r.lonE7[i] / 1e7)
                    })
                }
            }
            // route: the track ahead (up to 1200 m), the trail: behind
            // (up to 300 m) — like the app on the road
            val route = ArrayList<LatLon>()
            for (i in s.idx until s.idx + 1) route.add(LatLon(s.lat, s.lon))
            // (route built below with the track context)
            val (routePts, trailPts) = routeAndTrail(track, s.idx)
            val frame = FrameBuilder.buildFrame(n and 0xFF, heading, maxS,
                routePts, streets, trailPts)
            // render every zoom level for this sample
            for (sc in zoomLevels) {
                val f = FrameBuilder.buildFrame(n and 0xFF, heading, sc, routePts, streets, trailPts)
                val bin = File(outDir, "tmp.bin")
                bin.writeBytes(f.encode())
                val png = File(outDir, "gpx-%04d_s%d.png".format(n, sc))
                val p = ProcessBuilder(renderTool.absolutePath, bin.absolutePath, png.absolutePath)
                    .redirectErrorStream(true).start()
                val out = p.inputStream.bufferedReader().readText()
                p.waitFor()
                assertTrue("render_frame failed: $out", p.exitValue() == 0)
            }
            val roads = frame.segments.count {
                it.type == de.jacksitlab.libremoto.protocol.SegmentType.Road
            }
            val line = "sample %04d: %.5f,%.5f  roads=%d  route=%d pts".format(
                n, s.lat, s.lon, roads, routePts.size)
            println(line)
            report.appendLine(line)
            html.append("<p>sample %04d (%.5f,%.5f) roads=%d</p>\n".format(n, s.lat, s.lon, roads))
            for (sc in zoomLevels)
                html.append("<img src=\"gpx-%04d_s%d.png\" width=\"360\"> \n".format(n, sc))
            html.append("<hr>\n")
        }
        File(outDir, "report.txt").writeText(report.toString())
        html.append("</body></html>\n")
        File(outDir, "index.html").writeText(html.toString())
        println("\nGPX replay done: ${outDir.absolutePath}/index.html")
    }

    // ---- helpers ---------------------------------------------------------

    data class Sample(val lat: Double, val lon: Double, val idx: Int, val bearingDeg: Double)

    /** Parse <trkpt lat lon> in document order (any GPX 1.x). */
    private fun parseGpx(f: File): List<DoubleArray> {
        val db = DocumentBuilderFactory.newInstance()
        db.isNamespaceAware = false
        val doc = db.newDocumentBuilder().parse(f)
        val pts = ArrayList<DoubleArray>()
        val nodes = doc.getElementsByTagName("trkpt")
        for (i in 0 until nodes.length) {
            val el = nodes.item(i)
            val lat = el.attributes.getNamedItem("lat")?.nodeValue?.toDoubleOrNull()
            val lon = el.attributes.getNamedItem("lon")?.nodeValue?.toDoubleOrNull()
            if (lat != null && lon != null) pts.add(doubleArrayOf(lat, lon))
        }
        return pts
    }

    /** Sample the track every `stepM` metres (great-circle-ish, local). */
    private fun sampleAlong(track: List<DoubleArray>, stepM: Double): List<Sample> {
        val out = ArrayList<Sample>()
        var acc = 0.0
        var last = track.first()
        out.add(Sample(last[0], last[1], 0, 0.0))
        for (i in 1 until track.size) {
            val p = track[i]
            val dLat = (p[0] - last[0]) * MapProjector.METERS_PER_DEG_LAT
            val dLon = (p[1] - last[1]) * metersPerDegLon(p[0])
            val d = kotlin.math.sqrt(dLat * dLat + dLon * dLon)
            acc += d
            if (acc >= stepM) {
                val bearing = Math.toDegrees(kotlin.math.atan2(dLon, dLat))
                out.add(Sample(p[0], p[1], i, (bearing + 360.0) % 360.0))
                acc = 0.0
            }
            last = p
        }
        return out
    }

    /** Route = the track ahead of the sample (up to 1200 m), trail = behind
     *  (up to 300 m) — mirroring what the app sends on the road. */
    private fun routeAndTrail(track: List<DoubleArray>, idx: Int): Pair<List<LatLon>, List<LatLon>> {
        val route = ArrayList<LatLon>()
        var len = 0.0
        var prev = track[idx]
        for (i in idx until track.size) {
            val p = track[i]
            val dLat = (p[0] - prev[0]) * MapProjector.METERS_PER_DEG_LAT
            val dLon = (p[1] - prev[1]) * metersPerDegLon(p[0])
            len += kotlin.math.sqrt(dLat * dLat + dLon * dLon)
            route.add(LatLon(p[0], p[1]))
            if (len > 1200.0) break
            prev = p
        }
        val trail = ArrayList<LatLon>()
        var tlen = 0.0
        var tprev = track[idx]
        for (i in idx downTo 0) {
            val p = track[i]
            val dLat = (p[0] - tprev[0]) * MapProjector.METERS_PER_DEG_LAT
            val dLon = (p[1] - tprev[1]) * metersPerDegLon(p[0])
            tlen += kotlin.math.sqrt(dLat * dLat + dLon * dLon)
            trail.add(LatLon(p[0], p[1]))
            if (tlen > 300.0) break
            tprev = p
        }
        return route to trail
    }

    private fun headingDeg10(bearingDeg: Double): Int =
        ((bearingDeg * 10).toInt() % 3600 + 3600) % 3600

    /** Compile render_frame.cpp (host g++ + zlib) — same as TourPreview. */
    private fun compileRenderTool(): File {
        val tool = File("build/render_frame_gpx")
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
