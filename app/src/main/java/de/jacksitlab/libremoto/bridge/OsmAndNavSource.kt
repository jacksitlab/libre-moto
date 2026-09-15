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

    // The last directionInfo we got, exposed for tests/inspection.
    var lastDirection: ADirectionInfo? = null
        private set

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
            // Fallback: straight placeholder from distanceTo
            val scale = 10
            val lenPx = (distanceToM / 10).coerceIn(40, 220)
            val route = listOf(Point(0, 0), Point(0, -lenPx))
            return MapFrame(
                seq = seq,
                flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
                headingDeg10 = 0,
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

        val scalePxPerMeter100 = 10  // 0.10 px/m → 1 px per 10 m
        val pxPerMeter = scalePxPerMeter100 / 100.0

        val maxPts = minOf(pts.size, MapFrame.MAX_PTS)
        val pixelPts = ArrayList<Point>(maxPts)

        for (i in 0 until maxPts) {
            val dLat = pts[i].latitude - refLat
            val dLon = pts[i].longitude - refLon
            val dNorthM = dLat * metersPerDegLat
            val dEastM = dLon * metersPerDegLon

            // Vehicle-relative: +x = east, -y = north (up on screen)
            val px = (dEastM * pxPerMeter).toInt().coerceIn(-260, 260)
            val py = (-dNorthM * pxPerMeter).toInt().coerceIn(-260, 260)
            pixelPts.add(Point(px, py))
        }

        return MapFrame(
            seq = seq,
            flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
            headingDeg10 = 0,  // AIDL gives no bearing; assume straight-ahead
            scalePxPerMeter100 = scalePxPerMeter100,
            segments = listOf(Segment(SegmentType.Route, 10, pixelPts)),
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

    /** Pull the active route geometry from OsmAnd. Called on connect + on recalculation. */
    private fun pullRouteGeometry() {
        val s = svc ?: return
        try {
            val params = GetActiveRouteParams()
            params.setIncludePassedSegment(false)
            val result = ActiveRouteGeometry()
            val ok = s.getActiveRouteGeometry(params, result)
            if (ok && result.points.size >= 2) {
                routePoints = result.points
                routeVersion++
                log("osmAnd: Route-Geometrie: ${result.points.size} Punkte, " +
                    "${result.totalDistanceM} m total, ${result.remainingDistanceM} m rest")
            } else {
                if (routePoints.isNotEmpty()) {
                    log("osmAnd: keine aktive Route (ok=$ok)")
                }
                // Keep old geometry if we had one; OsmAnd may recalculate shortly
            }
        } catch (e: RemoteException) {
            log("osmAnd: getActiveRouteGeometry fehlgeschlagen: $e")
        }
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
