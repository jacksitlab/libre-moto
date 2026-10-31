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
    /** Log line sink (UI-free, test-friendly). In the app this sink is wired to
     *  MainActivity.log, which mirrors every line to logcat (tag "LibreMoto"),
     *  so `adb logcat -s LibreMoto` shows the full BLE traffic (payloads included). */
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

    // ---- serialized TX queue -------------------------------------------
    // Android BLE allows exactly ONE writeCharacteristic in flight; a second
    // call while one is pending returns false and the payload is silently
    // dropped. NavData ticks (main loop) and map chunks (postDelayed pump) used
    // to collide exactly there — visible as stutter once the app is backgrounded.
    // Every write now goes through this queue: send next only after
    // onCharacteristicWrite of the previous one fired (callback-driven, no gaps).
    private object Tx {
        val queue = java.util.concurrent.ConcurrentLinkedQueue<ByteArray>()
        @Volatile var busy = false            // one write in flight
        @Volatile var active = false         // set once the GATT link is usable
    }
    @Volatile private var droppedMaps = 0L
    @Volatile private var lastMapDropLog = 0L

    /** Thread-safe: queues a raw payload and pumps if idle. */
    private fun enqueue(data: ByteArray) {
        if (!Tx.active) { log("tx: verworfen (nicht verbunden)"); return }
        Tx.queue.add(data)
        pumpTx()
    }

    /** Sends the head of the queue if no write is in flight. Called from
     *  enqueue() and from onCharacteristicWrite (queue head advanced).
     *  Synchronized: enqueue runs on the pump thread, the write callback
     *  arrives on a binder thread — both may pump concurrently. */
    @Synchronized
    private fun pumpTx() {
        if (Tx.busy) return
        val data = Tx.queue.poll() ?: return
        val g = gatt.get() ?: run { clearTx("gatt weg"); return }
        val c = when (data[0].toInt() and 0xFF) {
            0x01 -> chNav    // NAV marker byte (see sendNavData)
            0x02 -> chMap    // MAP marker byte (see sendMap)
            else -> chCtl    // CTL
        } ?: run { clearTx("channel weg"); return }
        Tx.busy = true
        try {
            c.value = data.copyOfRange(1, data.size)
            @Suppress("DEPRECATION")
            val ok = g.writeCharacteristic(c)
            if (!ok) { Tx.busy = false; log("tx: writeCharacteristic abgelehnt — Chunk verworfen") }
        } catch (e: SecurityException) {
            Tx.busy = false
            log("tx: fehlende Berechtigung")
        } catch (e: Exception) {
            Tx.busy = false
            log("tx: ${e.javaClass.simpleName}: ${e.message}")
        }
    }

    /** onCharacteristicWrite → advance. */
    private fun onWriteDone(ok: Boolean) {
        Tx.busy = false
        pumpTx()
    }

    /** Drops everything queued (disconnect, missing channels). */
    private fun clearTx(reason: String) {
        val n = Tx.queue.size
        Tx.queue.clear()
        Tx.busy = false
        if (n > 0) log("tx: $n Einträge verworfen ($reason)")
    }


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
                    clearTx("Verbindung getrennt")
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
            if (mtu < 100) {
                // MTU negotiation failed earlier — map chunks stay small (20 B)
                // and a NavData JSON may not fit. Log loudly once.
                log("BLE: MTU nur $mtu — NavData könnte > ${mtu - 3} B sein (stottern!)")
            }
            log("Service bereit — Chars: nav=" + (chNav != null) +
                " sts=" + (chSts != null) +
                " map=" + (chMap != null) +
                " ctl=" + (chCtl != null))
            subscribeStatus(g)
            readStatusOnce(g)
            Tx.active = true
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
            if (status != 0) log("Write " + c.uuid.toString().lowercase() + " status=$status ⚠")
            onWriteDone(status == 0)   // pump the TX queue whatever happened
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
    // All writes go through the serialized TX queue (marker byte + payload):
    // Android allows only one GATT write in flight, so sendNavData()/sendMap()
    // chunks must not be issued in parallel — see enqueue()/pumpTx().

    @SuppressLint("MissingPermission")
    fun sendNavData(json: String) {
        val payload = json.toByteArray(Charsets.UTF_8)
        if (payload.size > mtu - 3) {
            // Would be rejected by the stack anyway; drop instead of stalling the queue.
            log("nav: verworfen (${payload.size}B > MTU ${mtu - 3}B — MTU-Verhandlung prüfen)")
            return
        }
        log("TX nav  (${payload.size}B): $json")
        enqueue(byteArrayOf(0x01) + payload)
    }

    @SuppressLint("MissingPermission")
    fun sendControl(json: String) {
        val payload = json.toByteArray(Charsets.UTF_8)
        if (payload.size > mtu - 3) {
            log("ctl: verworfen (${payload.size}B > MTU ${mtu - 3}B)")
            return
        }
        log("TX ctl  (${payload.size}B): $json")
        enqueue(byteArrayOf(0x03) + payload)
    }

    /** Sends a binary MapFrame (F004), chunked to fit MTU. Chunks are queued
     *  atomically: either the whole frame is enqueued (previous map already
     *  drained) or it is dropped — two frames must never interleave on the
     *  characteristic, or the firmware reassembly desynchronizes. */
    @SuppressLint("MissingPermission")
    fun sendMap(frame: ByteArray) {
        val c = chMap ?: return log("map: Channel fehlt")
        val chunk = mapChunk
        // Header (11 B) + segments must not exceed the protocol frame budget;
        // encoding already guarantees ≤ MAX_FRAME (4096 B).
        // Refuse a second concurrent frame: at 1 Hz and full BLE throughput a
        // frame takes ~0.5 s, but if the link is congested we drop the new one.
        val queued = Tx.queue.size
        if (queued > 8) {
            droppedMaps++
            val now = System.currentTimeMillis()
            if (now - lastMapDropLog > 5000) {
                log("map: Frame verworfen (Queue=$queued, insgesamt $droppedMaps) — Link langsamer als 1 Hz")
                lastMapDropLog = now
            }
            return
        }
        val n = (frame.size + chunk - 1) / chunk
        log("TX map  (${frame.size}B, $n Chunks)")
        for (i in 0 until n) {
            val off = i * chunk
            val piece = frame.copyOfRange(off, minOf(off + chunk, frame.size))
            enqueue(byteArrayOf(0x02) + piece)
        }
    }

    fun disconnect() {
        Tx.active = false
        clearTx("disconnect")
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

/** Compact uppercase hex for binary payloads (MapFrame) in the logcat TX line. */
@Suppress("unused")
private fun hex(b: ByteArray): String = b.joinToString("") { "%02X".format(it) }
