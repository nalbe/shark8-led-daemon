package com.bastet.ledgui

import android.content.Context

/** VoIP tab: the unified standard settings for messenger/VoIP call light
 *  (color, cap = silence grace, renderer, timing) + the package trigger
 *  list. Defaults to the rainbow wave. */
class VoipView(context: Context) : ConfPage(context) {

    private lateinit var knobs: EventKnobs
    private lateinit var packages: android.widget.EditText

    override fun buildBody() {
        knobs = EventKnobs(
            "Messenger / VoIP call", "VoIP renderer",
            "Messenger calls default to a rainbow wave.",
            capLabel = "max sec (0 == inf)"
        )
        body.addView(knobs)
        body.addView(card {
            addView(sectionTitle("VoIP packages"))
            addView(spacer(4))
            addView(text(
                "[voip] packages: one messenger app per line, trigger for the " +
                    "VoIP call rainbow. Empty = daemon default list.",
                12f, parse("#FF727272")
            ))
            addView(spacer(6))
            packages = multiLine("com.whatsapp")
            addView(packages)
        })
    }

    override fun applyTo(c: LedConf) {
        knobs.load(c.voipRender, "wave")
        knobs.color.setColor(c.voipColor)
        knobs.cap.setText(c.voipMaxSec.toString())
        packages.setText(if (c.voipPackages.isBlank()) "" else
            c.voipPackages.split(',').map { it.trim() }.filter { it.isNotBlank() }
                .joinToString("\n"))
    }

    override fun collectFrom(c: LedConf) {
        knobs.collect(c.voipRender)
        c.voipColor = knobs.color.getColor()
        c.voipMaxSec = knobs.cap.getLong(300L)
        c.voipPackages = packages.text.toString().lines()
            .map { it.trim() }.filter { it.isNotBlank() }.joinToString(",")
    }
}