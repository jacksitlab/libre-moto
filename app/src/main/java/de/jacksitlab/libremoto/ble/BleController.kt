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

/** TX marker bytes: first byte of every queued payload selects the channel.
 *  CH_GAP is a timing marker, not a write: it keeps a small pause after a
 *  map frame so the firmware's 500 ms fallback timeout closes the LAST
 *  frame of a burst (frames before it are closed deterministically by
 *  the next frame's magic — see map_reassembly.h). */
private const val CH_NAV = 0x01
private const val CH_MAP = 0x02
private const val CH_CTL = 0x03
private const val CH_GAP = 0x04
/** Pause after the last map chunk of a frame. The firmware closes frames
 *  deterministically (magic), so this only bounds the latency of the very
 *  last frame — 120 ms is plenty and keeps the stream flowing. */
private const val MAP_GAP_MS = 120L
/** TX pacing: a write slot is released after this time even when the stack
 *  never delivers onCharacteristicWrite (proven on the test tablet). */
private const val TX_PACE_MS = 40L

class BleController(private val context: Context, log: (String) -> Unit) {
    /** Log line sink (UI-free, test-friendly). In the app this sink is wired to
     *  MainActivity.log, which mirrors every line to logcat (tag "LibreMoto"),
     *  so `adb logcat -s LibreMoto` shows the full BLE traffic (payloads included). */
    private val log: (String) -> Unit = log
    private val gatt = java.util.concurrent.atomic.AtomicReference<android.bluetooth.BluetoothGatt>()
    @Volatile var connected = false; private set
    /** Notified on connect AND disconnect (main thread). The UI subscribes
     *  to drive its auto-connect/auto-stream behaviour. */
    @Volatile var onConnectionChanged: ((Boolean) -> Unit)? = null
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
    // dropped. Every write goes through this queue: send next only after
    // onCharacteristicWrite of the previous one fired.
    //
    // MAP GAP: the firmware closes a MapData frame only after 500 ms WITHOUT a
    // new map chunk (ble_link.h reassembly — no length prefix, no delimiter).
    // Chunks of consecutive frames must therefore NEVER arrive back-to-back,
    // or the firmware concatenates both frames into one buffer and the strict
    // parser drops the result (display freeze). sendMap() enqueues a CH_GAP
    // marker after the last chunk; pumpTx() holds it (and everything behind
    // it) until MAP_GAP_MS passed since the last map write. Nav/ctl writes
    // are small and 1 Hz — the occasional delay is invisible.
    private object Tx {
        val queue = java.util.concurrent.ConcurrentLinkedQueue<ByteArray>()
        @Volatile var busy = false            // one write in flight (pacing slot)
        @Volatile var busySinceMs = 0L        // pacing reference
        @Volatile var writeCbs = 0L           // onCharacteristicWrite received
        @Volatile var pacedWrites = 0L        // writes released by the 40 ms timer
        @Volatile var active = false         // set once the GATT link is usable
        @Volatile var rejects = 0            // consecutive writeCharacteristic rejections
    }
    @Volatile private var droppedMaps = 0L
    @Volatile private var lastMapDropLog = 0L
    @Volatile private var lastMapWriteAtMs = 0L

    /** Dedicated TX thread (pacing + gap timing). NOT the main looper:
     *  pumpTx() is @Synchronized and shared with the pump thread and the BLE
     *  binder callbacks — if the UI thread ever waits for that lock (it did,
     *  via a main-looper Handler: the Stream button became unresponsive),
     *  the whole app freezes. */
    private val txThread = android.os.HandlerThread("LibreMoto-tx").apply { start() }
    private val gapHandler = android.os.Handler(txThread.looper)

    /** Thread-safe: queues a raw payload and pumps if idle. */
    private fun enqueue(data: ByteArray) {
        if (!Tx.active) { log("tx: dropped (not connected)"); return }
        Tx.queue.add(data)
        pumpTx()
    }

    /** Sends the head of the queue — PACING-FIRST, not callback-first.
     *
     *  The log proved this tablet never delivers onCharacteristicWrite for
     *  our characteristics (queue grew monotonically, zero callbacks, zero
     *  watchdog fires). A pipeline that WAITS for the callback stalls on
     *  such a stack. So the TX thread releases a write slot every
     *  TX_PACE_MS on its own; an arriving callback only releases the slot
     *  EARLY (bonus throughput on stacks that do deliver it).
     *
     *  This is safe even without callbacks: 40 ms ≫ the ~10-15 ms a
     *  Write-Without-Response needs in the stack — the same pacing the
     *  original code used when frames DID flow. */
    @Synchronized
    private fun pumpTx() {
        if (!Tx.active) { clearTx("inaktiv"); return }
        while (true) {
            if (Tx.busy) {
                val since = System.currentTimeMillis() - Tx.busySinceMs
                if (since >= TX_PACE_MS) {
                    Tx.busy = false
                    Tx.pacedWrites++
                } else {
                    gapHandler.postDelayed({ pumpTx() }, TX_PACE_MS - since)
                    return
                }
            }
            val data = Tx.queue.peek() ?: return
            when (data[0].toInt() and 0xFF) {
                CH_GAP -> {
                    // small pause after a map frame (firmware fallback timeout)
                    val since = System.currentTimeMillis() - lastMapWriteAtMs
                    if (since < MAP_GAP_MS) {
                        gapHandler.postDelayed({ pumpTx() }, MAP_GAP_MS - since)
                        return
                    }
                    Tx.queue.poll()          // gap elapsed — consume marker
                }
                else -> {
                    val g = gatt.get() ?: run { clearTx("gatt weg"); return }
                    val c = when (data[0].toInt() and 0xFF) {
                        CH_NAV -> chNav
                        CH_MAP -> chMap
                        else -> chCtl
                    } ?: run { clearTx("channel weg"); return }
                    Tx.queue.poll()   // CONSUME the head — peek() alone made the
                                      // TX thread rewrite the same item forever
                    Tx.busy = true
                    Tx.busySinceMs = System.currentTimeMillis()
                    try {
                        c.value = data.copyOfRange(1, data.size)
                        @Suppress("DEPRECATION")
                        val ok = g.writeCharacteristic(c)
                        if (!ok) {
                            Tx.busy = false
                            /* WEDGE DETECTION: 'ok=false' means the stack
                             * refused the command — on this tablet it
                             * refuses with 'prior command is not finished'
                             * once its ATT pipeline is wedged (a command
                             * that never completed, e.g. after the peer
                             * reset mid-write). The stack never recovers
                             * from that on its own; only a fresh GATT
                             * object does. After a few consecutive
                             * rejections, tear the connection down —
                             * the normal auto-reconnect rebuilds it. */
                            Tx.rejects++
                            if (Tx.rejects >= 5) {
                                log("tx: ${Tx.rejects}x rejected — GATT wedged, rebuilding connection")
                                recoverWedgedGatt(g)
                            } else {
                                log("tx: writeCharacteristic rejected — chunk dropped")
                            }
                            return
                        }
                        Tx.rejects = 0
                        if (data[0].toInt() and 0xFF == CH_MAP) {
                            lastMapWriteAtMs = System.currentTimeMillis()
                        }
                    } catch (e: SecurityException) {
                        Tx.busy = false
                        log("tx: missing permission")
                    } catch (e: Exception) {
                        Tx.busy = false
                        log("tx: ${e.javaClass.simpleName}: ${e.message}")
                    }
                    return   // slot busy; timer or callback releases it
                }
            }
        }
    }

    /** onCharacteristicWrite → release the pacing slot EARLY (stacks that
     *  deliver callbacks get bonus throughput; those that don't rely on
     *  the TX_PACE_MS timer and lose nothing). */
    private fun onWriteDone(ok: Boolean) {
        if (!ok) log("tx: write status != 0")
        if (Tx.busy) {
            Tx.writeCbs++
            Tx.busy = false
        }
        pumpTx()
    }

    /** Diagnostics for the log: how writes were released. */
    fun txStats(): String =
        "tx: cbs=${Tx.writeCbs} paced=${Tx.pacedWrites} queue=${Tx.queue.size}"

    /** Drops everything queued (disconnect, missing channels). */
    private fun clearTx(reason: String) {
        val n = Tx.queue.size
        Tx.queue.clear()
        Tx.busy = false
        Tx.rejects = 0
        if (n > 0) log("tx: $n entries dropped ($reason)")
    }

    /** Recover a wedged ATT pipeline: the stack refuses every
     *  writeCharacteristic ('prior command is not finished') forever —
     *  only a fresh GATT object clears it. Tearing the connection down
     *  triggers the normal disconnect → auto-reconnect cycle, which
     *  creates exactly that fresh object. Called from the TX thread. */
    @SuppressLint("MissingPermission")
    private fun recoverWedgedGatt(g: android.bluetooth.BluetoothGatt) {
        Tx.active = false
        clearTx("GATT verkeilt")
        try { g.disconnect() } catch (_: Exception) {}
        // close() happens in onConnectionStateChange(STATE_DISCONNECTED)
    }


    fun adapterState(): String = when {
        bt == null -> "BLE: unavailable"
        adapter == null -> "BLE: adapter missing"
        !adapter!!.isEnabled -> "BLE: switched off"
        else -> "BLE: ready"
    }

    @SuppressLint("MissingPermission")
    private val gattCb = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: android.bluetooth.BluetoothGatt, status: Int, newState: Int) {
            when (newState) {
                android.bluetooth.BluetoothProfile.STATE_CONNECTED -> {
                    log("connected")
                    requestMtuAndDiscover(g)
                }
                android.bluetooth.BluetoothProfile.STATE_DISCONNECTED -> {
                    connected = false
                    onConnectionChanged?.invoke(false)
                    clearTx("Verbindung getrennt")
                    log("disconnected (status=$status)")
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
            if (svc == null) { log("BLE: service ${Uuid.SVC} not found"); return }
            chNav = svc.getCharacteristic(Uuid.NAV)
            chSts = svc.getCharacteristic(Uuid.STS)
            chMap = svc.getCharacteristic(Uuid.MAP)
            chCtl = svc.getCharacteristic(Uuid.CTL)
            connected = true
            onConnectionChanged?.invoke(true)
            if (mtu < 100) {
                // MTU negotiation failed earlier — map chunks stay small (20 B)
                // and a NavData JSON may not fit. Log loudly once.
                log("BLE: MTU only $mtu — NavData may exceed ${mtu - 3} B (stuttering!)")
            }
            log("Service ready — chars: nav=" + (chNav != null) +
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
            } catch (e: Exception) { log("status notify failed: $e") }
        }

        override fun onCharacteristicRead(g: android.bluetooth.BluetoothGatt, c: android.bluetooth.BluetoothGattCharacteristic, status: Int) {
            if (c.uuid == Uuid.STS) c.value?.let { log("status read: " + HexUtil.decode(it)) }
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
    /** The ONE active scan callback (null = no scan running). startScan
     *  stops any previous scan first — repeated retries used to LEAK
     *  parallel scans (scannerIds 8..13 in the logcat), and Android blocks
     *  the app after 5 concurrent scans. */
    @Volatile private var activeScan: android.bluetooth.le.ScanCallback? = null

    @SuppressLint("MissingPermission")
    fun stopScan() {
        val a = adapter ?: return
        activeScan?.let { cb ->
            try { a.bluetoothLeScanner.stopScan(cb) } catch (_: Exception) {}
            activeScan = null
        }
    }

    @SuppressLint("MissingPermission")
    fun startScan(onFound: (name: String, address: String) -> Unit) {
        val a = adapter
        if (a == null) { log("BLE: kein Adapter"); return }
        if (!a.isEnabled) { log("BLE: erst einschalten"); return }
        stopScan()   // never two scans at once
        try {
            val cb = object : android.bluetooth.le.ScanCallback() {
                override fun onScanResult(type: Int, res: android.bluetooth.le.ScanResult) {
                    val name = res.scanRecord?.deviceName ?: res.device.name
                    if (name == Uuid.NAME) {
                        stopScan()
                        onFound(name, res.device.address)
                    }
                }
            }
            activeScan = cb
            a.bluetoothLeScanner.startScan(cb)
            log("scanning: \"${Uuid.NAME}\" …")
        } catch (e: SecurityException) {
            log("BLE: missing permission (BLUETOOTH_SCAN/CONNECT)")
        }
    }

    @SuppressLint("MissingPermission")
    fun connect(address: String) {
        val a = adapter
        if (a == null) { log("BLE: kein Adapter"); return }
        if (!a.isEnabled) { log("BLE: aus"); return }
        val dev = a.getRemoteDevice(address)
        log("connecting: " + address)
        val g = dev.connectGatt(context, false, gattCb, BluetoothDevice.TRANSPORT_LE)
        gatt.set(g)
    }



    /** Reads current Status (F003) — protocol §3. */
    @SuppressLint("MissingPermission")
    fun readStatus() {
        val c = chSts ?: return log("status: channel missing")
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
            log("nav: dropped (${payload.size}B > MTU ${mtu - 3}B — check MTU negotiation)")
            return
        }
        log("TX nav  (${payload.size}B): $json")
        enqueue(byteArrayOf(CH_NAV.toByte()) + payload)
    }

    @SuppressLint("MissingPermission")
    fun sendControl(json: String) {
        val payload = json.toByteArray(Charsets.UTF_8)
        if (payload.size > mtu - 3) {
            log("ctl: dropped (${payload.size}B > MTU ${mtu - 3}B)")
            return
        }
        log("TX ctl  (${payload.size}B): $json")
        enqueue(byteArrayOf(CH_CTL.toByte()) + payload)
    }

    /** Sends a binary MapFrame (F004), chunked to fit MTU. Chunks are queued
     *  atomically: either the whole frame is enqueued (previous map already
     *  drained) or it is dropped — two frames must never interleave on the
     *  characteristic, or the firmware reassembly desynchronizes. */
    @SuppressLint("MissingPermission")
    fun sendMap(frame: ByteArray) {
        val c = chMap ?: return log("map: channel missing")
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
                log("map: frame dropped (queue=$queued, total $droppedMaps) — link slower than 1 Hz")
                lastMapDropLog = now
            }
            return
        }
        val n = (frame.size + chunk - 1) / chunk
        log("TX map  (${frame.size}B, $n Chunks)")
        // Precompute magic-safe chunk boundaries: no continuation chunk may
        // start with 4C 4D 01 (would falsely close the frame at the firmware —
        // sender contract, map_reassembly.h).
        val bounds = ArrayList<Int>()
        bounds.add(0)
        var b = chunk
        while (b < frame.size) {
            if (frame.size - b >= 3 &&
                frame[b] == 0x4C.toByte() && frame[b + 1] == 0x4D.toByte() && frame[b + 2] == 0x01.toByte()) {
                b += 1   // shift boundary so the magic stays inside a chunk
            }
            bounds.add(b)
            b += chunk
        }
        bounds.add(frame.size)
        for (i in 0 until bounds.size - 1) {
            val s = bounds[i]
            val e = bounds[i + 1]
            enqueue(byteArrayOf(CH_MAP.toByte()) + frame.copyOfRange(s, e))
        }
        // gap marker: short pause so the firmware's 500 ms fallback closes
        // the last frame of the burst (earlier frames close via magic)
        enqueue(byteArrayOf(CH_GAP.toByte()))
    }

    fun disconnect() {
        Tx.active = false
        clearTx("disconnect")
        gapHandler.removeCallbacksAndMessages(null)   // drop pending timers
        gatt.get()?.let { g ->
            try { g.disconnect() } catch (e: Exception) {}
            g.close()
        }
        gatt.set(null)
        connected = false
        log("disconnected (user)")
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
