package de.jacksitlab.libremoto.bridge

import android.content.Context
import android.content.res.Resources
import de.jacksitlab.libremoto.R

enum class SourceKind { Mock, OsmAnd }

object NavSourceFactory {
    fun create(kind: SourceKind, ctx: Context, res: Resources, log: (String) -> Unit): NavSource = when (kind) {
        SourceKind.Mock -> MockNavSource()
        SourceKind.OsmAnd -> OsmAndNavSource(ctx, res) { line -> log(res.getString(R.string.log_bridge_prefix, line)) }
    }
}
