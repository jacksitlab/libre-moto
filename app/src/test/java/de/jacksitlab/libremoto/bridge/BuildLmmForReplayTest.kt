package de.jacksitlab.libremoto.bridge

import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.ByteArrayInputStream
import java.io.File

/**
 * BuildLmmForReplayTest — one-shot helper: build a .lmm from a pbf
 * to a FILE (the GPX replay needs a path, not bytes).
 *
 * Usage:
 *   ./gradlew testDebugUnitTest --tests '*BuildLmm*' --rerun-tasks \
 *       -DbuildPbf=/path/region.osm.pbf -DbuildOut=/path/region.lmm
 */
class BuildLmmForReplayTest {
    private val enabled = System.getProperty("buildPbf")?.takeIf { it.isNotBlank() } != null
    private val outPath = System.getProperty("buildOut", "")

    @Test
    fun buildLmm() {
        assumeTrue("disabled (-DbuildPbf=… -DbuildOut=…)", enabled)
        val pbf = File(System.getProperty("buildPbf"))
        assertTrue("pbf missing: ${pbf.absolutePath}", pbf.exists())
        println("scanning pbf …")
        val res = OsmPbfReader.scan(pbf)
        println("building .lmm …")
        val built = LmmBuilder.build(res)
        val out = File(outPath)
        out.parentFile?.mkdirs()
        out.writeBytes(built.bytes)
        println("written: ${out.absolutePath} (${built.bytes.size / 1024} KB, " +
                "${built.tileCount} tiles)")
    }
}
