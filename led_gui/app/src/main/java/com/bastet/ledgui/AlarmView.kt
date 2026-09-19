package com.bastet.ledgui

import android.content.Context

/** Alarm tab: the unified standard settings for the desk-clock alarm LED. */
class AlarmView(context: Context) : ConfPage(context) {

    private lateinit var knobs: EventKnobs

    override fun buildBody() {
        knobs = EventKnobs(
            "Alarm", "Alarm renderer",
            "Shown when com.android.deskclock / com.google.android.deskclock fires.",
            capLabel = "max sec (0 == inf)"
        )
        body.addView(knobs)
    }

    override fun applyTo(c: LedConf) {
        knobs.load(c.alarmRender, "breath")
        knobs.color.setColor(c.alarmColor)
        knobs.cap.setText(c.alarmMaxSec.toString())
    }

    override fun collectFrom(c: LedConf) {
        knobs.collect(c.alarmRender)
        c.alarmColor = knobs.color.getColor()
        c.alarmMaxSec = knobs.cap.getLong(0L)
    }
}