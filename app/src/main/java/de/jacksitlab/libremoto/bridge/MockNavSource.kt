/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * MockNavSource — deterministic fake route, so the display can be exercised
 * without OsmAnd / GPS. Mirrors the firmware demo scenarios (mt0..mt3).
 */
package de.jacksitlab.libremoto.bridge

import de.jacksitlab.libremoto.protocol.Point
import de.jacksitlab.libremoto.protocol.Maneuver
import de.jacksitlab.libremoto.protocol.MapFrame
import de.jacksitlab.libremoto.protocol.MapFrame.Companion.FLAG_SCALE_BAR
import de.jacksitlab.libremoto.protocol.MapFrame.Companion.FLAG_ROTATE
import de.jacksitlab.libremoto.protocol.MapFrame.Companion.FLAG_VEHICLE
import de.jacksitlab.libremoto.protocol.NavData
import de.jacksitlab.libremoto.protocol.NavType
import de.jacksitlab.libremoto.protocol.Segment
import de.jacksitlab.libremoto.protocol.SegmentType

class MockNavSource : NavSource {
    override val label: String = "Mock route"
    override var isNavigating: Boolean = true
        private set

    private var headingDeg = 0
    private var distM = 350
    private val maneuver = Maneuver.Straight
    private val roadName = "Demo Str."
    private var tick = 0

    override fun navData(): NavData = NavData(
        type = NavType.Nav,
        maneuver = maneuver,
        text = roadName,
        distM = distM,
        limitKmh = 50,
        mapOn = true,
    )

    /** Straight-ahead route + one cross road, exactly like firmware mt0, but
     * in the fixed "north up" frame the firmware rotates by heading. */
    override fun mapFrame(seq: Int): MapFrame {
        tick++; if (tick % 30 == 0) distM = if (distM <= 10) 350 else distM - 10
        val route = listOf(Point(0, 0), Point(0, -140), Point(0, -280))
        val road = listOf(Point(-200, -40), Point(200, -40))
        return MapFrame(
            seq = seq,
            flags = FLAG_ROTATE or FLAG_VEHICLE,
            headingDeg10 = headingDeg * 10,
            scalePxPerMeter100 = 20, // 0.20 px/m (demo scale)
            segments = listOf(
                Segment(SegmentType.Road, 12, road),
                Segment(SegmentType.Route, 10, route),
            ),
        )
    }

    override fun batteryPercent(): Int? = null

    override fun start() { isNavigating = true }
    override fun stop() { isNavigating = false }
}
