package com.bastet.ledgui

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
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
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

/** AW2033 color simulation for previews: the chip scales each channel's
 *  drive current (0..15) against the color's PWM amplitude (0..255), so
 *  the eye sees color * cur/15 per channel. cur=15 reproduces the raw
 *  color exactly; a zeroed channel goes dark. */
object LedSim {
    fun rgbWithCurrent(c: Triple<Int, Int, Int>, cur: Triple<Int, Int, Int>): Triple<Int, Int, Int> {
        fun ch(v: Int, kw: Int) = (v * kw / 15f).roundToInt().coerceIn(0, 255)
        return Triple(ch(c.first, cur.first), ch(c.second, cur.second), ch(c.third, cur.third))
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
        if (cached != null) applyTo(cached) else reload()
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
                showMsg("load failed: root not granted? Press Reload after granting.", error = true)
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
         *  swatch previews color * cur/15, so the user sees the light the
         *  combination actually produces, not the raw picker color. */
        var previewCur: Triple<Int, Int, Int> = Triple(15, 15, 15)
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
            val s = LedSim.rgbWithCurrent(color, previewCur)
            g.setColor(Color.rgb(s.first, s.second, s.third))
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

        private val trackPaint = Paint().apply { color = parse("#FF333A46"); isAntiAlias = true }
        private val fillPaint = Paint().apply { color = channelColor; isAntiAlias = true }
        private val thumbPaint = Paint().apply { color = parse("#FFF0F0F0"); isAntiAlias = true }

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
            canvas.drawRoundRect(RectF(left, cy - t / 2f, thumbX, cy + t / 2f), t / 2f, t / 2f, fillPaint)
            canvas.drawCircle(thumbX, cy, dpi(11).toFloat(), thumbPaint)
        }

        override fun onTouchEvent(e: MotionEvent): Boolean {
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

        fun getTriple(def: Triple<Int, Int, Int>): Triple<Int, Int, Int> = Triple(
            edits[0].text.toString().toIntOrNull() ?: def.first,
            edits[1].text.toString().toIntOrNull() ?: def.second,
            edits[2].text.toString().toIntOrNull() ?: def.third
        )
    }

    /** Per-event chip renderer editor (v3): the [sec] mode + the knobs of
     *  the ACTIVE [sec.solid]/[sec.breath]/[sec.wave] section. Only the
     *  active mode's card is shown (radio change re-syncs). */
    protected inner class RenderCard(title: String, hint: String = "", showColor: Boolean = false) : LinearLayout(context) {
        private val modeNames = listOf("off", "solid", "breath", "wave")

        lateinit var mode: NamePicker
            private set
        var color: RgbPicker? = null
            private set

        /** Color picker whose swatch previews current * color. Set via
         *  attachPreview() - for showColor cards it's the card's own
         *  picker; EventKnobs attaches the standalone color card's. */
        private var previewTarget: RgbPicker? = null

        private lateinit var solidCard: LinearLayout
        private lateinit var breathCard: LinearLayout
        private lateinit var waveCard: LinearLayout
        lateinit var solidCur: TripleField
            private set
        lateinit var brRepeat: NumField
            private set
        lateinit var brCur: TripleField
            private set
        lateinit var brRise: NumField
            private set
        lateinit var brHold: NumField
            private set
        lateinit var brFall: NumField
            private set
        lateinit var brOfft: NumField
            private set
        lateinit var waveT0: TripleField
            private set
        lateinit var waveRepeat: NumField
            private set
        lateinit var waveRise: NumField
            private set
        lateinit var waveHold: NumField
            private set
        lateinit var waveFall: NumField
            private set
        lateinit var waveOfft: NumField
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
                addView(sectionTitle("Solid param"))
                addView(spacer(4))
                addView(text("Constant color on this event's channels, full current. Timing = none.", 12f, parse("#FF727272")))
                addView(spacer(2))
                solidCur = TripleField("current (amps) r,g,b (0-15)", Triple(15, 15, 15))
                addView(solidCur)
            }
            breathCard = card {
                addView(sectionTitle("Breath param"))
                addView(spacer(4))
                addView(text("Chip-driven breathing. Timing is owned by [sec.breath] (no fallback).", 12f, parse("#FF727272")))
                addView(spacer(2))
                brRepeat = numRow("repeat (0 = infinite, 1..15)", "0")
                addView(spacer(2))
                brCur = TripleField("current (amps) r,g,b (0-15)", Triple(15, 15, 15))
                addView(brCur)
                addView(spacer(2))
                brRise = numRow("rise (ms)", "500")
                addView(spacer(2))
                brHold = numRow("hold (ms)", "100")
                addView(spacer(2))
                brFall = numRow("fall (ms)", "500")
                addView(spacer(2))
                brOfft = numRow("off time (ms)", "1200")
            }
            waveCard = card {
                addView(sectionTitle("Wave param"))
                addView(spacer(4))
                addView(text("Breathing with a per-channel t0 phase lag = traveling rainbow. Timing is owned by [sec.wave] (no fallback).", 12f, parse("#FF727272")))
                addView(spacer(2))
                waveT0 = TripleField("channel phase t0 (ms)", Triple(0, 1300, 2600))
                addView(waveT0)
                addView(spacer(2))
                waveRepeat = numRow("repeat (0 = infinite, 1..15)", "0")
                addView(spacer(2))
                waveRise = numRow("rise (ms)", "500")
                addView(spacer(2))
                waveHold = numRow("hold (ms)", "100")
                addView(spacer(2))
                waveFall = numRow("fall (ms)", "500")
                addView(spacer(2))
                waveOfft = numRow("off time (ms)", "1200")
            }
            addView(solidCard)
            addView(breathCard)
            addView(waveCard)
            syncMode()
            solidCur.onChanged = { refreshPreview() }
            brCur.onChanged = { refreshPreview() }
            if (color != null) attachPreview(color!!)
        }

        /** The current triple the active mode actually drives. solid and
         *  breath read their knobs; wave has no per-channel current knob in
         *  the GUI, so it previews the chip default (full). off = dark. */
        private fun activeCur(): Triple<Int, Int, Int> = when (modeNames[mode.get()]) {
            "off" -> Triple(0, 0, 0)
            "solid" -> solidCur.getTriple(Triple(15, 15, 15))
            "breath" -> brCur.getTriple(Triple(15, 15, 15))
            else -> Triple(15, 15, 15)
        }

        fun attachPreview(p: RgbPicker) {
            previewTarget = p
            refreshPreview()
        }

        fun refreshPreview() {
            previewTarget?.previewCur = activeCur()
        }

        private fun syncMode() {
            val m = modeNames[mode.get()]
            solidCard.visibility = if (m == "solid") VISIBLE else GONE
            breathCard.visibility = if (m == "breath") VISIBLE else GONE
            waveCard.visibility = if (m == "wave") VISIBLE else GONE
            refreshPreview()
        }

        fun load(r: Render, fallbackMode: String) {
            val idx = modeNames.indexOf(r.mode)
            val fb = modeNames.indexOf(fallbackMode)
            mode.set(if (idx >= 0) idx else if (fb >= 0) fb else 2)
            solidCur.setTriple(r.solidCur)
            brRepeat.setText(r.brRepeat.toString())
            brCur.setTriple(r.brCur)
            brRise.setText(r.brRise.toString())
            brHold.setText(r.brHold.toString())
            brFall.setText(r.brFall.toString())
            brOfft.setText(r.brOfft.toString())
            waveT0.setTriple(r.waveT0)
            waveRepeat.setText(r.waveRepeat.toString())
            waveRise.setText(r.waveRise.toString())
            waveHold.setText(r.waveHold.toString())
            waveFall.setText(r.waveFall.toString())
            waveOfft.setText(r.waveOfft.toString())
            syncMode()
        }

        fun setColor(c: Triple<Int, Int, Int>) { color?.setColor(c) }
        fun getColor(): Triple<Int, Int, Int>? = color?.getColor()

        fun collect(r: Render) {
            val m = modeNames[mode.get()]
            r.mode = m
            r.solidCur = clampTriple(solidCur.getTriple(Triple(15, 15, 15)))
            r.brRepeat = brRepeat.getInt(0).coerceIn(0, 15)
            r.brCur = clampTriple(brCur.getTriple(Triple(15, 15, 15)))
            r.brRise = brRise.getInt(500)
            r.brHold = brHold.getInt(100)
            r.brFall = brFall.getInt(500)
            r.brOfft = brOfft.getInt(1200)
            r.waveT0 = waveT0.getTriple(Triple(0, 1300, 2600))
            r.waveRepeat = waveRepeat.getInt(0).coerceIn(0, 15)
            r.waveRise = waveRise.getInt(500)
            r.waveHold = waveHold.getInt(100)
            r.waveFall = waveFall.getInt(500)
            r.waveOfft = waveOfft.getInt(1200)
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
        colorBuilder: (() -> View)? = null
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
