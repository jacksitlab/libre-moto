/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * FrameBuilder — pure MapFrame construction, extracted from
 * OsmAndNavSource.mapFrame() so the END-TO-END test exercises exactly
 * the production code path (filters, road ranking, budget, decimation,
 * widths) — no divergence between test and app.
 *
 * Inputs are plain types (LatLon polylines) — no AIDL, no Android.
 */
package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.MapFrame
import de.jacksitlab.libremoto.protocol.Point
import de.jacksitlab.libremoto.protocol.Segment
import de.jacksitlab.libremoto.protocol.SegmentType

/** A side street: highway type + polyline (same shape as ARoadPolyline). */
data class StreetRoad(val roadType: String, val points: List<LatLon>)

object FrameBuilder {

    /** Roads the firmware can actually RENDER per frame: the ESP32 line
     *  pool (MAP_POOL=44, map_render.h) holds casing+fill for
     *  (44 - route_room)/2 = 20 roads (route_room=3 reserves route +
     *  destination slots). The selection IS the render set — the firmware
     *  must never silently drop a tail. */
    const val RENDER_ROAD_CAPACITY = 20

    /** Streets selected in the previous frame (first-point keys) — the
     *  stickiness memory that stops cut-line flicker between frames.
     *  Public for tests: FlickerStabilityTest asserts that a street
     *  selected in frame N and still a candidate in frame N+1 stays
     *  selected (that invariant IS the anti-flicker guarantee). */
    @Volatile var prevFrameStreetKeys: Set<Long> = emptySet()
        private set

    /** Stable identity for a street segment across frames: rounded
     *  lat/lon of its first point (1e-6° ≈ 11 cm — fine enough to
     *  distinguish streets, coarse enough to survive float jitter). */
    private fun streetKey(p: LatLon): Long =
        ((Math.round(p.latitude * 1e6).toLong()) shl 32) or
         (Math.round(p.longitude * 1e6).toLong() and 0xFFFFFFFFL)

    /** Douglas-Peucker-style simplification with a point budget: keeps both
     *  endpoints, then repeatedly keeps the point with the largest distance
     *  from the current simplified chain until the budget is exhausted.
     *  Straight stretches lose points early, curves keep them — unlike
     *  uniform sampling, which mangled distant curves into polygons. */
    private fun simplifyKeepEnds(pts: List<LatLon>, maxPoints: Int): List<LatLon> {
        if (pts.size <= 2 || pts.size <= maxPoints) return pts
        // squared-distance in degrees is not uniform, but good enough for
        // RANKING which points matter most (the scale factor is common).
        val keep = BooleanArray(pts.size)
        keep[0] = true
        keep[pts.size - 1] = true
        var kept = 2
        // priority queue of (error, lo, mid, hi) — split the worst range first
        val pq = java.util.PriorityQueue<DoubleArray>(compareByDescending { it[0] })
        fun enqueueRange(lo: Int, hi: Int) {
            if (hi - lo < 2) return
            val a = pts[lo]; val b = pts[hi]
            var worst = -1.0; var worstIdx = -1
            for (i in lo + 1 until hi) {
                val p = pts[i]
                val dx = b.longitude - a.longitude
                val dy = b.latitude - a.latitude
                val len2 = dx * dx + dy * dy
                val t = if (len2 == 0.0) 0.0
                        else (((p.longitude - a.longitude) * dx + (p.latitude - a.latitude) * dy) / len2)
                            .coerceIn(0.0, 1.0)
                val ex = a.longitude + t * dx - p.longitude
                val ey = a.latitude + t * dy - p.latitude
                val d2 = ex * ex + ey * ey
                if (d2 > worst) { worst = d2; worstIdx = i }
            }
            if (worstIdx > 0) pq.add(doubleArrayOf(worst, lo.toDouble(), worstIdx.toDouble(), hi.toDouble()))
        }
        enqueueRange(0, pts.size - 1)
        while (kept < maxPoints && pq.isNotEmpty()) {
            val e = pq.poll()
            val mid = e[2].toInt()
            if (keep[mid]) continue
            keep[mid] = true; kept++
            enqueueRange(e[1].toInt(), mid)
            enqueueRange(mid, e[3].toInt())
        }
        val out = ArrayList<LatLon>(kept)
        for (i in pts.indices) if (keep[i]) out.add(pts[i])
        return out
    }

    /** Straight placeholder route (no route data). */
    fun fallbackFrame(seq: Int, headingDeg10: Int, scale100: Int,
                      distanceToM: Int): MapFrame {
        val pxPerMeter = scale100 / 100.0
        val lenPx = (distanceToM * pxPerMeter).toInt().coerceIn(40, 220)
        val route = listOf(Point(0, 0), Point(0, -lenPx))
        return MapFrame(
            seq = seq,
            flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
            headingDeg10 = headingDeg10,
            scalePxPerMeter100 = scale100,
            segments = listOf(Segment(SegmentType.Route, 10, route)),
        )
    }

    /**
     * THE production frame build: route + side streets.
     *
     * - route: projected polyline, stroke = routeWidthPx (≈60 % of a
     *   residential street's edge distance)
     * - streets: ranked by road class when there are more than the frame
     *   can hold (MAX_SEGS-1), decimated to ≤20 points spread over the
     *   WHOLE street, width byte = edge distance from the highway type
     * - byte budget: 3000 B for streets (each = 3 header + pts*4)
     */
    fun buildFrame(seq: Int, headingDeg10: Int, scale100: Int,
                   route: List<LatLon>, streets: List<StreetRoad>,
                   trail: List<LatLon> = emptyList(),
                   log: ((String) -> Unit)? = null): MapFrame {
        if (route.size < 2) throw IllegalArgumentException("route < 2 points")
        val ref = route[0]
        val segments = ArrayList<Segment>(2 + streets.size)

        // ---- visible window (lat/lon bbox) — computed FIRST because the
        // route + trail are clipped against it below. Asymmetric (vehicle at
        // one third from the bottom, MAP_CY=320): AHEAD_PX ahead, BEHIND_PX
        // behind, ±SIDE_PX sideways. +10% margin so lines enter off-screen.
        val pxPerMeter = scale100 / 100.0
        val aheadM = (MapProjector.AHEAD_PX / pxPerMeter) * 1.1
        val behindM = (MapProjector.BEHIND_PX / pxPerMeter) * 1.1
        val sideM = (MapProjector.SIDE_PX / pxPerMeter) * 1.1
        val mPerDegLon = MapProjector.METERS_PER_DEG_LAT *
                kotlin.math.cos(Math.toRadians(ref.latitude))
        val minLat = ref.latitude - behindM / MapProjector.METERS_PER_DEG_LAT
        val maxLat = ref.latitude + aheadM / MapProjector.METERS_PER_DEG_LAT
        val minLon = ref.longitude - sideM / mPerDegLon
        val maxLon = ref.longitude + sideM / mPerDegLon

        // ---- route segment: CLIPPED to the HEADING-ROTATED window ----
        // The old bbox clip cut against a NORTH-ALIGNED box while the
        // display is heading-rotated — driving south, the route left the
        // box after BEHIND_M and ended mid-screen. The rotated window
        // follows the view: ahead along the heading, ±side across.
        val headingDeg = headingDeg10 / 10.0
        val routeClipped = MapProjector.clipPolylineToWindow(
            route, ref, headingDeg, aheadM, behindM, sideM)
        val routeVisible = routeClipped.maxByOrNull { it.size } ?: emptyList()
        if (routeVisible.size < 2) {
            // ref itself is outside its own window (should not happen) — fall
            // back to the unclipped head so the frame still has a route line
            val maxPts = minOf(route.size, MapFrame.MAX_PTS)
            segments.add(Segment(SegmentType.Route, MapProjector.routeWidthPx(scale100),
                MapProjector.projectPolyline(ref, route.subList(0, maxPts), scale100)))
        } else {
            val maxPts = minOf(routeVisible.size, MapFrame.MAX_PTS)
            segments.add(Segment(SegmentType.Route, MapProjector.routeWidthPx(scale100),
                MapProjector.projectPolyline(ref, routeVisible.subList(0, maxPts), scale100)))
        }

        // ---- trail segment (the road BEHIND the vehicle) ----
        // OsmAnd delivers the route with the passed segment cut off
        // (setIncludePassedSegment(false)), so streets behind us have no
        // corridor and vanished. The caller feeds the recent real track
        // (breadcrumbs) here; it renders as a second route segment and the
        // corridor filter treats it like the route — streets alongside the
        // road we just rode stay visible.
        if (trail.size >= 2) {
            val trailVisible = MapProjector.clipPolylineToWindow(
                trail, ref, headingDeg, aheadM, behindM, sideM)
                .maxByOrNull { it.size } ?: emptyList()
            if (trailVisible.size >= 2) {
                val tPts = MapProjector.projectPolyline(ref, trailVisible, scale100)
                if (tPts.size >= 2) {
                    segments.add(Segment(SegmentType.Route, MapProjector.routeWidthPx(scale100), tPts))
                }
            }
        }

        // ---- street segments (corridor → clip → chain → rank → budget) ----
        // (the window bbox above is reused for the street clipping)

        // 0. CORRIDOR FILTER (Beeline-style): only streets relevant to the
        //    route — crossing/mouthing streets (shown IN FULL) and parallels
        //    within a narrow corridor. Non-crossing parallels further out are
        //    culled BEFORE they can churn the selection. The corridor scales
        //    with the zoom (60 m at scale 48, 120 m at scale 24) so the
        //    visual corridor width stays ~12 px regardless of zoom.
        //    The corridor follows route + TRAIL (the road behind us), so
        //    streets alongside the road we just rode stay visible too.
        // Route-contact filter (Beeline, tightened): only streets that
        // CROSS route+trail or MOUTH onto it (endpoint ≤ 15 m). The old
        // distance corridor kept non-crossing parallels — on the round
        // display they read as random streets floating in the landscape.
        // Crossing streets are additionally LENGTH-LIMITED around their
        // contact point (300 m ≈ the visible circle) so they don't run
        // to the horizon.
        val contactPath = if (trail.size >= 2) route + trail else route
        val maxContactLenM = 300.0
        val corridorStreets = ArrayList<StreetRoad>(streets.size)
        var culled = 0
        for (s in streets) {
            if (s.points.size < 2) continue
            when (MapProjector.streetRouteContact(s.points, contactPath, ref.latitude)) {
                MapProjector.CONTACT_ALONG ->
                    // the road we DRIVE on: full length, no cut — the A115
                    // case (800 m of roads=0 on the motorway)
                    corridorStreets.add(s)
                MapProjector.CONTACT_CROSS -> {
                    val cut = MapProjector.cutToContactLength(s.points, contactPath,
                                                              maxContactLenM, ref.latitude)
                    if (cut.size >= 2)
                        corridorStreets.add(StreetRoad(s.roadType, cut))
                }
                else -> culled++
            }
        }
        if (log != null && culled > 0 && corridorStreets.size + culled > 8) {
            log("contact: ${corridorStreets.size} relevant, $culled culled")
        }
        val candidates = corridorStreets

        // 1. CLIP every street to the visible window — long streets used to
        //    be decimated over their WHOLE length (20 points over km), so
        //    the frame contained 2-3 points of geometry and the display
        //    drew broken/invented straight lines. Clipped segments keep the
        //    real course; a street can contribute MULTIPLE segments.
        //    The ANCHOR (the street's first global coordinate) travels with
        //    every clipped segment: the clipped first point MOVES along the
        //    window edge as the vehicle moves, so it would make a new key
        //    per frame — the anchor is stable for the street's lifetime.
        data class Clipped(val roadType: String, val pts: List<LatLon>, val rank: Int,
                           val anchor: Long)
        val clipped = ArrayList<Clipped>()
        for (street in candidates) {
            if (street.points.size < 2) continue
            val anchor = streetKey(street.points.first())
            for (seg in MapProjector.clipPolylineToWindow(street.points, ref, headingDeg, aheadM, behindM, sideM)) {
                if (seg.size >= 2) clipped.add(Clipped(street.roadType, seg, MapProjector.roadRank(street.roadType), anchor))
            }
        }

        // 1b. CHAIN clipped segments that connect end-to-start (same road
        //     type) into ONE continuous segment. OSM splits a through-street
        //     into separate ways at EVERY junction; the build-time merging
        //     only bridges 2-way chains, so crossings still deliver the
        //     street in pieces. Selecting pieces independently cut holes
        //     into through-streets (the 'interrupted streets' symptom) and
        //     wasted render slots. A chain = ONE frame segment: selected as
        //     a whole, decimated as a whole, rendered as ONE line.
        fun keyOf(p: LatLon): Long =
            ((Math.round(p.latitude * 1e6).toLong()) shl 32) or
             (Math.round(p.longitude * 1e6).toLong() and 0xFFFFFFFFL)

        // union-find over the clipped segments
        val parent = IntArray(clipped.size) { it }
        fun find(x: Int): Int {
            var r = x
            while (parent[r] != r) { parent[r] = parent[parent[r]]; r = parent[r] }
            return r
        }
        fun union(a: Int, b: Int) { parent[find(a)] = find(b) }

        // index endpoints: start/end key → segments
        val byStart = HashMap<Long, MutableList<Int>>()
        val byEnd = HashMap<Long, MutableList<Int>>()
        clipped.forEachIndexed { i, c ->
            byStart.getOrPut(keyOf(c.pts.first())) { mutableListOf() }.add(i)
            byEnd.getOrPut(keyOf(c.pts.last())) { mutableListOf() }.add(i)
        }
        // connect segments whose end meets another's start (same type) —
        // both directions: OSM way orientation is arbitrary
        for (i in clipped.indices) {
            val a = clipped[i]
            for (j in byStart[keyOf(a.pts.last())] ?: emptyList())
                if (j != i && clipped[j].roadType == a.roadType) union(i, j)
            for (j in byEnd[keyOf(a.pts.first())] ?: emptyList())
                if (j != i && clipped[j].roadType == a.roadType) union(i, j)
        }

        // collect chains: order the group's segments end-to-start
        data class Chain(val roadType: String, val rank: Int, val anchors: HashSet<Long>,
                         val pts: ArrayList<LatLon>)
        val chains = ArrayList<Chain>(clipped.size)
        val groups = HashMap<Int, MutableList<Int>>()
        clipped.forEachIndexed { i, _ -> groups.getOrPut(find(i)) { mutableListOf() }.add(i) }
        for (members in groups.values) {
            // build adjacency within the group
            val memberSet = members.toHashSet()
            val nextOf = HashMap<Int, MutableList<Int>>()   // i → segments starting where i ends
            val prevOf = HashMap<Int, MutableList<Int>>()
            for (i in members) {
                val a = clipped[i]
                for (j in byStart[keyOf(a.pts.last())] ?: emptyList())
                    if (j != i && j in memberSet && clipped[j].roadType == a.roadType)
                        nextOf.getOrPut(i) { mutableListOf() }.add(j)
                for (j in byEnd[keyOf(a.pts.first())] ?: emptyList())
                    if (j != i && j in memberSet && clipped[j].roadType == a.roadType)
                        prevOf.getOrPut(i) { mutableListOf() }.add(j)
            }
            // walk from chain ends (segments with no prev)
            val usedIn = HashSet<Int>()
            for (start in members.sortedBy { it }) {   // deterministic order
                if (start in usedIn) continue
                if (prevOf[start]?.any { it !in usedIn } == true) continue   // not a chain end
                // walk forward
                val pts = ArrayList<LatLon>()
                val anchors = HashSet<Long>()
                var rank = 0
                var cur = start
                var firstType: String? = null
                while (true) {
                    usedIn.add(cur)
                    val c = clipped[cur]
                    if (firstType == null) firstType = c.roadType
                    // append points, dropping the duplicated joint point
                    if (pts.isNotEmpty() && pts.last() == c.pts.first()) {
                        for (k in 1 until c.pts.size) pts.add(c.pts[k])
                    } else {
                        pts.addAll(c.pts)
                    }
                    anchors.add(c.anchor)
                    if (c.rank > rank) rank = c.rank
                    val nxt = nextOf[cur]?.firstOrNull { it !in usedIn } ?: break
                    cur = nxt
                }
                if (pts.size >= 2 && firstType != null)
                    chains.add(Chain(firstType!!, rank, anchors, pts))
            }
        }
        // 2. RANK + SELECT with STICKINESS — on CHAINS, not pieces. Rank
        //    first, distance to the vehicle as tie-break (stable vs.
        //    tile-iteration order). A chain that was in the PREVIOUS frame
        //    keeps its slot while it is still a candidate (any of its
        //    member streets still in the window). CAPACITY: maxRoadSegs
        //    matches what the firmware can RENDER (13 roads × casing+fill)
        //    — the selection IS the render set.
        data class Ranked(val roadType: String, val pts: List<LatLon>, val rank: Int,
                          val dist2: Double, val anchors: Set<Long>)
        val rankedAll: List<Ranked> = chains.map {
            val p0 = MapProjector.projectPolyline(ref, listOf(it.pts.first()), scale100)
            val d2 = if (p0.isNotEmpty()) (p0[0].x * p0[0].x + p0[0].y * p0[0].y).toDouble()
                     else Double.MAX_VALUE
            Ranked(it.roadType, it.pts, it.rank, d2, it.anchors)
        }.sortedWith(
            compareByDescending<Ranked> { it.rank }.thenBy { it.dist2 }
        )

        val maxRoadSegs = minOf(MapFrame.MAX_SEGS - 1, RENDER_ROAD_CAPACITY)
        var ranked: List<Ranked> = rankedAll
        if (rankedAll.size > maxRoadSegs) {
            // sticky: a chain whose ANY member street was selected before
            val sticky = rankedAll.filter { r -> r.anchors.any { it in prevFrameStreetKeys } }
            if (sticky.size >= maxRoadSegs) {
                ranked = sticky.sortedWith(
                    compareByDescending<Ranked> { it.rank }.thenBy { it.dist2 }
                ).take(maxRoadSegs)
            } else {
                val stickyAnchors = sticky.flatMap { it.anchors }.toHashSet()
                val fill = rankedAll.filter { it.anchors.none { a -> a in stickyAnchors } }
                    .take(maxRoadSegs - sticky.size)
                ranked = sticky + fill
            }
        }
        // remember this frame's street anchors for the next frame's stickiness
        // — only streets that actually made it INTO the frame (the byte
        // budget can cut the tail; a "selected but not sent" street must not
        // be sticky, or it would block a fresh candidate forever)
        val sentAnchors = HashSet<Long>()

        var totalSegs = 1
        var budgetBytes = 3000
        for (street in ranked) {
            if (totalSegs >= MapFrame.MAX_SEGS || budgetBytes < 83) break
            val roadPts = street.pts
            // 3. SIMPLIFY the (already clipped + chained!) visible part.
            //    Uniform sampling to ≤20 points turned distant curves into
            //    crude polygons (20 points over hundreds of metres); the
            //    Douglas-Peucker-style simplification keeps the geometry:
            //    straight stretches lose points, curves keep them.
            //    Budget-driven: each point costs 4 wire bytes, so the
            //    street gets as many points as the remaining budget allows
            //    (capped at MAX_PTS).
            val allowedPts = minOf(MapFrame.MAX_PTS,
                (budgetBytes - 3 - 83) / 4).coerceAtLeast(2)
            val sampled = simplifyKeepEnds(roadPts, allowedPts)
            val rPts = MapProjector.projectPolyline(ref, sampled, scale100)
            if (rPts.size >= 2) {
                val widthPx = MapProjector.roadWidthPx(street.roadType, scale100)
                segments.add(Segment(SegmentType.Road, widthPx, rPts))
                totalSegs++
                budgetBytes -= (3 + rPts.size * 4)
                sentAnchors.addAll(street.anchors)
            }
        }
        prevFrameStreetKeys = sentAnchors
        log?.invoke("frame: route + ${totalSegs - 1} roads " +
            "(${segments.sumOf { it.points.size }} pts, ${budgetBytes}B Budget restlich)")
        // FLAG_SCALE_BAR dropped: the user removed the scale bar from the
        // display ("nobody needs metre numbers while riding") — the flag
        // made the PREVIEW renderer draw a two-bar scale at (20..120,455)
        // that read as a stray street piece bottom-left on EVERY image.
        return MapFrame(
            seq = seq,
            flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
            headingDeg10 = headingDeg10,
            scalePxPerMeter100 = scale100,
            segments = segments,
        )
    }
}
