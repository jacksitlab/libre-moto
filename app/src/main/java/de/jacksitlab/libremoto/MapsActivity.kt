package de.jacksitlab.libremoto

import android.app.Activity
import android.app.AlertDialog
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.view.View
import android.widget.ArrayAdapter
import android.widget.ListView
import android.widget.ProgressBar
import android.widget.TabHost
import android.widget.TextView
import de.jacksitlab.libremoto.bridge.GeofabrikRegion
import de.jacksitlab.libremoto.bridge.MapManager

/** Map manager: tabs (available / installed).
 *  The "available" tab drills through the Geofabrik hierarchy:
 *  Kontinent → Land → Bundesland (wie OsmAnd). Antippen einer Region
 *  childless regions start the download; regions with children open
 *  the next level (breadcrumbs + back button). */
class MapsActivity : Activity() {

    private lateinit var mapManager: MapManager
    private val main = Handler(Looper.getMainLooper())

    private lateinit var availableList: ListView
    private lateinit var installedList: ListView
    private lateinit var crumb: TextView
    private lateinit var progress: ProgressBar
    private lateinit var dlLabel: TextView

    /** Drill-down stack: null = continents; each entry = a parent id. */
    private val drillStack = ArrayList<String?>()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_maps)

        mapManager = (application as LibreMotoApp).mapManager

        val tabs = findViewById<TabHost>(android.R.id.tabhost)
        tabs.setup()
        tabs.addTab(tabs.newTabSpec("avail").setIndicator(getString(R.string.tab_available))
            .setContent(R.id.tab_available))
        tabs.addTab(tabs.newTabSpec("inst").setIndicator(getString(R.string.tab_installed))
            .setContent(R.id.tab_installed))

        availableList = findViewById(R.id.available_list)
        installedList = findViewById(R.id.installed_list)
        crumb = findViewById(R.id.crumb)
        progress = findViewById(R.id.dl_progress)
        dlLabel = findViewById(R.id.dl_label)

        availableList.setOnItemClickListener { _, _, which, _ ->
            val r = shownRegions[which]
            if (mapManager.hasChildren(r.id)) {
                drillStack.add(r.id)
                refreshAvailable()
            } else if (mapManager.isRegionDownloaded(r.id)) {
                confirmDelete(r)
            } else {
                startDownload(r)
            }
        }
        installedList.setOnItemClickListener { _, _, which, _ ->
            val id = installedIds.getOrNull(which) ?: return@setOnItemClickListener
            val region = mapManager.region(id) ?: return@setOnItemClickListener
            showRegionActions(region, updateState[id] == false)
        }

        refresh()
    }

    override fun onBackPressed() {
        if (drillStack.isNotEmpty()) {
            drillStack.removeAt(drillStack.size - 1)
            refreshAvailable()
        } else {
            super.onBackPressed()
        }
    }

    private fun refresh() {
        if (mapManager.regionList().isEmpty()) {
            crumb.text = getString(R.string.index_loading)
            mapManager.fetchIndex { ok ->
                main.post {
                    if (ok) refresh() else crumb.text = getString(R.string.index_failed)
                }
            }
        } else {
            refreshAvailable()
        }
        refreshInstalled()
    }

    // ---- Tab: available (hierarchy) -----------------------------------

    private var shownRegions = listOf<GeofabrikRegion>()

    private fun refreshAvailable() {
        val parent = drillStack.lastOrNull()
        val children = mapManager.childRegions(parent)
        shownRegions = children
        // breadcrumb: Kontinente / Europe / Germany / …
        val path = ArrayList<String>()
        var p = parent
        while (p != null) {
            val r = mapManager.region(p)
            path.add(0, r?.name ?: p)
            p = r?.parentId
        }
        crumb.text = if (path.isEmpty()) getString(R.string.continents) else path.joinToString("  ›  ")

        val names = children.map { r ->
            val arrow = if (mapManager.hasChildren(r.id)) "  ›" else ""
            val state = if (mapManager.isRegionDownloaded(r.id)) "  ✓" else ""
            r.name + arrow + state
        }
        availableList.adapter = ArrayAdapter(this, android.R.layout.simple_list_item_1, names)
    }

    // ---- Tab: installed --------------------------------------------

    private var installedIds = listOf<String>()
    /** Update state per region id: true = up to date, false = UPDATE
     *  AVAILABLE, null = unknown (offline / no metadata). */
    private val updateState = HashMap<String, Boolean?>()

    /** One installed row: name, status text, status colour, size text. */
    private data class InstRow(
        val id: String, val name: String,
        val status: String, val statusColor: Int, val size: String,
    )

    private fun refreshInstalled() {
        installedIds = mapManager.installedIds().sorted()
        updateState.clear()
        renderInstalled()
        // async update check (one md5 fetch per region)
        mapManager.checkUpdates(
            { id, upToDate ->
                main.post { updateState[id] = upToDate; renderInstalled() }
            },
            { main.post { renderInstalled() } }
        )
    }

    private fun renderInstalled() {
        val rows = installedIds.map { id ->
            val r = mapManager.region(id)
            val sizeMb = mapManager.regionBytes(id) / 1048576.0
            val size = if (sizeMb >= 10.0) getString(R.string.size_mb_large, sizeMb) else getString(R.string.size_mb_small, sizeMb)
            val (status, color) = when (updateState[id]) {
                false -> getString(R.string.status_update_available) to 0xFFB25000.toInt()   // orange-brown
                true -> getString(R.string.status_up_to_date) to 0xFF2E7D32.toInt()             // green
                null -> getString(R.string.status_unknown) to 0xFF888888.toInt()                   // grey (checking)
            }
            InstRow(id, r?.name ?: id, status, color, size)
        }
        findViewById<TextView>(R.id.installed_hint).text =
            if (installedIds.isEmpty()) getString(R.string.installed_hint_none)
            else getString(R.string.installed_hint)
        installedList.adapter = object : android.widget.BaseAdapter() {
            override fun getCount() = rows.size
            override fun getItem(pos: Int) = rows[pos]
            override fun getItemId(pos: Int) = pos.toLong()
            override fun getView(pos: Int, cv: View?, parent: android.view.ViewGroup): View {
                val v = cv ?: layoutInflater.inflate(R.layout.row_installed, parent, false)
                val row = rows[pos]
                v.findViewById<TextView>(R.id.col_name).text = row.name
                val st = v.findViewById<TextView>(R.id.col_status)
                st.text = row.status
                st.setTextColor(row.statusColor)
                v.findViewById<TextView>(R.id.col_size).text = row.size
                // zebra striping: every second row slightly tinted
                v.setBackgroundColor(if (pos % 2 == 1) 0x11000000 else 0x00000000)
                return v
            }
        }
    }

    // ---- download / delete -------------------------------------------

    /** Tap on an installed region: action sheet with BOTH options —
     *  update (only if available) and delete (always). The delete path
     *  had become hard to find when an update was shown. */
    private fun showRegionActions(region: GeofabrikRegion, updateAvailable: Boolean) {
        val actions = ArrayList<String>()
        if (updateAvailable) actions.add(getString(R.string.action_update))
        actions.add(getString(R.string.action_delete))
        AlertDialog.Builder(this)
            .setTitle(region.name)
            .setItems(actions.toTypedArray()) { _, which ->
                when (actions[which]) {
                    getString(R.string.action_update) -> startDownload(region)
                    getString(R.string.action_delete) -> confirmDelete(region)
                }
            }
            .setNegativeButton(getString(R.string.btn_cancel), null)
            .show()
    }

    private fun startDownload(r: GeofabrikRegion) {
        progress.visibility = View.VISIBLE
        dlLabel.visibility = View.VISIBLE
        dlLabel.text = getString(R.string.download_start, r.name)
        progress.isIndeterminate = false
        mapManager.downloadRegion(r, { ok ->
            main.post {
                progress.visibility = View.GONE
                dlLabel.visibility = View.GONE
                if (ok) { refreshAvailable(); refreshInstalled() }
            }
        }) { phase, pct ->
            main.post {
                if (phase == 0) {
                    dlLabel.text = getString(R.string.download_progress, r.name, pct)
                    progress.isIndeterminate = false
                    progress.progress = pct
                } else {
                    dlLabel.text = getString(R.string.converting, r.name)
                    progress.isIndeterminate = true
                }
            }
        }
    }

    private fun confirmDelete(r: GeofabrikRegion) {
        AlertDialog.Builder(this)
            .setTitle(r.name)
            .setMessage(getString(R.string.confirm_delete))
            .setPositiveButton(getString(R.string.action_delete)) { _, _ ->
                mapManager.deleteRegion(r.id)
                main.postDelayed({ refreshAvailable(); refreshInstalled() }, 500)
            }
            .setNegativeButton(getString(R.string.btn_cancel), null)
            .show()
    }
}
