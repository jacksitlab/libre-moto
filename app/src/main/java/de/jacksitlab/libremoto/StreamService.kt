package de.jacksitlab.libremoto

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import de.jacksitlab.libremoto.ble.BleController
import de.jacksitlab.libremoto.ble.BleLog
import de.jacksitlab.libremoto.bridge.NavSource
import de.jacksitlab.libremoto.bridge.NavSourceFactory
import de.jacksitlab.libremoto.bridge.SourceKind
import de.jacksitlab.libremoto.protocol.NavData
import de.jacksitlab.libremoto.protocol.NavType

/**
 * Foreground service that keeps the BLE stream alive when the app goes to background.
 * Started when streaming begins, stopped when streaming stops.
 */
class StreamService : Service() {

    companion object {
        private const val TAG = "LibreMoto"
        private const val CH_ID = "stream"
        private const val NOTIF_ID = 1
        const val EXTRA_STOP = "stop"

        @Volatile var ble: BleController? = null
        @Volatile var source: NavSource? = null
        @Volatile var mapEnabled: Boolean = false
        @Volatile var seq: Int = 0
        @Volatile @JvmStatic var running: Boolean = false

        fun start(ctx: Context, bleCtrl: BleController, navSource: NavSource, mapOn: Boolean) {
            ble = bleCtrl
            source = navSource
            mapEnabled = mapOn
            val intent = Intent(ctx, StreamService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(intent)
            } else {
                ctx.startService(intent)
            }
        }

        fun stop(ctx: Context) {
            running = false
            ctx.stopService(Intent(ctx, StreamService::class.java))
        }
    }

    private val handler = Handler(Looper.getMainLooper())
    private var wakeLock: PowerManager.WakeLock? = null

    /** Own thread for the pump: refresh() makes a synchronous binder call
     *  into OsmAnd (route geometry, 10-500 ms) — running that on the main
     *  looper delayed every BLE callback when OsmAnd was in the foreground. */
    private var pumpThread: java.util.concurrent.atomic.AtomicReference<Thread?> =
        java.util.concurrent.atomic.AtomicReference(null)
    private val pumpSleep = java.util.concurrent.Semaphore(0)

    override fun onCreate() {
        super.onCreate()
        createChannel()
        val notif = buildNotification("Stream running")
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            startForeground(NOTIF_ID, notif)
        } else {
            startForeground(NOTIF_ID, notif)
        }
        // Acquire partial wake lock to keep CPU running in background
        val pm = getSystemService(POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "LibreMoto:stream")
        wakeLock?.acquire(10 * 60 * 60 * 1000L) // 10 hours max
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.getBooleanExtra(EXTRA_STOP, false) == true) {
            running = false
            stopSelf()
            return START_NOT_STICKY
        }
        // EXACTLY ONE pump thread: onStartCommand fires on EVERY
        // startService() call (auto-connect, nav start, screen-on, boot).
        // The old unconditional startPump() created one thread per start
        // — 2-3 threads each sent 5 Hz = 10-15 slightly-offset frames/s
        // (the firmware's [FPS] 9-15), the map jumped between the
        // threads' positions and every thread's frame reset the DR
        // drift — the user's 'ruckeln'.
        val current = pumpThread.get()
        if (current?.isAlive == true) {
            Log.d(TAG, "pump already running — no second thread")
            return START_STICKY
        }
        running = true
        startPump()   // startPump() replaces the (dead) reference atomically
        return START_STICKY
    }

    private fun startPump() {
        // Pump on its own thread (see field comment): refresh() blocks on a
        // binder call into OsmAnd, which the main looper must not wait for.
        val t = Thread {
            while (running) {
                val cycleStart = android.os.SystemClock.elapsedRealtime()
                try { pumpOnce() } catch (e: Exception) { Log.d(TAG, "pump: ${e.message}") }
                // FIXED TICTOC (the stutter fix): the old code slept a
                // FLAT 200 ms ON TOP of the work (60+300 ms) -> a ~1.5 Hz
                // pump. Now the sleep fills the REST of a 200 ms cycle:
                // work + sleep = 200 ms -> a steady 5 Hz regardless of
                // how long a frame took.
                val workMs = android.os.SystemClock.elapsedRealtime() - cycleStart
                val restMs = (PUMP_PERIOD_MS - workMs).coerceAtLeast(0)
                try { pumpSleep.tryAcquire(restMs, java.util.concurrent.TimeUnit.MILLISECONDS) } catch (_: InterruptedException) { break }
            }
        }
        t.name = "LibreMoto-pump"
        pumpThread.set(t)
        t.start()
    }

    /** Tracks whether the display already knows we are idle — the idle
     *  NavData is sent ONCE when navigation ends (so the display goes to
     *  home), then the stream stays silent until nav starts again.
     *  User requirement: BLE data ONLY while a navigation is active. */
    @Volatile private var idleAnnounced = false

    /** Heartbeat while idle: keeps the TX path exercised (a wedged ATT
     *  pipeline is only detectable with traffic — the wedge detector
     *  needs writes to reject) and lets the firmware see the link is
     *  alive. 5 s = far below the 15 s link timeout, negligible airtime. */
    private var lastBeatMs = 0L
    private var pumpStat = 0

    /** Fixed pump cycle: work + sleep = 200 ms -> 5 Hz steady. */
    private val PUMP_PERIOD_MS = 200L
    private val beatIntervalMs = 5_000L

    private fun pumpOnce() {
        val b = ble ?: return
        val s = source ?: return
        if (!b.isReady) return

        val t0 = android.os.SystemClock.elapsedRealtime()
        s.refresh()
        val tRefresh = android.os.SystemClock.elapsedRealtime() - t0
        val t1 = android.os.SystemClock.elapsedRealtime()
        val nav = s.navData()
        if (!s.isNavigating) {
            val now = System.currentTimeMillis()
            if (!idleAnnounced) {
                idleAnnounced = true
                b.sendNavData(nav.toJson())   // one "idle" → display goes home
                lastBeatMs = now
            } else if (now - lastBeatMs >= beatIntervalMs) {
                b.sendNavData(NavData.BEAT.toJson())   // heartbeat ping
                lastBeatMs = now
            }
            return   // no map frames, no nav data without navigation
        }
        idleAnnounced = false
        val nd = if (mapEnabled) nav.copy(mapOn = true) else nav
        b.sendNavData(nd.toJson())
        if (mapEnabled) {
            try {
                val f = s.mapFrame(nextSeq())
                val t2 = android.os.SystemClock.elapsedRealtime()
                val bytes = f.encode()
                val t3 = android.os.SystemClock.elapsedRealtime()
                b.sendMap(bytes)
                val t4 = android.os.SystemClock.elapsedRealtime()
                if (++pumpStat >= 25) {   // every 25 frames = ~5 s at 5 Hz
                    pumpStat = 0
                    Log.d(TAG, "pump timing: refresh=${tRefresh}ms navData=${t1 - t0 - tRefresh}ms " +
                        "mapFrame=${t2 - t1}ms encode=${t3 - t2}ms sendMap=${t4 - t3}ms")
                }
            } catch (e: Exception) {
                Log.d(TAG, "pump timing: refresh=${tRefresh}ms — Map: error ${e.message}")
            }
        }
        // once a minute: TX health (callbacks vs pacing, queue depth)
        if (++tickCounter % 300 == 0) ble?.txStats()?.let { Log.d(TAG, it) }
    }

    private var tickCounter = 0

    private fun nextSeq(): Int { val v = seq; seq = (seq + 1) % 256; return v }

    private fun createChannel() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            val ch = NotificationChannel(CH_ID, "Stream", NotificationManager.IMPORTANCE_LOW)
            ch.description = "BLE stream to LibreMoto display"
            val nm = getSystemService(NOTIFICATION_SERVICE) as NotificationManager
            nm.createNotificationChannel(ch)
        }
    }

    private fun buildNotification(text: String): Notification {
        val builder = Notification.Builder(this, CH_ID)
            .setContentTitle("LibreMoto")
            .setContentText(text)
            .setSmallIcon(android.R.drawable.stat_notify_sync)
            .setOngoing(true)
        return builder.build()
    }

    override fun onDestroy() {
        running = false
        pumpThread.get()?.interrupt()
        wakeLock?.release()
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
