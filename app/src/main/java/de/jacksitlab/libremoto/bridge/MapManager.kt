/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * MapManager — region download + .lmm cache + route coverage check.
 *
 * Owns everything around the prebuilt vector maps:
 *  - fetch the Geofabrik region index (index-v1.json, 554 regions with
 *    pbf URLs and bboxes)
 *  - download a region's .osm.pbf (public Geofabrik server, no own infra)
 *  - convert it ON DEVICE via OsmPbfReader + LmmBuilder into <id>.lmm
 *    and cache it in filesDir/maps/
 *  - loaded-region registry: which .lmm files are active (auto-load at
 *    start: ALL cached regions — a city region is a few MB)
 *  - route coverage: is a given polyline fully inside loaded regions?
 *    Reports the first gap so the app can warn the user.
 *
 * All callbacks are marshalled to the caller's thread by the caller;
 * MapManager itself works on its own executor.
 */
package de.jacksitlab.libremoto.bridge

import android.content.Context
import org.json.JSONArray
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import java.util.concurrent.Executors

/** One Geofabrik region from the index. parentId builds the hierarchy
 *  (continent → country → state); null = top level (continent). */
data class GeofabrikRegion(
    val id: String,
    val name: String,
    val pbfUrl: String,
    val parentId: String?,
    val minLat: Double, val minLon: Double,
    val maxLat: Double, val maxLon: Double,
)

/** Coverage result for a route polyline. */
data class CoverageResult(
    val covered: Boolean,
    val gapLat: Double,     // first uncovered point (valid if !covered)
    val gapLon: Double,
)

class MapManager(
    private val context: Context,
    private val log: (String) -> Unit,
    private val ui: (String) -> Unit,   // user-visible progress line
) {
    private val executor = Executors.newSingleThreadExecutor { r ->
        Thread(r, "LibreMoto-maps").apply { priority = Thread.NORM_PRIORITY - 1 }
    }

    val mapsDir: File get() = File(context.filesDir, "maps").also { it.mkdirs() }

    /** Loaded maps: id → LmmMap. Filled by loadAll(). */
    @Volatile private var maps: Map<String, LmmMap> = emptyMap()

    /** Region index (id → region), loaded lazily / after fetchIndex(). */
    @Volatile private var regions: Map<String, GeofabrikRegion> = emptyMap()

    // ------------------------------------------------------------------ index

    /** Fetch + parse the Geofabrik index (network). Executor thread. */
    fun fetchIndex(onDone: (Boolean) -> Unit) {
        executor.execute {
            try {
                val json = httpGet("https://download.geofabrik.de/index-v1.json")
                val arr = JSONObject(json).getJSONArray("features")
                val out = HashMap<String, GeofabrikRegion>(arr.length())
                for (i in 0 until arr.length()) {
                    val f = arr.getJSONObject(i)
                    val p = f.getJSONObject("properties")
                    val urls = p.optJSONObject("urls") ?: continue
                    val pbf = urls.optString("pbf", null) ?: continue
                    val gid = p.optString("id") ?: continue
                    val name = p.optString("name", gid)
                    // bbox from the geometry
                    var minLat = 90.0; var maxLat = -90.0
                    var minLon = 180.0; var maxLon = -180.0
                    fun scan(o: Any) {
                        when (o) {
                            is JSONArray -> for (i in 0 until o.length()) scan(o.get(i))
                            is JSONObject -> for (k in o.keys()) scan(o.get(k))
                            is Number -> Unit
                        }
                    }
                    // coordinates are nested arrays of [lon, lat]
                    fun scanCoords(o: Any) {
                        when (o) {
                            is JSONArray -> {
                                if (o.length() == 2 && o.get(0) is Number && o.get(1) is Number) {
                                    val lon = o.getDouble(0); val lat = o.getDouble(1)
                                    if (lat < minLat) minLat = lat; if (lat > maxLat) maxLat = lat
                                    if (lon < minLon) minLon = lon; if (lon > maxLon) maxLon = lon
                                } else for (i in 0 until o.length()) scanCoords(o.get(i))
                            }
                        }
                    }
                    f.optJSONObject("geometry")?.let { scanCoords(it) }
                    // hierarchy: "parent" names the containing region
                    // (e.g. brandenburg → germany, germany → europe)
                    val parentId = if (p.has("parent")) p.optString("parent") else null
                    out[gid] = GeofabrikRegion(gid, name, pbf, parentId, minLat, minLon, maxLat, maxLon)
                }
                regions = out
                log("maps: ${out.size} Geofabrik-Regionen im Index")
                onDone(true)
            } catch (e: Exception) {
                log("maps: Index-Fehler: ${e.message}")
                onDone(false)
            }
        }
    }

    fun regionList(): List<GeofabrikRegion> = regions.values.sortedBy { it.name }

    /** Region by id (null if unknown). */
    fun region(id: String): GeofabrikRegion? = regions[id]

    /** Hierarchy: children of the given parent (null = top level,
     *  the continents). Sorted by name. */
    fun childRegions(parentId: String?): List<GeofabrikRegion> =
        regions.values.filter { it.parentId == parentId }.sortedBy { it.name }

    /** Hierarchy: does this region have children (a country with states)? */
    fun hasChildren(id: String): Boolean = regions.values.any { it.parentId == id }

    /** Installed regions (with their index entry if the index is loaded). */
    fun installedRegions(): List<GeofabrikRegion> =
        mapsDir.listFiles { f -> f.name.endsWith(".lmm") }
            ?.mapNotNull { f -> regions[f.name.removeSuffix(".lmm")] }
            ?: emptyList()

    /** All installed region ids — even if the index is not loaded yet. */
    fun installedIds(): List<String> =
        mapsDir.listFiles { f -> f.name.endsWith(".lmm") }
            ?.map { it.name.removeSuffix(".lmm") } ?: emptyList()

    // ------------------------------------------------- region metadata (.meta)

    /** Persisted metadata for one installed region (JSON next to the .lmm):
     *  { "md5": "<geofabrik pbf md5 at download time>", "date": <epoch ms>,
     *    "bytes": <lmm size> }. The md5 enables UPDATE detection: Geofabrik
     *  publishes a .md5 next to every .osm.pbf; when it changes, the
     *  upstream data changed → re-download + re-convert. */
    private fun metaFile(regionId: String) = File(mapsDir, "$regionId.meta")

    fun readMeta(regionId: String): JSONObject? = try {
        if (metaFile(regionId).exists())
            JSONObject(metaFile(regionId).readText()) else null
    } catch (_: Exception) { null }

    private fun writeMeta(regionId: String, md5: String?, bytes: Long) {
        try {
            val o = JSONObject()
            o.put("md5", md5 ?: JSONObject.NULL)
            o.put("date", System.currentTimeMillis())
            o.put("bytes", bytes)
            metaFile(regionId).writeText(o.toString())
        } catch (e: Exception) { log("maps: Meta-Schreibfehler $regionId: ${e.message}") }
    }

    /** Installed .lmm size in bytes (0 if missing). */
    fun regionBytes(regionId: String): Long =
        File(mapsDir, "$regionId.lmm").takeIf { it.exists() }?.length() ?: 0L

    /** Fetch Geofabrik's CURRENT md5 for a region's pbf (network).
     *  Returns null on any failure (offline, 404 …). Executor thread. */
    fun fetchCurrentMd5(region: GeofabrikRegion, onDone: (String?) -> Unit) {
        executor.execute {
            val md5 = try {
                val txt = httpGet(region.pbfUrl + ".md5").trim()
                // file format: "<hash>  <filename>"
                txt.splitWhitespace().firstOrNull { it.length == 32 }?.lowercase()
            } catch (_: Exception) { null }
            onDone(md5)
        }
    }

    /** Check ALL installed regions for updates (one md5 request each,
     *  sequential on the executor). onResult per region:
     *  (regionId, upToDate: Boolean?) — null = unknown (offline/404). */
    fun checkUpdates(onResult: (String, Boolean?) -> Unit, onAllDone: () -> Unit) {
        executor.execute {
            for (id in installedIds()) {
                val region = regions[id] ?: continue
                val stored = readMeta(id)?.optString("md5", null)
                val current = try {
                    val txt = httpGet(region.pbfUrl + ".md5").trim()
                    txt.splitWhitespace().firstOrNull { it.length == 32 }?.lowercase()
                } catch (_: Exception) { null }
                val upToDate = when {
                    current == null -> null                  // unknown
                    stored.isNullOrBlank() -> false         // no stored hash → outdated metadata
                    else -> current == stored.lowercase()
                }
                onResult(id, upToDate)
            }
            onAllDone()
        }
    }

    private fun String.splitWhitespace(): List<String> =
        split(' ', '\t', '\r', '\n').filter { it.isNotBlank() }

    // --------------------------------------------------------------- download

    /** Download + convert a region (network + CPU). Executor thread.
     *  progress: (phase, percent) — phase 0 = download, 1 = convert. */
    fun downloadRegion(region: GeofabrikRegion, onDone: (Boolean) -> Unit,
                       progress: ((Int, Int) -> Unit)? = null) {
        executor.execute {
            val target = File(mapsDir, "${region.id}.lmm")
            try {
                ui("Lade ${region.name} …")
                val tmp = File(context.cacheDir, "${region.id}.osm.pbf")
                // Geofabrik's md5 for the pbf — stored in .meta for UPDATE
                // detection (checkUpdates compares it later)
                val md5 = try {
                    val txt = httpGet(region.pbfUrl + ".md5").trim()
                    txt.splitWhitespace().firstOrNull { it.length == 32 }?.lowercase()
                } catch (_: Exception) { null }
                httpDownload(region.pbfUrl, tmp) { done, total ->
                    val pct = if (total > 0) (done * 100 / total).toInt() else 0
                    ui("Lade ${region.name}: ${done / 1048576} / ${total / 1048576} MB")
                    progress?.invoke(0, pct)
                }
                ui("Konvertiere ${region.name} …")
                progress?.invoke(1, 0)
                val t0 = System.currentTimeMillis()
                val res = OsmPbfReader.scan(tmp)
                val built = LmmBuilder.build(res)
                progress?.invoke(1, 100)
                FileOutputStream(target).use { it.write(built.bytes) }
                writeMeta(region.id, md5, built.bytes.size.toLong())
                tmp.delete()
                ui("${region.name}: ${built.roadCount} Straßen, " +
                   "${built.bytes.size / 1048576} MB — fertig " +
                   "(${(System.currentTimeMillis() - t0) / 1000} s)")
                log("maps: ${region.id} konvertiert: ${built.roadCount} Straßen, " +
                    "Tiles ${built.tileCount}, dropped ${built.droppedIncomplete}")
                loadAll()
                onDone(true)
            } catch (e: Exception) {
                tmpSafeDelete(region.id)
                target.delete()
                ui("${region.name} fehlgeschlagen: ${e.message}")
                log("maps: Download-Fehler ${region.id}: ${e.message}")
                onDone(false)
            }
        }
    }

    private fun tmpSafeDelete(id: String) {
        try { File(context.cacheDir, "$id.osm.pbf").delete() } catch (_: Exception) {}
    }

    fun deleteRegion(regionId: String) {
        executor.execute {
            File(mapsDir, "$regionId.lmm").delete()
            loadAll()
            ui("Region gelöscht: $regionId")
        }
    }

    // ------------------------------------------------------------------- load

    /** Load every cached .lmm (called at start + after download/delete). */
    fun loadAll() {
        val out = HashMap<String, LmmMap>()
        for (f in mapsDir.listFiles { f -> f.name.endsWith(".lmm") } ?: emptyArray()) {
            try {
                out[f.name.removeSuffix(".lmm")] = LmmMap.load(f.inputStream())
            } catch (e: Exception) {
                log("maps: Ladefehler ${f.name}: ${e.message}")
            }
        }
        maps = out
        log("maps: ${out.size} Regionen geladen " +
            "(${out.entries.joinToString { "${it.key}=${it.value.roadCount}" }})")
    }

    fun loadedRegions(): Map<String, LmmMap> = maps

    fun isRegionDownloaded(regionId: String): Boolean = File(mapsDir, "$regionId.lmm").exists()

    // --------------------------------------------------------------- coverage

    /** Is the route fully inside loaded regions? Returns the first gap.
     *  Pure in-memory: bbox check of each point against each region's
     *  tile coverage — O(points × regions), fast enough for 1000-pt routes. */
    fun checkRoute(waypoints: List<Pair<Double, Double>>): CoverageResult {
        val loaded = maps
        if (loaded.isEmpty()) return CoverageResult(false,
            waypoints.firstOrNull()?.first ?: 0.0, waypoints.firstOrNull()?.second ?: 0.0)
        for ((lat, lon) in waypoints) {
            var inside = false
            for (m in loaded.values) {
                // cheap: does any tile of the map contain this point?
                val tx = LmmMap.lonToTileX(lon)
                val ty = LmmMap.latToTileY(lat)
                if (m.hasTile(tx, ty)) { inside = true; break }
            }
            if (!inside) return CoverageResult(false, lat, lon)
        }
        return CoverageResult(true, 0.0, 0.0)
    }

    // ------------------------------------------------------------------ utils

    private fun httpGet(url: String): String {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 15_000
        conn.readTimeout = 60_000
        conn.setRequestProperty("User-Agent", "LibreMoto/1.0")
        try {
            return conn.inputStream.bufferedReader().readText()
        } finally {
            conn.disconnect()
        }
    }

    private fun httpDownload(url: String, target: File, progress: (Long, Long) -> Unit) {
        val conn = URL(url).openConnection() as HttpURLConnection
        conn.connectTimeout = 20_000
        conn.readTimeout = 120_000
        conn.setRequestProperty("User-Agent", "LibreMoto/1.0")
        try {
            val total = conn.contentLengthLong
            FileOutputStream(target).use { out ->
                conn.inputStream.use { inp ->
                    val buf = ByteArray(1 shl 16)
                    var done = 0L
                    while (true) {
                        val n = inp.read(buf)
                        if (n < 0) break
                        out.write(buf, 0, n)
                        done += n
                        progress(done, total)
                    }
                }
            }
        } finally {
            conn.disconnect()
        }
    }
}
