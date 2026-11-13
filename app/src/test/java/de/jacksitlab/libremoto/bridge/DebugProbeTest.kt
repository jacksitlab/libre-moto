package de.jacksitlab.libremoto.bridge

import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

/**
 * DebugProbeTest — one-shot pipeline probe at a coordinate (the "why is
 * this street missing" tool). Runs the exact GpxReplay pipeline steps at
 * ONE position and logs every stage:
 *   queryBbox hits → per-street contact classification → FrameBuilder
 *   input/output → segment count.
 *
 * Usage:
 *   ./gradlew testDebugUnitTest --tests '*DebugProbe*' --rerun-tasks \
 *       -Dprobe=true -DprobeMap=a.lmm,b.lmm -DprobeLat=52.44254 \
 *       -DprobeLon=13.20304 [-DprobeScale=48]
 */
class DebugProbeTest {

    private val enabled = System.getProperty("probe", "false").toBoolean()

    @Test
    fun probePipeline() {
        assumeTrue("disabled (-Dprobe=true -DprobeMap=… -DprobeLat=… -DprobeLon=…)", enabled)
        val maps = System.getProperty("probeMap", "").split(",")
            .filter { it.isNotBlank() }
            .map { LmmMap.load(ByteArrayInputStream(File(it.trim()).readBytes())) }
        assertTrue("no maps", maps.isNotEmpty())
        val lat = System.getProperty("probeLat").toDouble()
        val lon = System.getProperty("probeLon").toDouble()
        val scale = System.getProperty("probeScale", "48").toInt()
        val ref = LatLon(lat, lon)

        // query radius like the replay: max window + 10%
        val windowM = MapProjector.AHEAD_PX / (scale / 100.0) * 1.1
        val dLat = windowM / MapProjector.METERS_PER_DEG_LAT
        val dLon = windowM / (MapProjector.METERS_PER_DEG_LAT *
                kotlin.math.cos(Math.toRadians(lat)))
        val queried = maps.flatMap { m ->
            m.queryBbox(lat - dLat, lon - dLon, lat + dLat, lon + dLon).map { r ->
                StreetRoad(r.highway, (0 until r.npts).map { i ->
                    LatLon(r.latE7[i] / 1e7, r.lonE7[i] / 1e7)
                })
            }
        }
        println("PROBE query: ${queried.size} streets within ${windowM.toInt()} m")
        queried.take(40).forEach {
            val d = it.points.minOf { p ->
                kotlin.math.sqrt(
                    ((p.latitude - lat) * MapProjector.METERS_PER_DEG_LAT).let { a -> a * a } +
                    ((p.longitude - lon) * MapProjector.METERS_PER_DEG_LAT *
                        kotlin.math.cos(Math.toRadians(lat))).let { b -> b * b })
            }
            println("PROBE street: ${it.roadType} npts=${it.points.size} nearest=${d.toInt()} m")
        }

        // route: straight ahead 1200 m north (the probe has no track) —
        // for classification we test against a synthetic route THROUGH
        // the position along the nearest street, like the replay does
        val nearest = queried.minByOrNull { s ->
            s.points.minOf { p ->
                kotlin.math.sqrt(
                    ((p.latitude - lat) * MapProjector.METERS_PER_DEG_LAT).let { a -> a * a } +
                    ((p.longitude - lon) * MapProjector.METERS_PER_DEG_LAT *
                        kotlin.math.cos(Math.toRadians(lat))).let { b -> b * b })
            }
        }
        val route = if (nearest != null) {
            listOf(ref) + nearest.points
        } else {
            listOf(ref, LatLon(lat + 1200.0 / MapProjector.METERS_PER_DEG_LAT, lon))
        }
        println("PROBE route: ${route.size} pts (${nearest?.roadType ?: "synthetic"})")

        for (s in queried) {
            val c = MapProjector.streetRouteContact(s.points, route, lat)
            if (c != MapProjector.CONTACT_NONE) {
                println("PROBE contact: ${s.roadType} npts=${s.points.size} -> ${
                    if (c == MapProjector.CONTACT_ALONG) "ALONG" else "CROSS"}")
            }
        }

        val frame = FrameBuilder.buildFrame(1, 0, scale, route, queried)
        println("PROBE frame: ${frame.segments.size} segments, " +
                "${frame.segments.count { it.type == de.jacksitlab.libremoto.protocol.SegmentType.Road }} roads")
        println("PROBE frame bytes: ${frame.encode().size}")
    }
}
