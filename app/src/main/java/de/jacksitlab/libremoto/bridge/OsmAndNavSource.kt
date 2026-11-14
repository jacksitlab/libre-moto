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
    /** Position extrapolation (the sawtooth fix): the last REAL OsmAnd
     *  position and when it was seen; each pump tick walks the vehicle
     *  along the route by v*dt so all 5 frames/s carry a fresh position. */
    @Volatile private var anchorPos: ALatLon? = null
    @Volatile private var anchorAtMs = 0L
    @Volatile private var extrapSpeedMs = 0.0   // m/s along the route
    /** The position the frames should show: the extrapolated vehicle
     *  position (anchor + v*elapsed along the route). mapFrame reads
     *  this instead of the raw stale routePoints[0]. */
    @Volatile private var displayPos: ALatLon? = null
    @Volatile private var lastDisplayAtMs = 0L

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
     *  Reference: AHEAD_PX (480) — the round display's visible circle
     *  reaches 480 px ahead of the vehicle, so 'lookahead' metres must
     *  fill 480 px. (The old 320 reference was stale after the window
     *  widened: scale 24 claimed 1000 m but actually showed 2000 m —
     *  nobody needs that.) Metre specs stay: 500 m city … 1000 m
     *  country road. */
    private fun targetScale(): Int {
        val t = ((smoothedSpeedKmh - zoomLowSpeedKmh) /
                 (zoomHighSpeedKmh - zoomLowSpeedKmh)).coerceIn(0.0, 1.0)
        val lookahead = zoomMinLookaheadM + t * (zoomMaxLookaheadM - zoomMinLookaheadM)
        return (MapProjector.AHEAD_PX / lookahead * 100.0).toInt()
    }

    /** DISCRETE zoom levels: quantize the continuous target to steps of 8
     *  (scale 24 = 1000 m, 32 = 750 m, 40 = 600 m, 48 = 500 m …) with a
     *  ±(step/2+2) hysteresis. The EMA speed wobbles ±10 km/h in the city —
     *  a continuously-tracked scale made the window breathe constantly
     *  (96 changes in 7 min in lm-4.log), and every scale step dropped and
     *  re-added the streets at the window edge (visible flicker). With
     *  quantized levels the window only changes on REAL speed changes. */
    private var quantTargetScale = 24   // start: long Voraussicht
    private var lastZoomChangeMs = 0L

    /** Min dwell time per zoom level: the level must HOLD this long
     *  before another change is allowed. Kills the in/out ping-pong
     *  when speed hovers around a step boundary (user: the zoom
     *  going in and out constantly is annoying). */
    private val zoomDwellMs = 6_000L

    private fun updateQuantTarget(contTarget: Int): Int {
        val step = 16
        val margin = step / 2 + 4   // 12: switch only clearly beyond half a step
        val now = android.os.SystemClock.elapsedRealtime()
        if (now - lastZoomChangeMs < zoomDwellMs) return quantTargetScale
        if (contTarget > quantTargetScale + margin) {
            quantTargetScale += step; lastZoomChangeMs = now
        } else if (contTarget < quantTargetScale - margin) {
            quantTargetScale -= step; lastZoomChangeMs = now
        }
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

        // 2b. Speed from vehicle movement (routePoints[0] = position).
        //     The ANCHOR logic: OsmAnd updates the position ~1 Hz while
        //     the pump ticks 5 Hz. We remember the last REAL position and
        //     its timestamp; between real updates every tick extrapolates
        //     along the route polyline (v*dt), so frames carry a moving
        //     position instead of frozen-then-jumping.
        if (pts.isNotEmpty()) {
            val pos = pts[0]
            val now = System.currentTimeMillis()
            val last = lastVehiclePos
            val lastAt = lastVehiclePosAtMs
            // REAL update? (position changed or first sight)
            if (last == null || distanceM(last, pos) > 1.0) {
                if (last != null && lastAt > 0) {
                    val dtReal = (now - lastAt) / 1000.0
                    val dReal = distanceM(last, pos)
                    if (dtReal > 0.05 && dReal / dtReal < 70.0) {   // < 250 km/h
                        // EMA on the speed: OsmAnd delivers anchors in
                        // ~2 s steps with snapping jitter — a raw d/dt
                        // jumps frame to frame and the map ping-pongs
                        val v = dReal / dtReal
                        extrapSpeedMs = extrapSpeedMs * 0.5 + v * 0.5
                    }
                }
                anchorPos = pos
                anchorAtMs = now
                lastVehiclePos = pos
                lastVehiclePosAtMs = now
                // RE-ANCHOR, two-tier (ping-pong fix + teleport sync):
                // - small error (<= 50 m: GPS drift + extrapolation noise):
                //   SOFT — pull 40 % toward the anchor, the rest decays
                //   over the next anchors. A hard snap here made the map
                //   lurch forward/backward every ~2 s (OsmAnd's interval).
                // - big error (> 50 m: simulation reset, route recalc,
                //   teleport): HARD sync — snap fully and drop the trail
                //   (the road 'behind us' is elsewhere now). A soft pull
                //   would crawl for minutes (user: after a simulated
                //   km-jump it never caught up).
                val dp = displayPos
                if (dp != null && extrapSpeedMs > 0.3) {
                    val errM = distanceM(dp, pos)
                    if (errM > 50.0) {
                        displayPos = pos
                        synchronized(trail) { trail.clear() }
                        trailMetres = 0.0
                    } else {
                        displayPos = ALatLon(dp.latitude + (pos.latitude - dp.latitude) * 0.4,
                                            dp.longitude + (pos.longitude - dp.longitude) * 0.4)
                    }
                } else {
                    displayPos = pos
                }
            }
            // advance the display position by v*dt since the LAST display
            // step (the position integrates itself; the anchor only steers)
            val dp = displayPos
            if (dp != null && extrapSpeedMs > 0.3) {   // ignore jitter below ~1 km/h
                val dtDisp = (now - lastDisplayAtMs) / 1000.0
                if (dtDisp > 0.02 && dtDisp < 2.0) {
                    val adv = extrapSpeedMs * dtDisp
                    if (adv > 0.02 && adv < 100.0) {
                        val moved = advanceAlongRoute(pts, dp, adv)
                        if (moved != null) displayPos = moved
                    }
                }
            }
            lastDisplayAtMs = now
            val posNow = displayPos ?: pos
            if (last != null && lastAt > 0 && now > lastAt) {
                val dtS = (now - lastAt) / 1000.0
                val distM = distanceM(last, posNow)
                // breadcrumb: extend the trail behind the vehicle
                if (distM > 4.0) {   // ignore GPS jitter below 4 m
                    trail.add(0, LatLon(last.latitude, last.longitude))
                    // NOTE: trail from REAL anchors only would lag; the
                    // extrapolated positions keep it continuous.
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
                    val alpha = 0.2   // EMA: ~5 ticks until a new speed registers
                                     // (slow on purpose — it only DRIVES the zoom)
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

        // The vehicle position the frame CENTRES on: the extrapolated
        // displayPos (fresh every pump tick), not OsmAnd's stale pts[0].
        val vehicle = displayPos ?: pts[0]

        // route + streets through the SHARED FrameBuilder (also used by the
        // end-to-end preview test — no divergence between test and app)
        // BOUND to ~5 km of arc length: OsmAnd delivers the WHOLE remaining
        // route — a long tour is 10k+ points, and every one of them was
        // allocated, converted and clipped per frame at 5 Hz. The visible
        // window needs ~1.2 km; 5 km is generous margin.
        val route = ArrayList<LatLon>(1024)
        if (pts.size >= 2) {
            val lat0 = vehicle.latitude
            val mLon = 111320.0 * Math.cos(Math.toRadians(lat0))
            route.add(LatLon(lat0, vehicle.longitude))
            var acc = 0.0
            for (i in 1 until pts.size) {
                val a = pts[i - 1]; val b = pts[i]
                val dLat = (b.latitude - a.latitude) * 111320.0
                val dLon = (b.longitude - a.longitude) * mLon
                acc += Math.sqrt(dLat * dLat + dLon * dLon)
                route.add(LatLon(b.latitude, b.longitude))
                if (acc >= 5000.0) break
            }
        }
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
    /** Walk `distM` metres along the route polyline starting at `from`
     *  (nearest point on the first segments). Returns the advanced
     *  position or null if the route is too short. */
    private fun advanceAlongRoute(pts: List<ALatLon>, from: ALatLon, distM: Double): ALatLon? {
        if (pts.size < 2) return null
        // find the segment containing `from` (nearest segment start)
        var si = 0
        var bestD = Double.MAX_VALUE
        val lim = minOf(pts.size - 1, 40)
        for (i in 0 until lim) {
            val d = distanceM(pts[i], from)
            if (d < bestD) { bestD = d; si = i }
        }
        var remaining = distM
        var i = si
        // START AT `from` (not at pts[si]): starting at the nearest VERTEX
        // rounded every call up to ±5 m differently — the map jittered
        // around the true position and aggravated the anchor jumps.
        var cur: ALatLon = from
        while (i < pts.size - 1 && remaining > 0) {
            val nxt = pts[i + 1]
            val segLen = distanceM(cur, nxt)
            if (segLen <= remaining) {
                remaining -= segLen
                cur = nxt
                i++
            } else {
                val t = remaining / segLen
                return ALatLon(cur.latitude + (nxt.latitude - cur.latitude) * t,
                               cur.longitude + (nxt.longitude - cur.longitude) * t)
            }
        }
        return cur
    }

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

    /** maxspeed of the road the ROUTE runs on (user report #1: OsmAnd
     *  showed 50, the display stayed 30 — the geometrically nearest road
     *  is regularly a side street at junctions). Selection mirrors the
     *  map's own ALONG logic: a road qualifies when the route runs on
     *  it; among qualifiers the nearest wins. Roads farther than 40 m
     *  are ignored (on a motorway the opposite carriageway is 20-30 m
     *  away and the correct side is nearer). Returns 0 if nothing
     *  qualifies. */
    private fun routeRoadMaxspeed(roads: List<LmmRoad>, lat: Double, lon: Double): Int {
        // BOUND to ~3 km of arc length: the road under the vehicle decides
        // the limit; scanning OsmAnd's whole remaining route (long tours:
        // 10k+ points) built a giant contact index per call on the
        // background thread.
        val full = routePoints
        val route = ArrayList<LatLon>(768)
        if (full.size >= 2) {
            val mLon = 111320.0 * Math.cos(Math.toRadians(full[0].latitude))
            route.add(LatLon(full[0].latitude, full[0].longitude))
            var acc = 0.0
            for (i in 1 until full.size) {
                val a = full[i - 1]; val b = full[i]
                val dLat = (b.latitude - a.latitude) * 111320.0
                val dLon = (b.longitude - a.longitude) * mLon
                acc += Math.sqrt(dLat * dLat + dLon * dLon)
                route.add(LatLon(b.latitude, b.longitude))
                if (acc >= 3000.0) break
            }
        }
        if (route.size < 2) return nearestRoadMaxspeed(roads, lat, lon)
        var best = Double.MAX_VALUE
        var bestSpeed = 0
        val refLat = MapProjector.METERS_PER_DEG_LAT
        val refMPerLon = refLat * Math.cos(Math.toRadians(lat))
        val vx = lon * refMPerLon
        val vy = lat * refLat
        val mLat = refLat
        val mLon = refMPerLon
        for (r in roads) {
            // does the route run ALONG this road? (same test as the map
            // filter: >= 4 consecutive points near the route)
            var run = 0; var bestRun = 0
            for (i in 0 until r.npts) {
                val px = r.latE7[i] / 1e7
                val py = r.lonE7[i] / 1e7
                var near = false
                for (j in 0 until route.size - 1) {
                    val d = MapProjector.pointSegmentDistM(LatLon(px, py), route[j], route[j + 1], mLat, mLon)
                    if (d <= 15.0) { near = true; break }
                }
                if (near) { run++; if (run > bestRun) bestRun = run } else run = 0
            }
            if (bestRun < 4) continue   // not our road — can't own the limit
            // distance of the VEHICLE to this road (nearest segment)
            var dBest = Double.MAX_VALUE
            for (i in 0 until r.npts - 1) {
                val ax = r.lonE7[i] / 1e7 * refMPerLon
                val ay = r.latE7[i] / 1e7 * refLat
                val bx = r.lonE7[i + 1] / 1e7 * refMPerLon
                val by = r.latE7[i + 1] / 1e7 * refLat
                val dx = bx - ax; val dy = by - ay
                val len2 = dx * dx + dy * dy
                val t = if (len2 == 0.0) 0.0
                        else (((vx - ax) * dx + (vy - ay) * dy) / len2).coerceIn(0.0, 1.0)
                val px2 = ax + t * dx; val py2 = ay + t * dy
                val d2 = (vx - px2) * (vx - px2) + (vy - py2) * (vy - py2)
                if (d2 < dBest) dBest = d2
            }
            if (dBest < best) { best = dBest; bestSpeed = r.maxspeedKmh }
        }
        return if (best <= 40.0 * 40.0) bestSpeed else 0
    }

    /** maxspeed of the road nearest to the vehicle (fallback without a
     *  route, e.g. before the first route update). */
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
            val newLimit = routeRoadMaxspeed(lmmRoads, centerLat, centerLon)
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
