package de.jacksitlab.libremoto.bridge

import android.annotation.SuppressLint
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.RemoteException
import de.jacksitlab.libremoto.protocol.MapFrame
import de.jacksitlab.libremoto.protocol.NavData
import de.jacksitlab.libremoto.protocol.NavType
import de.jacksitlab.libremoto.protocol.Maneuver
import de.jacksitlab.libremoto.protocol.Point
import de.jacksitlab.libremoto.protocol.Segment
import de.jacksitlab.libremoto.protocol.SegmentType
import net.osmand.aidlapi.IOsmAndAidlCallback
import net.osmand.aidlapi.IOsmAndAidlInterface
import net.osmand.aidlapi.map.ALatLon
import net.osmand.aidlapi.navigation.ADirectionInfo
import net.osmand.aidlapi.navigation.ANavigationUpdateParams
import net.osmand.aidlapi.navigation.ARouteUpdate
import net.osmand.aidlapi.navigation.ARouteUpdateParams
import net.osmand.aidlapi.navigation.ActiveRouteGeometry
import net.osmand.aidlapi.navigation.GetActiveRouteParams
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt
import kotlin.math.atan2

/**
 * OsmAnd navigation source via V2 AIDL (net.osmand.aidlapi).
 *
 * Uses two AIDL features:
 *  - registerForNavigationUpdates: turn/distance ticks (ADirectionInfo)
 *  - getActiveRouteGeometry: WGS84 polyline of the calculated route
 *  - registerForRouteUpdates: lifecycle events (recalculated, cancelled, finished)
 *
 * The route polyline is converted to a MapFrame (pixel coordinates relative
 * to the vehicle) and sent to the hardware. The hardware renders the actual
 * route with curves, not just a straight placeholder.
 */
class OsmAndNavSource(
    private val context: Context,
    private val log: (String) -> Unit,
) : NavSource {

    override val label: String = "OsmAnd (AIDL V2)"

    @Volatile private var bound = false
    @Volatile private var navCallbackId = -1L
    @Volatile private var routeCallbackId = -1L
    private var svc: IOsmAndAidlInterface? = null
    private val main = Handler(Looper.getMainLooper())

    @Volatile private var navActive = false
    @Volatile private var distanceToM: Int = 0
    @Volatile private var turnType: Int = 1

    // Route geometry (WGS84 polyline)
    @Volatile private var routePoints: List<ALatLon> = emptyList()
    @Volatile private var routeVersion: Int = 0

    // Nearby roads (WGS84 polylines) — refreshed every tick.
    // PRIMARY SOURCE: the prebuilt .lmm vector maps (MapManager) — a bbox
    // query in memory, deterministic and complete (no 50 m proximity filter,
    // no maxRoads cap, no OsmAnd CPU load). FALLBACK: AIDL getNearbyRoads,
    // used when no .lmm region covers the position.
    @Volatile private var nearbyRoads: List<net.osmand.aidlapi.map.ARoadPolyline> = emptyList()
    /** MapManager reference — set via attachMapManager(); owns the .lmm cache. */
    @Volatile private var mapManager: MapManager? = null

    fun attachMapManager(mm: MapManager) { mapManager = mm }

    // The last directionInfo we got, exposed for tests/inspection.
    var lastDirection: ADirectionInfo? = null
        private set

    // ---- speed-adaptive zoom -------------------------------------------
    // Voraussicht je nach Tempo: ~500 m in der Stadt (langsam, ≤ 30 km/h),
    // ~1000 m ahead (fast, ≥ 100 km/h), linear in between.
    // Skala: 240 px sind vor dem Fahrzeug sichtbar (480-px-Panel, Fahrzeug
    // mittig) → scale = 240 / lookahead_m × 100 (px/m × 100).
    private val zoomMinLookaheadM = 500.0
    private val zoomMaxLookaheadM = 1000.0
    private val zoomLowSpeedKmh = 30.0
    private val zoomHighSpeedKmh = 100.0

    /** Smoothed speed km/h (EMA over ticks, robust against GPS jumps). */
    @Volatile private var smoothedSpeedKmh = 0.0
    @Volatile private var lastVehiclePos: ALatLon? = null
    @Volatile private var lastVehiclePosAtMs = 0L

    /** Breadcrumb trail behind the vehicle (most recent first): the road we
     *  actually rode. OsmAnd cuts the passed segment from the route, so the
     *  corridor filter would cull the streets behind us — the trail gives
     *  them their corridor back. Capped at ~1.5 km / 120 points. */
    private val trail = ArrayList<LatLon>()
    private var trailMetres = 0.0

    /** Current projection scale px/m × 100 (soft-follows the speed). */
    @Volatile private var dynamicScale = 24   // Start: 1000 m Voraussicht
    /** Lookahead in m that the current dynamicScale covers (for the road radius). */
    @Volatile private var currentLookaheadM = 1000.0
    /** Zoom-Hysterese: true = der Zoom folgt dem Ziel gerade (siehe 2c). */
    @Volatile private var zoomTracking = true

    /** Target scale from the smoothed speed (px/m × 100).
     *  Reference: AHEAD_PX (320) — the display area in front of the vehicle
     *  (look-ahead layout). The SAME metre specs (500 m city … 1000 m
     *  country) now fill 320 px instead of 240 → everything renders 33 %
     *  larger → much better readability at unchanged lookahead metres. */
    private fun targetScale(): Int {
        val t = ((smoothedSpeedKmh - zoomLowSpeedKmh) /
                 (zoomHighSpeedKmh - zoomLowSpeedKmh)).coerceIn(0.0, 1.0)
        val lookahead = zoomMinLookaheadM + t * (zoomMaxLookaheadM - zoomMinLookaheadM)
        return (320.0 / lookahead * 100.0).toInt()
    }

    /** DISCRETE zoom levels: quantize the continuous target to steps of 8
     *  (scale 24 = 1000 m, 32 = 750 m, 40 = 600 m, 48 = 500 m …) with a
     *  ±(step/2+2) hysteresis. The EMA speed wobbles ±10 km/h in the city —
     *  a continuously-tracked scale made the window breathe constantly
     *  (96 changes in 7 min in lm-4.log), and every scale step dropped and
     *  re-added the streets at the window edge (visible flicker). With
     *  quantized levels the window only changes on REAL speed changes. */
    private var quantTargetScale = 24   // start: 1000 m Voraussicht

    private fun updateQuantTarget(contTarget: Int): Int {
        val step = 8
        val margin = step / 2 + 2   // 6: switch level only clearly beyond half a step
        if (contTarget > quantTargetScale + margin) quantTargetScale += step
        else if (contTarget < quantTargetScale - margin) quantTargetScale -= step
        return quantTargetScale
    }

    private val callback = object : IOsmAndAidlCallback.Stub() {
        override fun onSearchComplete(resultSet: List<net.osmand.aidlapi.search.SearchResult>) {}
        override fun onUpdate() {}
        override fun onAppInitialized() {}
        override fun onGpxBitmapCreated(bitmap: net.osmand.aidlapi.gpx.AGpxBitmap?) {}
        override fun updateNavigationInfo(directionInfo: ADirectionInfo?) {
            directionInfo?.let { d ->
                navActive = true
                distanceToM = d.distanceTo
                turnType = d.turnType
                lastDirection = d
                log("osmAnd: in ${d.distanceTo} m → ${maneuver(d.turnType)}")
            }
        }
        override fun onContextMenuButtonClicked(buttonId: Int, pointId: String?, layerId: String?) {}
        override fun onVoiceRouterNotify(params: net.osmand.aidlapi.navigation.OnVoiceNavigationParams) {}
        override fun onKeyEvent(params: android.view.KeyEvent?) {}
        override fun onRouteUpdate(update: ARouteUpdate?) {
            update?.let { u ->
                log("osmAnd: route event ${routeEventName(u.eventType)} (v=${u.routeVersion})")
                if (u.eventType == ARouteUpdate.ROUTE_EVENT_RECALCULATED) {
                    pullRouteGeometry()
                } else if (u.eventType == ARouteUpdate.ROUTE_EVENT_CANCELLED ||
                           u.eventType == ARouteUpdate.ROUTE_EVENT_FINISHED) {
                    routePoints = emptyList()
                    routeVersion++
                    navActive = false
                    log("osmAnd: route cleared")
                }
            }
        }
        override fun onLogcatMessage(params: net.osmand.aidlapi.logcat.OnLogcatMessageParams?) {}
    }

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, iBinder: IBinder) {
            svc = IOsmAndAidlInterface.Stub.asInterface(iBinder)
            log("osmAnd: service connected (V2)")
        }
        override fun onServiceDisconnected(name: ComponentName) {
            svc = null
            bound = false
            navCallbackId = -1L
            routeCallbackId = -1L
            log("osmAnd: service disconnected")
        }
    }

    /** TurnType int → LibreMoto Maneuver. Mirrors net.osmand.router.TurnType. */
    private fun maneuver(t: Int): Maneuver = when (t) {
        1 -> Maneuver.Straight
        2, 4 -> Maneuver.Left
        3 -> Maneuver.SlightLeft
        5, 7 -> Maneuver.Right
        6 -> Maneuver.SlightRight
        10, 11 -> Maneuver.UTurn
        8, 9 -> Maneuver.Merge
        12 -> Maneuver.Straight
        13, 14 -> Maneuver.Left
        else -> Maneuver.Straight
    }

    private fun routeEventName(eventType: Int): String = when (eventType) {
        ARouteUpdate.ROUTE_EVENT_RECALCULATED -> "RECALCULATED"
        ARouteUpdate.ROUTE_EVENT_CANCELLED -> "CANCELLED"
        ARouteUpdate.ROUTE_EVENT_FINISHED -> "FINISHED"
        else -> "UNKNOWN($eventType)"
    }

    override val isNavigating: Boolean get() = bound && navActive

    /** Smoothed heading degrees × 10 — the map ROTATES by this, so raw
     *  GPS bearing jitter (±5-10°) made the whole map snap every frame.
     *  EMA over the shortest angular path + a capped turn rate per tick
     *  (max 1.5°/tick at 2 Hz = 3°/s) let the view ease into turns. */
    @Volatile private var smoothHeadingDeg10 = 0
    @Volatile private var smoothHeadingValid = false

    /** Last GPS bearing in degrees × 10 (for MapFrame heading).
     *  Updated by refresh() from the Android LocationManager. */
    @Volatile private var headingDeg10: Int = 0

    override fun refresh() {
        // 1. Pull current route geometry (fast: a few hundred points, ~10 ms)
        if (bound && svc != null && navActive) {
            pullRouteGeometry()
        }

        // 2. Compute bearing from the route polyline — SMOOTHED: the raw
        //    bearing wobbles with GPS noise; feeding it straight into the
        //    frame rotation made the map snap on every frame.
        val pts = routePoints
        if (pts.size >= 2) {
            val raw = ((bearingFromPolyline(pts) * 10).toInt() % 3600 + 3600) % 3600
            if (!smoothHeadingValid) {
                smoothHeadingDeg10 = raw
                smoothHeadingValid = true
            } else {
                // shortest angular delta raw - smooth
                var d = raw - smoothHeadingDeg10
                if (d > 1800) d -= 3600
                else if (d < -1800) d += 3600
                // EMA (alpha 0.5) + turn-rate cap 45 (=4.5°/tick at
                // 5 Hz = 22°/s): real curves settle in ~2-4 s while
                // still filtering GPS bearing noise
                val step = (d * 0.5).toInt().coerceIn(-45, 45)
                smoothHeadingDeg10 = ((smoothHeadingDeg10 + step) % 3600 + 3600) % 3600
            }
            headingDeg10 = smoothHeadingDeg10
        }

        // 2b. Speed from vehicle movement (routePoints[0] = position, 1 Hz ticks)
        //     EMA-smoothed; skips implausible jumps (> 250 km/h).
        if (pts.isNotEmpty()) {
            val pos = pts[0]
            val now = System.currentTimeMillis()
            val last = lastVehiclePos
            val lastAt = lastVehiclePosAtMs
            if (last != null && lastAt > 0 && now > lastAt) {
                val dtS = (now - lastAt) / 1000.0
                val distM = distanceM(last, pos)
                // breadcrumb: extend the trail behind the vehicle
                if (distM > 4.0) {   // ignore GPS jitter below 4 m
                    trail.add(0, LatLon(last.latitude, last.longitude))
                    trailMetres += distM
                    while (trailMetres > 1500.0 && trail.size > 2) {
                        val drop = trail.removeAt(trail.size - 1)
                        val nxt = trail.lastOrNull()
                        if (nxt != null) {
                            trailMetres -= distanceM(
                                net.osmand.aidlapi.map.ALatLon(drop.latitude, drop.longitude),
                                net.osmand.aidlapi.map.ALatLon(nxt.latitude, nxt.longitude))
                        }
                    }
                    if (trail.size > 120) trail.subList(120, trail.size).clear()
                }
                val kmh = (distM / dtS) * 3.6
                if (kmh <= 250.0) {
                    val alpha = 0.3   // EMA: ~3 ticks until a new speed registers
                    smoothedSpeedKmh = smoothedSpeedKmh * (1 - alpha) + kmh * alpha
                }
            }
            lastVehiclePos = pos
            lastVehiclePosAtMs = now
        }

        // 2c. Zoom in DISCRETE LEVELS (quantized target, hysteresis ±6):
        //     the continuous EMA-speed target wobbled ±10 km/h and the
        //     ±2/tick ramp chased it — the window breathed and edge
        //     streets flickered with every scale step (96 zoom changes
        //     in 7 min, lm-4.log). Discrete levels only change on real
        //     speed transitions; between them the window is ROCK STABLE.
        val target = updateQuantTarget(targetScale())
        val diff = target - dynamicScale
        when {
            diff != 0 -> zoomTracking = true
            else -> zoomTracking = false
        }
        if (zoomTracking) {
            // ramp at most ±2 per tick toward the discrete level (no jump)
            dynamicScale = if (kotlin.math.abs(diff) <= 2) target
                           else dynamicScale + (if (diff > 0) 2 else -2)
            currentLookaheadM = 320.0 / (dynamicScale / 100.0)
            if (dynamicScale != lastLoggedScale) {
                log("osmAnd: zoom: ${"%.0f".format(smoothedSpeedKmh)} km/h → " +
                    "Voraussicht ${"%.0f".format(currentLookaheadM)} m (scale=$dynamicScale)")
                lastLoggedScale = dynamicScale
            }
        }

        // 3. Pull nearby roads on a background thread. THROTTLED per source:
        //    the .lmm query is cheap in-memory work (~40 ms) — pull it every
        //    LMM_INTERVAL_MS (1 s) so the frame's street set stays fresh as
        //    the window moves; the AIDL fallback is a slow binder call
        //    (200-500 ms route-index scan) and stays at NEARBY_INTERVAL_MS.
        val mm = mapManager
        val interval = if (mm != null && mm.loadedRegions().isNotEmpty())
                           LMM_INTERVAL_MS else NEARBY_INTERVAL_MS
        if (bound && svc != null && navActive && routePoints.isNotEmpty() && !nearbyPulling &&
            System.currentTimeMillis() - lastNearbyPullMs >= interval) {
            nearbyPulling = true
            lastNearbyPullMs = System.currentTimeMillis()
            nearbyExecutor.execute {
                pullNearbyRoads()
                nearbyPulling = false
            }
        }
    }

    override fun navData(): NavData {
        if (!navActive) return NavData(NavType.Idle)
        return NavData(
            type = NavType.Nav,
            maneuver = maneuver(turnType),
            distM = distanceToM,
            limitKmh = if (currentLimitKmh > 0) currentLimitKmh else null,
            mapOn = true,
            speedKmh = smoothedSpeedKmh.toInt().coerceIn(0, 255),
        )
    }

    /**
     * Build a MapFrame from the route polyline.
     *
     * The polyline (WGS84 lat/lon) is transformed into vehicle-relative pixel
     * coordinates. The vehicle is at (0, 0); the route stretches "up" (negative y).
     * We use a simple equirectangular projection scaled to ~0.1 px/m.
     */
    override fun mapFrame(seq: Int): MapFrame {
        val pts = routePoints
        if (pts.size < 2) {
            // Fallback: straight placeholder from distanceTo, at the current zoom
            return FrameBuilder.fallbackFrame(seq, headingDeg10, dynamicScale, distanceToM)
        }

        // route + streets through the SHARED FrameBuilder (also used by the
        // end-to-end preview test — no divergence between test and app)
        val route = pts.map { LatLon(it.latitude, it.longitude) }
        val streets = nearbyRoads.map { r ->
            StreetRoad(r.roadType, r.points.map { LatLon(it.latitude, it.longitude) })
        }
        val trailSnapshot = synchronized(trail) { ArrayList(trail) }
        return FrameBuilder.buildFrame(seq, headingDeg10, dynamicScale, route, streets, trailSnapshot) { line ->
            log("osmAnd: $line")
        }
    }

    override fun batteryPercent(): Int? {
        val filter = android.content.IntentFilter(android.content.Intent.ACTION_BATTERY_CHANGED)
        val i = context.registerReceiver(null, filter) ?: return null
        val lvl = i.getIntExtra(android.os.BatteryManager.EXTRA_LEVEL, -1)
        val scale = i.getIntExtra(android.os.BatteryManager.EXTRA_SCALE, 100)
        return if (scale > 0) lvl * 100 / scale else null
    }

    @SuppressLint("MissingPermission")
    override fun start() {
        if (bound) return
        // .lmm vector maps are owned by MapManager (attached via
        // attachMapManager); if none is attached, side streets fall back
        // to AIDL. MapManager.loadAll() runs in the MainActivity.
        // V2 service action
        val intent = Intent("net.osmand.aidl.OsmandAidlServiceV2")
        for (pkg in listOf("net.osmand.plus", "net.osmand", "net.osmand.dev")) {
            if (isInstalled(pkg)) { intent.`package` = pkg; break }
        }
        var flags = Context.BIND_AUTO_CREATE
        if (android.os.Build.VERSION.SDK_INT >= 34) {
            flags = flags or Context.BIND_ALLOW_ACTIVITY_STARTS
        }
        val ok = try { context.bindService(intent, connection, flags) }
                 catch (e: Exception) { log("osmAnd: bind failed: $e"); false }
        if (ok) {
            bound = true
            log("osmAnd: bind requested … (V2)")
            main.postDelayed({ tryRegisterNav() }, 1500)
        } else {
            log("osmAnd: not found (check installation)")
        }
    }

    @SuppressLint("MissingPermission")
    private fun tryRegisterNav() {
        if (!bound) return
        val s = svc ?: run {
            log("osmAnd: service not connected yet — retrying …")
            main.postDelayed({ tryRegisterNav() }, 2000)
            return
        }
        try {
            // 1. Register for navigation updates (turn/distance ticks)
            val p = ANavigationUpdateParams()
            p.setSubscribeToUpdates(true)
            val id = s.registerForNavigationUpdates(p, callback)
            if (id >= 0) {
                navCallbackId = id
                log("osmAnd: nav updates registered (id=$id)")
            } else {
                log("osmAnd: id=-1 → app not yet enabled in OsmAnd. " +
                    "Our package: ${context.packageName}. " +
                    "OsmAnd: menu → plugins → tap \"LibreMoto\". (retrying …)")
                main.postDelayed({ tryRegisterNav() }, 5000)
                return
            }

            // 2. Register for route lifecycle updates (recalc, cancel, finish)
            val rp = ARouteUpdateParams()
            rp.setSubscribeToUpdates(true)
            val rid = s.registerForRouteUpdates(rp, callback)
            if (rid >= 0) {
                routeCallbackId = rid
                log("osmAnd: route updates registered (id=$rid)")
            }

            // 3. Pull the current route geometry (if a route is already active)
            pullRouteGeometry()
        } catch (e: RemoteException) {
            log("osmAnd: registration failed: $e")
            main.postDelayed({ tryRegisterNav() }, 5000)
        }
    }

    /** Pull the active route geometry from OsmAnd. Called on connect + on recalc + every tick. */
    private fun pullRouteGeometry() {
        val s = svc ?: return
        try {
            val params = GetActiveRouteParams()
            params.setIncludePassedSegment(false)
            val result = ActiveRouteGeometry()
            val ok = s.getActiveRouteGeometry(params, result)
            if (ok && result.points.size >= 2) {
                val newVersion = result.routeVersion
                if (newVersion != routeVersion || routePoints.isEmpty()) {
                    routePoints = result.points
                    routeVersion = newVersion
                    log("osmAnd: route geometry: ${result.points.size} points, " +
                        "${result.totalDistanceM} m total, ${result.remainingDistanceM} m rest, v=$newVersion")
                    checkRouteCoverage(result.points)
                } else {
                    // Same version but points may have shifted (vehicle moved)
                    routePoints = result.points
                    routeVersion = newVersion
                }
            } else {
                if (routePoints.isNotEmpty()) {
                    log("osmAnd: no active route (ok=$ok)")
                    routePoints = emptyList()
                }
            }
        } catch (e: RemoteException) {
            log("osmAnd: getActiveRouteGeometry failed: $e")
        }
    }

    /** ONE coverage check per new route version: are all route points inside
     *  loaded .lmm regions? Warn loudly if not — the user must download the
     *  missing region (or the display falls back to AIDL side streets). */
    @Volatile private var lastCoverageVersion = Int.MIN_VALUE
    private fun checkRouteCoverage(points: List<ALatLon>) {
        if (routeVersion == lastCoverageVersion) return
        lastCoverageVersion = routeVersion
        val mm = mapManager ?: return
        if (mm.loadedRegions().isEmpty()) return   // no maps → AIDL fallback, no warning spam
        val cov = mm.checkRoute(points.map { it.latitude to it.longitude })
        if (cov.covered) {
            log("lmm: route fully covered (${mm.loadedRegions().size} regions)")
        } else {
            log("WARNING: no map data for the route at " +
                "${"%.4f".format(cov.gapLat)}, ${"%.4f".format(cov.gapLon)} — " +
                "please download the region in the app! (roads via AIDL)")
        }
    }

    /** Pull nearby roads for the current position. PRIMARY: .lmm vector maps
     *  via MapManager (bbox query in memory, no binder call). FALLBACK: AIDL. */
    /** Speed limit of the road under the vehicle (0 = unknown).
     *  Updated with the nearby-roads pull from the .lmm map. */
    @Volatile private var currentLimitKmh = 0

    /** maxspeed of the road nearest to the vehicle (equirectangular
     *  point-to-polyline distance; roads farther than 40 m are ignored —
     *  on a motorway the parallel opposite carriageway is ~20-30 m away,
     *  and the correct side is nearer). Returns 0 if nothing qualifies. */
    private fun nearestRoadMaxspeed(roads: List<LmmRoad>, lat: Double, lon: Double): Int {
        var best = Double.MAX_VALUE
        var bestSpeed = 0
        val refLat = MapProjector.METERS_PER_DEG_LAT
        val refMPerLon = refLat * Math.cos(Math.toRadians(lat))
        val vx = lon * refMPerLon
        val vy = lat * refLat
        for (r in roads) {
            for (i in 0 until r.npts - 1) {
                val ax = r.lonE7[i] / 1e7 * refMPerLon
                val ay = r.latE7[i] / 1e7 * refLat
                val bx = r.lonE7[i + 1] / 1e7 * refMPerLon
                val by = r.latE7[i + 1] / 1e7 * refLat
                val dx = bx - ax; val dy = by - ay
                val len2 = dx * dx + dy * dy
                val t = if (len2 == 0.0) 0.0
                        else (((vx - ax) * dx + (vy - ay) * dy) / len2).coerceIn(0.0, 1.0)
                val px = ax + t * dx; val py = ay + t * dy
                val d2 = (vx - px) * (vx - px) + (vy - py) * (vy - py)
                if (d2 < best) {
                    best = d2
                    bestSpeed = r.maxspeedKmh
                }
            }
        }
        return if (best <= 40.0 * 40.0) bestSpeed else 0
    }

    private fun pullNearbyRoads() {
        val pts = routePoints
        if (pts.isEmpty()) return
        val centerLat = pts[0].latitude
        val centerLon = pts[0].longitude

        val mm = mapManager
        val lmmRoads = if (mm != null) {
            // query ALL loaded regions; the bbox is small, only the region
            // containing the position returns anything.
            // Radius = the LARGER visible dimension (AHEAD_PX at the current
            // scale) plus 10% margin — NOT the lookahead. The window is wider
            // than the lookahead half; querying only the lookahead made
            // streets pop in/out mid-screen when they crossed the query edge.
            val windowM = MapProjector.AHEAD_PX / (dynamicScale / 100.0)
            val radiusM = windowM * 1.1
            val dLat = radiusM / 111_320.0
            val dLon = radiusM / (111_320.0 * cos(Math.toRadians(centerLat)))
            val roads = ArrayList<LmmRoad>(64)
            for (m in mm.loadedRegions().values) {
                roads.addAll(m.queryBbox(centerLat - dLat, centerLon - dLon,
                                         centerLat + dLat, centerLon + dLon))
            }
            roads
        } else null

        if (lmmRoads != null && lmmRoads.isNotEmpty()) {
            val newLimit = nearestRoadMaxspeed(lmmRoads, centerLat, centerLon)
            if (newLimit != currentLimitKmh) {
                log("limit: ${if (newLimit > 0) "$newLimit km/h" else "none"} " +
                    "(road under the vehicle, ${lmmRoads.size} candidates)")
                currentLimitKmh = newLimit
            }
            val out = ArrayList<net.osmand.aidlapi.map.ARoadPolyline>(lmmRoads.size)
            for (r in lmmRoads) {
                val poly = net.osmand.aidlapi.map.ARoadPolyline()
                poly.setRoadType(r.highway)
                val pl = ArrayList<ALatLon>(r.npts)
                for (i in 0 until r.npts) pl.add(ALatLon(r.latE7[i] / 1e7, r.lonE7[i] / 1e7))
                poly.setPoints(pl)
                out.add(poly)
            }
            if (out.size != lastNearbyCount) {
                log("lmm: ${out.size} roads in window (of ${mm!!.loadedRegions().size} regions)")
                lastNearbyCount = out.size
            }
            nearbyRoads = out
            return
        }

        // FALLBACK: AIDL (no .lmm region covers this position)
        val s = svc ?: run { log("osmAnd: nearby: svc null"); return }
        try {
            val params = net.osmand.aidlapi.map.NearbyRoadsParams()
            params.setCenterLat(centerLat)
            params.setCenterLon(centerLon)
            params.setRadiusMeters(currentLookaheadM.toInt().coerceAtMost(400))  // half the visible window, capped at 400 m for perf
            params.setMaxRoads(20)
            val result = net.osmand.aidlapi.map.NearbyRoadsResult()
            val ok = s.getNearbyRoads(params, result)
            log("osmAnd: nearby: ok=$ok roads=${result.roads.size} total=${result.totalFound}")
            if (ok && result.roads.isNotEmpty()) {
                nearbyRoads = result.roads
                if (nearbyRoads.size != lastNearbyCount) {
                    log("osmAnd: nearby roads: ${nearbyRoads.size} roads (total=${result.totalFound})")
                    lastNearbyCount = nearbyRoads.size
                }
            } else {
                nearbyRoads = emptyList()
            }
        } catch (e: RemoteException) {
            log("osmAnd: nearby RemoteException: ${e.message}")
        } catch (e: Exception) {
            log("osmAnd: nearby error: ${e.javaClass.simpleName}: ${e.message}")
        }
    }

    @Volatile private var lastNearbyCount: Int = -1
    @Volatile private var nearbyPulling: Boolean = false
    @Volatile private var lastLoggedScale: Int = -1
    @Volatile private var lastNearbyPullMs: Long = 0

    /** NearbyRoads pull interval — see refresh(). */
    private companion object {
        const val NEARBY_INTERVAL_MS = 1000L
        /** .lmm query interval (in-memory, ~40 ms — much cheaper than AIDL). */
        const val LMM_INTERVAL_MS = 1000L
    }

    /** Reusable single-thread executor for the nearby-roads binder calls. */
    private val nearbyExecutor = java.util.concurrent.Executors.newSingleThreadExecutor { r ->
        Thread(r, "LibreMoto-nearby").apply { priority = Thread.NORM_PRIORITY - 1 }
    }

    /** Equirectangular distance in metres between two lat/lon points. */
    private fun distanceM(a: ALatLon, b: ALatLon): Double {
        val metersPerDegLat = 111_320.0
        val metersPerDegLon = 111_320.0 * cos(Math.toRadians(a.latitude))
        val dLat = b.latitude - a.latitude
        val dLon = b.longitude - a.longitude
        val dNorthM = dLat * metersPerDegLat
        val dEastM = dLon * metersPerDegLon
        return sqrt(dNorthM * dNorthM + dEastM * dEastM)
    }

    /** Compute bearing (degrees, 0=north, clockwise) from a route polyline.
     *  Point[0] = vehicle position. We look a few points ahead (or up to ~50 m)
     *  to get a stable heading, avoiding jitter from closely-spaced points. */
    private fun bearingFromPolyline(pts: List<ALatLon>): Double {
        if (pts.size < 2) return 0.0
        val p0 = pts[0]
        // Find a point at least ~50 m ahead along the route for a stable bearing.
        // If the route is short, just use the last point.
        var target = pts[pts.size - 1]
        var bestDist = 0.0
        val refLat = p0.latitude
        val metersPerDegLat = 111_320.0
        val metersPerDegLon = 111_320.0 * cos(Math.toRadians(refLat))
        for (i in 1 until pts.size) {
            val dLat = pts[i].latitude - p0.latitude
            val dLon = pts[i].longitude - p0.longitude
            val distM = sqrt(dLat * dLat * metersPerDegLat * metersPerDegLat +
                             dLon * dLon * metersPerDegLon * metersPerDegLon)
            if (distM >= 50.0) {
                target = pts[i]
                bestDist = distM
                break
            }
            bestDist = distM
        }
        val dLat = target.latitude - p0.latitude
        val dLon = target.longitude - p0.longitude
        // bearing = atan2(east, north) in degrees
        val dEast = dLon * metersPerDegLon
        val dNorth = dLat * metersPerDegLat
        val bearing = Math.toDegrees(atan2(dEast, dNorth))
        return if (bearing < 0) bearing + 360.0 else bearing
    }

    private fun isInstalled(pkg: String): Boolean = try {
        context.packageManager.getPackageInfo(pkg, 0); true
    } catch (e: Exception) { false }

    @SuppressLint("MissingPermission")
    override fun stop() {
        try {
            val s = svc
            if (s != null) {
                if (navCallbackId >= 0) {
                    val p = ANavigationUpdateParams()
                    p.setCallbackId(navCallbackId)
                    p.setSubscribeToUpdates(false)
                    try { s.registerForNavigationUpdates(p, callback) } catch (_: Exception) {}
                }
                if (routeCallbackId >= 0) {
                    try { s.unregisterFromRouteUpdates(routeCallbackId) } catch (_: Exception) {}
                }
            }
            context.unbindService(connection)
        } catch (e: Exception) { log("osmAnd: stop: $e") }
        bound = false
        navActive = false
        navCallbackId = -1L
        routeCallbackId = -1L
        routePoints = emptyList()
        synchronized(trail) { trail.clear(); trailMetres = 0.0 }
    }
}
