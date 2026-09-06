package de.jacksitlab.libremoto.bridge

import android.content.Context

/** Which navigation source is live. */
enum class SourceKind { Mock, OsmAnd }

object NavSourceFactory {
    fun create(kind: SourceKind, ctx: Context, log: (String) -> Unit): NavSource = when (kind) {
        SourceKind.Mock -> MockNavSource()
        SourceKind.OsmAnd -> OsmAndNavSource(ctx) { line -> log("bridge: $line") }
    }
}
