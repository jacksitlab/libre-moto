package de.jacksitlab.libremoto

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View


import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import de.jacksitlab.libremoto.ble.BleController
import de.jacksitlab.libremoto.bridge.NavSourceFactory
import de.jacksitlab.libremoto.bridge.SourceKind

/** Minimalist start screen: logo, connection status, auto-connect.
 *  Everything else lives in the menu (maps view, log) — no manual
 *  send/stream buttons: the stream starts automatically once BLE is
 *  connected and OsmAnd navigation is active. */
class MainActivity : AppCompatActivity() {

    private lateinit var ble: BleController
    private lateinit var bleStatus: TextView
    private lateinit var bleDetail: TextView
    private lateinit var retryBtn: Button
    private lateinit var mapStatus: TextView
    private lateinit var logView: TextView
    private lateinit var logToggle: TextView
    private var logVisible = false

    private val main = Handler(Looper.getMainLooper())
    private var connectAttempts = 0
    private var retryRunnable: Runnable? = null

    companion object {
        const val REQ_PERMS = 42
        const val CONNECT_TIMEOUT_MS = 15_000L
        val PERMS = if (android.os.Build.VERSION.SDK_INT >= 31)
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
            )
        else
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    private val logListener: (String) -> Unit = { line -> renderLog(line) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ble = BleController(this) { line -> AppLog.append(line) }

        bleStatus = findViewById(R.id.ble_status)
        bleDetail = findViewById(R.id.ble_detail)
        retryBtn = findViewById(R.id.btn_retry)
        mapStatus = findViewById(R.id.map_status)
        logView = findViewById(R.id.log)
        logToggle = findViewById(R.id.log_toggle)

        retryBtn.setOnClickListener { autoConnect() }
        logToggle.setOnClickListener { toggleLog() }

        // Menu (⋮) top right: classic PopupMenu — deterministic
        // visible regardless of theme (the toolbar only showed
        // den Titel, keine drei Punkte).
        findViewById<TextView>(R.id.menu_btn).setOnClickListener { anchor ->
            val pm = android.widget.PopupMenu(this, anchor)
            pm.menuInflater.inflate(R.menu.menu_main, pm.menu)
            pm.setOnMenuItemClickListener { item ->
                when (item.itemId) {
                    R.id.menu_maps -> { startActivity(Intent(this, MapsActivity::class.java)); true }
                    R.id.menu_log -> { toggleLog(); true }
                    else -> false
                }
            }
            pm.show()
        }

        // Build stamp: which code is installed? (git describe at build time)
        findViewById<TextView>(R.id.build_id).text = BuildStamp.id(this)

        // connection state → UI
        ble.onConnectionChanged = { connected ->
            main.post {
                renderConnection(connected)
                if (!connected && StreamService.running) {
                    StreamService.stop(this)
                }
            }
        }

        ensurePerms()
        AppLog.subscribe(logListener)
        autoConnect()
    }

    // ---- connection -------------------------------------------------

    private fun autoConnect() {
        // cancel any pending retry — one connect cycle at a time
        retryRunnable?.let { main.removeCallbacks(it) }
        retryRunnable = null
        connectAttempts++
        retryBtn.visibility = View.GONE
        bleStatus.text = "Suche LibreMoto-Display …"
        bleDetail.text = "Versuch $connectAttempts"
        ble.startScan { _, address ->
            main.post {
                bleStatus.text = "Verbinde …"
                ble.connect(address)
            }
        }
        // timeout: no connection after N s → retry (with backoff: the ESP32
        // may still hold a PHANTOM connection after Android dropped it —
        // it cannot advertise until its own supervision timeout fires
        // (seconds). Blind rescanning during that window finds nothing.
        val r = Runnable {
            if (!ble.isReady) {
                renderConnection(false)
                // auto-retry with growing delay: 3 s, 6 s, 12 s … capped 30 s
                val delay = (3000L * (1 shl (connectAttempts - 1).coerceAtMost(4)))
                    .coerceAtMost(30_000L)
                retryBtn.visibility = View.VISIBLE
                retryBtn.text = "Erneut versuchen (auto in ${delay / 1000} s)"
                val rr = Runnable {
                    if (!ble.isReady && retryBtn.visibility == View.VISIBLE) autoConnect()
                }
                retryRunnable = rr
                main.postDelayed(rr, delay)
            }
        }
        main.postDelayed(r, CONNECT_TIMEOUT_MS)
    }

    private fun renderConnection(connected: Boolean) {
        if (connected) {
            bleStatus.text = "✓ Verbunden"
            bleDetail.text = ""
            retryBtn.visibility = View.GONE
            startStreamIfReady()
        } else {
            bleStatus.text = "Nicht verbunden"
            bleDetail.text = if (ble.adapterState().contains("aus")) "Bluetooth ist aus" else "Display nicht gefunden"
            retryBtn.visibility = View.VISIBLE
        }
    }

    private fun startStreamIfReady() {
        if (!ble.isReady) return
        if (StreamService.running) return
        StreamService.start(this, ble, navSource, mapOn = true)
    }

    // The nav source: OsmAnd only (Mock removed from the UI; the factory
    // still supports it for tests).
    private val navSource by lazy {
        val s = NavSourceFactory.create(SourceKind.OsmAnd, this) { line -> AppLog.append(line) }
        (s as? de.jacksitlab.libremoto.bridge.OsmAndNavSource)?.attachMapManager(
            (application as LibreMotoApp).mapManager)
        s.start()
        s
    }

    // ---- log --------------------------------------------------------

    private fun toggleLog() {
        logVisible = !logVisible
        logView.visibility = if (logVisible) View.VISIBLE else View.GONE
        logToggle.text = if (logVisible) "Log ▾" else "Log ▸"
        if (logVisible) logView.text = AppLog.history().joinToString("\n")
    }

    private fun renderLog(line: String) {
        if (!logVisible) return
        logView.text = AppLog.history().takeLast(60).joinToString("\n")
    }

    // ---- permissions ------------------------------------------------

    private fun ensurePerms() {
        val missing = PERMS.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isNotEmpty()) {
            ActivityCompat.requestPermissions(this, missing.toTypedArray(), REQ_PERMS)
        }
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (requestCode == REQ_PERMS) {
            val granted = grantResults.all { it == PackageManager.PERMISSION_GRANTED }
            if (granted) autoConnect()
        }
    }

    // ---- lifecycle ---------------------------------------------------

    override fun onResume() {
        super.onResume()
        renderConnection(ble.isReady)
        // keep the map status line fresh
        val mm = (application as LibreMotoApp).mapManager
        mapStatus.text = if (mm.loadedRegions().isEmpty()) "No maps installed (menu → map manager)"
        else "Karten: ${mm.loadedRegions().keys.joinToString()}"
    }

    override fun onDestroy() {
        AppLog.unsubscribe(logListener)
        super.onDestroy()
    }
}
