/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * NavSource — the data contract that the BLE sender consumes.
 * Two implementations ship:
 *   MockNavSource   — deterministic canned route (device bring-up, no osmAnd)
 *   OsmAndNavSource — reads live nav data from OsmAnd via the official AIDL API
 * Adding a third source (Organic Maps, Navit, …) needs no BLE changes.
 */
package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.MapFrame
import de.jacksitlab.libremoto.protocol.NavData

interface NavSource {
    /** Human readable identity, shown in the UI + logs. */
    val label: String

    /** true while the source believes a navigation session is active. */
    val isNavigating: Boolean

    /** Current turn instruction (null when no nav active). */
    fun navData(): NavData

    /** Current vector map frame (route + nearby roads). May return a cheap
     * "route only" frame when the full window isn't ready. */
    fun mapFrame(seq: Int): MapFrame

    /** Battery % of the phone, if cheaply readable. */
    fun batteryPercent(): Int?

    fun start()
    fun stop()
}
