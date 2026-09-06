package de.jacksitlab.libremoto.protocol

/** A 2-integer value pair `(x, y)`. Kotlin's stdlib `Pair` collides on
 * property names (`.first`/`.second`), so we use a dedicated name.
 * Use for screen-relative coordinates (vehicle at (0,0), +x east, +y south). */
data class Point(val x: Int = 0, val y: Int = 0)
