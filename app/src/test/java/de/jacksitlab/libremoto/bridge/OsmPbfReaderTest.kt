/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * OsmPbfReaderTest — real-data round-trip: parse the committed Geofabrik
 * extract (Liechtenstein, src/test/resources/liechtenstein.osm.pbf),
 * build a .lmm via LmmBuilder, load it via LmmMap and query a bbox in
 * Vaduz. Verifies the whole on-device map pipeline without network.
 */
package de.jacksitlab.libremoto.bridge

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

class OsmPbfReaderTest {

    private val pbf = File("src/test/resources/liechtenstein.osm.pbf")

    @Test
    fun parseBuildLoadQuery() {
        assertTrue("Fixture fehlt: ${pbf.absolutePath}", pbf.exists())

        val t0 = System.currentTimeMillis()
        val res = OsmPbfReader.scan(pbf)
        val tParse = System.currentTimeMillis() - t0
        println("pbf: ${res.ways.size} Straßen, ${res.nodeCount()} referenzierte Nodes in ${tParse} ms")
        assertTrue("erwartet > 1000 Straßen, got ${res.ways.size}", res.ways.size > 1000)

        val t1 = System.currentTimeMillis()
        val built = LmmBuilder.build(res)
        val tBuild = System.currentTimeMillis() - t1
        println("lmm: ${built.roadCount} Straßen, ${built.tileCount} Kacheln, " +
                "${built.bytes.size / 1024} KB, ${built.droppedIncomplete} unvollständig verworfen (${tBuild} ms)")

        // load + query: Vaduz centre
        val map = LmmMap.load(ByteArrayInputStream(built.bytes))
        assertEquals(built.roadCount, map.roadCount)

        val t2 = System.currentTimeMillis()
        val roads = map.queryBbox(47.10, 9.50, 47.16, 9.54)
        val tQuery = System.currentTimeMillis() - t2
        println("query Vaduz: ${roads.size} Straßen in ${tQuery} ms")
        assertTrue("erwartet Straßen in Vaduz, got ${roads.size}", roads.size > 50)

        // sanity: query returns plausible types
        val types = roads.map { it.highway }.toSet()
        println("Typen: ${types.sorted()}")
        assertTrue("residential fehlt", "residential" in types)

        // a road polyline must have valid coords (Liechtenstein bounds)
        val r = roads.first()
        assertTrue("lat ${r.latE7[0]}", r.latE7[0] in 470_000_000..475_000_000)
        assertTrue("lon ${r.lonE7[0]}", r.lonE7[0] in 94_000_000..97_000_000)

        // performance guard: a bbox query must be fast (no binder call!)
        assertTrue("query zu langsam: ${tQuery} ms", tQuery < 500)
    }
}
