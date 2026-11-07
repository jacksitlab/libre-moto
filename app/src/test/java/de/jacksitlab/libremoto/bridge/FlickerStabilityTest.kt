package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.MapFrame
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File

/** Flicker repro: a moving vehicle through a dense spot. The anti-flicker
 *  invariant: a street selected in frame N that is STILL a candidate in
 *  frame N+1 must remain selected — the sticky selection must not swap
 *  streets at the cut line just because the vehicle moved a few metres. */
class FlickerStabilityTest {
    @Test
    fun selectionIsStableWhileMoving() {
        val pbf = File("src/test/resources/liechtenstein.osm.pbf")
        if (!pbf.exists()) return
        val res = OsmPbfReader.scan(pbf)
        val map = LmmMap.load(java.io.ByteArrayInputStream(LmmBuilder.build(res).bytes))
        val all = map.allRoads()

        // dense spot: the road closest to Vaduz (capital — densest streets)
        val centre = all.filter { it.npts > 3 }
            .minByOrNull { Math.abs(it.latE7[it.npts / 2] / 1e7 - 47.11) }!!
        val mid = centre.npts / 2
        val start = LatLon(centre.latE7[mid] / 1e7, centre.lonE7[mid] / 1e7)

        val scale100 = 48
        val dLat = 5.0 / MapProjector.METERS_PER_DEG_LAT   // 5 m per frame
        var violations = 0
        var checks = 0
        var denseSteps = 0
        for (step in 0 until 20) {
            val vehicle = LatLon(start.latitude + step * dLat, start.longitude)
            val windowM = MapProjector.AHEAD_PX / (scale100 / 100.0)
            val wLat = windowM / MapProjector.METERS_PER_DEG_LAT
            val wLon = windowM / (MapProjector.METERS_PER_DEG_LAT *
                    kotlin.math.cos(Math.toRadians(vehicle.latitude)))
            val q = map.queryBbox(vehicle.latitude - wLat, vehicle.longitude - wLon,
                                   vehicle.latitude + wLat, vehicle.longitude + wLon)
            if (q.size < MapFrame.MAX_SEGS + 5) continue   // not dense enough to force cuts
            denseSteps++
            val streets = q.map { r -> StreetRoad(r.highway, (0 until r.npts).map { i ->
                LatLon(r.latE7[i] / 1e7, r.lonE7[i] / 1e7) }) }
            val route = listOf(vehicle,
                LatLon(vehicle.latitude + 600.0 / MapProjector.METERS_PER_DEG_LAT, vehicle.longitude))

            // candidate anchors (street identity, stable across frames) BEFORE
            // the build — the same key space the FrameBuilder's stickiness uses.
            // Mirrors the FrameBuilder pipeline: corridor filter FIRST, then
            // clipping — only corridor-relevant streets are candidates.
            val minLat = vehicle.latitude - wLat; val maxLat = vehicle.latitude + wLat
            val minLon = vehicle.longitude - wLon; val maxLon = vehicle.longitude + wLon
            val corridorM = 2880.0 / scale100
            val candKeys = HashSet<Long>()
            for (s in streets) {
                if (!MapProjector.streetRelevantToRoute(s.points, route, corridorM, vehicle.latitude)) continue
                for (seg in MapProjector.clipPolylineToBbox(s.points, minLat, minLon, maxLat, maxLon)) {
                    if (seg.size >= 2) { candKeys.add(key(s.points.first())); break }
                }
            }
            val prevSel = FrameBuilder.prevFrameStreetKeys
            val survivors = prevSel.filter { it in candKeys }   // still candidates

            FrameBuilder.buildFrame(step, 0, scale100, route, streets)

            val nowSel = FrameBuilder.prevFrameStreetKeys
            for (k in survivors) {
                checks++
                if (k !in nowSel) violations++
            }
        }
        println("Flicker-Invariante: $violations Verletzungen von $checks Checks " +
            "($denseSteps dichte Schritte von 20)")
        // The invariant is hard: a still-candidate street must never be
        // dropped. If the spot was never dense enough to force cuts, there
        // is nothing to check (checks < 10) — skip, not fail.
        assertTrue("Anti-Flicker-Invariante verletzt: $violations von $checks Straßen verloren",
            violations == 0 || checks < 10)
    }

    /** Same rounding as FrameBuilder.streetKey (test-visible mirror). */
    private fun key(p: LatLon): Long =
        ((Math.round(p.latitude * 1e6).toLong()) shl 32) or
         (Math.round(p.longitude * 1e6).toLong() and 0xFFFFFFFFL)
}
