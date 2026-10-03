package com.bastet.ledgui

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.graphics.RectF
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.graphics.drawable.GradientDrawable
import android.widget.LinearLayout
import kotlin.math.abs
import kotlin.math.roundToInt

/** Priority tab: the [priority] ranking as ONE ordered list.
 *
 *  The list is the whole truth in the GUI. Rank numbers are derived from
 *  the row order on every save (top row wins, step 10), and on load the
 *  rows are sorted by the numbers the file really has: the configured
 *  rank, then the default order as the tie-break. That is the daemon's
 *  chain, so a hand-tuned file and a GUI-sorted one mean the same thing.
 *  A key the file does not carry is shown (the GUI keeps the list complete
 *  and editable) but the daemon drops that effect, so the banner says so.
 *
 *  Dragging starts on the handle only. It asks the whole chain up to the
 *  activity to keep its hands off the event (DOWN/UP pair), so neither the
 *  page ScrollView nor the horizontal pager can steal the drag - and a
 *  swipe anywhere else on the row still scrolls the page normally.
 */
class PriorityView(context: Context) : ConfPage(context) {

    private companion object {
        const val ROW_H_DP = 58
        /** what each row does, so the order is readable without the daemon */
        val DESCR = mapOf(
            "ring" to "incoming call rainbow",
            "voip" to "messenger call rainbow",
            "alarm" to "desk-clock alarm",
            "missed" to "missed-call tombstone",
            "notify" to "ordinary notifications (one entry per app)",
            "charge" to "charge band (wins only what is free)"
        )
    }

    /** row order, top = wins */
    private val order = mutableListOf<String>()
    private val rows = mutableListOf<LinearLayout>()
    private val handles = mutableListOf<DragHandle>()
    private lateinit var listBox: LinearLayout

    // drag state: `from` is the row under the finger, `to` the slot it would
    // land in (they differ the moment the finger crosses a row boundary)
    private var from = -1
    private var to = -1
    private var dy = 0f
    private var downRawY = 0f
    private var lifting = false
    private var active: DragHandle? = null

    override fun buildBody() {
        body.addView(card {
            addView(sectionTitle("Priority order"))
            addView(text(
                "Higher wins. One owner at a time: the top row takes the LEDs " +
                    "from everything below it and gets them back the moment " +
                    "that effect ends.",
                13f, parse("#FF9E9E9E")))
            addView(spacer(6))
            addView(text(
                "Drag the handle on the left. The numbers are written for you, " +
                    "10, 20, 30 ... counting down the list.",
                13f, parse("#FF9E9E9E")))
        })
        body.addView(card {
            addView(sectionTitle("Effects"))
            listBox = LinearLayout(context).apply { orientation = VERTICAL }
            addView(listBox)
        })
    }

    override fun applyTo(c: LedConf) {
        order.clear()
        order.addAll(c.priorityOrder())
        renderRows()
        if (c.warnings.isNotEmpty()) showMsg(c.warnings.joinToString("\n"), error = true)
    }

    override fun collectFrom(c: LedConf) {
        c.setPriorityOrder(order)
    }

    private fun renderRows() {
        listBox.removeAllViews()
        rows.clear()
        handles.clear()
        order.forEachIndexed { i, name ->
            val r = makeRow(name, i)
            rows.add(r)
            listBox.addView(r)
        }
    }

    private fun makeRow(name: String, index: Int): LinearLayout {
        val row = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dpi(6), 0, dpi(10), 0)
            background = rowBg(false)
            layoutParams = LayoutParams(mP, dpi(ROW_H_DP)).apply {
                if (index > 0) topMargin = dpi(6)
            }
        }
        val handle = DragHandle().apply {
            layoutParams = LayoutParams(dpi(42), dpi(ROW_H_DP))
            onTouch = { ev -> onHandleTouch(index, ev) }
        }
        handles.add(handle)
        row.addView(handle)
        val col = LinearLayout(context).apply {
            orientation = VERTICAL
            layoutParams = LayoutParams(0, wP, 1f)
        }
        col.addView(text(name, 15f, parse("#FFE0E0E0"), bold = true, mono = true))
        col.addView(text(DESCR[name] ?: "", 12f, parse("#FF727272")))
        row.addView(col)
        return row
    }

    private fun onHandleTouch(index: Int, ev: MotionEvent): Boolean {
        when (ev.actionMasked) {
            MotionEvent.ACTION_DOWN -> {
                from = index
                to = index
                dy = 0f
                downRawY = ev.rawY
                lifting = false
                // both ends of the pair: the pager also listens for UP to
                // decide whether a swipe was a page change
                parent?.requestDisallowInterceptTouchEvent(true)
                return true
            }
            MotionEvent.ACTION_MOVE -> {
                dy = ev.rawY - downRawY
                if (!lifting && abs(dy) > touchSlop()) lift()
                if (lifting) followFinger()
                return true
            }
            MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                parent?.requestDisallowInterceptTouchEvent(false)
                drop(commit = ev.actionMasked == MotionEvent.ACTION_UP)
                return true
            }
        }
        return true
    }

    private fun touchSlop(): Int = ViewConfiguration.get(context).scaledTouchSlop

    private fun rowHeight(): Int = dpi(ROW_H_DP) + dpi(6)

    /** the picked row lifts out of the list so the finger keeps its grip */
    private fun lift() {
        lifting = true
        active = handles[from]
        rows[from].apply {
            alpha = 0.94f
            background = rowBg(true)
        }
        active?.invalidate()
    }

    private fun followFinger() {
        rows[from].translationY = dy
        val target = (from + (dy / rowHeight()).roundToInt()).coerceIn(0, rows.size - 1)
        if (target != to) {
            to = target
            openGap()
        }
    }

    /** rows between the picked one and its slot slide one row-height out of
     *  the way, so the gap shows where the row will land */
    private fun openGap() {
        val h = rowHeight().toFloat()
        for (i in rows.indices) {
            if (i == from) continue
            rows[i].translationY = when {
                to > from && i in (from + 1)..to -> -h
                to < from && i in to until from -> h
                else -> 0f
            }
        }
    }

    private fun drop(commit: Boolean) {
        val moved = lifting && commit && to != from
        if (moved) order.add(to, order.removeAt(from))
        // Reset the drag state NOW, before any rebuild: a plain tap (no move
        // at all) must not touch the view tree either - see below.
        active = null
        from = -1
        to = -1
        dy = 0f
        lifting = false
        if (!moved) {
            // nothing reordered: undo the lifted style in place, the view tree
            // stays untouched so the draw pass has nothing new to lay out
            for (r in rows) {
                r.translationY = 0f
                r.alpha = 1f
                r.background = rowBg(false)
            }
            return
        }
        // Rebuilding the rows means removeAllViews() + six addView() calls, and
        // doing that INSIDE the touch dispatch is what killed the app: the
        // release draw pass then walked a child array the input pass had just
        // rewritten (NPE in ViewGroup.dispatchDraw, null mViewFlags). post()
        // moves the mutation past the current input event entirely, so the
        // handler returns with the tree exactly as the frame pass expects it.
        post { renderRows() }
    }

    private fun rowBg(lifted: Boolean): GradientDrawable = GradientDrawable().apply {
        shape = GradientDrawable.RECTANGLE
        cornerRadius = dpf(8).toFloat()
        setColor(if (lifted) parse("#FF2E3A57") else parse("#FF262626"))
        setStroke(dpi(1), if (lifted) parse("#FF5C6BC0") else parse("#FF3A3A3A"))
    }

    /** The drag grip: three bars drawn in onDraw - an ASCII source file has
     *  no room for a grip glyph, and drawing it keeps the tint under our
     *  control when the row is lifted. */
    private inner class DragHandle : View(context) {
        var onTouch: (MotionEvent) -> Boolean = { false }
        private val bar = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = parse("#FF90A4AE") }
        private val hot = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = parse("#FF90CAF9") }
        private val r = RectF()

        init {
            isClickable = true
            contentDescription = "drag to reorder"
        }

        override fun onDraw(canvas: Canvas) {
            val w = width.toFloat()
            val bw = minOf(w * 0.5f, dpf(20).toFloat())
            val bh = maxOf(2f, dpf(2).toFloat())
            val left = (w - bw) / 2f
            val pitch = bh + dpf(2)
            val top = height / 2f - pitch
            val p = if (lifting && this === active) hot else bar
            for (i in 0..2) {
                val y = top + i * pitch
                r.set(left, y, left + bw, y + bh)
                canvas.drawRoundRect(r, bh / 2f, bh / 2f, p)
            }
        }

        override fun onTouchEvent(ev: MotionEvent): Boolean = onTouch(ev)
    }
}