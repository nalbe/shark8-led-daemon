package com.bastet.ledgui

import android.content.Context

/** VoIP tab: the messenger/VoIP call light settings (color, cap =
 *  silence grace, renderer, timing). Defaults to the rainbow wave.
 *  Which apps count as a call notification is decided by the bridge
 *  (notification category/channel), not by a package list here. */
class VoipView(context: Context) : ConfPage(context) {

    private lateinit var knobs: EventKnobs

    override fun buildBody() {
        knobs = EventKnobs(
            "Messenger / VoIP call", "VoIP renderer",
            "Messenger calls default to a rainbow wave.",
            capLabel = "max sec (0 == inf)"
        )
        body.addView(knobs)
    }

    override fun applyTo(c: LedConf) {
        knobs.load(c.voipRender, "wave")
        knobs.color.setColor(c.voipColor)
        knobs.cap.setText(c.voipMaxSec.toString())
    }

    override fun collectFrom(c: LedConf) {
        knobs.collect(c.voipRender)
        c.voipColor = knobs.color.getColor()
        c.voipMaxSec = knobs.cap.getLong(300L)
    }
}