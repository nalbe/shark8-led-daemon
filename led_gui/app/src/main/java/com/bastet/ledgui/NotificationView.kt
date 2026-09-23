package com.bastet.ledgui

import android.app.AlertDialog
import android.content.Context
import android.graphics.Color
import android.graphics.drawable.GradientDrawable
import android.util.TypedValue
import android.view.ContextThemeWrapper
import android.view.Gravity
import android.view.View
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/** Notification tab, split in two sub-pages like before:
 *
 *  "default" = the shared preset: notifications WITHOUT a per-app rule
 *  (own color, cap, renderer) plus the shared pending window.
 *
 *  "app" = per-app rules: apps WITH a rule entry. Each rule owns its
 *  color (pkg=r,g,b) plus its OWN full preset (cap, renderer),
 *  serialized as the extended [rules] line. The list shows the classic
 *  color swatch + package name + delete button; tapping a row opens the
 *  full per-app editor (color, cap, renderer). New rules are seeded from
 *  the default preset. The pending window is NOT per-app: one shared
 *  [notify] value edited in the "Pending window" card.
 *
 *  Suppressed apps stay a plain list. Rules and suppressed apps are
 *  edited through an app picker (no free text); each list lives in a
 *  fixed-height internal-scroll box. Rule swatches preview through that
 *  rule's OWN renderer (cur + sync), like the Status live swatch.
 *
 *  Legacy: pre-v5 files used a shared [notify.app] preset for color-only
 *  rules. LedConf seeds those rules from it once and promotes them to
 *  full lines (custom=true), so nothing is lost on save - the GUI no
 *  longer writes [notify.app]. */
class NotificationView(context: Context) : ConfPage(context) {

    /** App sub-page rule blocks:
     *    on 1080x2460: 845 + 64 + 845 = 1754.
     *  Frame-to-frame gap = 32px + 32px margins. */
    private companion object {
        const val APP_CARD_PX_TOP = 845
        const val APP_CARD_PX_BOTTOM = 845
    }

    private lateinit var viewPicker: NamePicker
    private lateinit var defaultGroup: LinearLayout
    private lateinit var appGroup: LinearLayout
    private lateinit var defaultKnobs: EventKnobs
    private lateinit var nPending: NumField
    private lateinit var ruleList: LinearLayout
    private lateinit var suppressList: LinearLayout

    private val localRules = mutableListOf<Rule>()
    private val localSuppress = mutableListOf<String>()
    private val appCatalog: Map<String, String> by lazy { installedAppCatalog() }

    private class RuleRow(val sw: View, val pkg: String)
    private val ruleRows = mutableListOf<RuleRow>()

    override fun buildBody() {
        viewPicker = NamePicker("notify view", listOf("default", "app")) { syncView() }
        body.addView(viewPicker)

        defaultGroup = LinearLayout(context).apply { orientation = VERTICAL }
        defaultKnobs = EventKnobs(
            "Default behavior (apps WITHOUT a rule)",
            "Default renderer",
            "Apps without a rule share this preset.",
            capLabel = "max sec (0 == inf)"
        )
        defaultGroup.addView(defaultKnobs)
        defaultGroup.addView(card {
            addView(sectionTitle("Pending window (all apps)"))
            addView(spacer(4))
            addView(text(
                "Shared: while the screen is on, ANY parked notification may hold this long for the screen to turn off before flashing.",
                12f, parse("#FF727272")
            ))
            addView(spacer(2))
            nPending = numRow("pending window (ms, screen-off flash)", "60000")
        })
        body.addView(defaultGroup)

        appGroup = LinearLayout(context).apply { orientation = VERTICAL }
        val rulesCard = card {
            addView(titleRow("Per-app rules (apps WITH a rule)") {
                pickApp("Add rule (flashing app)", rulePkgs()) { pkg, _ -> addRule(pkg) }
            })
            addView(spacer(4))
            addView(text(
                "Each rule owns its color, cap and renderer. Tap a row to edit its settings - the swatch previews that app's own renderer (cur + sync).",
                12f, parse("#FF727272")
            ))
            addView(spacer(4))
            ruleList = scrollListBox(stretch = true)
        }
        rulesCard.layoutParams = LayoutParams(mP, APP_CARD_PX_TOP).apply {
            setMargins(dpi(12), dpi(6), dpi(12), 32)
        }
        appGroup.addView(rulesCard)
        val suppressCard = card {
            addView(titleRow("Suppressed packages") {
                pickApp("Add suppressed app", suppressPkgs()) { pkg, _ -> addSuppressed(pkg) }
            })
            addView(spacer(4))
            addView(text("Apps that should not flash at all.", 12f, parse("#FF727272")))
            addView(spacer(4))
            suppressList = scrollListBox(stretch = true)
        }
        suppressCard.layoutParams = LayoutParams(mP, APP_CARD_PX_BOTTOM).apply {
            setMargins(dpi(12), 32, dpi(12), dpi(6))
        }
        appGroup.addView(suppressCard)
        body.addView(appGroup)
        syncView()
    }

    /** Card title row: the "+ Add" pill pinned at the right edge. */
    private fun titleRow(title: String, onAdd: () -> Unit): View {
        val r = LinearLayout(context).apply {
            orientation = HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            addView(sectionTitle(title), LayoutParams(0, wP, 1f))
            addView(addPill(onAdd))
        }
        return r
    }

    private fun addPill(onClick: () -> Unit): View {
        val pill = TextView(context).apply {
            text = "+ Add"
            setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f)
            setTextColor(parse("#FF90CAF9"))
            setPadding(dpi(14), dpi(4), dpi(14), dpi(4))
            gravity = Gravity.CENTER
            layoutParams = LayoutParams(wP, wP).apply { marginStart = dpi(8) }
            val g = GradientDrawable()
            g.shape = GradientDrawable.RECTANGLE
            g.cornerRadius = dpf(15)
            g.setColor(parse("#FF1E1E1E"))
            g.setStroke(dpi(1), parse("#FF5C6BC0"))
            background = g
            setOnClickListener { onClick() }
        }
        return pill
    }

    private fun syncView() {
        val app = viewPicker.get() == 1
        defaultGroup.visibility = if (app) GONE else VISIBLE
        appGroup.visibility = if (app) VISIBLE else GONE
    }

    override fun applyTo(c: LedConf) {
        defaultKnobs.load(c.notifyRender, "breath")
        defaultKnobs.color.setColor(c.notifyColor)
        defaultKnobs.cap.setText(c.notifMaxSec.toString())
        nPending.setText(c.notifyScreenDelayMs.toString())
        localRules.clear()
        localRules.addAll(c.rules)
        localSuppress.clear()
        localSuppress.addAll(c.suppress)
        renderRules()
        renderSuppress()
    }

    override fun collectFrom(c: LedConf) {
        defaultKnobs.collect(c.notifyRender)
        c.notifyColor = defaultKnobs.color.getColor()
        c.notifMaxSec = defaultKnobs.cap.getLong(0L)
        c.notifyScreenDelayMs = nPending.getLong(60000L)
        // lists replace, never append (base carries live config)
        c.suppress.clear()
        c.suppress.addAll(localSuppress)
        c.rules.clear()
        c.rules.addAll(localRules)
    }

    private fun rulePkgs(): Set<String> = localRules.map { it.pkg }.toSet()
    private fun suppressPkgs(): Set<String> = localSuppress.toSet()

    /** New rule: white color, everything else seeded from the CURRENT
     *  default preset (the settings a rule without its own tail used to
     *  get before the split). */
    private fun addRule(pkg: String) {
        if (localRules.any { it.pkg == pkg }) return
        val r = Render()
        defaultKnobs.collect(r)
        localRules.add(Rule(
            pkg, 255, 255, 255,
            maxSec = defaultKnobs.cap.getLong(0L),
            render = r,
            custom = true
        ))
        renderRules()
    }

    private fun addSuppressed(pkg: String) {
        if (pkg !in localSuppress) localSuppress.add(pkg)
        renderSuppress()
    }

    private fun renderRules() {
        ruleList.removeAllViews()
        ruleRows.clear()
        for (rule in localRules) {
            val sw = swatchView(30)
            val line = row(
                sw,
                appLine(appCatalog[rule.pkg] ?: rule.pkg, rule.pkg),
                iconBtn("x") { localRules.remove(rule); renderRules() }
            )
            sw.setOnClickListener { openRuleEditor(rule.pkg) }
            line.setOnClickListener { openRuleEditor(rule.pkg) }
            ruleRows.add(RuleRow(sw, rule.pkg))
            ruleList.addView(line)
        }
        repaintRules()
    }

    private fun renderSuppress() {
        suppressList.removeAllViews()
        for (pkg in localSuppress) {
            suppressList.addView(row(
                appLine(appCatalog[pkg] ?: pkg, pkg),
                iconBtn("x") { localSuppress.remove(pkg); renderSuppress() }
            ))
        }
    }

    /** Full per-app editor: color, cap and the renderer, composed in a
     *  modal like the other event pages. Applies on OK. */
    private fun openRuleEditor(pkg: String) {
        val rule = localRules.find { it.pkg == pkg } ?: return
        val editorColor = RgbPicker("color - event color")
        editorColor.setColor(Triple(rule.r, rule.g, rule.b))
        val cap = NumField("max sec (0 == inf)",
            rule.maxSec.toString())
        val renderer = RenderCard("Renderer", "This app's own renderer (the rule carries it inline).")
        renderer.load(rule.render, "breath")
        renderer.attachPreview(editorColor)

        val content = LinearLayout(context).apply {
            orientation = VERTICAL
            setPadding(dpi(4), dpi(4), dpi(4), dpi(4))
            addView(text(appCatalog[pkg] ?: pkg, 15f, parse("#FF90CAF9"), bold = true))
            addView(text(pkg, 12f, parse("#FF727272"), mono = true))
            addView(spacer(6))
            addView(editorColor)
            addView(spacer(4))
            addView(cap)
            addView(spacer(4))
            addView(renderer)
        }
        val scroll = ScrollView(context)
        scroll.isFillViewport = true
        scroll.addView(content, LayoutParams(mP, wP))
        scroll.layoutParams = LayoutParams(mP, dpi(640))

        val dctx = ContextThemeWrapper(context, android.R.style.Theme_Material)
        AlertDialog.Builder(dctx)
            .setTitle("Rule settings")
            .setView(scroll)
            .setPositiveButton("OK") { d, _ ->
                val i = localRules.indexOfFirst { it.pkg == pkg }
                if (i >= 0) {
                    val r = localRules[i]
                    val c = editorColor.getColor()
                    r.r = c.first; r.g = c.second; r.b = c.third
                    r.maxSec = cap.getLong(0L)
                    renderer.collect(r.render)
                    r.custom = true
                    repaintRules()
                }
                d.dismiss()
            }
            .setNegativeButton("Cancel") { d, _ -> d.dismiss() }
            .show()
    }

    /** (cur, sync) a renderer drives, from that render's own config -
     *  same math as the Status live swatch. off = all dark. */
    private fun driveFor(r: Render): Pair<Triple<Int, Int, Int>, Boolean> {
        val sync = (r.mode == "breath" && r.brSync) ||
            (r.mode == "wave" && r.waveSync)
        val cur = when (r.mode) {
            "solid" -> r.solidCur
            "breath" -> r.brCur
            "off" -> Triple(0, 0, 0)
            else -> Triple(15, 15, 15)
        }
        return Pair(cur, sync)
    }

    /** Rule swatches preview the ACTUAL light each rule produces: the rule
     *  color driven through ITS OWN renderer (cur + sync). */
    private fun repaintRules() {
        if (!::ruleList.isInitialized) return
        for (row in ruleRows) {
            val rule = localRules.find { it.pkg == row.pkg } ?: continue
            val (cur, sync) = driveFor(rule.render)
            val rgb = Triple(rule.r, rule.g, rule.b)
            val s = LedSim.rgbWithCurrent(rgb, cur, sync)
            (row.sw.background as? GradientDrawable)?.setColor(
                Color.rgb(s.first, s.second, s.third)
            )
        }
    }
}