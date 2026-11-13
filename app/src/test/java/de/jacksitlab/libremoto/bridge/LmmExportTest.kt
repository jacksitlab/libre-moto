package de.jacksitlab.libremoto.bridge

import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

/**
 * LmmExportTest — map-data verification in QGIS (user request:
 * "the stripped-down pbf in some format to look at, maybe QGIS").
 *
 * Converts a .lmm (the app's own map format, built from the OSM pbf)
 * to GeoJSON — one LineString feature per road, properties:
 *   highway  (OSM highway tag)
 *   maxspeed (km/h, 0 = untagged)
 *   npts     (point count after way merging)
 *
 * Usage:
 *   ./gradlew testDebugUnitTest --tests '*LmmExport*' --rerun-tasks \
 *       -DrunExport=true -DexportMap=/path/to/region.lmm
 *   (or -DexportPbf=/path/to/region.osm.pbf to build the .lmm first)
 *
 * Output: build/map-preview/export/region.geojson
 *   → QGIS: Layer → Add Vector Layer → the .geojson. Roads render as
 *     lines; style by 'highway' or 'maxspeed' to spot gaps or wrong
 *     tags (e.g. filter maxspeed=0 on trunk roads).
 */
class LmmExportTest {

    private val enabled = System.getProperty("runExport", "false").toBoolean()
    private val mapPath = System.getProperty("exportMap")
    private val pbfPath = System.getProperty("exportPbf")

    @Test
    fun exportLmmToGeoJson() {
        // blank ("") counts as not provided — the gradle mapping defaults
        // to "", and `!= null` would treat that as a real (broken) path
        val mapIn = mapPath?.takeIf { it.isNotBlank() }
        val pbfIn = pbfPath?.takeIf { it.isNotBlank() }
        assumeTrue("export disabled (-DrunExport=true, -DexportMap=… or -DexportPbf=…)", enabled)
        assertTrue("need -DexportMap=<file.lmm> or -DexportPbf=<file.osm.pbf>",
            mapIn != null || pbfIn != null)

        // where the .lmm comes from: a ready file, or build it from pbf
        val lmmBytes: ByteArray = if (mapIn != null) {
            assertTrue("map missing: $mapIn", File(mapIn).exists())
            File(mapIn).readBytes()
        } else {
            val pbf = File(pbfIn!!)
            assertTrue("pbf missing: ${pbf.absolutePath}", pbf.exists())
            println("scanning pbf (pass 1/2) …")
            val res = OsmPbfReader.scan(pbf)
            println("building .lmm (pass 2/2) …")
            LmmBuilder.build(res).bytes
        }

        val map = LmmMap.load(ByteArrayInputStream(lmmBytes))
        assertTrue("map has no roads", map.roadCount > 0)
        println("map: ${map.roadCount} roads, v2=${map.hasMaxspeed}")

        val outDir = File("build/map-preview/export").apply { mkdirs() }
        val out = File(outDir, "region.geojson")
        val sb = StringBuilder(1 shl 20)
        sb.append("{\n  \"type\": \"FeatureCollection\",\n  \"features\": [\n")
        val roads = map.allRoads()
        var first = true
        var untagged = 0
        for (r in roads) {
            if (r.maxspeedKmh == 0) untagged++
            if (!first) sb.append(",\n")
            first = false
            sb.append("    {\"type\": \"Feature\",\n")
            sb.append("     \"properties\": {\"highway\": \"${r.highway}\", " +
                    "\"maxspeed\": ${r.maxspeedKmh}, \"npts\": ${r.npts}},\n")
            sb.append("     \"geometry\": {\"type\": \"LineString\", \"coordinates\": [")
            for (i in 0 until r.npts) {
                if (i > 0) sb.append(", ")
                sb.append("[%.7f, %.7f]".format(r.lonE7[i] / 1e7, r.latE7[i] / 1e7))
            }
            sb.append("]}}")
        }
        sb.append("\n  ]\n}\n")
        out.writeText(sb.toString())
        println("GeoJSON: ${out.absolutePath} (${roads.size} roads, " +
                "$untagged without maxspeed)")
        println("→ QGIS: Layer → Add Vector Layer → ${out.name}")
    }
}
