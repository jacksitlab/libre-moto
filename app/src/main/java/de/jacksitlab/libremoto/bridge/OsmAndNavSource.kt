package de.jacksitlab.libremoto.bridge

import android.annotation.SuppressLint
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.os.PowerManager
import android.os.RemoteException
import de.jacksitlab.libremoto.protocol.Point
import de.jacksitlab.libremoto.protocol.Maneuver
import de.jacksitlab.libremoto.protocol.MapFrame
import de.jacksitlab.libremoto.protocol.NavData
import de.jacksitlab.libremoto.protocol.NavType
import de.jacksitlab.libremoto.protocol.Segment
import de.jacksitlab.libremoto.protocol.SegmentType
import net.osmand.aidl.IOsmAndAidlCallback
import net.osmand.aidl.IOsmAndAidlInterface
import net.osmand.aidl.navigation.ADirectionInfo
import net.osmand.aidl.navigation.ANavigationUpdateParams

/**
 * OsmAnd navigation source via the official OsmAnd AIDL API (net.osmand.aidl).
 *
 * This is the "saubere" bridge for external apps: we bind OsmAnd's exported
 * `OsmandAidlService`, register for navigation updates, and receive
 * `ADirectionInfo { distanceTo, turnType, leftSide }` on each guidance tick.
 *
 * What this gives us cleanly from OsmAnd:
 *   - live turn type + distance to next maneuver → NavData (the headline ask).
 *
 * What it does NOT give (verified in the AIDL surface, both V1/V2):
 *   - route geometry (no polyline/lat-lon list getter) and current heading.
 *   → the vector MapFrame is therefore a *clean route-ahead* placeholder
 *     (straight segment, length from distanceTo/scale, no fake side roads).
 *     A full multi-road vector map needs the OsmAnd full library SDK
 *     (in-process plugin) — tracked as Phase C.3.
 */
class OsmAndNavSource(
    private val context: Context,
    private val log: (String) -> Unit,
) : NavSource {

    override val label: String = "OsmAnd (AIDL)"

    @Volatile private var bound = false
    @Volatile private var callbackId = -1L
    private var svc: IOsmAndAidlInterface? = null

    @Volatile private var navActive = false
    @Volatile private var distanceToM: Int = 0
    @Volatile private var turnType: Int = 1

    override val isNavigating: Boolean get() = bound && navActive

    // The last directionInfo we got, exposed for tests/inspection.
    var lastDirection: ADirectionInfo? = null
        private set

    private val callback = object : IOsmAndAidlCallback.Stub() {
        override fun onSearchComplete(resultSet: List<net.osmand.aidl.search.SearchResult>) {}
        override fun onUpdate() {}
        override fun onAppInitialized() {}
        override fun onGpxBitmapCreated(bitmap: net.osmand.aidl.gpx.AGpxBitmap?) {}
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
        override fun onVoiceRouterNotify(params: net.osmand.aidl.navigation.OnVoiceNavigationParams) {}
    }

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName, iBinder: IBinder) {
            svc = IOsmAndAidlInterface.Stub.asInterface(iBinder)
            log("osmAnd: Service verbunden")
        }
        override fun onServiceDisconnected(name: ComponentName) {
            svc = null
            bound = false
            log("osmAnd: Service getrennt")
        }
    }

    /** TurnType int → LibreMoto Maneuver. Mirrors net.osmand.router.TurnType. */
    private fun maneuver(t: Int): Maneuver = when (t) {
        1 -> Maneuver.Straight          // C continue
        2, 4 -> Maneuver.Left           // TL, TSHL
        3 -> Maneuver.SlightLeft        // TSLL
        5, 7 -> Maneuver.Right          // TR, TSHR
        6 -> Maneuver.SlightRight       // TSLR
        10, 11 -> Maneuver.UTurn        // TU, TRU
        8, 9 -> Maneuver.Merge          // KL, KR (keep)
        12 -> Maneuver.Straight         // OFFR → go straight (off-route)
        13, 14 -> Maneuver.Left         // roundabout → treat as turn
        else -> Maneuver.Straight
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

    /** Clean route-ahead: a single straight route segment, length from the
     * distance-to-next-maneuver (clamped to the ±260 px viewport). No fake
     * side roads — see class doc for why. */
    override fun mapFrame(seq: Int): MapFrame {
        val scale = 10 // 0.10 px/m  → 1 px per 10 m (reads well for the 480² window)
        val lenPx = (distanceToM / 10).coerceIn(40, 220)
        val route = listOf(Point(0, 0), Point(0, -lenPx))
        return MapFrame(
            seq = seq,
            flags = MapFrame.FLAG_ROTATE or MapFrame.FLAG_VEHICLE,
            headingDeg10 = 0, // OsmAnd AIDL gives no bearing; assume straight-ahead
            scalePxPerMeter100 = scale, // 0.10 px/m
            segments = listOf(Segment(SegmentType.Route, 10, route)),
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
        val intent = Intent("net.osmand.aidl.OsmandAidlService")
        for (pkg in listOf("net.osmand.plus", "net.osmand", "net.osmand.dev")) {
            if (isInstalled(pkg)) { intent.`package` = pkg; break }
        }
        var flags = Context.BIND_AUTO_CREATE
        // API 34+: allow the bound service to start activities (matches the
        // official osmand-api-demo; needed once OsmAnd raises importance).
        if (android.os.Build.VERSION.SDK_INT >= 34) {
            flags = flags or Context.BIND_ALLOW_ACTIVITY_STARTS
        }
        val ok = try { context.bindService(intent, connection, flags) }
                 catch (e: Exception) { log("osmAnd: bind fehlgeschlagen: $e"); false }
        if (ok) {
            bound = true
            log("osmAnd: Bind angefragt …")
            // register for navigation updates once connected (retry a few times)
            android.os.Handler(android.os.Looper.getMainLooper()).postDelayed({
                tryRegisterNav()
            }, 1500)
        } else {
            log("osmAnd: nicht gefunden (Installation prüfen)")
        }
    }

    @SuppressLint("MissingPermission")
    private fun tryRegisterNav() {
        val s = svc ?: run { log("osmAnd: svc null — Navigation nicht verfügbar"); return }
        try {
            val p = ANavigationUpdateParams()
            p.setSubscribeToUpdates(true)
            callbackId = s.registerForNavigationUpdates(p, callback)
            log("osmAnd: Nav-Updates registriert (id=$callbackId)")
        } catch (e: RemoteException) {
            log("osmAnd: registerForNavigationUpdates fehlgeschlagen: $e")
        }
    }

    private fun isInstalled(pkg: String): Boolean = try {
        context.packageManager.getPackageInfo(pkg, 0); true
    } catch (e: Exception) { false }

    @SuppressLint("MissingPermission")
    override fun stop() {
        try {
            if (callbackId > 0) {
                val p = ANavigationUpdateParams()
                p.setCallbackId(callbackId)
                p.setSubscribeToUpdates(false)
                try { svc?.registerForNavigationUpdates(p, callback) } catch (_: Exception) {}
            }
            context.unbindService(connection)
        } catch (e: Exception) { log("osmAnd: stop: $e") }
        bound = false
        navActive = false
    }
}
