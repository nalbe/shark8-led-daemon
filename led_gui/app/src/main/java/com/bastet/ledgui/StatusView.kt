package com.bastet.ledgui

import android.content.Context
import android.content.Intent
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.provider.Settings
import android.util.TypedValue
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.core.app.NotificationManagerCompat
import androidx.core.content.FileProvider
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.util.Locale
import kotlin.math.roundToInt

/** Status tab rebuilt on classic Views (Compose cannot render smooth 120Hz
 * on this firmware; plain View framework does). All logic is identical to
 * the old composable StatusScreen: no Compose anywhere in the render path.
 *
 * Polling: collectAll() every 3s (one persistent-su-shell roundtrip),
 * live LED brightness read 10x/sec, tickers auto-start/stop with the view.
 */
class StatusView(context: Context) : LinearLayout(context) {

    private val mP = android.view.ViewGroup.LayoutParams.MATCH_PARENT
    private val wP = android.view.ViewGroup.LayoutParams.WRAP_CONTENT

    private enum class RootState { CHECKING, PENDING, GRANTED, DENIED }

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)
    private var tickerJob: Job? = null

// state
    private var rootState = RootState.CHECKING
    private var pid = ""
    private var led: LedStatus = LedStatus()
    private var conf: LedConf? = null
    private var logText = ""
    private var imax = 0
    private var loggingOn = true
    private var loggingBusy = false
    private var watchdogMs = 0L
    private var watchdogBusy = false
    private var bridgeConnected = false
    private var rootRequestPending = false
    private var rootDialogPending = false
    private var suUnreachable = false
    private lateinit var loggingListener: android.widget.CompoundButton.OnCheckedChangeListener
    private lateinit var watchdogEt: android.widget.EditText

    // widgets
    private lateinit var rootTv: TextView
    private lateinit var rootHintTv: TextView
    private lateinit var requestBtn: Button
    private lateinit var pidTv: TextView
    private lateinit var ledSwatch: View
    private lateinit var ledModeTv: TextView
    private lateinit var ledColorTv: TextView
    private lateinit var ledLightBandTv: TextView
    private lateinit var ledPkgTv: TextView
    private lateinit var ledSinceTv: TextView
    private lateinit var ledHintTv: TextView
    private lateinit var ledRendererTv: TextView
    private lateinit var logTv: TextView
    private lateinit var loggingCb: CheckBox
    private lateinit var bridgeTv: TextView

init {
        orientation = VERTICAL
        setBackgroundColor(parse("#FF121212"))
        buildUi()
        render()
    }

override fun onAttachedToWindow() {
        super.onAttachedToWindow()
        if (visibility == VISIBLE) startTicker()
    }

    override fun onDetachedFromWindow() {
        super.onDetachedFromWindow()
        stopTicker()
    }

    /** Stop hammering the root shell while this tab is hidden behind the
     *  Config tab (it's GONE, not detached, so the polling loops would
     *  otherwise run forever and time out the shared shell -> fake "no
     *  root" glitches). */
    override fun onVisibilityChanged(v: View, changedVisibility: Int) {
        super.onVisibilityChanged(v, changedVisibility)
        if (visibility == VISIBLE) startTicker() else stopTicker()
    }

private fun startTicker() {
        if (tickerJob != null && tickerJob!!.isActive) return
        tickerJob = scope.launch {
            launch { refreshLoop() }
        }
    }

    private fun stopTicker() {
        tickerJob?.cancel()
        tickerJob = null
    }

    // ---------------------------------------------------------------- loops

    private suspend fun refreshLoop() {
        while (true) {
            val snap = withContext(Dispatchers.IO) { collectAll() }
            applySnap(snap)
            delay(3000)
        }
    }

    // ---------------------------------------------------------------- render

    private fun applySnap(s: Snapshot) {
        if (!rootRequestPending) {
            rootState = when {
                s.rootOk -> {
                    rootDialogPending = false
                    RootState.GRANTED
                }
                rootDialogPending -> rootState
                else -> RootState.DENIED
            }
        }
        pid = s.daemon
        led = s.status
        conf = s.conf
        imax = s.imax
        logText = s.log
        if (!loggingBusy && loggingOn != s.logging) {
            loggingOn = s.logging
            loggingCb.setOnCheckedChangeListener(null)
            loggingCb.isChecked = s.logging
            loggingCb.setOnCheckedChangeListener(loggingListener)
        }
        if (!watchdogBusy && watchdogMs != s.watchdogMs) {
            watchdogMs = s.watchdogMs
            watchdogEt.setText(if (s.watchdogMs > 0) s.watchdogMs.toString() else "")
            watchdogEt.hint = if (s.watchdogMs > 0) "" else "off (0)"
        }
        if (bridgeConnected != s.bridgeConnected) {
            bridgeConnected = s.bridgeConnected
        }
        render()
    }

private fun render() {
        renderRoot()
        renderDaemon()
        renderBridge()
        renderLed()
        renderRenderer()
        logTv.text = logText.ifBlank { "(empty)" }
    }

    /** Active chip engine (from led_status) + global [led] Imax + the
     *  armed event's per-channel current from led.conf: what the chip is
     *  really doing right now. */
    private fun renderRenderer() {
        val cur = if (led.isArmed) statusCurrent() else null
        val curTxt = cur?.let { "  cur ${it.first},${it.second},${it.third}" } ?: ""
        ledRendererTv.text = if (led.engine.isBlank()) {
            "renderer: (led.status missing - old daemon?)"
        } else {
            "renderer: ${led.engine} @ $imax mA (chip)$curTxt"
        }
        ledRendererTv.setTextColor(if (led.engine.isBlank()) parse("#FF909090") else parse("#FFB0BEC5"))
    }

    /** Per-channel current (0..15) the armed event drives, resolved from
     *  led.conf by status mode/band/engine. wave has no per-channel knob
     *  in the GUI (chip default full); off = all channels dark. */
    private fun statusCurrent(): Triple<Int, Int, Int>? {
        val c = conf ?: return null
        val active = when (led.mode) {
            "charge" -> when (led.band) {
                "lower" -> c.chargeLower
                "middle" -> c.chargeMiddle
                "upper" -> c.chargeUpper
                else -> null
            }
            "notify" -> if (c.rules.any { it.pkg == led.pkg }) c.notifyAppRender else c.notifyRender
            "ring" -> c.ringRender
            "voip" -> c.voipRender
            "missed" -> c.missedRender
            "alarm" -> c.alarmRender
            else -> null
        } ?: return null
        return when (led.engine) {
            "solid" -> active.solidCur
            "breath" -> active.brCur
            "off" -> Triple(0, 0, 0)
            else -> Triple(15, 15, 15)
        }
    }

    private fun renderRoot() {
        when (rootState) {
            RootState.CHECKING -> {
                rootTv.text = "checking..."
                rootTv.setTextColor(parse("#FFE0E0E0"))
                rootHintTv.text = ""
            }
RootState.PENDING -> {
                if (suUnreachable) {
                    rootTv.text = "Root request: no su binary reachable"
                    rootHintTv.text = "KernelSU exposes su only to allowlisted apps. " +
                        "Open the manager, tap Superuser, add LED GUI, set Allow - " +
                        "this line turns green."
                } else {
                    rootTv.text = "Root request sent. Confirm the KernelSU dialog on screen."
                    rootHintTv.text = "If no dialog appears, KernelSU hides su until the" +
                        " app is allowed - open the manager below and grant LED GUI."
                }
                rootTv.setTextColor(parse("#FFEF6C00"))
            }
            RootState.GRANTED -> {
                rootTv.text = "root granted (uid=0)"
                rootTv.setTextColor(parse("#FF4CAF50"))
                rootHintTv.text = ""
            }
            RootState.DENIED -> {
                rootTv.text = "no root yet"
                rootTv.setTextColor(parse("#FFF44336"))
                rootHintTv.text =
                    "Open KernelSU Manager, tap Superuser, add LED GUI and set Allow."
            }
        }
requestBtn.isEnabled = rootState != RootState.GRANTED
        requestBtn.alpha = if (requestBtn.isEnabled) 1f else 0.45f
    }

    private fun renderDaemon() {
        pidTv.text = "chgd pid: ${if (pid.isBlank()) "down" else pid}"
    }

    private fun renderBridge() {
        val nlsOn = nlsEnabled()
        if (nlsOn) {
            bridgeTv.text = when {
                bridgeConnected -> "active (chgd listening)"
                else -> "granted, connecting..."
            }
            bridgeTv.setTextColor(
                parse(
                    if (bridgeConnected) "#FF4CAF50" else "#FFEF6C00"
                )
            )
        } else {
            bridgeTv.text = "off - grant notification access"
            bridgeTv.setTextColor(parse("#FFF44336"))
        }
    }

    private fun nlsEnabled(): Boolean {
        return try {
            val enabled = NotificationManagerCompat.getEnabledListenerPackages(context)
            enabled.any { it.startsWith("com.bastet.lednls") }
        } catch (_: Exception) {
            false
        }
    }

    private fun openNlsSettings() {
        try {
            context.startActivity(
                Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS)
                    .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            )
        } catch (_: Exception) {
        }
    }

    private fun renderLed() {
if (led.mode.isEmpty()) {
            ledModeTv.text = "no /data/local/tmp/led_status (old daemon binary?)"
            ledColorTv.visibility = GONE
            ledLightBandTv.visibility = GONE
            ledPkgTv.visibility = GONE
            ledSinceTv.visibility = GONE
            ledHintTv.visibility = GONE
            return
        }
        ledColorTv.visibility = VISIBLE
        ledLightBandTv.visibility = VISIBLE
        ledPkgTv.visibility = VISIBLE
        ledSinceTv.visibility = VISIBLE
ledModeTv.text = "mode: ${led.mode}  ${if (led.isArmed) "(armed)" else "(off)"}"
        ledLightBandTv.text = "band: ${led.band.ifBlank { "-" }}${
            if (led.engine.isBlank()) "" else "  engine: ${led.engine}"
        }"
        ledPkgTv.text = "pkg: ${led.pkg.ifBlank { "-" }}"
        ledSinceTv.text = "since: ${led.prettyTime}"
        updateLedLive()
    }

private fun updateLedLive() {
        if (led.mode.isEmpty()) return
        val sw = ledSwatch.background as? GradientDrawable ?: return
        /* Live color coming OUT of the chip is unreadable: the AW2033
         * executes breath/wave patterns in hardware and exposes no
         * readback of the instantaneous PWM (regs are write-only). So we
         * show the CONFIG color the daemon armed, scaled by the event's
         * per-channel current (LedSim) - the light the combination
         * produces. engine in parentheses tells what the chip does. */
        val cur = if (led.isArmed) statusCurrent() else null
        val raw = led.color
        val sim = if (led.isArmed) {
            LedSim.rgbWithCurrent(raw, cur ?: Triple(15, 15, 15))
        } else Triple(-1, -1, -1)
        sw.gradientType = GradientDrawable.LINEAR_GRADIENT
        sw.setColor(
            if (sim.first >= 0) Color.rgb(sim.first, sim.second, sim.third)
            else Color.DKGRAY
        )
        val hex = if (raw.first >= 0) {
            String.format(Locale.US, "#%02X%02X%02X", raw.first, raw.second, raw.third)
        } else ""
        val simHex = if (sim.first >= 0 && sim != raw) {
            String.format(Locale.US, "  sim #%02X%02X%02X", sim.first, sim.second, sim.third)
        } else ""
        val newText = if (led.engine.isEmpty()) {
            "color: $hex  (${raw.first},${raw.second},${raw.third})$simHex"
        } else {
            "color: $hex  (${raw.first},${raw.second},${raw.third})$simHex  [${led.engine}]"
        }
        if (ledColorTv.text.toString() != newText) ledColorTv.text = newText
        val pattern = led.engine == "wave" || led.engine == "breath"
        ledHintTv.visibility = if (pattern && led.isArmed) VISIBLE else GONE
        if (ledHintTv.visibility == VISIBLE) {
            ledHintTv.text = if (led.engine == "wave") {
                "traveling wave runs on the AW2033 chip (hardware pattern)"
            } else {
                "breathing runs on the AW2033 chip (hardware pattern)"
            }
        }
}

// ---------------------------------------------------------------- actions

    private fun refreshNow() {
        scope.launch {
            val snap = withContext(Dispatchers.IO) { collectAll() }
            applySnap(snap)
        }
    }

    private fun sendHook(sig: String) {
        scope.launch { withContext(Dispatchers.IO) { hook(sig) } }
    }

    private fun clearLog() {
        scope.launch {
            withContext(Dispatchers.IO) {
                hook("CONT")
                logText = logTail(25)
            }
            logTv.text = logText.ifBlank { "(empty)" }
        }
    }

    /** Copy the daemon log into the app cache and hand it to the default
     *  text viewer. The live file is root-only (SELinux blocks the app),
     *  so it is re-read via su; the copy is what the viewer actually gets. */
    private fun openLog() {
        scope.launch {
            val content = withContext(Dispatchers.IO) {
                Su.run("cat /data/local/tmp/ledd.log 2>/dev/null").out
            }
            val out = java.io.File(context.cacheDir, "logs/ledd.log")
            out.parentFile?.mkdirs()
            out.writeText(content.ifBlank { "(empty log)" })
            val uri = FileProvider.getUriForFile(
                context, "com.bastet.ledgui.fileprovider", out
            )
            val ok = runCatching {
                context.startActivity(
                    Intent(Intent.ACTION_VIEW)
                        .setDataAndType(uri, "text/plain")
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or
                            Intent.FLAG_GRANT_READ_URI_PERMISSION)
                )
            }.isSuccess
            if (!ok) {
                android.widget.Toast.makeText(
                    context, "no text viewer for the log", android.widget.Toast.LENGTH_SHORT
                ).show()
            }
        }
    }

    /** Flip the daemon's on-disk logging live. Touches ONLY the [led]
     *  logging= line in led.conf (no full-file rewrite, so unsaved edits
     *  in the Config tab survive) and pokes the daemon with SIGALRM to
     *  hot-reload it. */
    private fun toggleLogging(on: Boolean) {
        if (loggingBusy) return
        loggingBusy = true
        loggingCb.isChecked = on
        loggingOn = on
        scope.launch {
            withContext(Dispatchers.IO) {
                val valStr = if (on) "1" else "0"
                Su.run(
                    "CFG=/data/adb/modules/led_hal_root/led.conf;" +
                        " if grep -q '^logging=.\$' \$CFG; then " +
                        "  sed -i 's/^logging=./logging=$valStr/' \$CFG; " +
                        "else " +
                        "  (printf '\\n[led]\\nlogging=$valStr\\n' >> \$CFG); " +
                        "fi; kill -ALRM \$(pidof chgd) 2>/dev/null"
                )
            }
            loggingBusy = false
        }
    }


    /** Write a new [led] watchdog_ms into led.conf (single-key touch, like
     *  toggleLogging), then SIGALRM the daemon so it reloads and pushes
     *  "WD <ms>" over the socket - the NLS supervisor applies the fresh
     *  value with zero polling. Empty = 0 = watchdog disabled. */
    private fun applyWatchdog() {
        if (watchdogBusy) return
        watchdogBusy = true
        val ms = watchdogEt.text.toString().trim().toLongOrNull()?.coerceIn(0, 86400000L) ?: 0L
        scope.launch {
            withContext(Dispatchers.IO) {
                Su.run(
                    "CFG=/data/adb/modules/led_hal_root/led.conf;" +
                        " if grep -q '^watchdog_ms=.\$' \$CFG; then " +
                        "  sed -i 's/^watchdog_ms=.*/watchdog_ms=$ms/' \$CFG; " +
                        "else " +
                        "  (printf '\\n[led]\\nwatchdog_ms=$ms\\n' >> \$CFG); " +
                        "fi; kill -ALRM \$(pidof chgd) 2>/dev/null"
                )
            }
            withContext(Dispatchers.Main) {
                watchdogMs = ms
                watchdogEt.setText(if (ms > 0) ms.toString() else "")
                watchdogEt.hint = if (ms > 0) "" else "off (0)"
                watchdogBusy = false
            }
        }
    }

private fun requestRoot() {
        if (rootRequestPending) return
        rootRequestPending = true
        // No instant PENDING paint here: on this firmware a hidden su answers
        // "no root" within milliseconds, so the old optimistic flash looked
        // like a broken flicker. PENDING is only painted when a real su stood
        // up and is actually waiting on the manager's dialog.
        scope.launch {
            val r = withContext(Dispatchers.IO) { probeRoot() }
            rootRequestPending = false
            when (r) {
                RootState.GRANTED -> {
                    rootDialogPending = false
                    suUnreachable = false
                    rootState = RootState.GRANTED
                }
                RootState.PENDING -> {
                    rootDialogPending = true
                    suUnreachable = false
                    rootState = RootState.PENDING
                }
                else -> {
                    // su answered it can't help: on this firmware that means
                    // su is hidden for unallowlisted apps. Park the card on a
                    // readable orange explanation until root actually lands.
                    rootDialogPending = suUnreachable
                    rootState = if (suUnreachable) RootState.PENDING else RootState.DENIED
                }
            }
            render()
        }
    }

    // ---------------------------------------------------------------- UI build

private fun buildUi() {
        val scroll = ScrollView(context)
        scroll.isFillViewport = true
        val content = LinearLayout(context)
        content.orientation = VERTICAL
        content.setPadding(0, dpi(4), 0, dpi(8))
        scroll.addView(content, LayoutParams(mP, wP))
        addView(scroll, LayoutParams(mP, mP))

        // --- Root card
        content.addView(card {
            addView(sectionTitle("Root"))
            addView(spacer(6))
            rootTv = text("checking...")
            addView(rootTv)
            rootHintTv = text("", 13f, parse("#FF909090"))
            addView(rootHintTv)
            addView(spacer(4))
            requestBtn = filledBtn("Request root") { requestRoot() }
            addView(row(requestBtn, outlinedBtn("Open KernelSU Manager") {
                openRootManager(context)
            }))
        })

// --- Daemon card
        content.addView(card {
            addView(sectionTitle("Daemon"))
            addView(spacer(6))
            pidTv = text("")
            addView(pidTv)
            addView(spacer(8))
            addView(row(
                filledBtn("Refresh") { refreshNow() },
                outlinedBtn("Kill chgd") {
                    scope.launch {
                        val snap = withContext(Dispatchers.IO) {
                            Su.run("kill -9 \$(pidof chgd) 2>/dev/null")
                            collectAll()
                        }
                        applySnap(snap)
                    }
                }
            ))
                addView(spacer(4))
            addView(text(
                "chgd is supervised by the NLS watchdog in this app. " +
                    "Set 0 to disable the poll entirely (it only guards against crashes).",
                13f, parse("#FF909090")
            ))
            addView(spacer(6))
            addView(text("Watchdog poll interval (ms)", 13f, parse("#FFB0BEC5")))
            watchdogEt = android.widget.EditText(context)
            watchdogEt.inputType = android.text.InputType.TYPE_CLASS_NUMBER
            watchdogEt.setSingleLine(true)
            watchdogEt.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            watchdogEt.setTextColor(parse("#FFE0E0E0"))
            watchdogEt.hint = "60000"
            val wg = GradientDrawable()
            wg.shape = GradientDrawable.RECTANGLE
            wg.cornerRadius = dpf(8)
            wg.setColor(parse("#FF2A2A2A"))
            wg.setStroke(dpi(1), parse("#FF333333"))
            watchdogEt.background = wg
            watchdogEt.setPadding(dpi(8), dpi(6), dpi(8), dpi(6))
            watchdogEt.layoutParams = LayoutParams(0, wP, 1f)
            addView(row(
                watchdogEt,
                filledBtn("Apply") { applyWatchdog() }
            ))
            addView(spacer(2))
            addView(text(
                "0 = disabled. Changes apply live (no daemon restart).",
                12f, parse("#FF727272")
            ))
        })

        // --- Notification bridge card
        content.addView(card {
            addView(sectionTitle("Notification bridge (NLS)"))
            addView(spacer(6))
            bridgeTv = text("checking...", 13f, parse("#FFB0BEC5"))
            addView(bridgeTv)
            addView(spacer(4))
            addView(row(
                filledBtn("Notification access") { openNlsSettings() },
                outlinedBtn("Refresh") { renderBridge() }
            ))
            addView(spacer(4))
            addView(text(
                "The in-app listener feeds chgd directly, so the LED works on " +
                    "non-debuggable builds where event-log tags never fire.",
                12f, parse("#FF909090")
            ))
        })

        // --- LED + test hooks card
        content.addView(card {
            addView(sectionTitle("LED state"))
            addView(spacer(6))
            val sw = GradientDrawable()
            sw.shape = GradientDrawable.RECTANGLE
            sw.cornerRadius = dpf(6)
            sw.setColor(Color.DKGRAY)
            ledSwatch = View(context)
            ledSwatch.background = sw
            ledSwatch.layoutParams = LayoutParams(dpi(30), dpi(30))
            val liveCol = LinearLayout(context)
            liveCol.orientation = VERTICAL
ledModeTv = text("")
            ledColorTv = text("", 13f, parse("#FFB0BEC5"), mono = true)
            ledLightBandTv = text("")
            ledPkgTv = text("")
            ledSinceTv = text("")
            ledHintTv = text("", 12f, parse("#FFEF6C00"))
            ledHintTv.visibility = GONE
            ledRendererTv = text("", 13f, parse("#FFB0BEC5"), mono = true)
            liveCol.addView(ledModeTv)
            liveCol.addView(ledColorTv)
            liveCol.addView(ledLightBandTv)
            liveCol.addView(ledPkgTv)
            liveCol.addView(ledSinceTv)
            liveCol.addView(ledHintTv)
            liveCol.addView(ledRendererTv)
            val ledRow = row(ledSwatch, liveCol)
            for (i in 0 until ledRow.childCount) {
                val c = ledRow.getChildAt(i)
                if (i > 0) {
                    val lp = c.layoutParams as LayoutParams
                    lp.marginStart = dpi(10)
                    c.layoutParams = lp
                }
            }
            addView(ledRow)
            addView(spacer(8))
            addView(sectionTitle("Test hooks"))
            addView(spacer(6))
            addView(row(
                filledBtn("Fake Telegram") { sendHook("USR1") },
                outlinedBtn("Disarm") { sendHook("USR2") }
            ))
            addView(spacer(4))
            addView(row(
                filledBtn("Fake Dialer") { sendHook("HUP") },
                outlinedBtn("Rainbow") { sendHook("WINCH") }
            ))
            addView(spacer(4))
            addView(row(filledBtn("Charge Test") { sendHook("QUIT") }))
        })

        // --- log card
        content.addView(card {
            val titleLp = LayoutParams(0, wP, 1f)
            val title = sectionTitle("ledd.log (tail)")
            title.layoutParams = titleLp
            addView(row(title, outlinedBtn("Open") { openLog() }, outlinedBtn("Clear") { clearLog() }))
            addView(spacer(6))
            loggingCb = CheckBox(context)
            loggingCb.text = "Logging (daemon writes ledd.log)"
            loggingCb.setTextColor(parse("#FFB0BEC5"))
            loggingCb.textSize = 13f
            loggingCb.isChecked = loggingOn
            loggingListener = android.widget.CompoundButton.OnCheckedChangeListener { _, on -> toggleLogging(on) }
            loggingCb.setOnCheckedChangeListener(loggingListener)
            addView(loggingCb)
            addView(spacer(6))
            logTv = text("", 12f, parse("#FFB0BEC5"), mono = true)
            addView(logTv)
        })
    }

// ---------------------------------------------------------------- helpers

    private fun dpf(v: Float) = v * resources.displayMetrics.density

    private fun dpf(v: Int) = v * resources.displayMetrics.density

    private fun dpi(v: Float) = (v * resources.displayMetrics.density).roundToInt()

    private fun dpi(v: Int) = (v * resources.displayMetrics.density).roundToInt()

    private fun parse(hex: String) = Color.parseColor(hex)

    private fun card(builder: LinearLayout.() -> Unit): LinearLayout {
        val c = LinearLayout(context)
        c.orientation = VERTICAL
        c.setBackgroundResource(R.drawable.card_bg)
        c.setPadding(dpi(14), dpi(12), dpi(14), dpi(12))
        val lp = LayoutParams(mP, wP)
        lp.setMargins(dpi(12), dpi(6), dpi(12), dpi(6))
        c.layoutParams = lp
        c.builder()
        return c
    }

    private fun sectionTitle(s: String): TextView {
        val t = text(s, 15f, parse("#FF90CAF9"), bold = true)
        t.setPadding(0, dpi(2), 0, dpi(2))
        return t
    }

    private fun text(s: String, size: Float = 14f, color: Int = parse("#FFE0E0E0"),
                     bold: Boolean = false, mono: Boolean = false): TextView {
        val t = TextView(context)
        t.text = s
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
        t.setTextColor(color)
        if (bold) t.setTypeface(null, Typeface.BOLD)
        if (mono) t.typeface = Typeface.MONOSPACE
        return t
    }

    private fun spacer(h: Int): View {
        val v = View(context)
        v.layoutParams = LayoutParams(1, dpi(h))
        return v
    }

    private fun row(vararg views: View, gravity: Int = Gravity.CENTER_VERTICAL): LinearLayout {
        val l = LinearLayout(context)
        l.orientation = HORIZONTAL
        l.gravity = gravity
        views.forEachIndexed { i, v ->
            if (i < views.size - 1) {
                val lp = v.layoutParams as? LayoutParams
                    ?: LayoutParams(wP, wP)
                lp.marginEnd = dpi(8)
                v.layoutParams = lp
            }
            l.addView(v)
        }
        return l
    }

private fun filledBtn(label: String, onClick: () -> Unit): Button {
        val b = Button(context)
        b.text = label
        b.isAllCaps = false
        b.setTextColor(parse("#FF90CAF9"))
        b.textSize = 14f
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = dpf(8)
        g.setColor(parse("#FF242424"))
        g.setStroke(dpi(1), parse("#FF5C6BC0"))
        b.background = g
        b.setOnClickListener { onClick() }
        return b
    }

    private fun outlinedBtn(label: String, onClick: () -> Unit): Button {
        val b = Button(context)
        b.text = label
        b.isAllCaps = false
        b.setTextColor(parse("#FF90CAF9"))
        b.textSize = 14f
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = dpf(8)
        g.setColor(parse("#FF242424"))
        g.setStroke(dpi(1), parse("#FF5C6BC0"))
        b.background = g
        b.setOnClickListener { onClick() }
        return b
    }

    // ---------------------------------------------------------------- shell glue

private fun daemonPid(): String = Su.run("pidof chgd 2>/dev/null").out

    /** One snapshot of everything the Status screen shows in ONE su call. */
    private data class Snapshot(
        val rootOk: Boolean,
        val daemon: String,
        val status: LedStatus,
        val conf: LedConf?,
        val log: String,
        val logging: Boolean,
        val watchdogMs: Long,
        val bridgeConnected: Boolean,
        val imax: Int
    )

    private fun collectAll(): Snapshot {
        val out = Su.run(
            "echo 'U='\$(id -u);" +
                " echo 'D='\$(pidof chgd);" +
                " echo 'S<<'; cat /data/local/tmp/led_status 2>/dev/null; echo 'S>>';" +
                " echo 'L<<'; tail -n 25 /data/local/tmp/ledd.log 2>/dev/null; echo 'L>>';" +
                " echo 'C<<'; cat /data/adb/modules/led_hal_root/led.conf 2>/dev/null; echo 'C>>';" +
                " echo 'G='\$(grep -o '^logging=[01]' /data/adb/modules/led_hal_root/led.conf 2>/dev/null | head -1);" +
                " echo 'W='\$(grep -o '^watchdog_ms=[0-9]*' /data/adb/modules/led_hal_root/led.conf 2>/dev/null | head -1);" +
                " echo 'I='\$(grep -o '^imax=[0-9]*' /data/adb/modules/led_hal_root/led.conf 2>/dev/null | head -1);" +
                " echo 'N='\$(cat /data/local/tmp/lednls.status 2>/dev/null)"
        ).out

        var rootOk = false
        var daemon = ""
        var status = LedStatus()
        var conf: LedConf? = null
        var log = ""
        var logging = true
        var watchdogMs = 0L
        var bridgeConnected = false
        var imax = 0
        var section = ""
        val sb = StringBuilder()
        for (line in out.lineSequence()) {
            when {
                line == "U=0" -> rootOk = true
                line.startsWith("D=") -> daemon = line.substring(2)
                line.startsWith("G=logging=0") -> logging = false
                line.startsWith("G=logging=1") -> logging = true
                line.startsWith("W=watchdog_ms=") -> watchdogMs = line.substringAfter("=").toLongOrNull() ?: 0L
                line.startsWith("I=imax=") -> imax = line.substringAfter("=").toIntOrNull() ?: 0
                line.startsWith("N=connected=1") -> bridgeConnected = true
                line.startsWith("N=connected=0") -> bridgeConnected = false
                line == "S<<" -> { section = "S"; sb.clear() }
                line == "S>>" -> { status = LedStatusReader.parseStatus(sb.toString()); section = "" }
                line == "L<<" -> { section = "L"; sb.clear() }
                line == "L>>" -> { log = sb.toString(); section = "" }
                line == "C<<" -> { section = "C"; sb.clear() }
                line == "C>>" -> {
                    conf = LedConf().apply { parse(sb.toString()) }
                    section = ""
                }
                section == "S" -> sb.appendLine(line)
                section == "L" -> sb.appendLine(line)
                section == "C" -> sb.appendLine(line)
            }
        }
        val snap = Snapshot(rootOk, daemon, status, conf, log, logging, watchdogMs, bridgeConnected, imax)
        // The poll already fetched led.conf - hand it to the shared settings
        // cache so config tabs first paint instantly (no per-tab su roundtrip).
        if (rootOk && conf != null) LedConf.updateCache(conf)
        return snap
    }

    private fun logTail(n: Int): String =
        Su.run("tail -n $n /data/local/tmp/ledd.log 2>/dev/null").out

    private fun hook(sig: String) {
        Su.run("kill -$sig \$(pidof chgd) 2>/dev/null")
    }

private fun probeRoot(): RootState {
        // Persistent shell first. Blank answer from a LIVE shell = su spawned
        // but is silent (the manager's dialog is up, waiting).
        suUnreachable = false
        return try {
            val out = SuShell.exec("id", 8000)
            when {
                out.contains("uid=0") -> RootState.GRANTED
                out.isBlank() -> RootState.PENDING
                else -> RootState.DENIED
            }
        } catch (_: IllegalStateException) {
            // No shell at all: su unreachable from this app's namespace
            // (KernelSU hides it until the app is allowlisted). One-shot
            // probe over every candidate confirms it.
            val r = Su.probeRootAny("id", 8)
            when {
                r.pending -> RootState.PENDING
                r.out.contains("uid=0") -> RootState.GRANTED
                else -> {
                    suUnreachable = true
                    RootState.DENIED
                }
            }
        }
    }

/** Find and launch the KernelSU manager (any installed superuser app), or
     *  the generic launcher fallback. Never throws; shows the outcome on the
     *  root hint line so a dead tap can't look silent. Package visibility is
     *  covered by the manifest <queries> block. */
    private fun openRootManager(ctx: Context) {
        val pm = ctx.packageManager
        val known = listOf(
            "me.weishu.kernelsu",       // KernelSU / KernelSU Next
            "io.github.qingyi1.kernelsu",
            "com.topjohnwu.magisk",     // Magisk
            "me.bmax.apatch",           // APatch
            "eu.chainfire.supersu"      // classic SuperSU
        )
        val found = runCatching {
            var pkg = known.firstOrNull { pm.getLaunchIntentForPackage(it) != null }
            if (pkg == null) {
                pkg = pm.queryIntentActivities(
                    Intent(Intent.ACTION_MAIN).addCategory(Intent.CATEGORY_LAUNCHER), 0
                ).map { it.activityInfo.packageName }
                    .firstOrNull { name ->
                        name.contains("kernelsu", true) || name.contains("ksu", true) ||
                            name.contains("magisk", true) || name.contains("apatch", true) ||
                            name.contains("superuser", true)
                    }
            }
            pkg
        }.getOrNull()

        val ok = found?.let { pkg ->
            runCatching {
                val it = pm.getLaunchIntentForPackage(pkg)
                if (it == null) false
                else {
                    it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                    ctx.startActivity(it)
                    true
                }
            }.getOrDefault(false)
        } ?: false

        rootHintTv.text = when {
            ok -> ""
            found == null -> "no root manager installed - add LED GUI under Settings " +
                "> Superuser in KernelSU Manager once it exists."
            else -> "launch of $found failed (startActivity exception)"
        }
    }
}

