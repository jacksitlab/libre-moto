/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * BleController — GATT client for the LibreMoto board (ESP32 server).
 *  - Scans for the advertised device name (LibreMoto).
 *  - Negotiates MTU 512 (protocol.md §1), falls back to 23.
 *  - Subscribes to Status (F003) — read on connect + notify on change.
 *  - Writes NavData (F002, JSON), MapData (F004, binary, chunked), Control (F005).
 * Callbacks are marshalled to the main thread.
 */
package de.jacksitlab.libremoto.ble

import android.annotation.SuppressLint
import android.bluetooth.BluetoothAdapter
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothManager
import android.content.Context
import java.util.UUID

object Uuid {
    const val NAME = "LibreMoto"
    val SVC = UUID.fromString("6F1C4D52-5B66-4a3e-9c8a-3D2E7F1A4401")
    val NAV = UUID.fromString("6F1C4D52-5B66-4a3e-9c8a-3D2E7F1A4402")
    val STS = UUID.fromString("6F1C4D52-5B66-4a3e-9c8a-3D2E7F1A4403")
    val MAP = UUID.fromString("6F1C4D52-5B66-4a3e-9c8a-3D2E7F1A4404")
    val CTL = UUID.fromString("6F1C4D52-5B66-4a3e-9c8a-3D2E7F1A4405")
    val CCC = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
    const val MTU_TARGET = 512
}

/** Log sink so BleController stays UI-free (unit-test friendly). */
fun interface BleLog { fun log(line: String) }

class BleController(private val context: Context, log: (String) -> Unit) {
    /** Log line sink (UI-free, test-friendly). */
    private val log: (String) -> Unit = log
    private val gatt = java.util.concurrent.atomic.AtomicReference<android.bluetooth.BluetoothGatt>()
    @Volatile var connected = false; private set
    @Volatile var mtu = 23; private set
    @Volatile var mapChunk = 20; private set

    private var chNav: android.bluetooth.BluetoothGattCharacteristic? = null
    private var chSts: android.bluetooth.BluetoothGattCharacteristic? = null
    private var chMap: android.bluetooth.BluetoothGattCharacteristic? = null
    private var chCtl: android.bluetooth.BluetoothGattCharacteristic? = null

    private val bt: BluetoothManager?
        get() = context.getSystemService(Context.BLUETOOTH_SERVICE) as? BluetoothManager
    private val adapter: BluetoothAdapter?
        get() = bt?.adapter
    private val scanner get() = adapter?.bluetoothLeScanner

    val isReady: Boolean get() = connected && chNav != null && chMap != null

    fun adapterState(): String = when {
        bt == null -> "BLE: nicht verfügbar"
        adapter == null -> "BLE: Adapter fehlt"
        !adapter!!.isEnabled -> "BLE: ausgeschaltet"
        else -> "BLE: bereit"
    }

    @SuppressLint("MissingPermission")
    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: android.bluetooth.BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                android.bluetooth.BluetoothProfile.STATE_CONNECTED -> {
                    log("Verbunden")
                    requestMtuAndDiscover(g)
                }
                android.bluetooth.BluetoothProfile.STATE_DISCONNECTED -> {
                    connected = false
                    log("Getrennt (status=$status)")
                    g.close()
                }
            }
        }

        private fun requestMtuAndDiscover(g: android.bluetooth.BluetoothGatt) {
            try { g.requestMtu(Uuid.MTU_TARGET) } catch (e: Exception) { discover(g) }
        }

        override fun onMtuChanged(g: android.bluetooth.BluetoothGatt, mtu: Int, status: Int) {
            if (status == 0) {
                this@BleController.mtu = mtu
                mapChunk = (mtu - 3).coerceAtLeast(20)
                log("MTU=$mtu, MapChunk=$mapChunk")
            }
            // small settle then discover (some stacks need a beat after MTU)
            android.os.Handler(android.os.Looper.getMainLooper())
                .postDelayed({ discover(g) }, 120)
        }

        private fun discover(g: android.bluetooth.BluetoothGatt) {
            try { g.discoverServices() } catch (e: SecurityException) { log("BLE: no service-discovery perm") }
        }

        override fun onServicesDiscovered(g: android.bluetooth.BluetoothGatt, status: Int) {
            val svc = g.getService(Uuid.SVC)
            if (svc == null) { log("BLE: Service ${Uuid.SVC} nicht gefunden"); return }
            chNav = svc.getCharacteristic(Uuid.NAV)
            chSts = svc.getCharacteristic(Uuid.STS)
            chMap = svc.getCharacteristic(Uuid.MAP)
            chCtl = svc.getCharacteristic(Uuid.CTL)
            connected = true
            log("Service bereit — Chars: nav=" + (chNav != null) +
                " sts=" + (chSts != null) +
                " map=" + (chMap != null) +
                " ctl=" + (chCtl != null))
            subscribeStatus(g)
            readStatusOnce(g)
        }

        /** In-callback helper (avoids clashing with the public readStatus()). */
        private fun readStatusOnce(g: android.bluetooth.BluetoothGatt) {
            val c = chSts ?: return
            try { g.readCharacteristic(c) } catch (_: Exception) {}
        }

        private fun subscribeStatus(g: android.bluetooth.BluetoothGatt) {
            val c = chSts ?: return
            try {
                g.setCharacteristicNotification(c, true)
                c.getDescriptor(Uuid.CCC)?.let { d ->
                    d.value = android.bluetooth.BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                    g.writeDescriptor(d)
                }
                log("Status-Notify abonniert")
            } catch (e: Exception) { log("Status-Notify fehlgeschlagen: $e") }
        }

        override fun onCharacteristicRead(g: android.bluetooth.BluetoothGatt, c: android.bluetooth.BluetoothGattCharacteristic, status: Int) {
            if (c.uuid == Uuid.STS) c.value?.let { log("Status gelesen: " + HexUtil.decode(it)) }
        }

        override fun onCharacteristicWrite(g: android.bluetooth.BluetoothGatt, c: android.bluetooth.BluetoothGattCharacteristic, status: Int) {
            log("Write " + c.uuid.toString().lowercase() + " status=$status" + (if (status != 0) " ⚠" else " ✓"))
        }

        override fun onCharacteristicChanged(g: android.bluetooth.BluetoothGatt, c: android.bluetooth.BluetoothGattCharacteristic) {
            if (c.uuid == Uuid.STS) c.value?.let { log("Status-Notify: " + HexUtil.decode(it)) }
        }
    }
    // ---- scans -----------------------------------------------------
    @SuppressLint("MissingPermission")
    fun startScan(onFound: (name: String, address: String) -> Unit) {
        val a = adapter
        if (a == null) { log("BLE: kein Adapter"); return }
        if (!a.isEnabled) { log("BLE: erst einschalten"); return }
        try {
            val cb = object : android.bluetooth.le.ScanCallback() {
                override fun onScanResult(type: Int, res: android.bluetooth.le.ScanResult) {
                    val name = res.scanRecord?.deviceName ?: res.device.name
                    if (name == Uuid.NAME) {
                        a.bluetoothLeScanner.stopScan(this)
                        onFound(name, res.device.address)
                    }
                }
            }
            a.bluetoothLeScanner.startScan(cb)
            log("Suche: „${Uuid.NAME}“ …")
        } catch (e: SecurityException) {
            log("BLE: fehlende Berechtigung (BLUETOOTH_SCAN/CONNECT)")
        }
    }

    @SuppressLint("MissingPermission")
    fun connect(address: String) {
        val a = adapter
        if (a == null) { log("BLE: kein Adapter"); return }
        if (!a.isEnabled) { log("BLE: aus"); return }
        val dev = a.getRemoteDevice(address)
        log("Verbinde: " + address)
        val g = dev.connectGatt(context, false, gattCb, BluetoothDevice.TRANSPORT_LE)
        gatt.set(g)
    }



    /** Reads current Status (F003) — protocol §3. */
    @SuppressLint("MissingPermission")
    fun readStatus() {
        val c = chSts ?: return log("Status: Channel fehlt")
        val g = gatt.get() ?: return
        try { g.readCharacteristic(c) } catch (e: SecurityException) { log("BLE: no read perm") }
    }

    // ---- writes ----------------------------------------------------
    private inline fun writeChar(c: android.bluetooth.BluetoothGattCharacteristic, data: ByteArray, tag: String) {
        val g = gatt.get()
        if (g == null) { log("$tag: nicht verbunden"); return }
        if (data.size > mtu - 3) { log("$tag: zu groß ${data.size}B > ${mtu - 3}B"); return }
        try {
            c.value = data
            @Suppress("DEPRECATION")
            g.writeCharacteristic(c)
        } catch (e: SecurityException) {
            log("$tag: fehlende Berechtigung")
        }
    }

    @SuppressLint("MissingPermission")
    fun sendNavData(json: String) {
        val c = chNav ?: run { log("nav: Channel fehlt"); return }
        writeChar(c, json.toByteArray(Charsets.UTF_8), "nav")
    }

    @SuppressLint("MissingPermission")
    fun sendControl(json: String) {
        val c = chCtl ?: run { log("ctl: Channel fehlt"); return }
        writeChar(c, json.toByteArray(Charsets.UTF_8), "ctl")
    }

    /** Sends a binary MapFrame (F004), chunked to fit MTU. Serial chunks via a simple 40 ms gap. */
    @SuppressLint("MissingPermission")
    fun sendMap(frame: ByteArray) {
        val c = chMap ?: return log("map: Channel fehlt")
        val g = gatt.get() ?: return log("map: nicht verbunden")
        val chunk = mapChunk
        val n = (frame.size + chunk - 1) / chunk
        var idx = 0
        val handler = android.os.Handler(android.os.Looper.getMainLooper())
        fun pump() {
            if (idx >= n) { log("map: fertig ${frame.size}B in $n Chunks"); return }
            val off = idx * chunk
            val piece = frame.copyOfRange(off, minOf(off + chunk, frame.size))
            idx++
            try {
                c.value = piece
                @Suppress("DEPRECATION")
                g.writeCharacteristic(c)
            } catch (e: SecurityException) {
                log("map: fehlende Berechtigung"); return
            }
            handler.postDelayed({ pump() }, 40)
        }
        log("map: sende ${frame.size}B in $n Chunks …")
        pump()
    }

    fun disconnect() {
        gatt.get()?.let { g ->
            try { g.disconnect() } catch (e: Exception) {}
            g.close()
        }
        gatt.set(null)
        connected = false
        log("getrennt (User)")
    }
}

/** Decodes a Status payload. Firmware may send it as JSON bytes or a hex string;
 * we handle both so the log stays useful either way. */
object HexUtil {
    fun decode(b: ByteArray): String {
        // try UTF-8 JSON first (protocol.md §3)
        val s = String(b, Charsets.UTF_8)
        if (s.startsWith("{")) return s
        // fallback: hex dump
        return b.joinToString(" ") { "%02X".format(it) }
    }
}
