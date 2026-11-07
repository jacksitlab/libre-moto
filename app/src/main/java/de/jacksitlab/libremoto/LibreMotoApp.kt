package de.jacksitlab.libremoto

import android.app.Application
import android.os.Handler
import android.os.Looper
import de.jacksitlab.libremoto.bridge.MapManager

/** Process-wide singletons: ONE MapManager shared by MainActivity,
 *  MapsActivity and the nav source — one region registry, one cache. */
class LibreMotoApp : Application() {

    lateinit var mapManager: MapManager
        private set

    override fun onCreate() {
        super.onCreate()
        val main = Handler(Looper.getMainLooper())
        mapManager = MapManager(this,
            log = { line -> main.post { AppLog.append(line) } },
            ui = { _ -> })
        mapManager.loadAll()
    }
}

/** Process-wide log sink: every component logs here; the MainActivity
 *  view subscribes when visible. Also mirrors to logcat (tag LibreMoto). */
object AppLog {
    private val lines = ArrayDeque<String>(200)
    private val listeners = ArrayList<(String) -> Unit>()
    private val main = Handler(Looper.getMainLooper())

    @Synchronized
    fun append(line: String) {
        try { android.util.Log.d("LibreMoto", line) } catch (_: Throwable) {}
        if (lines.size >= 200) lines.removeFirst()
        lines.addLast(line)
        val snapshot = ArrayList(listeners)
        main.post { snapshot.forEach { it(line) } }
    }

    @Synchronized
    fun history(): List<String> = lines.toList()

    @Synchronized
    fun subscribe(l: (String) -> Unit) {
        listeners.add(l)
        main.post { history().forEach(l) }
    }

    @Synchronized
    fun unsubscribe(l: (String) -> Unit) {
        listeners.remove(l)
    }
}
