/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * CoverageTest — route coverage against loaded .lmm maps, WITHOUT any
 * Android classes (pure JVM): exercises the same logic MapManager.checkRoute
 * uses (LmmMap.containsPoint per waypoint) via the committed Liechtenstein
 * fixture. A route inside Liechtenstein must be covered; a route into
 * Austria must report the first gap.
 */
package de.jacksitlab.libremoto.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

class CoverageTest {

    private val fixture = File("src/test/resources/liechtenstein.osm.pbf")

    /** Same check MapManager.checkRoute performs, minus the Android context. */
    private fun covered(map: LmmMap, waypoints: List<Pair<Double, Double>>): Pair<Boolean, Pair<Double, Double>?> {
        for (wp in waypoints) {
            if (!map.containsPoint(wp.first, wp.second)) return false to wp
        }
        return true to null
    }

    @Test
    fun routeInsideLiechtensteinIsCovered() {
        val map = LmmBuilder.build(OsmPbfReader.scan(fixture))
            .let { LmmMap.load(ByteArrayInputStream(it.bytes)) }
        // Waypoints ON actual FL roads: take polyline points from the map
        // itself (Vaduz area) — a route the user would really drive.
        val near = map.queryBbox(47.10, 9.50, 47.13, 9.54)
        assertTrue("keine Straßen bei Vaduz gefunden", near.isNotEmpty())
        val route = ArrayList<Pair<Double, Double>>()
        for (r in near.take(10)) {
            route.add(r.latE7[0] / 1e7 to r.lonE7[0] / 1e7)
        }
        val (ok, gap) = covered(map, route)
        assertTrue("Route auf FL-Straßen sollte abgedeckt sein, gap=$gap", ok)
    }

    @Test
    fun routeLeavingRegionReportsFirstGap() {
        val map = LmmBuilder.build(OsmPbfReader.scan(fixture))
            .let { LmmMap.load(ByteArrayInputStream(it.bytes)) }
        // Route on real FL roads (from the map) heading east, then a jump
        // to Feldkirch (Austria) — outside the region.
        val near = map.queryBbox(47.10, 9.50, 47.13, 9.54)
        assertTrue(near.isNotEmpty())
        val route = ArrayList<Pair<Double, Double>>()
        for (r in near.take(5)) route.add(r.latE7[0] / 1e7 to r.lonE7[0] / 1e7)
        val gapPoint = 47.2380 to 9.6010   // Feldkirch, AT
        route.add(gapPoint)
        val (ok, gap) = covered(map, route)
        assertFalse("Route nach AT sollte Lücke melden", ok)
        assertEquals(gapPoint.first, gap!!.first, 0.0001)
        assertEquals(gapPoint.second, gap.second, 0.0001)
    }

    /** v2 format: maxspeed tags must survive the build → load round-trip.
     *  Liechtenstein's main roads carry maxspeed tags, so at least one
     *  road near Vaduz must report a limit > 0. */
    @Test
    fun maxspeedSurvivesRoundTrip() {
        val map = LmmBuilder.build(OsmPbfReader.scan(fixture))
            .let { LmmMap.load(ByteArrayInputStream(it.bytes)) }
        val near = map.queryBbox(47.10, 9.50, 47.13, 9.54)
        assertTrue("keine Straßen bei Vaduz gefunden", near.isNotEmpty())
        val withLimit = near.count { it.maxspeedKmh > 0 }
        println("maxspeed: $withLimit/${near.size} Straßen bei Vaduz mit Limit")
        assertTrue("erwartet: mind. eine Straße mit maxspeed bei Vaduz", withLimit > 0)
    }
}
