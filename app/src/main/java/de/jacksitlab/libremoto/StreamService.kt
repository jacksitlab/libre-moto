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
        val notif = buildNotification("Stream läuft")
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
        running = true
        startPump()
        return START_STICKY
    }

    private fun startPump() {
        // Pump on its own thread (see field comment): refresh() blocks on a
        // binder call into OsmAnd, which the main looper must not wait for.
        val t = Thread {
            while (running) {
                try { pumpOnce() } catch (e: Exception) { Log.d(TAG, "pump: ${e.message}") }
                try { pumpSleep.tryAcquire(1000, java.util.concurrent.TimeUnit.MILLISECONDS) } catch (_: InterruptedException) { break }
            }
        }
        t.name = "LibreMoto-pump"
        pumpThread.set(t)
        t.start()
    }

    private fun pumpOnce() {
        val b = ble ?: return
        val s = source ?: return
        if (!b.isReady) return

        s.refresh()
        val nav = s.navData()
        val nd = if (mapEnabled) nav.copy(mapOn = true) else nav
        b.sendNavData(nd.toJson())
        if (mapEnabled) {
            try {
                val f = s.mapFrame(nextSeq())
                b.sendMap(f.encode())
            } catch (e: Exception) {
                Log.d(TAG, "Map: error ${e.message}")
            }
        }
        // once a minute: TX health (callbacks vs pacing, queue depth)
        if (++tickCounter % 60 == 0) ble?.txStats()?.let { Log.d(TAG, it) }
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
