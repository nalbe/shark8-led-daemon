package com.bastet.ledgui

import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.widget.Button
import android.widget.FrameLayout
import android.widget.HorizontalScrollView
import android.widget.LinearLayout
import androidx.activity.ComponentActivity
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import kotlin.math.roundToInt

/**
*  Classic-Views shell (Compose built no smooth 120Hz scrolling on this
 *  firmware; the plain View framework does). Six tabs: Info / Charge /
 *  Notification / Call / VoIP / Alarm, with a follow-the-finger
 *  drag pager in between.
 */
class MainActivity : ComponentActivity() {

    private val tabs = arrayOf("Info", "Charge", "Notification", "Call", "VoIP", "Alarm")

    private lateinit var statusView: StatusView
    private var chargeView: ChargeView? = null
    private var notificationView: NotificationView? = null
    private var callView: CallView? = null
    private var voipView: VoipView? = null
    private var alarmView: AlarmView? = null
    private lateinit var container: PagerContainer
    private val tabButtons = mutableListOf<Button>()
    private var currentIndex = 0
    private val created = booleanArrayOf(false, false, false, false, false, false)

    // drag state (follow-the-finger tab pager)
    private var dragActive = false
    private var dragOffset = 0f
    private var dragLeft = false          // fixed drag direction (true = toward next tab)
    private var downX = 0f
    private var downY = 0f
    private val dragWidth: Float get() = resources.displayMetrics.widthPixels.toFloat()

    private val pageCount: Int get() = tabs.size

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main)

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        statusView = StatusView(this)
        created[0] = true

        val root = LinearLayout(this)
        root.orientation = LinearLayout.VERTICAL
        root.setBackgroundColor(Color.parseColor("#FF121212"))

        // top tab strip (scrollable for future tabs)
        val scroll = HorizontalScrollView(this)
        scroll.isHorizontalScrollBarEnabled = false
        scroll.isFillViewport = true
        val strip = LinearLayout(this)
        strip.orientation = LinearLayout.HORIZONTAL
        strip.setPadding(dpi(12f), dpi(8f), dpi(12f), dpi(4f))
        tabs.forEachIndexed { i, label ->
            val b = tabButton(label) { selectTab(i) }
            tabButtons.add(b)
            strip.addView(b)
        }
        scroll.addView(strip, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT
        ))
        root.addView(scroll)

        // content area
        container = PagerContainer(this)
        container.addView(statusView, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
        ))
        root.addView(container, LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f
        ))

        setContentView(root)
        statusView.visibility = View.VISIBLE
        tabsFor(0)

        // Pre-warm the settings cache and pre-construct every tab right
        // after the first frame, so the FIRST tap on a tab never pays for
        // view construction or the su config read (pages stay built and
        // painted from the shared cache for the whole session).
        root.post {
            scope.launch {
                val c = withContext(Dispatchers.IO) { LedConf.loadOrNull() }
                if (c != null) LedConf.updateCache(c)
                for (i in 1 until pageCount) ensure(i)
            }
        }
    }

    override fun onDestroy() {
        super.onDestroy()
        scope.cancel()
    }

    /** Follow-the-finger pager as a real ViewGroup interceptor. A child view
     *  that claims the touch (our sliders send requestDisallowInterceptTouchEvent
     *  on DOWN) wins over the pager; otherwise a mostly-horizontal drag flips
     *  tabs and swallows the stream so the inner ScrollView can't scroll too. */
    private inner class PagerContainer(ctx: Context) : FrameLayout(ctx) {

        override fun onInterceptTouchEvent(ev: MotionEvent): Boolean {
            when (ev.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downX = ev.x
                    downY = ev.y
                    dragOffset = 0f
                    dragActive = false
                }
                MotionEvent.ACTION_MOVE -> {
                    if (!dragActive) {
                        val dx = ev.x - downX
                        val dy = ev.y - downY
                        if (Math.abs(dx) > dpi(28f) && Math.abs(dx) > Math.abs(dy)) {
                            dragActive = true
                            dragLeft = dx < 0
                            dragOffset = dx
                            beginDrag()
                            return true
                        }
                    }
                }
            }
            return false
        }

        override fun onTouchEvent(ev: MotionEvent): Boolean {
            if (!dragActive) return false
            when (ev.actionMasked) {
                MotionEvent.ACTION_MOVE -> {
                    dragOffset = ev.x - downX
                    updateDrag(dragOffset)
                    return true
                }
                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    finishDrag(commit = ev.actionMasked == MotionEvent.ACTION_UP)
                    dragActive = false
                    return true
                }
            }
            return true
        }
    }

    private fun dpi(v: Float) = (v * resources.displayMetrics.density).roundToInt()

    /** Create a page on first need and register it in the container. */
    private fun ensure(index: Int): View {
        if (created[index]) return viewAt(index)
        created[index] = true
        val v = when (index) {
            1 -> ChargeView(this).also { chargeView = it }
            2 -> NotificationView(this).also { notificationView = it }
            3 -> CallView(this).also { callView = it }
            4 -> VoipView(this).also { voipView = it }
            else -> AlarmView(this).also { alarmView = it }
        }
        container.addView(v, FrameLayout.LayoutParams(
            FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT
        ))
        v.visibility = View.GONE
        return v
    }

    /** Neighbor page in a fixed drag direction, lazily created, or null at the edges. */
    private fun neighbor(direction: Int): View? {
        val to = currentIndex + direction
        if (to < 0 || to >= pageCount) return null
        return ensure(to)
    }

    private fun viewAt(i: Int): View = when (i) {
        0 -> statusView
        1 -> chargeView!!
        2 -> notificationView!!
        3 -> callView!!
        4 -> voipView!!
        else -> alarmView!!
    }

    /** Lay the current and target pages out for a drag. */
    private fun beginDrag() {
        val front = viewAt(currentIndex)
        val dir = if (dragLeft) 1 else -1
        val back = neighbor(dir) ?: return
        val backSide = if (dragLeft) dragWidth else -dragWidth
        front.translationX = 0f
        front.alpha = 1f
        back.translationX = backSide
        back.alpha = 1f
        front.visibility = View.VISIBLE
        back.visibility = View.VISIBLE
        // all other pages hidden so they don't sit on top
        for (i in 0 until pageCount) {
            if (created[i] && i != currentIndex && viewAt(i) !== back) viewAt(i).visibility = View.GONE
        }
        updateDrag(dragOffset)
    }

    /** Move both pages exactly with the finger. */
    private fun updateDrag(offset: Float) {
        val front = viewAt(currentIndex)
        val dir = if (dragLeft) 1 else -1
        val back = neighbor(dir) ?: return
        front.translationX = offset
        back.translationX = (if (dragLeft) dragWidth else -dragWidth) + offset
        back.alpha = (kotlin.math.abs(offset) / dragWidth).coerceIn(0f, 1f)
    }

    /** Snap to whichever page we've dragged far enough toward. */
    private fun finishDrag(commit: Boolean) {
        val front = viewAt(currentIndex)
        val dir = if (dragLeft) 1 else -1
        val back = neighbor(dir) ?: run {
            front.animate().translationX(0f).alpha(1f).setDuration(180).start()
            return
        }
        val threshold = dragWidth / 3f
        val doSwitch = commit && kotlin.math.abs(dragOffset) > threshold

        if (doSwitch) {
            front.animate().translationX(
                if (dragLeft) -dragWidth else dragWidth
            ).alpha(0f).setDuration(180).start()
            back.animate().translationX(0f).alpha(1f).setDuration(180).withEndAction {
                currentIndex += dir
                for (i in 0 until pageCount) {
                    if (created[i]) viewAt(i).visibility =
                        if (i == currentIndex) View.VISIBLE else View.GONE
                }
                front.translationX = 0f
                front.alpha = 1f
                tabsFor(currentIndex)
            }.start()
        } else {
            front.animate().translationX(0f).alpha(1f).setDuration(180).start()
            back.animate().translationX(
                if (dragLeft) dragWidth else -dragWidth
            ).alpha(0f).setDuration(180).withEndAction {
                back.visibility = View.GONE
                back.translationX = 0f
                back.alpha = 1f
            }.start()
        }
    }

    private fun tabsFor(index: Int) {
        tabButtons.forEachIndexed { i, b -> b.isSelected = i == index }
        paintTabs()
    }

    private fun selectTab(index: Int) {
        dragActive = false
        currentIndex = index
        val target = ensure(index)
        for (i in 0 until pageCount) {
            if (created[i]) {
                val v = viewAt(i)
                v.visibility = if (i == index) View.VISIBLE else View.GONE
                v.translationX = 0f
                v.alpha = 1f
            }
        }
        target.translationX = 0f
        target.alpha = 1f
        target.visibility = View.VISIBLE
        tabsFor(index)
    }

    private fun paintTabs() {
        tabButtons.forEach { styleTab(it, it.isSelected) }
    }

    private fun styleTab(b: Button, selected: Boolean) {
        val g = GradientDrawable()
        g.shape = GradientDrawable.RECTANGLE
        g.cornerRadius = resources.displayMetrics.density * 8f
        if (selected) {
            g.setColor(Color.parseColor("#FF1565C0"))
            b.setTextColor(Color.WHITE)
        } else {
            g.setColor(Color.parseColor("#FF2A2A2A"))
            b.setTextColor(Color.parseColor("#FFB0BEC5"))
        }
        b.background = g
    }

    private fun tabButton(label: String, onClick: () -> Unit): Button {
        val b = Button(this)
        b.text = label
        b.isAllCaps = false
        b.textSize = 15f
        b.gravity = Gravity.CENTER
        b.setPadding(dpi(10f), 0, dpi(10f), 0)
        b.setMinHeight(0)
        b.minHeight = 0
        b.minimumHeight = 0
        val lp = LinearLayout.LayoutParams(
            LinearLayout.LayoutParams.WRAP_CONTENT, dpi(44f)
        )
        lp.setMargins(0, 0, dpi(8f), 0)
        b.layoutParams = lp
        b.setOnClickListener { onClick() }
        return b
    }
}
