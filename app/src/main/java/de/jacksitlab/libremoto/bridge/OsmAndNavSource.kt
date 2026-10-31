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

    // Nearby roads (WGS84 polylines) — refreshed every tick
    @Volatile private var nearbyRoads: List<net.osmand.aidlapi.map.ARoadPolyline> = emptyList()

    // The last directionInfo we got, exposed for tests/inspection.
    var lastDirection: ADirectionInfo? = null
        private set

    // ---- speed-adaptive zoom -------------------------------------------
    // Voraussicht je nach Tempo: ~500 m in der Stadt (langsam, ≤ 30 km/h),
    // ~1000 m außerhalb (schnell, ≥ 100 km/h), dazwischen linear.
    // Skala: 240 px sind vor dem Fahrzeug sichtbar (480-px-Panel, Fahrzeug
    // mittig) → scale = 240 / lookahead_m × 100 (px/m × 100).
    private val zoomMinLookaheadM = 500.0
    private val zoomMaxLookaheadM = 1000.0
    private val zoomLowSpeedKmh = 30.0
    private val zoomHighSpeedKmh = 100.0

    /** Geglättete Geschwindigkeit km/h (EMA über Ticks, robust gegen GPS-Sprünge). */
    @Volatile private var smoothedSpeedKmh = 0.0
    @Volatile private var lastVehiclePos: ALatLon? = null
    @Volatile private var lastVehiclePosAtMs = 0L

    /** Aktueller Projektionsmaßstab px/m × 100 (wechselt weich mit der Geschwindigkeit). */
    @Volatile private var dynamicScale = 24   // Start: 1000 m Voraussicht
    /** Voraussicht in m, die der aktuelle dynamicScale abbildet (für Road-Radius). */
    @Volatile private var currentLookaheadM = 1000.0

    /** Ziel-Skala aus der geglätteten Geschwindigkeit (px/m × 100). */
    private fun targetScale(): Int {
        val t = ((smoothedSpeedKmh - zoomLowSpeedKmh) /
                 (zoomHighSpeedKmh - zoomLowSpeedKmh)).coerceIn(0.0, 1.0)
        val lookahead = zoomMinLookaheadM + t * (zoomMaxLookaheadM - zoomMinLookaheadM)
        return (240.0 / lookahead * 100.0).toInt()
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
            log("osmAnd: Service verbunden (V2)")
        }
        override fun onServiceDisconnected(name: ComponentName) {
            svc = null
            bound = false
            navCallbackId = -1L
            routeCallbackId = -1L
            log("osmAnd: Service getrennt")
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

    /** Last GPS bearing in degrees × 10 (for MapFrame heading).
     *  Updated by refresh() from the Android LocationManager. */
    @Volatile private var headingDeg10: Int = 0

    override fun refresh() {
        // 1. Pull current route geometry (fast: a few hundred points, ~10 ms)
        if (bound && svc != null && navActive) {
            pullRouteGeometry()
        }

        // 2. Compute bearing from the route polyline
        val pts = routePoints
        if (pts.size >= 2) {
            val bearing = bearingFromPolyline(pts)
            headingDeg10 = (bearing * 10).toInt() % 3600
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
                val kmh = (distM / dtS) * 3.6
                if (kmh <= 250.0) {
                    val alpha = 0.3   // EMA: ~3 ticks until a new speed registers
                    smoothedSpeedKmh = smoothedSpeedKmh * (1 - alpha) + kmh * alpha
                }
            }
            lastVehiclePos = pos
            lastVehiclePosAtMs = now
        }

        // 2c. Zoom softly toward the speed-dependent target scale
        //     (change at most ~2 px/m×100 per tick → no visible jumps).
        val target = targetScale()
        val diff = target - dynamicScale
        dynamicScale = if (kotlin.math.abs(diff) <= 2) target
                       else dynamicScale + (if (diff > 0) 2 else -2)
        currentLookaheadM = 240.0 / (dynamicScale / 100.0)
        if (dynamicScale != lastLoggedScale) {
            log("osmAnd: zoom: ${"%.0f".format(smoothedSpeedKmh)} km/h → " +
                "Voraussicht ${"%.0f".format(currentLookaheadM)} m (scale=$dynamicScale)")
            lastLoggedScale = dynamicScale
        }

        // 3. Pull nearby roads on a background thread (slow: 200-500 ms)
        //    mapFrame() uses the last-pulled roads; they update one tick late.
        if (bound && svc != null && navActive && routePoints.isNotEmpty() && !nearbyPulling) {
            nearbyPulling = true
            Thread {
                pullNearbyRoads()
                nearbyPulling = false
            }.start()
        }
    }

    override fun navData(): NavData {
        if (!navActive) return NavData(NavType.Idle)
        return NavData(
            type = NavType.Nav,
            maneuver = maneuver(turnType),
            distM = distanceToM,
            mapOn = true,
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
            val scale = dynamicScale
            val pxPerMeter = scale / 100.0
            val lenPx = (distanceToM * pxPerMeter).toInt().coerceIn(40, 220)
            val route = listOf(Point(0, 0), Point(0, -lenPx))
            return MapFrame(
                seq = seq,
                flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
                headingDeg10 = headingDeg10,
                scalePxPerMeter100 = scale,
                segments = listOf(Segment(SegmentType.Route, 10, route)),
            )
        }

        // Project the polyline to vehicle-relative pixels.
        // Reference point = first point of the route (= vehicle position).
        val refLat = pts[0].latitude
        val refLon = pts[0].longitude
        val metersPerDegLat = 111_320.0
        val metersPerDegLon = 111_320.0 * cos(Math.toRadians(refLat))

        val scalePxPerMeter100 = dynamicScale   // speed-adaptive, set in refresh()
        val pxPerMeter = scalePxPerMeter100 / 100.0

        // Collect all segments: route + nearby roads
        val segments = ArrayList<Segment>(1 + nearbyRoads.size)

        // Route segment
        val maxPts = minOf(pts.size, MapFrame.MAX_PTS)
        val pixelPts = ArrayList<Point>(maxPts)
        for (i in 0 until maxPts) {
            val dLat = pts[i].latitude - refLat
            val dLon = pts[i].longitude - refLon
            val dNorthM = dLat * metersPerDegLat
            val dEastM = dLon * metersPerDegLon
            val px = (dEastM * pxPerMeter).toInt().coerceIn(-260, 260)
            val py = (-dNorthM * pxPerMeter).toInt().coerceIn(-260, 260)
            pixelPts.add(Point(px, py))
        }
        segments.add(Segment(SegmentType.Route, 10, pixelPts))

        // Nearby road segments (limit total frame size to MAX_FRAME=4096 bytes)
        // Each segment = 3 header + pts*4 bytes. Budget: ~3000 B for roads
        var totalSegs = 1
        var budgetBytes = 3000
        for (road in nearbyRoads) {
            if (totalSegs >= MapFrame.MAX_SEGS || budgetBytes < 83) break  // 83 B = min segment (3 + 20*4)
            val roadPts = road.points
            if (roadPts.size < 2) continue
            val maxRoadPts = minOf(roadPts.size, 20)
            val rPts = ArrayList<Point>(maxRoadPts)
            for (i in 0 until maxRoadPts) {
                val dLat = roadPts[i].latitude - refLat
                val dLon = roadPts[i].longitude - refLon
                val dNorthM = dLat * metersPerDegLat
                val dEastM = dLon * metersPerDegLon
                val px = (dEastM * pxPerMeter).toInt().coerceIn(-260, 260)
                val py = (-dNorthM * pxPerMeter).toInt().coerceIn(-260, 260)
                rPts.add(Point(px, py))
            }
            if (rPts.size >= 2) {
                log("osmAnd: road seg#${totalSegs} type=${road.roadType} pts=${rPts.size} px=(${rPts[0].x},${rPts[0].y})-(${rPts[rPts.size-1].x},${rPts[rPts.size-1].y})")
                segments.add(Segment(SegmentType.Road, 3, rPts))
                totalSegs++
                budgetBytes -= (3 + rPts.size * 4)
            }
        }

        return MapFrame(
            seq = seq,
            flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
            headingDeg10 = headingDeg10,  // bearing × 10
            scalePxPerMeter100 = scalePxPerMeter100,
            segments = segments,
        )
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
                 catch (e: Exception) { log("osmAnd: bind fehlgeschlagen: $e"); false }
        if (ok) {
            bound = true
            log("osmAnd: Bind angefragt … (V2)")
            main.postDelayed({ tryRegisterNav() }, 1500)
        } else {
            log("osmAnd: nicht gefunden (Installation prüfen)")
        }
    }

    @SuppressLint("MissingPermission")
    private fun tryRegisterNav() {
        if (!bound) return
        val s = svc ?: run {
            log("osmAnd: Service noch nicht verbunden — versuche erneut …")
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
                log("osmAnd: Nav-Updates registriert (id=$id)")
            } else {
                log("osmAnd: id=-1 → App in OsmAnd noch nicht freigeschaltet. " +
                    "Unser Package: ${context.packageName}. " +
                    "OsmAnd: Menü → Plugins → \"LibreMoto\" antippen. (wiederhole …)")
                main.postDelayed({ tryRegisterNav() }, 5000)
                return
            }

            // 2. Register for route lifecycle updates (recalc, cancel, finish)
            val rp = ARouteUpdateParams()
            rp.setSubscribeToUpdates(true)
            val rid = s.registerForRouteUpdates(rp, callback)
            if (rid >= 0) {
                routeCallbackId = rid
                log("osmAnd: Route-Updates registriert (id=$rid)")
            }

            // 3. Pull the current route geometry (if a route is already active)
            pullRouteGeometry()
        } catch (e: RemoteException) {
            log("osmAnd: Registrierung fehlgeschlagen: $e")
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
                    log("osmAnd: Route-Geometrie: ${result.points.size} Punkte, " +
                        "${result.totalDistanceM} m total, ${result.remainingDistanceM} m rest, v=$newVersion")
                } else {
                    // Same version but points may have shifted (vehicle moved)
                    routePoints = result.points
                    routeVersion = newVersion
                }
            } else {
                if (routePoints.isNotEmpty()) {
                    log("osmAnd: keine aktive Route (ok=$ok)")
                    routePoints = emptyList()
                }
            }
        } catch (e: RemoteException) {
            log("osmAnd: getActiveRouteGeometry fehlgeschlagen: $e")
        }
    }

    /** Pull nearby roads from OsmAnd. Called every tick by refresh(). */
    private fun pullNearbyRoads() {
        val s = svc ?: run { log("osmAnd: nearby: svc null"); return }
        val pts = routePoints
        if (pts.isEmpty()) run { log("osmAnd: nearby: no route points"); return }

        // Use the first route point (vehicle position) as center
        val centerLat = pts[0].latitude
        val centerLon = pts[0].longitude

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
    }
}
