package com.bastet.ledgui

import android.content.Context
import android.widget.LinearLayout

/** Notification tab: two presets, each with its OWN full settings.
 *  "default" = notifications without a per-app rule (color, cap,
 *  renderer, pending window). "app" = notifications from apps that DO
 *  have a rules entry: shared app renderer/cap + the per-app color rules
 *  (pkg=r,g,b) + suppressed packages. The app preset itself has NO color
 *  slot - each rule in the list owns its app's color. */
class NotificationView(context: Context) : ConfPage(context) {

    private lateinit var viewPicker: NamePicker
    private lateinit var defaultGroup: LinearLayout
    private lateinit var defaultKnobs: EventKnobs
    private lateinit var nPending: NumField
    private lateinit var appGroup: LinearLayout
    private lateinit var appKnobs: EventKnobs
    private lateinit var appPending: NumField
    private lateinit var suppressEt: android.widget.EditText
    private lateinit var rulesEt: android.widget.EditText

    override fun buildBody() {
        viewPicker = NamePicker("notification preset", listOf("default", "app")) { syncView() }
        body.addView(viewPicker)

        defaultGroup = LinearLayout(context).apply { orientation = VERTICAL }
        defaultKnobs = EventKnobs(
            "Default behavior (apps WITHOUT a rule)",
            "Notify renderer",
            "For apps without a rule.",
            capLabel = "max sec (0 == inf)"
        )
        defaultGroup.addView(defaultKnobs)
        defaultGroup.addView(card {
            addView(sectionTitle("Pending window (default)"))
            addView(spacer(4))
            addView(text(
                "While the screen is on, default notifications may hold this long for the screen to turn off before flashing.",
                12f, parse("#FF727272")
            ))
            addView(spacer(2))
            nPending = numRow("pending window (ms, screen-off flash)", "60000")
        })
        body.addView(defaultGroup)

        appGroup = LinearLayout(context).apply { orientation = VERTICAL }
        appKnobs = EventKnobs(
            "Per-app rules (apps WITH a rule)",
            "App renderer",
            "Rule-matched apps share this renderer",
            colorBuilder = {
                addView(text("Rule color, one per line (pkg=r,g,b). Apps without a rule use the default behavior above.", 12f, parse("#FF727272")))
                addView(spacer(4))
                rulesEt = multiLine("com.whatsapp=0,255,0")
                rulesEt.layoutParams = LayoutParams(mP, dpi(176))
                rulesEt
            },
            capLabel = "max sec (0 == inf)"
        )
        appGroup.addView(appKnobs)
        appGroup.addView(card {
            addView(sectionTitle("Pending window (app)"))
            addView(spacer(4))
            addView(text(
                "While the screen is on, rule-matched notifications may hold this long for the screen to turn off before flashing.",
                12f, parse("#FF727272")
            ))
            addView(spacer(2))
            appPending = numRow("pending window (ms, screen-off flash)", "60000")
        })
        appGroup.addView(card {
            addView(sectionTitle("Suppressed packages (one per line)"))
            addView(spacer(6))
            suppressEt = multiLine("")
            addView(suppressEt)
        })
        body.addView(appGroup)
        syncView()
    }

    private fun syncView() {
        val app = viewPicker.get() == 1
        defaultGroup.visibility = if (app) GONE else VISIBLE
        appGroup.visibility = if (app) VISIBLE else GONE
    }

    override fun applyTo(c: LedConf) {
        viewPicker.set(0)
        // default preset
        defaultKnobs.load(c.notifyRender, "breath")
        defaultKnobs.color.setColor(c.notifyColor)
        defaultKnobs.cap.setText(c.notifMaxSec.toString())
        nPending.setText(c.notifyScreenDelayMs.toString())
        // app preset
        appKnobs.load(c.notifyAppRender, "breath")
        appKnobs.cap.setText(c.notifAppMaxSec.toString())
        appPending.setText(c.notifAppScreenDelayMs.toString())
        suppressEt.setText(c.suppress.joinToString("\n"))
        rulesEt.setText(c.rules.joinToString("\n") { "${it.pkg}=${it.r},${it.g},${it.b}" })
    }

    override fun collectFrom(c: LedConf) {
        defaultKnobs.collect(c.notifyRender)
        c.notifyColor = defaultKnobs.color.getColor()
        c.notifMaxSec = defaultKnobs.cap.getLong(0L)
        c.notifyScreenDelayMs = nPending.getLong(60000L)
        appKnobs.collect(c.notifyAppRender)
        c.notifAppMaxSec = appKnobs.cap.getLong(0L)
        c.notifAppScreenDelayMs = appPending.getLong(60000L)
        // rules/suppressed replace, never append (base carries live config)
        c.suppress.clear()
        c.rules.clear()
        var bad = ""
        for (line in rulesEt.text.toString().lines()) {
            if (line.isBlank()) continue
            val i = line.indexOf('=')
            if (i <= 0) { bad += "bad rule: $line\n"; continue }
            val pkg = line.substring(0, i).trim()
            val parts = line.substring(i + 1).split(',')
            if (parts.size != 3) { bad += "bad rule: $line\n"; continue }
            val r = parts[0].trim().toIntOrNull() ?: -1
            val g = parts[1].trim().toIntOrNull() ?: -1
            val b = parts[2].trim().toIntOrNull() ?: -1
            if (r !in 0..255 || g !in 0..255 || b !in 0..255) {
                bad += "bad rule: $line\n"
                continue
            }
            c.rules.add(Rule(pkg, r, g, b))
        }
        c.suppress.addAll(suppressEt.text.toString().lines().filter { it.isNotBlank() })
        if (bad.isNotEmpty()) showMsg(bad.trimEnd(), error = true)
    }
}