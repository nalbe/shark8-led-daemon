package com.bastet.ledgui

import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Live state from /data/local/tmp/led_status written by the daemon.
 * Format: ts / mode (idle|charge|notify|ring|voip|missed|alarm) / band
 * (lower|middle|upper|none) / pkg / color=r,g,b / engine.
 *
 * engine tells how the LED is ACTUALLY driven, which decides whether a
 * raw /sys/class/leds brightness read shows what the eye sees:
 *   breath chip-driven breathing/flash pulse - the driver pulses
 *          the channel by itself, brightness node just holds the peak -
 *          "live" reads are useless, the LED is visibly blinking
 *   wave   chip traveling-wave pattern (phase-staggered breathing); the
 *          brightness node holds the base color
 *   solid  static brightness (live read == the shown color)
 *   off    all dark
 * The GUI hides the misleading live value and shows a hint instead when
 * engine == breath or wave.
 */
data class LedStatus(
    val ts: Long = 0,
    val mode: String = "",
    val band: String = "",
    val pkg: String = "",
    val color: Triple<Int, Int, Int> = Triple(0, 0, 0),
    val engine: String = ""
) {
    val prettyTime: String
        get() = if (ts > 0) {
            SimpleDateFormat("HH:mm:ss", Locale.US).format(Date(ts * 1000))
        } else "-"

    /** True when some owner is actively driving the LED; mode=idle is the
     *  daemon's own word for a dark chip. */
    val isArmed: Boolean
        get() = mode.isNotEmpty() && mode != "!" &&
            (pkg.isNotEmpty() || (band.isNotEmpty() && band != "none"))
}

object LedStatusReader {

    /** Parse the daemon's led_status key=value dump (no shell roundtrip). */
    internal fun parseStatus(text: String): LedStatus {
        var ts = 0L
        var mode = ""
        var band = ""
        var pkg = ""
        var color = Triple(0, 0, 0)
        var engine = ""

        for (line in text.lineSequence()) {
            val i = line.indexOf('=')
            if (i <= 0) continue
            val k = line.substring(0, i)
            val v = line.substring(i + 1)
            when (k) {
                "ts" -> ts = v.toLongOrNull() ?: 0L
                "mode" -> mode = v
                "band" -> band = v
                "pkg" -> pkg = v
                "color" -> {
                    val parts = v.split(',')
                    if (parts.size == 3) {
                        color = Triple(
                            parts[0].trim().toIntOrNull() ?: 0,
                            parts[1].trim().toIntOrNull() ?: 0,
                            parts[2].trim().toIntOrNull() ?: 0
                        )
                    }
                }
                "engine" -> engine = v.trim()
            }
        }
        return LedStatus(ts, mode, band, pkg, color, engine)
    }
}