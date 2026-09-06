/* LibreMoto companion app.
 * Copyright (C) 2026 jacksitlab. GPLv3.
 *
 * NavData — JSON, protocol.md §2.
 * Hand-rolled encoder (no org.json dependency): small, deterministic and
 * unit-testable on a plain JVM. Field set follows protocol v1 exactly.
 */
package de.jacksitlab.libremoto.protocol

/** Maneuver strings understood by the firmware (protocol.md §2). */
enum class Maneuver(val wire: String) {
    Left("left"),
    Right("right"),
    Straight("straight"),
    UTurn("uturn"),
    Merge("merge"),
    SlightLeft("slight_left"),
    SlightRight("slight_right"),
}

/** Nav types understood by the firmware (protocol.md §2). */
enum class NavType(val wire: String) {
    Nav("nav"),
    Idle("idle"),
    Reroute("reroute"),
    Arrived("arrived"),
    Beep("beep"),
}

/** One NavData payload (protocol.md §2). All fields optional per protocol. */
data class NavData(
    val type: NavType,
    val maneuver: Maneuver? = null,
    val text: String? = null,
    val distM: Int? = null,
    val limitKmh: Int? = null,
    val battPct: Int? = null,
    val mapOn: Boolean? = null,
    val tsMs: Long = System.currentTimeMillis(),
) {
    fun toJson(): String {
        val sb = StringBuilder(96)
        sb.append("{\"v\":1,\"t\":\"").append(type.wire).append('"')
        if (maneuver != null) sb.append(",\"maneuver\":\"").append(maneuver.wire).append('"')
        text?.let {
            sb.append(",\"text\":\"").append(escaped(it.take(24))).append('"')
        }
        distM?.let { sb.append(",\"dist_m\":").append(it) }
        limitKmh?.let { sb.append(",\"limit_kmh\":").append(it) }
        battPct?.let { sb.append(",\"batt_pct\":").append(it) }
        mapOn?.let { sb.append(",\"map_on\":").append(it) }
        sb.append(",\"ts\":").append(tsMs).append('}')
        return sb.toString()
    }

    private fun escaped(s: String): String = buildString {
        s.forEach { c ->
            when (c) {
                '\\' -> append("\\\\")
                '"' -> append("\\\"")
                '\n' -> append("\\n")
                '\r' -> append("\\r")
                '\t' -> append("\\t")
                else -> if (c < 0x20.toChar()) append("\\u%04x".format(c.code)) else append(c)
            }
        }
    }

    companion object {
        val BEAT = NavData(NavType.Beep)
    }
}
