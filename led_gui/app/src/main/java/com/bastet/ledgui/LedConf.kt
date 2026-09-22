package com.bastet.ledgui

import java.util.Locale

/**
 * led.conf model + text parser/renderer (v4 - per-event chip renderer).
 *
 * The daemon hot-reloads the file by mtime on the next event, so a save
 * here takes effect without a restart. The file must stay ASCII-only
 * (the daemon parser and the shell toolchain are byte-oriented).
 *
 * v4 ownership: every setting lives in the section that owns it. The
 * event base section keeps only its own common keys (thresholds, cap,
 * mode, color); the chip sections own their timing:
 *   [sec]           mode=off|solid|breath|wave (+ event common keys)
 *   [sec.solid]     cur=r,g,b          0..15 per-channel current
 *   [sec.breath]    repeat, cur_r/cur_g/cur_b, rise/hold/fall/offt,
 *                   sync=0|1 (LCFG0.SYNC master-channel lock)
 *   [sec.wave]      t0=r,g,b phase offsets (ms), repeat, rise/hold/fall/offt,
 *                   sync=0|1 (LCFG0.SYNC master-channel lock)
 * There is NO fallback anywhere: a chip section carries its own timing,
 * nothing is inherited from the base section.
 * [led] carries only daemon/chip globals: logging, imax.
 */
data class Rule(val pkg: String, val r: Int, val g: Int, val b: Int)

/** Per-event chip renderer (v4): how ONE event (or charge band)
 *  animates the AW2033. Every chip section owns its own timing; there
 *  is no base-section timing and no fallback. */
data class Render(
    var mode: String = "breath",
    var solidCur: Triple<Int, Int, Int> = Triple(15, 15, 15),
    var brRepeat: Int = 0,
    var brCur: Triple<Int, Int, Int> = Triple(15, 15, 15),
    var brRise: Int = 500,
    var brHold: Int = 100,
    var brFall: Int = 500,
    var brOfft: Int = 1200,
    var brSync: Boolean = false,
    var waveT0: Triple<Int, Int, Int> = Triple(0, 1300, 2600),
    var waveRepeat: Int = 0,
    var waveRise: Int = 500,
    var waveHold: Int = 100,
    var waveFall: Int = 500,
    var waveOfft: Int = 1200,
    var waveSync: Boolean = false
)

/** charge bands breathe at 700/100/700/900, calls at 800/200/800/400 */
private fun Render.chargeTiming() = apply {
    brRise = 700; brHold = 100; brFall = 700; brOfft = 900
    waveRise = 700; waveHold = 100; waveFall = 700; waveOfft = 900
}

private fun Render.callTiming() = apply {
    brRise = 800; brHold = 200; brFall = 800; brOfft = 400
    waveRise = 800; waveHold = 200; waveFall = 800; waveOfft = 400
}

data class LedConf(
    val suppress: MutableList<String> = mutableListOf(),
    val rules: MutableList<Rule> = mutableListOf(),
    var firstThreshold: Int = 90,
    var secondThreshold: Int = 95,
    var lowerColor: Triple<Int, Int, Int> = Triple(255, 32, 32),
    var middleColor: Triple<Int, Int, Int> = Triple(255, 127, 32),
    var upperColor: Triple<Int, Int, Int> = Triple(64, 255, 32),
    var notifMaxSec: Long = 0,
    var notifyScreenDelayMs: Long = 60000,
    var notifyColor: Triple<Int, Int, Int> = Triple(255, 150, 150),
    var notifAppMaxSec: Long = 0,
    var notifAppScreenDelayMs: Long = 60000,
    var ringCapSec: Long = 0,
    var ringColor: Triple<Int, Int, Int> = Triple(255, 255, 255),
    var voipMaxSec: Long = 300,
    var voipColor: Triple<Int, Int, Int> = Triple(255, 255, 255),
    var logging: Boolean = true,
    var imax: Int = 30,
    var chargeLower: Render = Render().chargeTiming(),
    var chargeMiddle: Render = Render().chargeTiming(),
    var chargeUpper: Render = Render().chargeTiming(),
    var notifyRender: Render = Render(),
    var notifyAppRender: Render = Render(),
    var missedRender: Render = Render(),
    var alarmRender: Render = Render(),
    var ringRender: Render = Render(mode = "wave").callTiming(),
    var voipRender: Render = Render(mode = "wave").callTiming(),
    var alarmColor: Triple<Int, Int, Int> = Triple(255, 155, 0),
    var alarmMaxSec: Long = 0,
    var missedColor: Triple<Int, Int, Int> = Triple(255, 0, 0),
    var missedMaxSec: Long = 0,
    /** [preview] calibration: apparent per-LED brightness vs green=100
     *  and the perception-curve exponent. Only the GUI picture uses these
     *  (the picker swatch + the Info live swatch); the daemon ignores them. */
    var pvwR: Double = 50.0,
    var pvwG: Double = 100.0,
    var pvwB: Double = 80.0,
    var pvwGamma: Double = 2.2
) {

    companion object {
        const val CONF_PATH = "/data/adb/modules/led_hal_root/led.conf"
        val VALID_MODES = setOf("off", "solid", "breath", "wave")

        /** App-wide settings cache: every config tab shares ONE LedConf so
         *  switching tabs never re-fetches the device file. Warm from the
         *  Info tab's live poll, filled by the first page reload, refreshed
         *  after every successful save. */
        @Volatile private var shared: LedConf? = null

        fun cached(): LedConf? = shared
        fun updateCache(c: LedConf) { shared = c }

        private fun readConf(): LedConf? {
            val r = Su.run("cat $CONF_PATH 2>/dev/null")
            if (!r.ok || r.out.isBlank()) return null
            val conf = LedConf()
            conf.parse(r.out)
            return conf
        }

        fun load(): LedConf = readConf() ?: LedConf()

        fun loadOrNull(): LedConf? = readConf()

        private fun clampColor(v: Int) = v.coerceIn(0, 255)
    }

    private fun renderBySec(sec: String): Render? = when (sec) {
        "notify" -> notifyRender
        "notify.app" -> notifyAppRender
        "charge.lower" -> chargeLower
        "charge.middle" -> chargeMiddle
        "charge.upper" -> chargeUpper
        "missed" -> missedRender
        "alarm" -> alarmRender
        "ring" -> ringRender
        "voip" -> voipRender
        else -> null
    }

    private fun parseMode(v: String): String? = if (v.trim() in VALID_MODES) v.trim() else null

    private fun parseRenderSection(r: Render, kind: String, k: String, v: String) {
        when (kind) {
            "solid" -> if (k == "cur") parseTriple(v)?.let {
                r.solidCur = Triple(
                    it.first.coerceIn(0, 15), it.second.coerceIn(0, 15), it.third.coerceIn(0, 15)
                )
            }
            "breath" -> when (k) {
                "repeat" -> v.toIntOrNull()?.let { r.brRepeat = it.coerceIn(0, 15) }
                "cur_r" -> v.toIntOrNull()?.let { r.brCur = Triple(it.coerceIn(0, 15), r.brCur.second, r.brCur.third) }
                "cur_g" -> v.toIntOrNull()?.let { r.brCur = Triple(r.brCur.first, it.coerceIn(0, 15), r.brCur.third) }
                "cur_b" -> v.toIntOrNull()?.let { r.brCur = Triple(r.brCur.first, r.brCur.second, it.coerceIn(0, 15)) }
                "rise" -> v.toIntOrNull()?.let { r.brRise = it }
                "hold" -> v.toIntOrNull()?.let { r.brHold = it }
                "fall" -> v.toIntOrNull()?.let { r.brFall = it }
                "offt" -> v.toIntOrNull()?.let { r.brOfft = it }
                "sync" -> v.toIntOrNull()?.let { r.brSync = it != 0 }
            }
            "wave" -> when (k) {
                "t0" -> parseTriple(v)?.let { r.waveT0 = Triple(
                    it.first.coerceAtLeast(0), it.second.coerceAtLeast(0), it.third.coerceAtLeast(0)
                ) }
                "repeat" -> v.toIntOrNull()?.let { r.waveRepeat = it.coerceIn(0, 15) }
                "rise" -> v.toIntOrNull()?.let { r.waveRise = it }
                "hold" -> v.toIntOrNull()?.let { r.waveHold = it }
                "fall" -> v.toIntOrNull()?.let { r.waveFall = it }
                "offt" -> v.toIntOrNull()?.let { r.waveOfft = it }
                "sync" -> v.toIntOrNull()?.let { r.waveSync = it != 0 }
            }
        }
    }

    fun parse(text: String) {
        var section = ""
        for (rawLine in text.lineSequence()) {
            val line = rawLine.trim()
            if (line.isEmpty() || line.startsWith("#") || line.startsWith(";")) continue
            if (line.startsWith("[")) {
                val end = line.indexOf(']')
                section = if (end > 1) line.substring(1, end) else ""
                continue
            }
            when (section) {
                "suppress" -> {
                    if (line.isNotBlank() && !suppress.contains(line)) suppress.add(line)
                }
                "rules" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val pkg = line.substring(0, i).trim()
                    val rgb = parseRgb(line.substring(i + 1)) ?: continue
                    rules.removeAll { it.pkg == pkg }
                    rules.add(Rule(pkg, rgb.first, rgb.second, rgb.third))
                }
                "charge" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "first_threshold" -> v.toIntOrNull()?.let { firstThreshold = it }
                        "second_threshold" -> v.toIntOrNull()?.let { secondThreshold = it }
                    }
                }
                "charge.lower", "charge.middle", "charge.upper" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    val r = renderBySec(section) ?: continue
                    when (k) {
                        "mode" -> parseMode(v)?.let { r.mode = it }
                        "color" -> parseRgb(v)?.let {
                            if (section == "charge.lower") lowerColor = it
                            else if (section == "charge.middle") middleColor = it
                            else upperColor = it
                        }
                    }
                }
                "notify.app" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "notif_max_sec" -> v.toLongOrNull()?.let { notifAppMaxSec = it }
                        "notify_screen_delay_ms" -> v.toLongOrNull()?.let { notifAppScreenDelayMs = it }
                        "mode" -> parseMode(v)?.let { notifyAppRender.mode = it }
                    }
                }
                "notify" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "notif_max_sec" -> v.toLongOrNull()?.let { notifMaxSec = it }
                        "notify_screen_delay_ms" -> v.toLongOrNull()?.let { notifyScreenDelayMs = it }
                        "default_color" -> parseRgb(v)?.let { notifyColor = it }
                        "mode" -> parseMode(v)?.let { notifyRender.mode = it }
                    }
                }
                "ring" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "max_sec" -> v.toLongOrNull()?.let { ringCapSec = it }
                        "color" -> parseRgb(v)?.let { ringColor = it }
                        "mode" -> parseMode(v)?.let { ringRender.mode = it }
                    }
                }
                "voip" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "max_sec" -> v.toLongOrNull()?.let { voipMaxSec = it }
                        "color" -> parseRgb(v)?.let { voipColor = it }
                        "mode" -> parseMode(v)?.let { voipRender.mode = it }
                    }
                }
                "missed" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "color" -> parseRgb(v)?.let { missedColor = it }
                        "max_sec" -> v.toLongOrNull()?.let { missedMaxSec = it }
                        "mode" -> parseMode(v)?.let { missedRender.mode = it }
                    }
                }
                "alarm" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "color" -> parseRgb(v)?.let { alarmColor = it }
                        "max_sec" -> v.toLongOrNull()?.let { alarmMaxSec = it }
                        "mode" -> parseMode(v)?.let { alarmRender.mode = it }
                    }
                }
"led" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "logging" -> v.toIntOrNull()?.let { logging = it != 0 }
                        "imax" -> v.toIntOrNull()?.let { imax = it.coerceIn(1, 40) }
                    }
                }
                "preview" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim().toDoubleOrNull()?.let {
                        if (it > 0) it else null
                    } ?: continue
                    when (k) {
                        "r" -> pvwR = v
                        "g" -> pvwG = v
                        "b" -> pvwB = v
                        "gamma" -> pvwGamma = v.coerceIn(1.0, 4.0)
                    }
                }
                else -> {
                    val dot = section.lastIndexOf('.')
                    if (dot > 0) {
                        val r = renderBySec(section.substring(0, dot))
                        if (r != null) {
                            val i = line.indexOf('=')
                            if (i > 0) parseRenderSection(
                                r, section.substring(dot + 1),
                                line.substring(0, i).trim(), line.substring(i + 1).trim()
                            )
                        }
                    }
                }
            }
        }
        if (firstThreshold > secondThreshold) {
            val t = firstThreshold; firstThreshold = secondThreshold; secondThreshold = t
        }
        LedSim.cal = LedSim.Cal(pvwR / 100.0, pvwG / 100.0, pvwB / 100.0, pvwGamma)
    }

    fun render(): String {
        val sb = StringBuilder()
        sb.append("# led_hal_root runtime config - generated by LED GUI (v4)\n")
        sb.append("# ASCII only. Save applies on the next daemon event (mtime reload).\n")
        sb.append("#\n")
        sb.append("# [suppress] one package per line - never lights the LED\n")
        sb.append("# [rules]    pkg=r,g,b  (0-255 per channel)\n")
        sb.append("# [charge]   thresholds ONLY; each band owns color/timing\n")
        sb.append("# [notify]   shared behavior: notif_max_sec, default color\n")
        sb.append("# [ring]     incoming call rainbow: max_sec, base color\n")
        sb.append("# [voip]     messenger call rainbow: max_sec, base color\n")
        sb.append("# [led]      daemon logging + global chip Imax\n")
        sb.append("# [missed]   missed-call indication: color, max_sec\n")
        sb.append("# [alarm]    alarm clock indication: color, max_sec\n")
        sb.append("#\n")
        sb.append("# v4 renderer: every event owns [sec] mode and the\n")
        sb.append("# [sec.solid]/[sec.breath]/[sec.wave] chip sections. Each chip\n")
        sb.append("# section owns its own rise/hold/fall/offt - no base-section\n")
        sb.append("# timing, no fallback.\n")
        sb.append("\n[suppress]\n")
        for (p in suppress) sb.append(p).append('\n')
        sb.append("\n[rules]\n")
        for (rule in rules) {
            sb.append(rule.pkg).append('=').append(rule.r).append(',')
                .append(rule.g).append(',').append(rule.b).append('\n')
        }
        sb.append("\n[charge]\n")
        sb.append("first_threshold=").append(firstThreshold).append('\n')
        sb.append("second_threshold=").append(secondThreshold).append('\n')
        appendBandRender(sb, "lower", chargeLower, lowerColor)
        appendBandRender(sb, "middle", chargeMiddle, middleColor)
        appendBandRender(sb, "upper", chargeUpper, upperColor)
        sb.append("\n[notify]\n")
        sb.append("notif_max_sec=").append(notifMaxSec).append('\n')
        sb.append("notify_screen_delay_ms=").append(notifyScreenDelayMs).append('\n')
        sb.append("default_color=").append(rgb(notifyColor)).append('\n')
        appendMode(sb, "notify", notifyRender)
        sb.append("\n[notify.app]\n")
        sb.append("notif_max_sec=").append(notifAppMaxSec).append('\n')
        sb.append("notify_screen_delay_ms=").append(notifAppScreenDelayMs).append('\n')
        appendMode(sb, "notify.app", notifyAppRender)
        appendRenderChips(sb, "notify.app", notifyAppRender)
        sb.append("\n[ring]\n")
        sb.append("max_sec=").append(ringCapSec).append('\n')
        sb.append("color=").append(rgb(ringColor)).append('\n')
        appendMode(sb, "ring", ringRender)
        sb.append("\n[voip]\n")
        sb.append("max_sec=").append(voipMaxSec).append('\n')
        sb.append("color=").append(rgb(voipColor)).append('\n')
        appendMode(sb, "voip", voipRender)
        sb.append("\n[led]\n")
        sb.append("logging=").append(if (logging) 1 else 0).append('\n')
        sb.append("imax=").append(imax).append('\n')
        sb.append("\n# GUI preview calibration (daemon ignores): how bright each\n")
        sb.append("# LED looks vs the picker value. green=100 is the reference;\n")
        sb.append("# gamma bends the curve toward your eye. Tune to your LED set.\n")
        sb.append("[preview]\n")
        sb.append("r=").append(fmtW(pvwR)).append('\n')
        sb.append("g=").append(fmtW(pvwG)).append('\n')
        sb.append("b=").append(fmtW(pvwB)).append('\n')
        sb.append("gamma=").append(String.format(Locale.US, "%.1f", pvwGamma)).append('\n')
        appendRenderChips(sb, "notify", notifyRender)
        appendRenderChips(sb, "ring", ringRender)
        appendRenderChips(sb, "voip", voipRender)
        sb.append("\n[missed]\n")
        sb.append("color=").append(rgb(missedColor)).append('\n')
        sb.append("max_sec=").append(missedMaxSec).append('\n')
        appendMode(sb, "missed", missedRender)
        appendRenderChips(sb, "missed", missedRender)
        sb.append("\n[alarm]\n")
        sb.append("color=").append(rgb(alarmColor)).append('\n')
        sb.append("max_sec=").append(alarmMaxSec).append('\n')
        appendMode(sb, "alarm", alarmRender)
        appendRenderChips(sb, "alarm", alarmRender)
        return sb.toString()
    }

    /** [sec] mode=... */
    private fun appendMode(sb: StringBuilder, sec: String, r: Render) {
        sb.append("mode=").append(r.mode).append('\n')
    }

    /** one charge band's OWN renderer: [charge.<band>] mode + color
     *  header + the three chip sections (each chip owns its timing). */
    private fun appendBandRender(sb: StringBuilder, band: String, r: Render, color: Triple<Int, Int, Int>) {
        sb.append("\n[charge.").append(band).append("]\n")
        appendMode(sb, "charge.$band", r)
        sb.append("color=").append(rgb(color)).append('\n')
        appendRenderChips(sb, "charge.$band", r)
    }

    private fun appendRenderChips(sb: StringBuilder, sec: String, r: Render) {
        sb.append("\n[").append(sec).append(".solid]\n")
        sb.append("cur=").append(tripleClamp(r.solidCur, 0, 15)).append('\n')
        sb.append("\n[").append(sec).append(".breath]\n")
        sb.append("sync=").append(if (r.brSync) 1 else 0).append('\n')
        sb.append("repeat=").append(r.brRepeat.coerceIn(0, 15)).append('\n')
        sb.append("cur_r=").append(r.brCur.first.coerceIn(0, 15)).append('\n')
        sb.append("cur_g=").append(r.brCur.second.coerceIn(0, 15)).append('\n')
        sb.append("cur_b=").append(r.brCur.third.coerceIn(0, 15)).append('\n')
        sb.append("rise=").append(r.brRise).append('\n')
        sb.append("hold=").append(r.brHold).append('\n')
        sb.append("fall=").append(r.brFall).append('\n')
        sb.append("offt=").append(r.brOfft).append('\n')
        sb.append("\n[").append(sec).append(".wave]\n")
        sb.append("sync=").append(if (r.waveSync) 1 else 0).append('\n')
        sb.append("t0=").append(r.waveT0.first.coerceAtLeast(0)).append(',')
            .append(r.waveT0.second.coerceAtLeast(0)).append(',')
            .append(r.waveT0.third.coerceAtLeast(0)).append('\n')
        sb.append("repeat=").append(r.waveRepeat.coerceIn(0, 15)).append('\n')
        sb.append("rise=").append(r.waveRise).append('\n')
        sb.append("hold=").append(r.waveHold).append('\n')
        sb.append("fall=").append(r.waveFall).append('\n')
        sb.append("offt=").append(r.waveOfft).append('\n')
    }

    fun save(): Su.Result = Su.writeFile(CONF_PATH, render())

    private fun rgb(c: Triple<Int, Int, Int>): String =
        "${clampColor(c.first)},${clampColor(c.second)},${clampColor(c.third)}"

    private fun fmtW(w: Double): String =
        String.format(Locale.US, "%.0f", w.coerceAtLeast(1.0))

    private fun tripleClamp(c: Triple<Int, Int, Int>, lo: Int, hi: Int): String =
        "${c.first.coerceIn(lo, hi)},${c.second.coerceIn(lo, hi)},${c.third.coerceIn(lo, hi)}"

    private fun parseRgb(v: String): Triple<Int, Int, Int>? {
        val t = parseTriple(v) ?: return null
        if (t.first !in 0..255 || t.second !in 0..255 || t.third !in 0..255) return null
        return t
    }

    private fun parseTriple(v: String): Triple<Int, Int, Int>? {
        val parts = v.split(',')
        if (parts.size != 3) return null
        val r = parts[0].trim().toIntOrNull() ?: return null
        val g = parts[1].trim().toIntOrNull() ?: return null
        val b = parts[2].trim().toIntOrNull() ?: return null
        return Triple(r, g, b)
    }
}