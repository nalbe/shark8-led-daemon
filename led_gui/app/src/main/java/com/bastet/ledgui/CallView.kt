package com.bastet.ledgui

import android.content.Context
import android.widget.LinearLayout

/** Call tab: incoming-call vs missed-call presets, each with the unified
 *  standard settings. Rings default to the rainbow wave. */
class CallView(context: Context) : ConfPage(context) {

    private lateinit var viewPicker: NamePicker
    private lateinit var callGroup: LinearLayout
    private lateinit var ringKnobs: EventKnobs
    private lateinit var testSec: NumField
    private lateinit var missedGroup: LinearLayout
    private lateinit var missedKnobs: EventKnobs

    override fun buildBody() {
        viewPicker = NamePicker("call view", listOf("in-call", "missed")) { syncView() }
        body.addView(viewPicker)

        callGroup = LinearLayout(context).apply { orientation = VERTICAL }
        ringKnobs = EventKnobs(
            "Incoming call", "Ring renderer",
            "Incoming calls default to a rainbow wave.",
            capLabel = "max sec (0 == inf)"
        )
        callGroup.addView(ringKnobs)
        callGroup.addView(card {
            addView(sectionTitle("Test rainbow"))
            addView(spacer(4))
            addView(text(
                "How long the fake-dialer / test rainbow holds before disarm.",
                12f, parse("#FF727272")
            ))
            addView(spacer(2))
            testSec = numRow("test rainbow hold (sec)", "30")
        })
        body.addView(callGroup)

        missedGroup = LinearLayout(context).apply { orientation = VERTICAL }
        missedKnobs = EventKnobs(
            "Missed call", "Missed renderer",
            "For missed calls.",
            capLabel = "max sec (0 == inf)"
        )
        missedGroup.addView(missedKnobs)
        body.addView(missedGroup)
        syncView()
    }

    private fun syncView() {
        val missed = viewPicker.get() == 1
        callGroup.visibility = if (missed) GONE else VISIBLE
        missedGroup.visibility = if (missed) VISIBLE else GONE
    }

    override fun applyTo(c: LedConf) {
        ringKnobs.load(c.ringRender, "wave")
        ringKnobs.color.setColor(c.ringColor)
        ringKnobs.cap.setText(c.ringCapSec.toString())
        testSec.setText(c.ringTestSec.toString())
        missedKnobs.load(c.missedRender, "breath")
        missedKnobs.color.setColor(c.missedColor)
        missedKnobs.cap.setText(c.missedMaxSec.toString())
    }

    override fun collectFrom(c: LedConf) {
        ringKnobs.collect(c.ringRender)
        c.ringColor = ringKnobs.color.getColor()
        c.ringCapSec = ringKnobs.cap.getLong(0L)
        c.ringTestSec = testSec.getLong(30L)
        missedKnobs.collect(c.missedRender)
        c.missedColor = missedKnobs.color.getColor()
        c.missedMaxSec = missedKnobs.cap.getLong(0L)
    }
}