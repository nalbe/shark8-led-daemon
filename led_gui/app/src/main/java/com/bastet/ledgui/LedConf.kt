package com.bastet.ledgui

import java.util.Locale

/* File overview - led.conf model + text parser/renderer (v6).
 *
 * The daemon hot-reloads the file by mtime on the next event, so a save
 * here takes effect without a restart. The file must stay ASCII-only
 * (the daemon parser and the shell toolchain are byte-oriented).
 *
 * A SAVE IS A PATCH, not a rewrite: the module ships led.conf with its
 * comments as the documentation of the format, and the GUI edits values
 * into the file it finds on the device (ConfWriter). Comments, blank
 * lines, the section order and every key this model has no vocabulary
 * for therefore survive a save untouched, a key the file is missing is
 * added, and only the two list sections ([suppress], [rules]) are kept in
 * step with the model line for line, because there the list IS the value.
 *
 * v6 ownership: every setting lives in the section that owns it, and one
 * render= line carries that section's whole renderer:
 *   render=<r,g,b>,<mode>,<cur_r,g,b>,pattern,<sync>,<repeat>,
 *          <cur_r,g,b>,<rise>,<hold>,<fall>,<offt>,<t0_r,g,b>
 * The colour at the head of that line is the section's own colour and the
 * only place one is written (for [notify] it is the colour apps without a
 * [rules] entry paint in). mode is off|solid|breath|wave, then the solid
 * current, the literal "pattern" marker and the preset shared by breath
 * and wave: sync, repeat, pattern current, timing and the wave phase
 * offsets t0. t0 is used only by wave; all other preset fields are used
 * by both modes. The event base section keeps its own common keys
 * (thresholds, cap); [led] carries only daemon/chip globals: logging,
 * imax.
 *
 * [rules] entries carry that app's own cap and preset, which is exactly
 * the render line without its leading colour:
 *   pkg = r,g,b , max_sec , <the 17 render fields after the colour>
 * All 17 must be there, like in the render line: the GUI reads a short
 * tail as colour-only (what the daemon does with it) and says so, so a
 * damaged file never silently grows a preset on the next save. A
 * colour-only line resolves to the shared [notify] preset.
 * The basin window (notify_screen_delay_ms) is NOT per-app: it stays a
 * single shared [notify] value.
 *
 * [priority] carries the pool ranking (bigger wins, one key per kind).
 * The daemon compares nothing hardcoded, so these values ARE the
 * arbitration; the defaults below mirror RANK_* in led_hal_root/chgd.h.
 * All six kinds are ordinary entries of one pool, the charge band included
 * (it just has the lowest default rank); no test button owns a key.
 *
 * The Priority tab edits the section as an ORDERED LIST, never as typed
 * numbers: the ranks are derived from the row order (top row wins, step
 * 10) and on load the rows are sorted by the numbers the file really has.
 * An unknown key or a non-numeric value is IGNORED and collected into
 * [warnings]: a save leaves that line alone (it patches values into the
 * file), but the GUI cannot show it and must not pretend it read it. The
 * daemon is stricter: the same broken key DROPS the effect (channel.c),
 * so the GUI keeps the last good number instead of writing one the daemon
 * would reject.
 */
/**
 * One [rules] entry. The colour is always own; cap and the preset are own
 * once the line carries them (custom=true, and such a line is always
 * written back whole). A colour-only line (custom=false) resolves to the
 * shared [notify] preset. */
class Rule(
    val pkg: String,
    var r: Int,
    var g: Int,
    var b: Int,
    /** own cap (max_sec, 0 = unlimited) - written only when custom */
    var maxSec: Long = 0,
    var render: Render = Render(),
    var custom: Boolean = false
) {
    constructor(pkg: String, r: Int, g: Int, b: Int) :
        this(pkg, r, g, b, 0, Render(), false)
}

/** Per-event chip renderer: how ONE event (or charge band)
 *  animates the AW2033. Breath and wave share one preset; the phase
 *  offsets waveT0 are the only wave-specific part. */
data class Render(
    var mode: String = "breath",
    var solidCur: Triple<Int, Int, Int> = Triple(15, 15, 15),
    var patternRepeat: Int = 0,
    var patternCur: Triple<Int, Int, Int> = Triple(15, 15, 15),
    var patternRise: Int = 500,
    var patternHold: Int = 100,
    var patternFall: Int = 500,
    var patternOfft: Int = 1200,
    var patternSync: Boolean = false,
    var waveT0: Triple<Int, Int, Int> = Triple(0, 1300, 2600)
)

/** Defaults mirror module/led.conf, the canonical template that is always
 *  overwritten on install. They are the starting point for a NEW config:
 *  the daemon drops any key the file does not carry, so the GUI and the
 *  template must agree on every one of these or the first save would
 *  quietly change a light.
 *
 *  Charge bands breathe at 700/100/700/900, calls at 800/200/800/400. */
private fun Render.chargeTiming() = apply {
    patternRise = 700; patternHold = 100; patternFall = 700; patternOfft = 900
}

private fun Render.callTiming() = apply {
    patternRise = 800; patternHold = 200; patternFall = 800; patternOfft = 400
}

data class LedConf(
    val suppress: MutableList<String> = mutableListOf(),
    val rules: MutableList<Rule> = mutableListOf(),
    /** [priority] channel ranking: effect -> rank, bigger wins. Mirrors
     *  RANK_* in the daemon; a missing key keeps the default, equal ranks
     *  fall back to that same default order, and a key we do not know is
     *  reported in [warnings] instead of being taken over. */
    val priority: MutableMap<String, Int> = mutableMapOf(),
    var firstThreshold: Int = 70,
    var secondThreshold: Int = 95,
    var lowerColor: Triple<Int, Int, Int> = Triple(128, 8, 8),
    var middleColor: Triple<Int, Int, Int> = Triple(145, 56, 0),
    var upperColor: Triple<Int, Int, Int> = Triple(64, 128, 32),
    var notifMaxSec: Long = 0,
    var notifyScreenDelayMs: Long = 60000,
    var notifyColor: Triple<Int, Int, Int> = Triple(255, 255, 255),
    var ringCapSec: Long = 300,
    var ringColor: Triple<Int, Int, Int> = Triple(255, 255, 255),
    var voipMaxSec: Long = 300,
    var voipColor: Triple<Int, Int, Int> = Triple(255, 255, 255),
    var logging: Boolean = true,
    var imax: Int = 30,
    var chargeLower: Render = Render(
        mode = "solid", patternSync = true,
        patternCur = Triple(4, 0, 0)
    ).chargeTiming(),
    var chargeMiddle: Render = Render(
        mode = "solid", patternSync = true,
        patternCur = Triple(5, 1, 0)
    ).chargeTiming(),
    var chargeUpper: Render = Render(
        mode = "solid", patternSync = false,
        patternCur = Triple(15, 15, 15)
    ).chargeTiming(),
    var notifyRender: Render = Render(
        mode = "breath", patternSync = true,
        patternCur = Triple(15, 11, 11)
    ),
    var missedRender: Render = Render(
        mode = "breath", patternSync = false,
        patternCur = Triple(15, 15, 15)
    ),
    var alarmRender: Render = Render(
        mode = "breath", patternSync = false,
        patternCur = Triple(15, 15, 15)
    ),
    var ringRender: Render = Render(
        mode = "wave", patternSync = false,
        patternCur = Triple(15, 15, 15)
    ).callTiming(),
    var voipRender: Render = Render(
        mode = "wave", patternSync = false,
        patternCur = Triple(15, 15, 15)
    ).callTiming(),
    var alarmColor: Triple<Int, Int, Int> = Triple(255, 155, 0),
    var alarmMaxSec: Long = 0,
    var missedColor: Triple<Int, Int, Int> = Triple(0, 255, 255),
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

        /** Tokens of a render line that follow its colour: mode, the solid
         *  current, the literal pattern marker and the preset breath and
         *  wave share. The [rules] tail is the same fields after the cap. */
        const val RENDER_FIELDS = 17

        /** Every section this model owns. An unknown section is reported and
         *  left out of the model, so a save leaves the file's copy of it
         *  exactly as the operator wrote it. */
        val KNOWN_SECTIONS = setOf(
            "priority", "suppress", "rules", "charge",
            "charge.lower", "charge.middle", "charge.upper",
            "notify", "ring", "voip", "missed", "alarm", "led", "preview"
        )

        /** The AW2033 current steps the chip driver can program; [imax] must
         *  be one of these or the daemon refuses to power the rails. */
        val VALID_IMAX = setOf(5, 10, 15, 30)

        /** The whole [priority] vocabulary: render order = rank order.
         *  Values mirror RANK_* in led_hal_root/chgd.h. */
        val PRIORITY_EFFECTS = listOf(
            "ring" to 50, "voip" to 40, "alarm" to 30,
            "missed" to 20, "notify" to 10, "charge" to 0
        )

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

        /** The device file, or null when it cannot be read. A caller that needs a
         *  config MUST handle null (the daemon would run with nothing), so
         *  there is deliberately no load() that invents one. */
        fun loadOrNull(): LedConf? = readConf()

        private fun clampColor(v: Int) = v.coerceIn(0, 255)
    }

    /** Parse complaints, shown by the page that owns the section. The GUI
     *  has no log of its own, so an unreadable line must not be swallowed:
     *  the daemon logs the same ones and falls back to the default rank. */
    val warnings: MutableList<String> = mutableListOf()

    private fun warn(msg: String) {
        if (warnings.size < 8) warnings.add("warn: $msg")
    }

    /** The six effects in WINNING order: the rank the file gives, then the
     *  shipped order as the tie-break - the chain the daemon walks in
     *  channel.c. The shipped order is used ONLY to place a
     *  key the file omits so the list stays complete and draggable; it is
     *  never written back as a value the daemon would have had to guess.
     *  setPriorityOrder() renumbers all six on the next save, which puts an
     *  explicit rank in the file for every effect. */
    fun priorityOrder(): List<String> {
        val def = PRIORITY_EFFECTS.toMap()
        return PRIORITY_EFFECTS
            .sortedWith(compareByDescending<Pair<String, Int>> { priority[it.first] ?: it.second }
                .thenByDescending { def[it.first] ?: 0 })
            .map { it.first }
    }

    /** Write a row order as a clean descending ladder: the top row gets
     *  n * 10, the bottom one 10. The order is the whole truth in the GUI,
     *  so a save never has to reason about the numbers already on the
     *  device - it just renumbers. */
    fun setPriorityOrder(list: List<String>) {
        val known = list.filter { name -> PRIORITY_EFFECTS.any { it.first == name } }
        priority.clear()
        known.forEachIndexed { i, name -> priority[name] = (known.size - i) * 10 }
    }

    private fun renderBySec(sec: String): Render? = when (sec) {
        "notify" -> notifyRender
        "charge.lower" -> chargeLower
        "charge.middle" -> chargeMiddle
        "charge.upper" -> chargeUpper
        "missed" -> missedRender
        "alarm" -> alarmRender
        "ring" -> ringRender
        "voip" -> voipRender
        else -> null
    }

    /** The RENDER_FIELDS tokens at [i] into [r], all of them or nothing:
     *  mode, solid current, "pattern", sync, repeat, pattern current,
     *  timing, wave phase offsets. A short line or an unknown mode stops
     *  here, exactly where the daemon stops synthesizing that preset. */
    private fun parseRenderTail(t: List<String>, i: Int, r: Render): Boolean {
        if (t.size - i != RENDER_FIELDS) return false
        val mode = t[i].trim()
        if (mode !in VALID_MODES || t[i + 4].trim() != "pattern") return false
        val v = IntArray(RENDER_FIELDS - 2)
        var k = 0
        for (f in 1 until RENDER_FIELDS) {
            if (f == 4) continue
            v[k++] = t[i + f].trim().toIntOrNull() ?: return false
        }
        r.mode = mode
        r.solidCur = Triple(v[0].coerceIn(0, 15), v[1].coerceIn(0, 15), v[2].coerceIn(0, 15))
        r.patternSync = v[3] != 0
        r.patternRepeat = v[4].coerceIn(0, 15)
        r.patternCur = Triple(v[5].coerceIn(0, 15), v[6].coerceIn(0, 15), v[7].coerceIn(0, 15))
        r.patternRise = v[8]
        r.patternHold = v[9]
        r.patternFall = v[10]
        r.patternOfft = v[11]
        r.waveT0 = Triple(v[12].coerceAtLeast(0), v[13].coerceAtLeast(0), v[14].coerceAtLeast(0))
        return true
    }

    /** One render= line: the section's colour goes to [color], the preset
     *  into [r]. The colour survives a broken preset - the daemon reads it
     *  the same way - but nothing of that preset is taken over. */
    private fun parseRenderLine(sec: String, v: String, r: Render,
                                color: (Triple<Int, Int, Int>) -> Unit) {
        val t = v.split(',')
        if (t.size != RENDER_FIELDS + 3) {
            warn("[$sec] render= wants <r,g,b> and $RENDER_FIELDS preset fields, " +
                 "got ${t.size} - the whole preset is ignored")
            return
        }
        parseRgb(t.subList(0, 3).joinToString(","))?.let(color)
            ?: warn("[$sec] render= colour \"${t.subList(0, 3)}\" is not r,g,b in 0..255")
        if (!parseRenderTail(t, 3, r))
            warn("[$sec] render= preset fields are not mode/cur/pattern/sync/" +
                 "repeat/cur/timing/t0 - the whole preset is ignored")
    }

    /** The file as the daemon reads it: '#' or ';' opens a comment (a line
     *  of its own or right after a setting), and a value ending in a comma
     *  continues on the next line (a trailing backslash says the same).
     *  Fragments are trimmed and joined, so a value spread over several
     *  commented lines is one logical line - exactly what config.c
     *  assembles. */
    private fun logicalLines(text: String): List<String> {
        val out = ArrayList<String>()
        val cur = StringBuilder()
        var open = false
        for (raw in text.lineSequence()) {
            val line = raw.substringBefore('#').substringBefore(';').trim()
            val more = line.endsWith("\\") || line.endsWith(",")
            val frag = if (line.endsWith("\\")) line.dropLast(1).trim() else line
            if (open && !more && frag.isEmpty()) continue
            cur.append(frag)
            open = more
            if (more) continue
            if (cur.isNotBlank()) out.add(cur.toString())
            cur.setLength(0)
        }
        if (cur.isNotBlank()) out.add(cur.toString())
        return out
    }

    fun parse(text: String) {
        warnings.clear()
        var section = ""
        for (line in logicalLines(text)) {
            if (line.startsWith("[")) {
                val end = line.indexOf(']')
                section = if (end > 1) line.substring(1, end) else ""
                continue
            }
            when (section) {
                "priority" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val raw = line.substring(i + 1).trim()
                    // An unknown name and a non-number are both the
                    // daemon's business: channel.c warns and leaves the
                    // kind unranked, and so do we - but we say so,
                    // because a silently dropped line looks like a GUI bug.
                    if (PRIORITY_EFFECTS.none { it.first == k }) {
                        warn("[priority] unknown effect \"$k\" ignored")
                        continue
                    }
                    val v = raw.toIntOrNull()
                    if (v == null) {
                        warn("[priority] $k=\"$raw\" is not a number - using the default rank")
                        continue
                    }
                    priority[k] = v
                }
                "suppress" -> {
                    if (line.isNotBlank() && !suppress.contains(line)) suppress.add(line)
                }
                "rules" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val rule = parseRuleValue(line.substring(0, i).trim(),
                                              line.substring(i + 1))
                        ?: continue
                    rules.removeAll { it.pkg == rule.pkg }
                    rules.add(rule)
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
                        "render" -> parseRenderLine(section, v, r) {
                            if (section == "charge.lower") lowerColor = it
                            else if (section == "charge.middle") middleColor = it
                            else upperColor = it
                        }
                    }
                }
                "notify" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "max_sec" -> v.toLongOrNull()?.let { notifMaxSec = it }
                        "notify_screen_delay_ms" -> v.toLongOrNull()?.let { notifyScreenDelayMs = it }
                        "render" -> parseRenderLine(section, v, notifyRender) { notifyColor = it }
                    }
                }
                "ring" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "max_sec" -> v.toLongOrNull()?.let { ringCapSec = it }
                        "render" -> parseRenderLine(section, v, ringRender) { ringColor = it }
                    }
                }
                "voip" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "max_sec" -> v.toLongOrNull()?.let { voipMaxSec = it }
                        "render" -> parseRenderLine(section, v, voipRender) { voipColor = it }
                    }
                }
                "missed" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "max_sec" -> v.toLongOrNull()?.let { missedMaxSec = it }
                        "render" -> parseRenderLine(section, v, missedRender) { missedColor = it }
                    }
                }
                "alarm" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "max_sec" -> v.toLongOrNull()?.let { alarmMaxSec = it }
                        "render" -> parseRenderLine(section, v, alarmRender) { alarmColor = it }
                    }
                }
"led" -> {
                    val i = line.indexOf('=')
                    if (i <= 0) continue
                    val k = line.substring(0, i).trim()
                    val v = line.substring(i + 1).trim()
                    when (k) {
                        "logging" -> v.toIntOrNull()?.let { logging = it != 0 }
                        // imax is the chip current ceiling and the daemon
                        // accepts only the four hardware steps; keep an
                        // illegal value visible as a warning instead of
                        // rewriting it, so the file says what is actually
                        // broken (the daemon then leaves the chip off).
                        "imax" -> {
                            val n = v.toIntOrNull()
                            if (n != null && n in VALID_IMAX) imax = n
                            else warn("[led] imax=\"$v\" is not 5|10|15|30 - the daemon leaves the chip unpowered")
                        }
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
                else -> if (section.isNotEmpty() && line.indexOf('=') > 0 &&
                    section !in KNOWN_SECTIONS) {
                    warn("[$section] is not a section this GUI edits - left as it is")
                }
            }
        }
        if (firstThreshold > secondThreshold) {
            val t = firstThreshold; firstThreshold = secondThreshold; secondThreshold = t
        }
        LedSim.cal = LedSim.Cal(pvwR / 100.0, pvwG / 100.0, pvwB / 100.0, pvwGamma)
    }

    /** Parse one [rules] value ("r,g,b[,cap,<the render fields>]").
     *  The tail is the render line without its colour, so it is read by the
     *  same parser and is all-or-nothing: a tail that does not carry every
     *  field leaves the rule colour-only, which is what the daemon does with
     *  it. Growing a preset the file never fully said would silently change
     *  the light, so it is reported instead. */
    private fun parseRuleValue(pkg: String, value: String): Rule? {
        val t = value.split(',')
        if (t.size < 3) return null
        val r = clampColor(t[0].trim().toIntOrNull() ?: return null)
        val g = clampColor(t[1].trim().toIntOrNull() ?: return null)
        val b = clampColor(t[2].trim().toIntOrNull() ?: return null)
        if (t.size == 3) return Rule(pkg, r, g, b)
        val cap = t[3].trim().toLongOrNull()
        if (cap == null || cap < 0) {
            warn("$pkg: cap=\"${t[3].trim()}\" is not max_sec - " +
                 "the rule keeps its colour only")
            return Rule(pkg, r, g, b)
        }
        val render = Render()
        if (!parseRenderTail(t, 4, render)) {
            warn("$pkg: preset tail is not mode/cur/pattern/sync/repeat/cur/" +
                 "timing/t0 - the rule keeps its colour only")
            return Rule(pkg, r, g, b)
        }
        return Rule(pkg, r, g, b, maxSec = cap, render = render, custom = true)
    }

    /** One [rules] value: colour always, the full preset when custom. */
    private fun ruleValue(rule: Rule): String {
        val sb = StringBuilder()
        sb.append(rule.r).append(',').append(rule.g).append(',').append(rule.b)
        if (rule.custom) {
            sb.append(',').append(rule.maxSec)
            sb.append(',').append(renderTail(rule.render))
        }
        return sb.toString()
    }

    /** The RENDER_FIELDS preset tokens: the shared vocabulary behind both
     *  the section render= line and the [rules] tail, so the two can never
     *  drift apart. */
    private fun renderTail(r: Render): String = buildString {
        append(r.mode)
        append(',').append(tripleClamp(r.solidCur, 0, 15))
        append(",pattern,")
        append(if (r.patternSync) 1 else 0)
        append(',').append(r.patternRepeat.coerceIn(0, 15))
        append(',').append(tripleClamp(r.patternCur, 0, 15))
        append(',').append(r.patternRise).append(',').append(r.patternHold)
        append(',').append(r.patternFall).append(',').append(r.patternOfft)
        append(',').append(r.waveT0.first.coerceAtLeast(0)).append(',')
            .append(r.waveT0.second.coerceAtLeast(0)).append(',')
            .append(r.waveT0.third.coerceAtLeast(0))
    }

    /** One section's whole renderer: colour first, then the preset. */
    private fun renderLine(r: Render, color: Triple<Int, Int, Int>): String =
        "${rgb(color)},${renderTail(r)}"

    /** Every line this model owns, section by section: the whole of what a
     *  save may write, and the only vocabulary it recognizes anywhere. */
    private fun owned(): List<OwnedSection> = listOf(
        OwnedSection("priority", PRIORITY_EFFECTS.map {
            OwnedKey(it.first, (priority[it.first] ?: it.second).toString())
        }),
        OwnedSection("suppress", suppress.map { OwnedKey(it, null) }),
        OwnedSection("rules", rules.map { OwnedKey(it.pkg, ruleValue(it)) }),
        OwnedSection("charge", listOf(
            OwnedKey("first_threshold", firstThreshold.toString()),
            OwnedKey("second_threshold", secondThreshold.toString())
        )),
        OwnedSection("charge.lower", listOf(
            OwnedKey("render", renderLine(chargeLower, lowerColor)))),
        OwnedSection("charge.middle", listOf(
            OwnedKey("render", renderLine(chargeMiddle, middleColor)))),
        OwnedSection("charge.upper", listOf(
            OwnedKey("render", renderLine(chargeUpper, upperColor)))),
        OwnedSection("notify", listOf(
            OwnedKey("max_sec", notifMaxSec.toString()),
            OwnedKey("notify_screen_delay_ms", notifyScreenDelayMs.toString()),
            OwnedKey("render", renderLine(notifyRender, notifyColor))
        )),
        OwnedSection("ring", listOf(
            OwnedKey("max_sec", ringCapSec.toString()),
            OwnedKey("render", renderLine(ringRender, ringColor))
        )),
        OwnedSection("voip", listOf(
            OwnedKey("max_sec", voipMaxSec.toString()),
            OwnedKey("render", renderLine(voipRender, voipColor))
        )),
        OwnedSection("missed", listOf(
            OwnedKey("max_sec", missedMaxSec.toString()),
            OwnedKey("render", renderLine(missedRender, missedColor))
        )),
        OwnedSection("alarm", listOf(
            OwnedKey("max_sec", alarmMaxSec.toString()),
            OwnedKey("render", renderLine(alarmRender, alarmColor))
        )),
        OwnedSection("led", listOf(
            OwnedKey("logging", if (logging) "1" else "0"),
            OwnedKey("imax", imax.toString())
        )),
        OwnedSection("preview", listOf(
            OwnedKey("r", fmtW(pvwR)),
            OwnedKey("g", fmtW(pvwG)),
            OwnedKey("b", fmtW(pvwB)),
            OwnedKey("gamma", String.format(Locale.US, "%.1f", pvwGamma))
        ))
    )

    /** [text] - the device file - with this model's values put into it.
     *  Lines the file already has are rewritten in place, keys it lacks are
     *  added, entries the model dropped are removed, and every other byte
     *  (comments, blank lines, section order, a key this GUI cannot name)
     *  is left alone. */
    fun patch(text: String): String = ConfWriter(text, owned()).write()

    /** Patch the values into the file the device actually has, so a save
     *  keeps the comments and the layout the file was written with. An
     *  unreadable file is NOT rewritten from the model: that is the wipe
     *  this avoids, and a save that cannot see the file must write none. */
    fun save(): Su.Result {
        val cur = Su.run("cat $CONF_PATH 2>/dev/null")
        if (!cur.ok || cur.out.isBlank())
            return Su.Result("cannot read $CONF_PATH - nothing written", 1)
        return Su.writeFile(CONF_PATH, patch(cur.out))
    }

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

/** One line the model owns: [key] is the key (a package, for [rules] and
 *  [suppress]) and [value] the text after '=', or null when the line is the
 *  key alone. */
private class OwnedKey(val key: String, val value: String?)

/** One section of the model's vocabulary, in template order. */
private class OwnedSection(val name: String, val keys: List<OwnedKey>)

/**
 * The in-place writer behind a save: it puts the model's values into the
 * lines the file already has and adds only what is missing, so comments,
 * blank lines, the section order and every key the GUI has no vocabulary
 * for stay exactly where the operator put them.
 *
 * It works on LOGICAL lines - the group of physical rows the daemon
 * assembles from a value spread over comma-terminated rows - and writes
 * physical rows back: a value whose token count still fits is rewritten one
 * fragment per row, each keeping its own comment. A value whose shape
 * changed (a [rules] entry that grew or lost its preset tail) becomes a
 * single row, because the old per-row comments would then describe fields
 * that are no longer on them.
 *
 * The two list sections ([suppress] and [rules]) are set-valued: the model
 * IS the list, so an entry the file has and the model does not is removed
 * and one the model has and the file does not is added.
 */
private class ConfWriter(private val text: String, sections: List<OwnedSection>) {

    /** A physical row: the code the daemon reads, the comment a save must
     *  put back, and the raw line for a row nothing was patched into. */
    private class Row(
        val raw: String,
        val indent: String,
        val code: String,
        val comment: String,
        val col: Int,
        val more: Boolean
    )

    /** One logical line: its rows, the section it belongs to, its key. */
    private class Log(val rows: List<Row>, val section: String) {
        val first: Row get() = rows.first()
        val key: String = rows.first().code.substringBefore('=').trim()
        /** the code as the daemon assembles it, fragments joined by comma */
        val code: String = rows.joinToString(",") { it.code }
        val header: String? = if (rows.first().code.startsWith("[")) {
            val end = rows.first().code.indexOf(']')
            if (end > 0) rows.first().code.substring(1, end).trim() else null
        } else null
    }

    /** Sections whose entries the model owns as a set. */
    private val lists = setOf("suppress", "rules")

    private val owned = LinkedHashMap<String, OwnedSection>()
    private val keys = HashMap<String, OwnedKey>()

    init {
        for (s in sections) {
            owned[s.name] = s
            for (k in s.keys) keys["${s.name} ${k.key}"] = k
        }
    }

    fun write(): String {
        val out = ArrayList<String>()
        /** where a section's block ends in [out]: a key the file lacks goes
         *  there, before the blank line that closes the block */
        val blockEnd = HashMap<String, Int>()
        /** output index right under a section header, for an empty one */
        val headerAt = HashMap<String, Int>()
        val written = HashSet<String>()
        var section = ""

        for (log in logicals()) {
            val head = log.header
            if (head != null) {
                section = head
                out.add(log.first.raw)
                headerAt[section] = out.size
                continue
            }
            val id = "$section ${log.key}"
            val mine = keys[id]
            if (mine == null && section in lists && log.first.code.isNotEmpty()) continue
            if (mine != null) written.add(id)
            out.addAll(if (mine == null || sameValue(log, mine)) {
                log.rows.map { it.raw }
            } else {
                rewrite(log, mine)
            })
            if (log.rows.any { it.code.isNotEmpty() }) blockEnd[section] = out.size
        }

        // the file's own text is the base; what the model adds goes on top
        val inserts = ArrayList<Pair<Int, List<String>>>()
        val tail = ArrayList<String>()
        for (s in this.owned.values) {
            val missing = s.keys.filter { !written.contains("${s.name} ${it.key}") }
            val block = missing.map { if (it.value == null) it.key else "${it.key}=${it.value}" }
            val at = blockEnd[s.name] ?: headerAt[s.name]
            when {
                at != null && block.isEmpty() -> Unit
                at != null -> inserts.add(at to block)
                else -> {
                    // a section the file has none of: header plus what is missing
                    tail.add("")
                    tail.add("[${s.name}]")
                    tail.addAll(block)
                }
            }
        }
        for ((at, block) in inserts.sortedByDescending { it.first }) out.addAll(at, block)
        out.addAll(tail)
        return out.joinToString(if (text.contains("\r\n")) "\r\n" else "\n")
    }

    /** Whether the file's line already IS the value the model wants. A
     *  space around a comma is not part of a value - the daemon trims every
     *  token it reads - so a value that differs from the model's in nothing
     *  else is left byte for byte, spacing and comment included. */
    private fun sameValue(log: Log, mine: OwnedKey): Boolean =
        log.key == mine.key &&
            spaced(log.code.substringAfter('=', log.code)) == spaced(mine.value ?: mine.key)

    private fun spaced(value: String) = value.split(',').joinToString(",") { it.trim() }

    /** The rows that carry [mine]'s value. One row per old fragment while
     *  the token count still fits, a single row otherwise. */
    private fun rewrite(log: Log, mine: OwnedKey): List<String> {
        val tokens = (mine.value ?: mine.key).split(',')
        val bare = mine.value == null
        val sizes = log.rows.mapIndexed { i, r ->
            val code = if (bare || i > 0) r.code else r.code.substringAfter('=', "")
            if (code.isEmpty()) -1 else code.count { it == ',' } + 1
        }
        if (sizes.any { it < 0 } || sizes.sum() != tokens.size) {
            val one = if (bare) mine.key else "${mine.key}=${tokens.joinToString(",")}"
            return listOf(log.first.indent + one)
        }
        val parts = ArrayList<String>(log.rows.size)
        val before = ArrayList<Int>(log.rows.size)
        var k = 0
        log.rows.forEachIndexed { i, r ->
            val body = tokens.subList(k, k + sizes[i]).joinToString(",")
            k += sizes[i]
            val head = when {
                bare -> ""
                i == 0 -> mine.key + "="
                else -> ""
            }
            val tail = if (i < log.rows.size - 1) "," else ""
            parts.add(r.indent + head + body + tail)
            // a comment keeps the column it was written in
            before.add(r.col)
        }
        val col = maxOf(parts.maxOf { it.length } + 2, before.maxOf { it })
        return parts.mapIndexed { i, p ->
            val comment = log.rows[i].comment
            if (comment.isEmpty()) p else p.padEnd(col) + comment
        }
    }

    /** The physical rows as logical lines: a value that continues on the
     *  next row is one line, and a comment or blank row inside such a run
     *  belongs to it exactly as it does for the daemon. */
    private fun logicals(): List<Log> {
        val rows = parse()
        val out = ArrayList<Log>()
        var section = ""
        var i = 0
        while (i < rows.size) {
            val start = i
            while (i + 1 < rows.size && rows[i].more) i++
            val group = rows.subList(start, i + 1)
            val head = group.first().code
            if (head.startsWith("[")) {
                val end = head.indexOf(']')
                if (end > 0) section = head.substring(1, end).trim()
            }
            out.add(Log(group.toList(), section))
            i++
        }
        return out
    }

    /** The rows with their terminators off: the file's own line ending is put
     *  back once, when the rows are joined, so a rewritten row cannot end up
     *  with a different one than the rows around it. */
    private fun parse(): List<Row> = text.split('\n').map { line ->
        val raw = line.removeSuffix("\r")
        val at = commentAt(raw)
        val head = if (at < 0) raw else raw.substring(0, at)
        val code = head.trim()
        val more = code.endsWith(",") || code.endsWith("\\")
        Row(
            raw = raw,
            indent = head.takeWhile { it == ' ' || it == '\t' },
            code = if (more) code.dropLast(1).trim() else code,
            comment = if (at < 0) "" else raw.substring(at).trim(),
            col = if (at < 0) 0 else at,
            more = more
        )
    }

    private fun commentAt(raw: String): Int {
        val hash = raw.indexOf('#')
        val semi = raw.indexOf(';')
        return when {
            hash < 0 -> semi
            semi < 0 -> hash
            else -> minOf(hash, semi)
        }
    }
}