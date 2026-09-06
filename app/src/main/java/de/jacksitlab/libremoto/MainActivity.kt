package de.jacksitlab.libremoto

import android.Manifest
import android.content.pm.PackageManager
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.widget.Button
import android.widget.CheckBox
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import de.jacksitlab.libremoto.ble.BleLog
import de.jacksitlab.libremoto.ble.BleController
import de.jacksitlab.libremoto.bridge.NavSource
import de.jacksitlab.libremoto.bridge.NavSourceFactory
import de.jacksitlab.libremoto.bridge.SourceKind
import de.jacksitlab.libremoto.protocol.NavData
import de.jacksitlab.libremoto.protocol.NavType

class MainActivity : AppCompatActivity() {

    private lateinit var ble: BleController
    private var source: NavSource = NavSourceFactory.create(SourceKind.Mock, this, ::log)

    private val main = Handler(Looper.getMainLooper())
    @Volatile private var seq = 0
    private var streaming = false
    private val streamTimer = main

    private lateinit var bleStatus: TextView
    private lateinit var lastPayload: TextView
    private lateinit var logView: TextView
    private lateinit var streamBtn: Button
    private lateinit var sourceGroup: RadioGroup
    private lateinit var cbMap: CheckBox

    companion object {
        const val REQ_PERMS = 42
        val PERMS = if (android.os.Build.VERSION.SDK_INT >= 31)
            arrayOf(
                Manifest.permission.BLUETOOTH_SCAN,
                Manifest.permission.BLUETOOTH_CONNECT,
            )
        else
            arrayOf(Manifest.permission.ACCESS_FINE_LOCATION)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        ble = BleController(this) { line -> main.post { log(line) } }

        bleStatus = findViewById(R.id.ble_status)
        lastPayload = findViewById(R.id.last_payload)
        logView = findViewById(R.id.log)
        streamBtn = findViewById(R.id.btn_stream)
        sourceGroup = findViewById(R.id.source_group)
        cbMap = findViewById(R.id.cb_map)

        updateBleStatus()

        ensurePerms {
            findViewById<Button>(R.id.btn_connect).setOnClickListener { findAndConnect() }
            findViewById<Button>(R.id.btn_read_status).setOnClickListener {
                ble.readStatus()
            }
            findViewById<Button>(R.id.btn_disconnect).setOnClickListener { ble.disconnect(); updateBleStatus() }
            findViewById<Button>(R.id.btn_send_nav).setOnClickListener { sendOnce() }
            findViewById<Button>(R.id.btn_beep).setOnClickListener {
                ble.sendNavData(NavData(NavType.Beep).toJson())
                lastPayload.text = "beep"
            }
            streamBtn.setOnClickListener { toggleStream() }
            findViewById<Button>(R.id.btn_idle).setOnClickListener {
                ble.sendControl("{\"state\":\"idle\"}")
                stopStream()
            }

            sourceGroup.setOnCheckedChangeListener { _: RadioGroup?, id: Int ->
                val kind = if (id == R.id.rb_osmand) SourceKind.OsmAnd else SourceKind.Mock
                switchSource(kind)
            }
        }
    }

    private fun updateBleStatus() {
        main.post {
            bleStatus.text = if (ble.isReady) "BLE: verbunden" else ble.adapterState()
        }
    }

    private fun switchSource(kind: SourceKind) {
        stopStream()
        source.stop()
        source = NavSourceFactory.create(kind, this, ::log)
        source.start()
        log("Quelle: $kind — ${source.label}")
        updateBleStatus()
    }

    private fun ensurePerms(then: () -> Unit) {
        val missing = PERMS.filter {
            ContextCompat.checkSelfPermission(this, it) != PackageManager.PERMISSION_GRANTED
        }
        if (missing.isEmpty()) { then(); return }
        ActivityCompat.requestPermissions(this, missing.toTypedArray(), REQ_PERMS)
        // run the UI wiring regardless; permission grants will be applied on next tap
        then()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        if (requestCode == REQ_PERMS) {
            val parts = permissions.mapIndexed { i, p ->
                p.substringAfterLast('.') + if (grantResults.getOrNull(i) == PackageManager.PERMISSION_GRANTED) " OK" else " NEIN"
            }
            log("Berechtigungen: " + parts.joinToString(", "))
        }
    }

    private fun findAndConnect() {
        ble.startScan { name, address ->
            log("Gefunden: $name → $address")
            main.post { ble.connect(address) }
        }
    }

    // ---- send ------------------------------------------------------
    private fun sendOnce() {
        if (!ble.isReady) { log("nicht bereit"); updateBleStatus(); return }
        val nav = source.navData()
        val nd = if (cbMap.isChecked) nav.copy(mapOn = true) else nav
        val json = nd.toJson()
        ble.sendNavData(json)
        lastPayload.text = json
        if (cbMap.isChecked) {
            val f = source.mapFrame(nextSeq())
            val b = f.encode()
            ble.sendMap(b)
            log("Map: ${b.size} B, ${f.segments.size} Seg.")
        }
    }

    private fun nextSeq(): Int { val s = seq; seq = (seq + 1) % 256; return s }

    private fun toggleStream() {
        if (streaming) { stopStream(); return }
        if (!ble.isReady) { log("erst verbinden"); updateBleStatus(); return }
        streaming = true
        streamBtn.text = "Stream ⏸"
        log("Stream startet (1 Hz)…")
        fun pump() {
            if (!streaming) return
            sendOnce()
            streamTimer.postDelayed({ pump() }, 1000)
        }
        pump()
    }

    private fun stopStream() {
        streaming = false
        if (::streamBtn.isInitialized) streamBtn.text = "Stream ✕"
    }

    // ---- log -------------------------------------------------------
    private fun log(line: String) {
        val t = (System.currentTimeMillis() % 86_400_000) / 1000
        val stamp = "${t / 3600}:${(t / 60) % 60}:${(t % 60)}"
        val next = logView.text.toString() + "$stamp  $line\n"
        val lines = next.split('\n')
        logView.setText(if (lines.size > 60) lines.takeLast(60).joinToString("\n") else next)
    }

    override fun onDestroy() {
        stopStream()
        try { ble.disconnect() } catch (_: Exception) {}
        super.onDestroy()
    }
}
