package com.bastet.ledgui

import android.app.AlertDialog
import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.text.Editable
import android.text.InputType
import android.text.TextWatcher
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.ListView
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt
import java.util.Locale

/** AW2033 color simulation for previews. The chip scales each channel's
 *  drive current (0..15) against the color's PWM amplitude (0..255); the
 *  eye sees that optical output through each LED's own luminous weight
 *  and a perception curve. [Cal] is driven by [preview] in led.conf and
 *  set from LedConf.parse - green=100 is the reference weight.
 *  syncMode=true models LCFG0.SYNC: the master (red) PWM drives every
 *  channel while per-channel current stays alive. */
object LedSim {
    class Cal(val r: Double, val g: Double, val b: Double, val gamma: Double)

    @Volatile
    var cal = Cal(0.5, 1.0, 0.8, 2.2)

    fun rgbWithCurrent(c: Triple<Int, Int, Int>, cur: Triple<Int, Int, Int>, syncMode: Boolean = false): Triple<Int, Int, Int> {
        val w = arrayOf(cal.r, cal.g, cal.b)
        fun ch(i: Int): Int {
            val duty = if (syncMode) c.first else when (i) { 0 -> c.first; 1 -> c.second; else -> c.third }
            val opt = duty / 255.0 * (when (i) { 0 -> cur.first; 1 -> cur.second; else -> cur.third }) / 15.0 * w[i]
            return (255.0 * Math.pow(opt, 1.0 / cal.gamma).coerceIn(0.0, 1.0)).roundToInt().coerceIn(0, 255)
        }
        return Triple(ch(0), ch(1), ch(2))
    }
}

/**
 * Shared classic-Views widget kit + a common config-page base, so the
 * three settings tabs (Charge / Notification / Call) share every editor
 * and the load/save/reload plumbing instead of copy-pasting it.
 *
 * A ConfPage carries one ScrollView of cards and a pinned Save/Reload
 * footer. Subclasses only declare their fields and wire them onto the
 * shared LedConf in applyTo()/collectFrom().
 */
abstract class ConfPage(context: Context) : LinearLayout(context) {

    protected val mP = android.view.ViewGroup.LayoutParams.MATCH_PARENT
    protected val wP = android.view.ViewGroup.LayoutParams.WRAP_CONTENT

    protected val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    /** Pinned status banner above the scroll area, shown on every ConfPage
     *  (Charge / Notification / Call / VoIP / Alarm) for a few seconds after
     *  save / reload. Lives outside the ScrollView so it can never scroll away. */
    private lateinit var msgBanner: TextView
    private var msgHideJob: Job? = null

    init {
        orientation = VERTICAL
        setBackgroundColor(parse("#FF121212"))
        buildShell()
        // buildBody()/reload() must run as posts: subclass fields that buildBody()
        // touches are only initialized after this super-constructor returns.
        post { buildBody() }
        post { initData() }
    }

    /** First paint: read the app-wide settings cache (warm from the Info
     *  poll or an earlier page). Only when nothing is cached yet do we hit
     *  the device file - exactly once, never again on repeated tab
     *  switches, so the screen never re-paints over your edits. */
    private fun initData() {
        val cached = LedConf.cached()
        if (cached != null) {
            applyTo(cached)
        } else {
            reload()
            selfHealWhenRoot()
        }
    }

    /** This page was built before root answered (cold start right after a
     *  grant, adbd restart, module reinstall). The old code loaded exactly
     *  once at build time, so an empty page stayed empty - the app-wide
     *  cache only ever got warm from the Info poll or a manual Reload, and
     *  the Info poll only runs while that tab is VISIBLE. Retry in the
     *  background until the device config answers, then paint once through
     *  the shared cache so every page gets the same data. Bounded (~2 min)
     *  so a truly rootless page does not hammer su forever. */
    private fun selfHealWhenRoot() {
        scope.launch {
            for (attempt in 0 until 90) {
                delay(1500)
                val c = LedConf.cached()
                if (c != null) {
                    applyTo(c)
                    return@launch
                }
                val fresh = withContext(Dispatchers.IO) { LedConf.loadOrNull() }
                if (fresh != null) {
                    LedConf.updateCache(fresh)
                    applyTo(fresh)
                    return@launch
                }
            }
        }
    }

    private lateinit var saveBtn: Button
    private lateinit var reloadBtn: Button

    private fun buildShell() {
        // Everything (scroll + footer) lives in one FrameLayout; the status
        // banner is a SIBLING on top of it. Unlike an in-flow row, an overlay
        // never re-flows the page when it appears - content stays put whether
        // you are at the top of the scroll or scrolled down.
        val frame = android.widget.FrameLayout(context)
        addView(frame, LayoutParams(mP, mP))

        val col = LinearLayout(context)
        col.orientation = VERTICAL
        frame.addView(col, android.widget.FrameLayout.LayoutParams(mP, mP))

        val scroll = ScrollView(context)
        scroll.isFillViewport = true
        val content = LinearLayout(context)
        content.orientation = VERTICAL
        content.setPadding(0, dpi(4), 0, dpi(8))
        scroll.addView(content, LayoutParams(mP, wP))
        col.addView(scroll, LayoutParams(mP, 0, 1f))
        body = content

        val foot = LinearLayout(context)
        foot.orientation = HORIZONTAL
        foot.setPadding(dpi(12), dpi(8), dpi(12), dpi(8))
        saveBtn = filledBtn("Save to device") { save() }
        reloadBtn = outlinedBtn("Reload") { reload() }
        foot.addView(row(saveBtn, reloadBtn))
        col.addView(foot, LayoutParams(mP, wP))

        msgBanner = text("", 12f, parse("#FF90CAF9"), mono = true)
        msgBanner.setPadding(dpi(12), dpi(8), dpi(12), dpi(8))
        msgBanner.setBackgroundResource(R.drawable.card_bg)
        msgBanner.visibility = GONE
        val blp = android.widget.FrameLayout.LayoutParams(mP, wP, Gravity.TOP)
        blp.setMargins(dpi(12), dpi(6), dpi(12), 0)
        msgBanner.layoutParams = blp
        // added last => topmost child: floats above the scroll area
        frame.addView(msgBanner)
    }

    /** Show [text] pinned at the top of the visible area for a few seconds.
     *  Errors stay twice as long. A new call replaces the previous message. */
    protected fun showMsg(text: String, error: Boolean = false) {
        msgHideJob?.cancel()
        msgBanner.text = text
        msgBanner.setTextColor(parse(if (error) "#FFFF8A80" else "#FF90CAF9"))
        msgBanner.visibility = VISIBLE
        msgHideJob = scope.launch {
            delay(if (error) 8_000L else 5_000L)
            msgBanner.visibility = GONE
        }
    }

    protected lateinit var body: LinearLayout

    protected abstract fun buildBody()
    protected abstract fun applyTo(c: LedConf)
    protected abstract fun collectFrom(c: LedConf)

    /** One-line summary shown after a successful load. */
    protected open fun loadSummary(c: LedConf): String = ""

    private fun setBtnBusy(btn: Button, busy: Boolean, label: String) {
        btn.isEnabled = !busy
        btn.alpha = if (busy) 0.5f else 1f
        btn.text = if (busy) label else btn.tag as? String ?: btn.text
    }

    private fun reload() {
        val origTag = reloadBtn.tag as? String ?: reloadBtn.text.toString()
        reloadBtn.tag = origTag
        setBtnBusy(reloadBtn, true, "Loading...")
        scope.launch {
            var c: LedConf? = null
            for (attempt in 0 until 24) {
                val root = withContext(Dispatchers.IO) { Su.run("id") }
                var conf: LedConf? = null
                if (root.ok && root.out.contains("uid=0")) {
                    conf = withContext(Dispatchers.IO) { LedConf.loadOrNull() }
                }
                if (conf != null) {
                    c = conf
                    break
                }
                delay(500)
            }
            setBtnBusy(reloadBtn, false, "")
            if (c == null) {
                showMsg(
                    "load failed: root not granted yet - " +
                        "config auto-loads once the grant lands.", error = true
                )
                return@launch
            }
            applyTo(c)
            LedConf.updateCache(c)
        }
    }

    private fun save() {
        val origTag = saveBtn.tag as? String ?: saveBtn.text.toString()
        saveBtn.tag = origTag
        setBtnBusy(saveBtn, true, "Saving...")
        scope.launch {
            // Save must merge into the REAL device config, never into
            // constructor defaults: a page only owns its own fields, and
            // writing constructor defaults over it is what wiped led.conf.
            val base = withContext(Dispatchers.IO) {
                LedConf.loadOrNull()
            } ?: run {
                setBtnBusy(saveBtn, false, "")
                showMsg("Save ABORTED: cannot read device led.conf (root not ready?). Press Reload first.", error = true)
                return@launch
            }
            collectFrom(base)
            val msg = withContext(Dispatchers.IO) {
                val res = base.save()
                if (res.ok) {
                    LedConf.updateCache(base)
                    Su.run("kill -ALRM \$(pidof chgd) 2>/dev/null")
                    "Saved OK. Daemon reloaded (SIGALRM) - applied now."
                } else {
                    "Save FAILED (exit ${res.code}): ${res.out.ifBlank { "no su / no write?" }}"
                }
            }
            setBtnBusy(saveBtn, false, "")
            showMsg(msg, error = !msg.startsWith("Saved"))
        }
    }

    // ------------------------------------------------------------ helpers

    protected fun dpf(v: Float) = v * resources.displayMetrics.density
    protected fun dpf(v: Int) = v * resources.displayMetrics.density
    protected fun dpi(v: Float) = (v * resources.displayMetrics.density).roundToInt()
    protected fun dpi(v: Int) = (v * resources.displayMetrics.density).roundToInt()
    protected fun parse(hex: String) = Color.parseColor(hex)

    protected fun card(builder: LinearLayout.() -> Unit): LinearLayout {
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

    protected fun sectionTitle(s: String): TextView {
        val t = text(s, 15f, parse("#FF90CAF9"), bold = true)
        t.setPadding(0, dpi(2), 0, dpi(2))
        return t
    }

    protected fun text(s: String, size: Float = 14f, color: Int = parse("#FFE0E0E0"),
                       bold: Boolean = false, mono: Boolean = false): TextView {
        val t = TextView(context)
        t.text = s
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, size)
        t.setTextColor(color)
        if (bold) t.setTypeface(null, Typeface.BOLD)
        if (mono) t.typeface = Typeface.MONOSPACE
        return t
    }

    protected fun spacer(h: Int): View {
        val v = View(context)
        v.layoutParams = LayoutParams(1, dpi(h))
        return v
    }

    protected fun row(vararg views: View, gravity: Int = Gravity.CENTER_VERTICAL): LinearLayout {
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

    protected fun filledBtn(label: String, onClick: () -> Unit): Button {
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

    protected fun outlinedBtn(label: String, onClick: () -> Unit): Button {
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

    protected fun multiLine(hint: String): EditText {
        val e = EditText(context)
        e.hint = hint
        e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
        e.setTextColor(parse("#FFE0E0E0"))
        e.setHintTextColor(parse("#FF727272"))
        e.typeface = Typeface.MONOSPACE
        e.setMinLines(4)
        e.gravity = Gravity.TOP or Gravity.START
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = dpf(8)
        g.setColor(parse("#FF2A2A2A"))
        g.setStroke(dpi(1), parse("#FF333333"))
        e.background = g
        e.setPadding(dpi(8), dpi(6), dpi(8), dpi(6))
        return e
    }



    // ------------------------------------------------------------ widgets

    protected inner class NumField(label: String, init: String) : LinearLayout(context) {
        private val edit = EditText(context)

        init {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            val l = text(label, 13f, parse("#FFB0BEC5"))
            val llp = LayoutParams(dpi(180), wP)
            l.layoutParams = llp
            addView(l)
            edit.setText(init)
            edit.inputType = InputType.TYPE_CLASS_NUMBER
            edit.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
            edit.setTextColor(parse("#FFE0E0E0"))
            edit.setSingleLine(true)
            val g = GradientDrawable()
            g.shape = GradientDrawable.RECTANGLE
            g.cornerRadius = dpf(8)
            g.setColor(parse("#FF2A2A2A"))
            g.setStroke(dpi(1), parse("#FF333333"))
            edit.background = g
            edit.setPadding(dpi(8), dpi(4), dpi(8), dpi(4))
            addView(edit, LayoutParams(0, wP, 1f))
            setPadding(0, dpi(3), 0, dpi(3))
        }

        fun setText(v: String) { edit.setText(v) }
        fun getInt(def: Int): Int = edit.text.toString().toIntOrNull() ?: def
        fun getLong(def: Long): Long = edit.text.toString().toLongOrNull() ?: def
    }

    protected fun LinearLayout.numRow(label: String, init: String): NumField {
        val f = NumField(label, init)
        addView(f)
        return f
    }

    /** Named checkbox row (dark-theme match for the RenderCard knobs). */
    protected fun LinearLayout.syncCheck(label: String): CheckBox {
        val cb = CheckBox(context)
        cb.text = label
        cb.setTextColor(parse("#FFB0BEC5"))
        cb.textSize = 13f
        cb.isChecked = false
        addView(cb)
        return cb
    }

    /** Generic radio group over arbitrary names - the LED tab uses it for
     *  the [led] mode (off/solid/breath/wave) and Imax (5/10/15/30) picks. */
    protected inner class NamePicker(
        label: String,
        names: List<String>,
        onPick: (Int) -> Unit
    ) : LinearLayout(context) {
        private val count = names.size
        private val group = RadioGroup(context)

        init {
            orientation = VERTICAL
            addView(text(label, 13f, parse("#FFB0BEC5")))
            group.orientation = HORIZONTAL
            names.forEachIndexed { i, n ->
                val rb = RadioButton(context)
                rb.id = i
                rb.text = n
                rb.setTextColor(parse("#FFE0E0E0"))
                rb.setPadding(0, 0, dpi(10), 0)
                group.addView(rb)
            }
            group.check(0)
            group.setOnCheckedChangeListener { _, _ ->
                onPick(group.checkedRadioButtonId.coerceIn(0, count - 1))
            }
            addView(group)
            setPadding(0, dpi(3), 0, dpi(3))
        }

        fun set(at: Int) {
            val idx = at.coerceIn(0, count - 1)
            if (group.checkedRadioButtonId != idx) group.check(idx)
        }

        fun get(): Int = group.checkedRadioButtonId.coerceIn(0, count - 1)
    }

    protected inner class RgbPicker(label: String) : LinearLayout(context) {
        private var color = Triple(255, 0, 0)

        /** Per-channel LED current (0..15) the event's chip runs at. The
         *  swatch previews the optical result (weight + gamma), so the user
         *  sees the light the combination actually produces. */
        var previewCur: Triple<Int, Int, Int> = Triple(15, 15, 15)
            set(v) {
                field = v
                paintSwatch()
            }

        /** true = LCFG0.SYNC armed: the master (red) PWM drives all three
         *  LEDs, the picker's G/B sliders are locked to red. */
        var pwmSync: Boolean = false
            set(v) {
                field = v
                paintSwatch()
            }

        private val swatch = View(context)
        private val sliders = mutableListOf<SliderView>()
        private val valueTvs = mutableListOf<TextView>()

        init {
            orientation = VERTICAL
            val head = row(
                text(label, 13f, parse("#FFB0BEC5")),
                swatch
            )
            addView(head)
            val sw = GradientDrawable()
            sw.shape = GradientDrawable.RECTANGLE
            sw.cornerRadius = dpf(4)
            swatch.background = sw
            swatch.layoutParams = LayoutParams(dpi(22), dpi(22))
            val channels = listOf("R", "G", "B")
            channels.forEachIndexed { i, name ->
                val vt = text(intComponent(i).toString(), 13f, parse("#FFE0E0E0"), mono = true)
                vt.layoutParams = LayoutParams(dpi(36), wP)
                val sl = SliderView(channelColor(i), intComponent(i)) { v ->
                    color = withComponent(i, v)
                    valueTvs[i].text = colorOf(i).toString()
                    paintSwatch()
                }
                sl.layoutParams = LayoutParams(0, dpi(50), 1f)
                sliders.add(sl)
                valueTvs.add(vt)
                addView(row(text(name, 13f, parse("#FF90CAF9"), bold = true), sl, vt))
            }
            setPadding(0, dpi(3), 0, dpi(3))
            paintSwatch()
        }

        private fun colorOf(i: Int): Int = when (i) {
            0 -> color.first; 1 -> color.second; else -> color.third
        }

        private fun channelColor(i: Int): Int = when (i) {
            0 -> parse("#FFE05A4E")
            1 -> parse("#FF62B86B")
            else -> parse("#FF4E9BE0")
        }

        private fun intComponent(i: Int): Int = when (i) {
            0 -> color.first; 1 -> color.second; else -> color.third
        }

        private fun withComponent(i: Int, v: Int): Triple<Int, Int, Int> = when (i) {
            0 -> Triple(v, color.second, color.third)
            1 -> Triple(color.first, v, color.third)
            else -> Triple(color.first, color.second, v)
        }

        private fun paintSwatch() {
            val g = swatch.background as? GradientDrawable ?: return
            val s = LedSim.rgbWithCurrent(color, previewCur, pwmSync)
            g.setColor(Color.rgb(s.first, s.second, s.third))
        }

        /** Lock/unlock one channel's slider (sync keeps only red live). */
        fun setChannelEnabled(i: Int, enabled: Boolean) {
            sliders[i].interactive = enabled
            valueTvs[i].setTextColor(parse(if (enabled) "#FFE0E0E0" else "#FF727272"))
        }

        fun setColor(c: Triple<Int, Int, Int>) {
            color = c
            for (i in 0..2) {
                sliders[i].progress = colorOf(i)
                valueTvs[i].text = colorOf(i).toString()
            }
            paintSwatch()
        }

        fun getColor(): Triple<Int, Int, Int> = color
    }



    /** Minimal cheap slider: a single View drawn straight in onDraw. */
    protected inner class SliderView(
        private val channelColor: Int,
        initial: Int,
        private val onMove: (Int) -> Unit
    ) : View(context) {
        var progress: Int = initial.coerceIn(0, 255)
            set(v) {
                field = v.coerceIn(0, 255)
                invalidate()
            }

        /** false = locked channel (sync): touch is ignored, drawn grey. */
        var interactive = true
            set(v) {
                field = v
                invalidate()
            }

        private val trackPaint = Paint().apply { color = parse("#FF333A46"); isAntiAlias = true }
        private val fillPaint = Paint().apply { color = channelColor; isAntiAlias = true }
        private val fillOffPaint = Paint().apply { color = parse("#FF3A3A3A"); isAntiAlias = true }
        private val thumbPaint = Paint().apply { color = parse("#FFF0F0F0"); isAntiAlias = true }
        private val thumbOffPaint = Paint().apply { color = parse("#FF8A8A8A"); isAntiAlias = true }

        init {
            setMinimumHeight(dpi(50))
            setMinimumWidth(dpi(80))
        }

        override fun onDraw(canvas: Canvas) {
            val t = dpi(6).toFloat()
            val cy = height / 2f
            val pad = dpi(14).toFloat()
            val left = pad
            val right = (width - pad).coerceAtLeast(left + 1f)
            val range = right - left
            val thumbX = left + range * (progress / 255f)
            canvas.drawRoundRect(RectF(left, cy - t / 2f, right, cy + t / 2f), t / 2f, t / 2f, trackPaint)
            if (interactive) {
                canvas.drawRoundRect(RectF(left, cy - t / 2f, thumbX, cy + t / 2f), t / 2f, t / 2f, fillPaint)
                canvas.drawCircle(thumbX, cy, dpi(11).toFloat(), thumbPaint)
            } else {
                canvas.drawRoundRect(RectF(left, cy - t / 2f, thumbX, cy + t / 2f), t / 2f, t / 2f, fillOffPaint)
                canvas.drawCircle(thumbX, cy, dpi(11).toFloat(), thumbOffPaint)
            }
        }

        override fun onTouchEvent(e: MotionEvent): Boolean {
            if (!interactive) return false
            when (e.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    parent?.requestDisallowInterceptTouchEvent(true)
                    updateFromX(e.x)
                    return true
                }
                MotionEvent.ACTION_MOVE -> updateFromX(e.x)
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    parent?.requestDisallowInterceptTouchEvent(false)
                }
            }
            return true
        }

        private fun updateFromX(x: Float) {
            val pad = dpi(14).toFloat()
            val left = pad
            val right = (width - pad).coerceAtLeast(left + 1f)
            val v = ((x - left) / (right - left) * 255f)
                .coerceIn(0f, 255f).toInt()
            if (v != progress) {
                progress = v
                onMove(v)
            }
        }
    }

    /** Fixed-height internal-scroll list used inside a card: the card keeps
     *  its size and a long list scrolls INSIDE the box (like the old
     *  multiline text field) instead of stretching the page. Returns the
     *  inner LinearLayout to fill with rows. */
    protected fun LinearLayout.scrollListBox(heightDp: Int = 176, stretch: Boolean = false): LinearLayout {
        val frame = FrameLayout(context)
        val scroll = object : ScrollView(context) {
            override fun onTouchEvent(ev: MotionEvent?): Boolean {
                // The page ScrollView steals any vertical drag at touch-slop
                // BEFORE this box becomes dragged (its intercept runs first),
                // then clamps to zero and the gesture dies. When the box
                // content overflows, block ancestor interception for this
                // gesture at DOWN so the box itself receives the full drag.
                if (ev?.action == MotionEvent.ACTION_DOWN) {
                    val c = getChildAt(0)
                    if (c != null && c.height > height - paddingTop - paddingBottom) {
                        parent?.requestDisallowInterceptTouchEvent(true)
                    }
                }
                return super.onTouchEvent(ev)
            }
        }
        // NO isFillViewport: with it the wrapper re-measures a wrap-content
        // child to the exact viewport, clips the overflow and the box can
        // never scroll on this ROM. Without it the child keeps its natural
        // height and the box scrolls; the box itself stays 176dp via its
        // own layout params.
        val list = LinearLayout(context)
        list.orientation = VERTICAL
        list.setPadding(dpi(2), dpi(2), dpi(2), dpi(2))
        scroll.addView(list, LayoutParams(mP, wP))

        // Scroll fades on the box edges: they show ONLY while content is
        // clipped on that side (bottom = more below, top = already scrolled).
        // Pinned to the frame so they stay put while the list scrolls.
        val shadowH = dpi(22)
        val cardBg = parse("#FF1E1E1E")
        val clear = cardBg and 0x00FFFFFF
        fun fadeUp(): GradientDrawable =
            GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(cardBg, clear))
        fun fadeDown(): GradientDrawable =
            GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM, intArrayOf(clear, cardBg))
        val shadowTop = View(context).apply { background = fadeUp() }
        val shadowBottom = View(context).apply { background = fadeDown() }
        shadowTop.visibility = GONE
        shadowBottom.visibility = GONE
        fun updateShadows() {
            val c = scroll.getChildAt(0) ?: return
            val maxY = c.height - (scroll.height - scroll.paddingTop - scroll.paddingBottom)
            shadowTop.visibility = if (scroll.scrollY > 0) VISIBLE else GONE
            shadowBottom.visibility = if (scroll.scrollY < maxY) VISIBLE else GONE
        }
        scroll.setOnScrollChangeListener { _, _, _, _, _ -> updateShadows() }
        list.addOnLayoutChangeListener { _, _, _, _, _, _, _, _, _ -> updateShadows() }

        frame.addView(scroll, FrameLayout.LayoutParams(mP, mP))
        frame.addView(shadowTop, FrameLayout.LayoutParams(mP, shadowH, Gravity.TOP))
        frame.addView(shadowBottom, FrameLayout.LayoutParams(mP, shadowH, Gravity.BOTTOM))
        if (stretch) {
            // fill the parent card instead of a fixed height: the parent
            // LinearLayout owns the exact height, so weighted stretch is
            // measured EXACT and never hits the page-ScrollView wrap-quirk.
            addView(frame, LayoutParams(mP, 0, 1f))
        } else {
            addView(frame, LayoutParams(mP, dpi(heightDp)))
        }
        return list
    }

    /** Re-measure was NOT needed: a non-MATCH_PARENT child of a ScrollView
     *  is measured UNSPECIFIED on this ROM too, so wrap-content rows keep
     *  their natural height (child 775px in a 422px box = real range).
     *  The scroll blocker was the outer page ScrollView stealing vertical
     *  drags at touch-slop; fixed in scrollListBox via
     *  requestDisallowInterceptTouchEvent on overflow. */
    protected fun swatchView(sizeDp: Int): View {
        val v = View(context)
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = dpf(4)
        v.background = g
        v.layoutParams = LayoutParams(dpi(sizeDp), dpi(sizeDp))
        return v
    }

    /** Two-line "label + mono package" column for app list rows.
     *  dim renders the row grey - used for packages already present
     *  in a target list so the picker still shows and finds them. */
    protected fun appLine(label: String, pkg: String, dim: Boolean = false): LinearLayout {
        val col = LinearLayout(context)
        col.orientation = VERTICAL
        col.addView(text(label, 14f, parse(if (dim) "#FF727272" else "#FFE0E0E0")))
        val pv = text(pkg, 11f, parse("#FF727272"), mono = true)
        pv.setSingleLine(true)
        pv.ellipsize = android.text.TextUtils.TruncateAt.END
        col.addView(pv)
        col.layoutParams = LayoutParams(0, wP, 1f)
        return col
    }

    /** Small circular delete chip for list rows. */
    protected fun iconBtn(symbol: String, onClick: () -> Unit): TextView {
        val t = TextView(context)
        t.text = symbol
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15f)
        t.setTextColor(parse("#FFFF8A80"))
        t.gravity = Gravity.CENTER
        val g = GradientDrawable()
        g.shape = GradientDrawable.OVAL
        g.setColor(parse("#FF2A2A2A"))
        g.setStroke(dpi(1), parse("#FF4A4A4A"))
        t.background = g
        t.layoutParams = LayoutParams(dpi(30), dpi(30))
        t.setOnClickListener { onClick() }
        return t
    }

    /** All installed apps (label by package), excluding the GUI itself.
     *  Shared by the picker and the rule/suppress row labels. Labels fall
     *  back to the package name. */
    protected fun installedAppCatalog(): Map<String, String> = runCatching {
        val pm = context.packageManager
        pm.getInstalledApplications(0)
            .filter { !it.packageName.startsWith("com.bastet.ledgui") }
            .associate { a ->
                a.packageName to (
                    runCatching { pm.getApplicationLabel(a).toString() }.getOrNull()
                        ?.takeUnless { it.isBlank() } ?: a.packageName
                    )
            }
    }.getOrElse { emptyMap() }

    /** Modal pick of an installed app: searchable list with icon + label +
     *  package. Already-present packages (exclude) stay VISIBLE but greyed
     *  with an "(added)" suffix and pick is refused - so the state of the
     *  target list is obvious and search still finds them. */
    protected fun pickApp(title: String, exclude: Set<String>, onPick: (String, String) -> Unit) {
        val pm = context.packageManager
        val infos = runCatching {
            pm.getInstalledApplications(0).filter {
                !it.packageName.startsWith("com.bastet.ledgui")
            }
        }.getOrElse { emptyList() }
        // (ApplicationInfo, label, added)
        val apps = infos.mapNotNull { a ->
            val label = runCatching { pm.getApplicationLabel(a).toString() }.getOrNull()
                ?.takeUnless { it.isBlank() } ?: a.packageName
            Triple(a, label, a.packageName in exclude)
        }.sortedWith(compareBy {
            it.second.lowercase(Locale.US)
        })

        val search = EditText(context)
        search.hint = "filter by name or package"
        search.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
        search.setTextColor(parse("#FFE0E0E0"))
        search.setHintTextColor(parse("#FF727272"))
        search.setSingleLine(true)
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = dpf(8)
        g.setColor(parse("#FF2A2A2A"))
        g.setStroke(dpi(1), parse("#FF333333"))
        search.background = g
        search.setPadding(dpi(8), dpi(6), dpi(8), dpi(6))

        val list = ListView(context)
        var shown: List<Triple<android.content.pm.ApplicationInfo, String, Boolean>> = emptyList()
        val adapter = object : android.widget.BaseAdapter() {
            override fun getCount() = shown.size
            override fun getItem(i: Int) = shown[i]
            override fun getItemId(i: Int) = i.toLong()
            override fun getView(i: Int, reuse: View?, parent: ViewGroup): View {
                val (info, label, added) = shown[i]
                val pkg = info.packageName
                val boxCol = LinearLayout(context).apply {
                    orientation = HORIZONTAL
                    gravity = Gravity.CENTER_VERTICAL
                    setPadding(0, dpi(6), 0, dpi(6))
                }
                val ic = ImageView(context)
                ic.setImageDrawable(runCatching { info.loadIcon(pm) }.getOrNull())
                if (added) ic.alpha = 0.4f
                ic.layoutParams = LayoutParams(dpi(22), dpi(22))
                boxCol.addView(ic)
                boxCol.addView(appLine(
                    if (added) "${label} (added)" else label, pkg, dim = added))
                return boxCol
            }
        }
        list.adapter = adapter

        fun refilter() {
            val q = search.text.toString().trim().lowercase(Locale.US)
            shown = if (q.isEmpty()) apps
            else apps.filter { info ->
                info.first.packageName.contains(q) ||
                    info.second.lowercase(Locale.US).contains(q)
            }
            adapter.notifyDataSetChanged()
        }
        search.addTextChangedListener(object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
            override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) { refilter() }
            override fun afterTextChanged(s: Editable?) {}
        })
        refilter()

        val box = LinearLayout(context).apply {
            orientation = VERTICAL
            setPadding(dpi(14), dpi(8), dpi(14), 0)
            addView(search)
            addView(list, LayoutParams(mP, dpi(320)))
        }
        // Default AlertDialog theme is Material LIGHT: our dark-app text
        // colors (light-grey labels) would wash out against the white
        // window and look inverted. Force the dark Material dialog theme.
        val dctx = android.view.ContextThemeWrapper(context, android.R.style.Theme_Material)
        val dialog = AlertDialog.Builder(dctx)
            .setTitle(title)
            .setView(box)
            .setNegativeButton("Cancel") { d, _ -> d.dismiss() }
            .create()
        list.setOnItemClickListener { _, _, pos, _ ->
            val t = shown[pos]
            if (t.third) {
                android.widget.Toast.makeText(context,
                    "Already in the list", android.widget.Toast.LENGTH_SHORT).show()
                return@setOnItemClickListener
            }
            dialog.dismiss()
            onPick(t.first.packageName, t.second)
        }
        dialog.show()
    }

    /** Modal RGB picker (3 sliders + live hex) reused for list entries. */
    protected fun pickColor(title: String, initial: Triple<Int, Int, Int>, onApply: (Triple<Int, Int, Int>) -> Unit) {
        var color = initial
        val sw = swatchView(28)
        val hexTv = text("", 12f, parse("#FF90CAF9"), mono = true)
        val box = LinearLayout(context).apply {
            orientation = VERTICAL
            setPadding(dpi(4), dpi(4), dpi(4), dpi(4))
            addView(row(sw))
            addView(hexTv)
        }
        val vals = mutableListOf<TextView>()
        val channelPaint = listOf(
            parse("#FFE05A4E"), parse("#FF62B86B"), parse("#FF4E9BE0")
        )
        fun comp(i: Int) = when (i) {
            0 -> color.first; 1 -> color.second; else -> color.third
        }
        fun paint() {
            (sw.background as? GradientDrawable)?.setColor(
                Color.rgb(color.first, color.second, color.third)
            )
            hexTv.text = String.format(Locale.US, "#%02X%02X%02X",
                color.first, color.second, color.third)
        }
        listOf("R", "G", "B").forEachIndexed { i, name ->
            val vt = text(comp(i).toString(), 13f, parse("#FFE0E0E0"), mono = true)
            vt.layoutParams = LayoutParams(dpi(36), wP)
            vals.add(vt)
            val sl = SliderView(channelPaint[i], comp(i)) { v ->
                color = when (i) {
                    0 -> Triple(v, color.second, color.third)
                    1 -> Triple(color.first, v, color.third)
                    else -> Triple(color.first, color.second, v)
                }
                vals[i].text = comp(i).toString()
                paint()
            }
            sl.layoutParams = LayoutParams(0, dpi(50), 1f)
            box.addView(row(text(name, 13f, parse("#FF90CAF9"), bold = true), sl, vt))
        }
        paint()
        // Same dark Material dialog theme as the app picker (the stock
        // light dialog would wash out the dark-app colors).
        val dctx = android.view.ContextThemeWrapper(context, android.R.style.Theme_Material)
        AlertDialog.Builder(dctx)
            .setTitle(title)
            .setView(box)
            .setPositiveButton("OK") { _, _ -> onApply(color) }
            .setNegativeButton("Cancel") { d, _ -> d.dismiss() }
            .show()
    }

    /** Labeled row of three small numeric fields (cur r,g,b, phase t0, ...).
     *  Homemade: no external number-picker dependency. */
    protected inner class TripleField(label: String, def: Triple<Int, Int, Int>) : LinearLayout(context) {
        private val edits = mutableListOf<EditText>()

        /** Fired on every keystroke of any of the three fields. */
        var onChanged: (() -> Unit)? = null

        init {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            val l = text(label, 13f, parse("#FFB0BEC5"))
            val llp = LayoutParams(dpi(190), wP)
            l.layoutParams = llp
            addView(l)
            val dims = listOf(def.first.toString(), def.second.toString(), def.third.toString())
            dims.forEach { initText ->
                val e = EditText(context)
                e.setText(initText)
                e.inputType = InputType.TYPE_CLASS_NUMBER
                e.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f)
                e.setTextColor(parse("#FFE0E0E0"))
                e.setSingleLine(true)
                e.gravity = Gravity.CENTER
                e.addTextChangedListener(object : TextWatcher {
                    override fun beforeTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) {}
                    override fun onTextChanged(s: CharSequence?, a: Int, b: Int, c: Int) { onChanged?.invoke() }
                    override fun afterTextChanged(s: Editable?) {}
                })
                val g = GradientDrawable()
                g.shape = GradientDrawable.RECTANGLE
                g.cornerRadius = dpf(8)
                g.setColor(parse("#FF2A2A2A"))
                g.setStroke(dpi(1), parse("#FF333333"))
                e.background = g
                e.setPadding(dpi(4), dpi(4), dpi(4), dpi(4))
                val elp = LayoutParams(0, wP, 1f)
                if (edits.isNotEmpty()) elp.marginStart = dpi(6)
                addView(e, elp)
                edits.add(e)
            }
            setPadding(0, dpi(3), 0, dpi(3))
        }

        fun setTriple(t: Triple<Int, Int, Int>) {
            edits[0].setText(t.first.toString())
            edits[1].setText(t.second.toString())
            edits[2].setText(t.third.toString())
        }

        /** Lock/unlock one field (sync keeps only the red one live). */
        fun setFieldEnabled(i: Int, enabled: Boolean) {
            edits[i].isEnabled = enabled
            edits[i].setTextColor(parse(if (enabled) "#FFE0E0E0" else "#FF727272"))
        }

        fun getTriple(def: Triple<Int, Int, Int>): Triple<Int, Int, Int> = Triple(
            edits[0].text.toString().toIntOrNull() ?: def.first,
            edits[1].text.toString().toIntOrNull() ?: def.second,
            edits[2].text.toString().toIntOrNull() ?: def.third
        )
    }

    protected inner class RenderCard(title: String, hint: String = "", showColor: Boolean = false) : LinearLayout(context) {
        private val modeNames = listOf("off", "solid", "breath", "wave")
        private val timeMax = 8300

        lateinit var mode: NamePicker
            private set
        var color: RgbPicker? = null
            private set
        private var previewTarget: RgbPicker? = null
        var onDriveChanged: (() -> Unit)? = null

        private lateinit var solidCard: LinearLayout
        private lateinit var patternCard: LinearLayout
        lateinit var solidCur: TripleField
            private set
        lateinit var patternRepeat: NumField
            private set
        lateinit var patternCur: TripleField
            private set
        lateinit var patternRise: NumField
            private set
        lateinit var patternHold: NumField
            private set
        lateinit var patternFall: NumField
            private set
        lateinit var patternOfft: NumField
            private set
        lateinit var patternSync: CheckBox
            private set
        lateinit var waveT0: TripleField
            private set

        init {
            orientation = VERTICAL
            addView(card {
                addView(sectionTitle(title))
                if (hint.isNotBlank()) {
                    addView(spacer(2))
                    addView(text(hint, 12f, parse("#FF727272")))
                }
                addView(spacer(4))
                if (showColor) {
                    color = RgbPicker("color (color1) - event color")
                    addView(color!!)
                    addView(spacer(4))
                }
                mode = NamePicker("renderer mode", modeNames) { syncMode() }
                addView(mode)
            })
            solidCard = card {
                addView(sectionTitle("Solid params"))
                addView(spacer(4))
                addView(text("Constant color on this event's channels. Timing = none.", 12f, parse("#FF727272")))
                addView(spacer(2))
                solidCur = TripleField("current r,g,b (0-15)", Triple(15, 15, 15))
                addView(solidCur)
            }
            patternCard = card {
                addView(sectionTitle("Pattern params (breath + wave)"))
                addView(spacer(4))
                addView(text("One chip preset is shared by both animated modes. t0 is used only by wave.", 12f, parse("#FF727272")))
                addView(spacer(2))
                patternRepeat = numRow("repeat (0 = infinite, 1..15)", "0")
                addView(spacer(2))
                patternSync = syncCheck("sync: all channels on master red PWM")
                addView(spacer(2))
                waveT0 = TripleField("wave phase t0 (ms)", Triple(0, 1300, 2600))
                addView(waveT0)
                addView(spacer(2))
                patternCur = TripleField("current r,g,b (0-15)", Triple(15, 15, 15))
                addView(patternCur)
                addView(spacer(2))
                patternRise = numRow("rise (ms, max $timeMax)", "500")
                addView(spacer(2))
                patternHold = numRow("hold (ms, max $timeMax)", "100")
                addView(spacer(2))
                patternFall = numRow("fall (ms, max $timeMax)", "500")
                addView(spacer(2))
                patternOfft = numRow("off time (ms, max $timeMax)", "1200")
            }
            addView(solidCard)
            addView(patternCard)
            patternSync.setOnCheckedChangeListener { _, _ -> applySyncGray() }
            syncMode()
            solidCur.onChanged = { refreshPreview() }
            patternCur.onChanged = { refreshPreview() }
            if (color != null) attachPreview(color!!)
        }

        private fun applySyncGray() {
            val m = modeNames[mode.get()]
            val sync = (m == "breath" || m == "wave") && patternSync.isChecked
            val p = color ?: previewTarget
            if (p != null) {
                p.pwmSync = sync
                p.setChannelEnabled(0, true)
                p.setChannelEnabled(1, !sync)
                p.setChannelEnabled(2, !sync)
            }
            val wave = m == "wave"
            waveT0.setFieldEnabled(0, wave)
            waveT0.setFieldEnabled(1, wave && !sync)
            waveT0.setFieldEnabled(2, wave && !sync)
            onDriveChanged?.invoke()
        }

        private fun activeCur(): Triple<Int, Int, Int> = when (modeNames[mode.get()]) {
            "off" -> Triple(0, 0, 0)
            "solid" -> solidCur.getTriple(Triple(15, 15, 15))
            "breath", "wave" -> patternCur.getTriple(Triple(15, 15, 15))
            else -> Triple(0, 0, 0)
        }

        fun attachPreview(p: RgbPicker) {
            previewTarget = p
            applySyncGray()
            refreshPreview()
        }

        fun refreshPreview() {
            previewTarget?.previewCur = activeCur()
            onDriveChanged?.invoke()
        }

        fun liveDrive(): Pair<Triple<Int, Int, Int>, Boolean> {
            val m = modeNames[mode.get()]
            return Pair(activeCur(), (m == "breath" || m == "wave") && patternSync.isChecked)
        }

        private fun syncMode() {
            val m = modeNames[mode.get()]
            solidCard.visibility = if (m == "solid") VISIBLE else GONE
            patternCard.visibility = if (m == "breath" || m == "wave") VISIBLE else GONE
            waveT0.visibility = if (m == "wave") VISIBLE else GONE
            applySyncGray()
            refreshPreview()
        }

        fun load(r: Render, fallbackMode: String) {
            val idx = modeNames.indexOf(r.mode)
            val fb = modeNames.indexOf(fallbackMode)
            mode.set(if (idx >= 0) idx else if (fb >= 0) fb else 2)
            solidCur.setTriple(r.solidCur)
            patternRepeat.setText(r.patternRepeat.toString())
            patternCur.setTriple(r.patternCur)
            patternRise.setText(r.patternRise.toString())
            patternHold.setText(r.patternHold.toString())
            patternFall.setText(r.patternFall.toString())
            patternOfft.setText(r.patternOfft.toString())
            patternSync.isChecked = r.patternSync
            waveT0.setTriple(r.waveT0)
            syncMode()
        }

        fun setColor(c: Triple<Int, Int, Int>) { color?.setColor(c) }
        fun getColor(): Triple<Int, Int, Int>? = color?.getColor()

        fun collect(r: Render) {
            val m = modeNames[mode.get()]
            r.mode = m
            r.solidCur = clampTriple(solidCur.getTriple(Triple(15, 15, 15)))
            r.patternRepeat = patternRepeat.getInt(0).coerceIn(0, 15)
            r.patternCur = clampTriple(patternCur.getTriple(Triple(15, 15, 15)))
            r.patternRise = patternRise.getInt(500).coerceIn(0, timeMax)
            r.patternHold = patternHold.getInt(100).coerceIn(0, timeMax)
            r.patternFall = patternFall.getInt(500).coerceIn(0, timeMax)
            r.patternOfft = patternOfft.getInt(1200).coerceIn(0, timeMax)
            r.patternSync = patternSync.isChecked
            r.waveT0 = waveT0.getTriple(Triple(0, 1300, 2600))
        }

        private fun clampTriple(t: Triple<Int, Int, Int>): Triple<Int, Int, Int> =
            Triple(t.first.coerceIn(0, 15), t.second.coerceIn(0, 15), t.third.coerceIn(0, 15))
    }

    /** The unified per-event "standard settings" template: color,
     *  optional duration cap and the renderer card (mode + per-chip
     *  params/timing). Every event tab uses this exact shape - variants
     *  (charge bands, notify default/app, call/missed) and section
     *  extras (thresholds, packages, pending window) bolt on around it.
     *  color/cap map to the event's own LedConf fields by the owning
     *  view; the chip timing lives inside the RenderCard. */
    protected inner class EventKnobs(
        colorTitle: String,
        rendererTitle: String,
        rendererHint: String = "",
        showColor: Boolean = true,
        showCap: Boolean = true,
        capLabel: String = "max sec (0 == inf)",
        colorBuilder: (LinearLayout.() -> View)? = null
    ) : LinearLayout(context) {
        lateinit var color: RgbPicker
            private set
        lateinit var cap: NumField
            private set
        lateinit var renderer: RenderCard
            private set

        init {
            orientation = VERTICAL
            if (showColor) {
                addView(card {
                    addView(sectionTitle(colorTitle))
                    addView(spacer(4))
                    if (colorBuilder != null) {
                        addView(colorBuilder())
                    } else {
                        color = RgbPicker("color (color1)")
                        addView(color)
                    }
                })
            }
            if (showCap) {
                addView(card {
                    addView(sectionTitle("Duration cap"))
                    addView(spacer(4))
                    addView(text("stops the event after this many seconds (0 = forever)", 12f, parse("#FF727272")))
                    addView(spacer(2))
                    cap = numRow(capLabel, "0")
                })
            }
            renderer = RenderCard(rendererTitle, rendererHint)
            addView(renderer)
            if (::color.isInitialized) renderer.attachPreview(color)
        }

        fun load(r: Render, fallbackMode: String) = renderer.load(r, fallbackMode)

        fun collect(r: Render) = renderer.collect(r)
    }
}
