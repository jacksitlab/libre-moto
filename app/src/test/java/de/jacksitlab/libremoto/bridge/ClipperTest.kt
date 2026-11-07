/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * ClipperTest — MapProjector.clipPolylineToBbox edge cases. The clipper
 * decides what part of a street the display shows; broken output here
 * means broken streets on the display.
 */
package de.jacksitlab.libremoto.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ClipperTest {

    private val minLat = 0.0; val maxLat = 1.0
    private val minLon = 0.0; val maxLon = 1.0

    private fun ll(lat: Double, lon: Double) = LatLon(lat, lon)

    @Test
    fun completelyInside() {
        val pts = listOf(ll(0.2, 0.2), ll(0.5, 0.5), ll(0.8, 0.8))
        val res = MapProjector.clipPolylineToBbox(pts, minLat, minLon, maxLat, maxLon)
        assertEquals(1, res.size)
        assertEquals(3, res[0].size)
    }

    @Test
    fun completelyOutside() {
        val pts = listOf(ll(2.0, 2.0), ll(3.0, 3.0))
        val res = MapProjector.clipPolylineToBbox(pts, minLat, minLon, maxLat, maxLon)
        assertTrue(res.isEmpty())
    }

    @Test
    fun crossingOnce() {
        // enters west at lon=0, exits east at lon=1
        val pts = listOf(ll(0.5, -1.0), ll(0.5, 0.5), ll(0.5, 2.0))
        val res = MapProjector.clipPolylineToBbox(pts, minLat, minLon, maxLat, maxLon)
        assertEquals(1, res.size)
        val seg = res[0]
        assertEquals(0.0, seg.first().longitude, 1e-9)   // clipped at west border
        assertEquals(1.0, seg.last().longitude, 1e-9)   // clipped at east border
        assertEquals(0.5, seg.first().latitude, 1e-9)
        assertEquals(0.5, seg.last().latitude, 1e-9)
    }

    @Test
    fun enteringAndLeavingTwice() {
        // a U-shaped street: inside → leaves north → re-enters from the west
        val pts = listOf(
            ll(0.5, 0.2), ll(0.5, 0.4),      // inside
            ll(1.5, 0.5),                    // leaves through the north border
            ll(0.5, -0.5),                   // outside, west of the box
            ll(0.5, 0.5),                    // re-enters through the west border
            ll(0.5, 0.9),
        )
        val res = MapProjector.clipPolylineToBbox(pts, minLat, minLon, maxLat, maxLon)
        assertEquals(2, res.size)
        // first segment: from (0.5, 0.2) to the north border crossing
        assertEquals(0.5, res[0].first().latitude, 1e-9)
        assertEquals(1.0, res[0].last().latitude, 1e-9)   // cut at maxLat
        // second segment: from the west border into the box
        assertEquals(0.0, res[1].first().longitude, 1e-9)  // cut at minLon
        assertEquals(0.9, res[1].last().longitude, 1e-9)
    }

    @Test
    fun cornerCrossing() {
        // diagonal through the box corner
        val pts = listOf(ll(-0.5, -0.5), ll(1.5, 1.5))
        val res = MapProjector.clipPolylineToBbox(pts, minLat, minLon, maxLat, maxLon)
        assertEquals(1, res.size)
        assertEquals(0.0, res[0].first().latitude, 1e-9)
        assertEquals(0.0, res[0].first().longitude, 1e-9)
        assertEquals(1.0, res[0].last().latitude, 1e-9)
        assertEquals(1.0, res[0].last().longitude, 1e-9)
    }
}
