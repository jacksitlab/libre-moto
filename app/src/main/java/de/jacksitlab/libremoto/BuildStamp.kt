package de.jacksitlab.libremoto

import android.content.Context

/** UI feature stamp: a hand-maintained marker of which UI generation
 *  is installed. Bumped on every user-visible UI change so a stale
 *  install is instantly recognisable on the start screen — independent
 *  of git (the user makes local commits; git describe is meaningless
 *  across machines).
 *
 *  History:
 *    1 — original UI (all buttons, radio group, inline log)
 *    2 — minimalist UI + invisible overflow menu (BROKEN — no anchor)
 *    3 — maps/log as borderless buttons on the start screen
 *    4 — + build stamp line at the bottom
 *    5 — maps/log as FILLED buttons 56 dp (borderless vanished on Samsung)
 *    6 — overflow menu (drei Punkte) via transparent Toolbar
 *    7 — maps view: tabs (verfügbar/installiert) + Geofabrik hierarchy
 *    8 — menu as explicit ⋮ button + PopupMenu (Toolbar failed on Samsung)
 *    9 — maps: sizes on installed tab + Geofabrik md5 update check
 *   10 — installed tab as table: name/status/size columns, zebra rows,
 *        tap → action sheet (update/delete)
 */
object BuildStamp {
    const val UI_VERSION = 10
    fun id(ctx: Context): String = try {
        val pkg = ctx.packageManager.getPackageInfo(ctx.packageName, 0)
        "UI v$UI_VERSION · ${pkg.versionName}"
    } catch (_: Exception) {
        "UI v$UI_VERSION"
    }
}
