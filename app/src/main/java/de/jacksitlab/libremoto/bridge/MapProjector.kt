/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * MapProjector — the pure projection used by OsmAndNavSource.mapFrame().
 *
 * Everything the display receives is computed HERE (WGS84 → vehicle-relative
 * pixels), so unit tests exercise exactly the code path that goes over BLE:
 *
 *   projectPolyline(): lat/lon list → vehicle-relative pixels (x east, y north
 *   NEGATIVE = up on screen), clamped to the ±260 px protocol window.
 *
 * The old copy of this logic inside OsmAndNavSource.mapFrame() has been
 * replaced by calls into this class — no divergence between test and app.
 */
package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.Point
import kotlin.math.cos

/** Minimal lat/lon value type for tests (mirrors ALatLon from the AIDL API). */
data class LatLon(val latitude: Double, val longitude: Double)

object MapProjector {

    const val METERS_PER_DEG_LAT = 111_320.0

    // Protocol window around the vehicle — ASYMMETRIC since the look-ahead
    // layout (vehicle at one third from the bottom, MAP_CY=320):
    //   ahead: 320 px of world (the full display above the vehicle)
    //   behind: 160 px (the display below it)
    //   sideways: ±320 px (the display is 480 wide; 240+320 = 560 > 480,
    //             so corners clip — fine, the firmware just won't draw
    //             off-screen pixels)
    // The i16 point space (±32767) has plenty of headroom for this.
    const val AHEAD_PX = 480   /* round display: the circle reaches 480 px
                                   ahead of the vehicle — the world must too */
    const val BEHIND_PX = 160
    const val SIDE_PX = 480

    /** Equirectangular meters-offset from ref, at the ref latitude. */
    fun toMetersDelta(ref: LatLon, p: LatLon): Pair<Double, Double> {
        val metersPerDegLon = METERS_PER_DEG_LAT * cos(Math.toRadians(ref.latitude))
        val dNorthM = (p.latitude - ref.latitude) * METERS_PER_DEG_LAT
        val dEastM = (p.longitude - ref.longitude) * metersPerDegLon
        return dEastM to dNorthM
    }

    /** Project one polyline to vehicle-relative pixels.
     *  scale100 = px per meter × 100 (MapFrame.scalePxPerMeter100).
     *  x = east, y = -north (route goes "up" = negative y).
     *
     *  NO window clamp: the old clamp folded everything outside the
     *  NORTH-aligned window onto its border — with the heading-rotated
     *  clip window this CORRUPTED geometry (a point 100 m ahead while
     *  driving SE got y=+BEHIND_PX: the route bent double, user saw a
     *  90° curve rendered at ~150°). Visible geometry is decided by
     *  clipPolylineToWindow; this projection is faithful. A wide
     *  ±3000 px guard only protects the i16 wire range; the firmware's
     *  brush clips off-screen pixels itself (mb_brush). */
    fun projectPolyline(ref: LatLon, pts: List<LatLon>, scale100: Int): List<Point> {
        val pxPerMeter = scale100 / 100.0
        val out = ArrayList<Point>(pts.size)
        val GUARD = 3000
        for (p in pts) {
            val (dEastM, dNorthM) = toMetersDelta(ref, p)
            val px = (dEastM * pxPerMeter).toInt().coerceIn(-GUARD, GUARD)
            val py = (-dNorthM * pxPerMeter).toInt().coerceIn(-GUARD, GUARD)
            out.add(Point(px, py))
        }
        return out
    }

    /** Clip a polyline against the HEADING-ROTATED visible window.
     *  The old bbox clip cut against a NORTH-ALIGNED box while the display
     *  is heading-rotated — driving south, the route left the box after
     *  BEHIND_M and the frame showed it ending mid-screen (user: "the
     *  street at the top is missing; it should bend 90° left up there").
     *  This clip works in vehicle-relative metres: the window is the
     *  rectangle [-ahead, +behind] along the driving axis, ±side across,
     *  rotated by `headingDeg`. Segments are returned in lat/lon. */
    fun clipPolylineToWindow(pts: List<LatLon>, ref: LatLon, headingDeg: Double,
                             aheadM: Double, behindM: Double, sideM: Double): List<List<LatLon>> {
        if (pts.size < 2) return emptyList()
        val mPerDegLon = METERS_PER_DEG_LAT * cos(Math.toRadians(ref.latitude))
        val h = Math.toRadians(headingDeg)
        val sh = kotlin.math.sin(h); val ch = kotlin.math.cos(h)
        // vehicle-relative metres (east, north) → window coords.
        // Rotation semantics MUST match the firmware's screen rotation
        // (map_render_impl.h): a point at bearing `heading` (the driving
        // direction) maps to FORWARD = screen up. The first version here
        // swapped the axes (forward ≈ lateral), so driving SE the route
        // landed BEHIND the window and was cut at behindM — the frame
        // showed it ending mid-screen. Correct:
        //   forward  = dN*cos(h) + dE*sin(h)
        //   lateral  = dE*cos(h) - dN*sin(h)   (+ = right of heading)
        fun toWin(p: LatLon): DoubleArray {
            val dE = (p.longitude - ref.longitude) * mPerDegLon
            val dN = (p.latitude - ref.latitude) * METERS_PER_DEG_LAT
            return doubleArrayOf(dN * ch + dE * sh,   // forward (+ = ahead)
                                 dE * ch - dN * sh)   // lateral (+ = right)
        }
        fun toLatLon(w: DoubleArray): LatLon {
            val dN = w[0] * ch - w[1] * sh
            val dE = w[1] * ch + w[0] * sh
            return LatLon(ref.latitude + dN / METERS_PER_DEG_LAT,
                          ref.longitude + dE / mPerDegLon)
        }
        val out = ArrayList<List<LatLon>>()
        var cur = ArrayList<LatLon>()
        var prevIn = false
        var prevW: DoubleArray? = null
        for (p in pts) {
            val w = toWin(p)
            val inside = w[0] >= -behindM && w[0] <= aheadM && kotlin.math.abs(w[1]) <= sideM
            if (inside) {
                if (!prevIn && prevW != null) {
                    // entered: interpolate the crossing point on the window border
                    val cross = borderCross(prevW!!, w, aheadM, behindM, sideM)
                    cur = ArrayList(); cur.add(toLatLon(cross)); cur.add(p)
                } else if (prevIn) {
                    cur.add(p)
                } else {
                    cur = ArrayList(); cur.add(p)
                }
            } else if (prevIn && prevW != null) {
                // left the window: interpolate the exit point
                val cross = borderCross(prevW!!, w, aheadM, behindM, sideM)
                cur.add(toLatLon(cross))
                if (cur.size >= 2) out.add(cur)
                cur = ArrayList()
            }
            prevIn = inside; prevW = w
        }
        if (cur.size >= 2) out.add(cur)
        return out
    }

    /** Where does the segment w0→w1 cross the window border? Simple
     *  parametric walk: find t where the path leaves the rectangle. */
    private fun borderCross(w0: DoubleArray, w1: DoubleArray,
                            aheadM: Double, behindM: Double, sideM: Double): DoubleArray {
        var lo = 0.0; var hi = 1.0
        fun insideAt(t: Double): Boolean {
            val f = w0[0] + (w1[0] - w0[0]) * t
            val l = w0[1] + (w1[1] - w0[1]) * t
            return f >= -behindM && f <= aheadM && kotlin.math.abs(l) <= sideM
        }
        if (insideAt(hi)) return w1.copyOf()
        for (i in 0 until 24) {
            val mid = (lo + hi) / 2
            if (insideAt(mid)) lo = mid else hi = mid
        }
        return doubleArrayOf(w0[0] + (w1[0] - w0[0]) * hi,
                             w0[1] + (w1[1] - w0[1]) * hi)
    }

    /** Clip a polyline to a lat/lon bbox. Returns the segments INSIDE the
     *  box — long streets contribute only their visible part instead of
     *  being clamped to the frame edge. A street crossing the box yields
     *  one segment; a street entering and leaving twice yields two. */
    fun clipPolylineToBbox(pts: List<LatLon>, minLat: Double, minLon: Double,
                           maxLat: Double, maxLon: Double): List<List<LatLon>> {
        if (pts.size < 2) return emptyList()
        val out = ArrayList<List<LatLon>>()
        var current = ArrayList<LatLon>()

        fun inside(p: LatLon) = p.latitude in minLat..maxLat && p.longitude in minLon..maxLon

        /** ALL intersections of segment a→b with the box borders (0..2),
         *  sorted along the segment direction. Handles plain crossings,
         *  corner grazes and through-tunnels uniformly. */
        fun crossings(a: LatLon, b: LatLon): List<LatLon> {
            data class Xing(val p: LatLon, val t: Double)
            val res = ArrayList<Xing>(4)
            // longitude borders
            if (a.longitude != b.longitude) {
                val t1 = (minLon - a.longitude) / (b.longitude - a.longitude)
                if (t1 in 0.0..1.0) {
                    val lat = a.latitude + t1 * (b.latitude - a.latitude)
                    if (lat in minLat..maxLat) res.add(Xing(LatLon(lat, minLon), t1))
                }
                val t2 = (maxLon - a.longitude) / (b.longitude - a.longitude)
                if (t2 in 0.0..1.0) {
                    val lat = a.latitude + t2 * (b.latitude - a.latitude)
                    if (lat in minLat..maxLat) res.add(Xing(LatLon(lat, maxLon), t2))
                }
            }
            // latitude borders
            if (a.latitude != b.latitude) {
                val t1 = (minLat - a.latitude) / (b.latitude - a.latitude)
                if (t1 in 0.0..1.0) {
                    val lon = a.longitude + t1 * (b.longitude - a.longitude)
                    if (lon in minLon..maxLon) res.add(Xing(LatLon(minLat, lon), t1))
                }
                val t2 = (maxLat - a.latitude) / (b.latitude - a.latitude)
                if (t2 in 0.0..1.0) {
                    val lon = a.longitude + t2 * (b.longitude - a.longitude)
                    if (lon in minLon..maxLon) res.add(Xing(LatLon(maxLat, lon), t2))
                }
            }
            // dedupe near-identical points (corner grazes hit two borders)
            val dedup = ArrayList<Xing>(res.size)
            for (x in res.sortedBy { it.t }) {
                val last = dedup.lastOrNull()?.p
                if (last == null ||
                    (kotlin.math.abs(last.latitude - x.p.latitude) > 1e-12 ||
                     kotlin.math.abs(last.longitude - x.p.longitude) > 1e-12)) {
                    dedup.add(x)
                }
            }
            return dedup.map { it.p }
        }

        var prev = pts[0]
        var prevIn = inside(prev)
        if (prevIn) current.add(prev)
        for (i in 1 until pts.size) {
            val p = pts[i]
            val pIn = inside(p)
            if (pIn && prevIn) {
                current.add(p)
            } else if (pIn != prevIn) {
                // one endpoint inside: the crossing starts or ends a segment
                val x = crossings(prev, p).firstOrNull() ?: (if (pIn) prev else p)
                if (pIn) {
                    current.add(x); current.add(p)
                } else {
                    current.add(x)
                    if (current.size >= 2) out.add(current)
                    current = ArrayList()
                }
            } else {
                // both outside: a THROUGH-tunnel contributes its inner piece
                val xs = crossings(prev, p)
                if (xs.size >= 2) {
                    val seg = ArrayList<LatLon>(2)
                    seg.add(xs[0]); seg.add(xs[1])
                    out.add(seg)
                }
            }
            prev = p; prevIn = pIn
        }
        if (current.size >= 2) out.add(current)
        return out
    }

    /** Route-contact classification (Beeline-style, per user feedback):
     *  0 = no contact (culled)
     *  1 = CROSSES the route/trail, or an endpoint LIES ON it (T-junction
     *      mouth) — shown BOUNDED (cutToContactLength)
     *  2 = the route RUNS ALONG the street (the road we drive on!) —
     *      shown IN FULL within the window.
     *  Type 2 was missing and bit on the motorway: the A115 is one long
     *  way with no junctions — it neither crosses the route nor ends on
     *  it, so the filter dropped the very road the user was driving on
     *  (roads=0 for 800 m on the A115). In the city this never showed:
     *  OSM splits streets into short ways at every junction, so the
     *  own road entered via the endpoint test. */
    const val CONTACT_NONE = 0
    const val CONTACT_CROSS = 1
    const val CONTACT_ALONG = 2

    /* ---- Route contact index (the 2 fps fix, take 2) ------------------
     * The first fix (route subsample to 8 m) barely helped: OsmAnd route
     * points arrive every ~5 m, so 8 m subsampling only cut the segment
     * count ~1.6x (240 -> 150) — measured contact stayed at 193-357 ms.
     * The real structure: the scan is O(street pts x route segs) and the
     * route is LONG. A 30 m grid fixes the lookup: each route segment is
     * registered in every cell its bbox touches; a point query scans the
     * 3x3 neighbourhood — a segment within 15 m of the point is always
     * registered there. Point tests become ~O(1).
     *
     * Built ONCE per frame in FrameBuilder and shared by all streets;
     * the 3-arg streetRouteContact() builds one per call for the other
     * callers (routeRoadMaxspeed, tests — background paths). */
    class RouteContactIndex(route: List<LatLon>, refLat: Double) {
        private val mLat = METERS_PER_DEG_LAT
        private val mLon = METERS_PER_DEG_LAT * kotlin.math.cos(Math.toRadians(refLat))
        private val cellLat = 30.0 / mLat
        private val cellLon = 30.0 / mLon
        /** route subsampled to >= 8 m spacing (first/last kept). */
        val pts: List<LatLon>
        private val grid = HashMap<Long, ArrayList<Int>>()

        init {
            val sub = ArrayList<LatLon>(route.size / 4 + 4)
            if (route.size >= 2) {
                var lastKept = route.first()
                sub.add(lastKept)
                for (i in 1 until route.size - 1) {
                    val p = route[i]
                    val dLat = (p.latitude - lastKept.latitude) * mLat
                    val dLon = (p.longitude - lastKept.longitude) * mLon
                    if (dLat * dLat + dLon * dLon >= 8.0 * 8.0) { sub.add(p); lastKept = p }
                }
                if (sub.last() != route.last()) sub.add(route.last())
            }
            pts = if (sub.size >= 2) sub else route
            for (i in 0 until pts.size - 1) {
                val a = pts[i]; val b = pts[i + 1]
                val la0 = latIdx(minOf(a.latitude, b.latitude))
                val la1 = latIdx(maxOf(a.latitude, b.latitude))
                val lo0 = lonIdx(minOf(a.longitude, b.longitude))
                val lo1 = lonIdx(maxOf(a.longitude, b.longitude))
                for (la in la0..la1) for (lo in lo0..lo1)
                    grid.getOrPut(pack(la, lo)) { ArrayList(2) }.add(i)
            }
        }

        private fun latIdx(lat: Double) = Math.floor(lat / cellLat).toInt()
        private fun lonIdx(lon: Double) = Math.floor(lon / cellLon).toInt()
        private fun pack(la: Int, lo: Int) =
            ((la.toLong()) shl 32) or (lo.toLong() and 0xFFFFFFFFL)

        /** True if any route segment is within 15 m of p (early exit). */
        fun within15(p: LatLon): Boolean {
            val la = latIdx(p.latitude); val lo = lonIdx(p.longitude)
            for (dla in -1..1) for (dlo in -1..1) {
                val segs = grid[pack(la + dla, lo + dlo)] ?: continue
                for (si in segs)
                    if (pointSegmentDistM(p, pts[si], pts[si + 1], mLat, mLon) <= 15.0) return true
            }
            return false
        }

        /** Min distance from p to the (indexed) route, in metres. */
        fun distToRoute(p: LatLon): Double {
            val la = latIdx(p.latitude); val lo = lonIdx(p.longitude)
            var best = Double.MAX_VALUE
            for (dla in -1..1) for (dlo in -1..1) {
                val segs = grid[pack(la + dla, lo + dlo)] ?: continue
                for (si in segs) {
                    val d = pointSegmentDistM(p, pts[si], pts[si + 1], mLat, mLon)
                    if (d < best) best = d
                }
            }
            return best
        }

        /** Route segments whose bbox shares a grid cell with the street
         *  segment's bbox (a crossing implies a shared cell — no expansion
         *  needed). Deduped into `out` (caller reuses the set). */
        fun segmentsNear(a1: LatLon, a2: LatLon, out: MutableSet<Int>) {
            val la0 = latIdx(minOf(a1.latitude, a2.latitude))
            val la1 = latIdx(maxOf(a1.latitude, a2.latitude))
            val lo0 = lonIdx(minOf(a1.longitude, a2.longitude))
            val lo1 = lonIdx(maxOf(a1.longitude, a2.longitude))
            for (la in la0..la1) for (lo in lo0..lo1) {
                val segs = grid[pack(la, lo)] ?: continue
                out.addAll(segs)
            }
        }
    }

    /** Contact classification against a prebuilt index (fast path used
     *  by FrameBuilder — one index per frame, all streets share it). */
    fun streetRouteContact(pts: List<LatLon>, idx: RouteContactIndex): Int {
        if (pts.size < 2 || idx.pts.size < 2) return CONTACT_NONE

        // (c) route runs ALONG the street = the road we DRIVE on.
        var run = 0; var bestRun = 0; var near = 0
        for (p in pts) {
            if (idx.within15(p)) { near++; run++; if (run > bestRun) bestRun = run }
            else run = 0
        }
        if (bestRun >= 4 || near * 5 >= pts.size) return CONTACT_ALONG

        // (a) any street segment crosses any route segment
        val cand = HashSet<Int>(16)
        for (i in 0 until pts.size - 1) {
            val a1 = pts[i]; val a2 = pts[i + 1]
            idx.segmentsNear(a1, a2, cand)
            for (j in cand) {
                val b1 = idx.pts[j]; val b2 = idx.pts[j + 1]
                val aMinLat = minOf(a1.latitude, a2.latitude); val aMaxLat = maxOf(a1.latitude, a2.latitude)
                val aMinLon = minOf(a1.longitude, a2.longitude); val aMaxLon = maxOf(a1.longitude, a2.longitude)
                if (b2.latitude < aMinLat || b1.latitude > aMaxLat) continue
                if (maxOf(b1.longitude, b2.longitude) < aMinLon ||
                    minOf(b1.longitude, b2.longitude) > aMaxLon) continue
                if (segmentsCross(a1, a2, b1, b2)) return CONTACT_CROSS
            }
            cand.clear()
        }
        // (b) T-junction: a street ENDPOINT on the route
        if (idx.within15(pts.first()) || idx.within15(pts.last())) return CONTACT_CROSS
        return CONTACT_NONE
    }

    /** Legacy 3-arg form: builds an index per call (background callers). */
    fun streetRouteContact(pts: List<LatLon>, route: List<LatLon>,
                           refLat: Double): Int =
        streetRouteContact(pts, RouteContactIndex(route, refLat))

    /** Bound a crossing street's drawn length around its closest contact
     *  with the route — index form (shared per-frame index). */
    fun cutToContactLength(pts: List<LatLon>, idx: RouteContactIndex,
                           maxLenM: Double): List<LatLon> {
        if (pts.size < 2 || idx.pts.size < 2) return pts
        var bestIdx = 0; var bestD = Double.MAX_VALUE
        for (i in pts.indices) {
            val d = idx.distToRoute(pts[i])
            if (d < bestD) { bestD = d; bestIdx = i }
        }
        var budget = maxLenM
        var lo = bestIdx
        while (lo > 0) {
            val d = haversineM(pts[lo - 1], pts[lo])
            if (d > budget) break
            budget -= d; lo--
        }
        var hi = bestIdx
        while (hi < pts.size - 1) {
            val d = haversineM(pts[hi], pts[hi + 1])
            if (d > budget) break
            budget -= d; hi++
        }
        return pts.subList(lo, hi + 1)
    }

    /** Legacy 4-arg cutToContactLength (index built per call). */
    fun cutToContactLength(pts: List<LatLon>, route: List<LatLon>,
                           maxLenM: Double, refLat: Double): List<LatLon> =
        cutToContactLength(pts, RouteContactIndex(route, refLat), maxLenM)

    /** Backwards-compatible boolean view of streetRouteContact. */
    fun streetTouchesRoute(pts: List<LatLon>, route: List<LatLon>,
                           refLat: Double): Boolean =
        streetRouteContact(pts, route, refLat) != CONTACT_NONE

    /** Point-to-point distance in metres (equirectangular, refLat). */
    private fun haversineM(a: LatLon, b: LatLon): Double {
        val mLat = METERS_PER_DEG_LAT
        val mLon = METERS_PER_DEG_LAT * kotlin.math.cos(Math.toRadians(a.latitude))
        val dLat = (b.latitude - a.latitude) * mLat
        val dLon = (b.longitude - a.longitude) * mLon
        return kotlin.math.sqrt(dLat * dLat + dLon * dLon)
    }

    /** 2D segment intersection test (lat/lon as plane coords — fine at
     *  city scales for a yes/no crossing decision). */
    private fun segmentsCross(a1: LatLon, a2: LatLon, b1: LatLon, b2: LatLon): Boolean {
        fun cross(ox: Double, oy: Double, ax: Double, ay: Double, bx: Double, by: Double) =
            (ax - ox) * (by - oy) - (ay - oy) * (bx - ox)
        val d1 = cross(b1.latitude, b1.longitude, b2.latitude, b2.longitude, a1.latitude, a1.longitude)
        val d2 = cross(b1.latitude, b1.longitude, b2.latitude, b2.longitude, a2.latitude, a2.longitude)
        val d3 = cross(a1.latitude, a1.longitude, a2.latitude, a2.longitude, b1.latitude, b1.longitude)
        val d4 = cross(a1.latitude, a1.longitude, a2.latitude, a2.longitude, b2.latitude, b2.longitude)
        return ((d1 > 0) != (d2 > 0)) && ((d3 > 0) != (d4 > 0))
    }

    /** Distance in metres from point p to segment a-b (equirectangular). */
    fun pointSegmentDistM(p: LatLon, a: LatLon, b: LatLon,
                                  mLat: Double, mLon: Double): Double {
        val px = (p.longitude - a.longitude) * mLon
        val py = (p.latitude - a.latitude) * mLat
        val dx = (b.longitude - a.longitude) * mLon
        val dy = (b.latitude - a.latitude) * mLat
        val len2 = dx * dx + dy * dy
        val t = if (len2 == 0.0) 0.0 else ((px * dx + py * dy) / len2).coerceIn(0.0, 1.0)
        val ex = px - t * dx; val ey = py - t * dy
        return kotlin.math.sqrt(ex * ex + ey * ey)
    }

    /** Rotate vehicle-relative points around the vehicle so that `headingDeg`
     *  points up on screen — the transform the firmware performs for display
     *  (map_render_impl.h). Used by tests to render "what the display shows".
     *  Screen coords: vehicle at (MAP_CX, MAP_CY) = (240, 320) — one third
     *  from the bottom (look-ahead layout); +x right, +y down. */
    fun rotateForDisplay(pts: List<Point>, headingDeg: Double,
                         cx: Double = 240.0, cy: Double = 320.0): List<Point> {
        val theta = Math.toRadians(headingDeg)
        val ct = cos(theta); val st = kotlin.math.sin(theta)
        return pts.map { p ->
            val x = p.x.toDouble(); val y = p.y.toDouble()
            Point(
                (x * ct + y * st + cx).toInt(),
                (-x * st + y * ct + cy).toInt(),
            )
        }
    }

    // ---- road widths (OSM highway type → display width) -------------------
    // The firmware draws a ROAD segment as TWO edge lines offset ±width/2 px
    // along the polyline normals (map_render_impl.h) — "black line with white
    // border". The width byte is the EDGE DISTANCE, not a stroke width.
    //
    // True-to-scale roads would be unreadably thin (6 m at 0.48 px/m = 3 px),
    // so widths are exaggerated by ROAD_EXAGGERATION for the two-line look.

    /** Assumed real carriageway width in metres per OSM highway type. */
    fun roadWidthMeters(highway: String): Double = when (highway) {
        "motorway", "motorway_link" -> 16.0
        "trunk", "trunk_link" -> 12.0
        "primary", "primary_link" -> 10.0
        "secondary", "secondary_link" -> 8.0
        "tertiary", "tertiary_link" -> 7.0
        "residential", "unclassified", "living_street" -> 6.0
        "service" -> 3.5
        else -> 6.0
    }

    /** Importance rank for budget-constrained segment selection:
     *  higher = more important, shown first when the frame budget is tight. */
    fun roadRank(highway: String): Int = when (highway) {
        "motorway", "motorway_link" -> 9
        "trunk", "trunk_link" -> 8
        "primary", "primary_link" -> 7
        "secondary", "secondary_link" -> 6
        "tertiary", "tertiary_link" -> 5
        "residential", "unclassified" -> 4
        "living_street" -> 3
        "road" -> 2
        "service" -> 1
        else -> 0
    }

    /** Display exaggeration: roads drawn wider than true scale so the
     *  casing+fill body stays readable at navigation zooms.
     *  1.8 (was 2.5): at scale 48 a residential street is 5 px fill / 7 px
     *  band. Two parallel streets 15 m apart (dual carriageways!) then keep
     *  a ~2 px black separator; at 2.5 the bands overlapped and the pair
     *  rendered as ONE wide street (user report at 52.393471,12.595995). */
    const val ROAD_EXAGGERATION = 1.8

    /** Minimum road body width (fill) so a street stays visible. */
    const val MIN_ROAD_PX = 3

    /** Road segment width byte = the road's FILL width in px (the firmware
     *  draws casing = fill+2 underneath). */
    fun roadWidthPx(highway: String, scale100: Int): Int {
        val pxPerMeter = scale100 / 100.0
        return (roadWidthMeters(highway) * pxPerMeter * ROAD_EXAGGERATION)
            .toInt().coerceIn(MIN_ROAD_PX, 13)
    }

    /** Route segment width byte (stroke width in px). Clearly WIDER than
     *  the road it lies on (residential fill + 2 px) so the route reads as
     *  the dominant white line on the black road body — user request after
     *  the first corridor rides ("make the white line thicker"). */
    fun routeWidthPx(scale100: Int): Int =
        (roadWidthPx("residential", scale100) + 2).toInt().coerceIn(3, 16)

    /** Both edge polylines of a road, offset ±half px along the averaged
     *  vertex normals — byte-exact mirror of the firmware (map_render_impl.h,
     *  pass 1a), used by tests to render what the display really shows. */
    fun roadEdges(pts: List<Point>, widthPx: Int): Pair<List<Point>, List<Point>> {
        var half = widthPx * 0.5
        if (half < 1.5) half = 1.5   // firmware floor
        val n = pts.size
        fun offset(sign: Double): List<Point> {
            val out = ArrayList<Point>(n)
            for (i in 0 until n) {
                val x = pts[i].x.toDouble(); val y = pts[i].y.toDouble()
                var nx = 0.0; var ny = 0.0
                if (n > 1) {
                    if (i < n - 1) {
                        val dx = pts[i + 1].x - x; val dy = pts[i + 1].y - y
                        val L = kotlin.math.sqrt(dx * dx + dy * dy)
                        if (L > 0.0001) { nx += -dy / L; ny += dx / L }
                    }
                    if (i > 0) {
                        val dx = x - pts[i - 1].x; val dy = y - pts[i - 1].y
                        val L = kotlin.math.sqrt(dx * dx + dy * dy)
                        if (L > 0.0001) { nx += -dy / L; ny += dx / L }
                    }
                }
                val L = kotlin.math.sqrt(nx * nx + ny * ny)
                if (L < 0.0001) { nx = 0.0; ny = 1.0 } else { nx /= L; ny /= L }
                out.add(Point((x + nx * half * sign).toInt(), (y + ny * half * sign).toInt()))
            }
            return out
        }
        return offset(1.0) to offset(-1.0)
    }
}
