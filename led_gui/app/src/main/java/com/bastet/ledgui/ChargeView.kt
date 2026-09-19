package com.bastet.ledgui

import android.content.Context
import android.widget.LinearLayout

/** Charge tab: the band selector lives in the HEADER; below it the
 *  settings owned by the selected band. Each band keeps its OWN renderer
 *  card (color + mode + chip params/timing) so edits survive switching.
 *  The [charge] base keeps only the thresholds. */
class ChargeView(context: Context) : ConfPage(context) {

    private val bandNames = listOf("low", "mid", "hi")
    private lateinit var bandPicker: NamePicker
    private lateinit var first: NumField
    private lateinit var second: NumField
    private lateinit var bandGroup: LinearLayout
    private lateinit var cardLow: RenderCard
    private lateinit var cardMid: RenderCard
    private lateinit var cardHi: RenderCard

    override fun buildBody() {
        bandPicker = NamePicker("charge band (own renderer per band)", bandNames) { syncBand() }
        body.addView(bandPicker)
        body.addView(card {
            addView(sectionTitle("Charge thresholds (percent, order-free)"))
            addView(spacer(4))
            first = numRow("first threshold", "90")
            second = numRow("second threshold", "95")
        })
        bandGroup = LinearLayout(context).apply { orientation = VERTICAL }

        cardLow = RenderCard(
            "Charge band LOW renderer",
            "Below first threshold.",
            showColor = true
        )
        bandGroup.addView(cardLow)

        cardMid = RenderCard(
            "Charge band MID renderer",
            "Between thresholds.",
            showColor = true
        )
        bandGroup.addView(cardMid)

        cardHi = RenderCard(
            "Charge band HIGH renderer",
            "At/above 2nd threshold / Full.",
            showColor = true
        )
        bandGroup.addView(cardHi)
        body.addView(bandGroup)
        syncBand()
    }

    private fun loadCardInto(card: RenderCard, r: Render, color: Triple<Int, Int, Int>) {
        card.load(r, "breath")
        card.setColor(color)
    }

    private fun syncBand() {
        val cards = listOf(cardLow, cardMid, cardHi)
        cards.forEachIndexed { i, v -> v.visibility = if (i == bandPicker.get()) VISIBLE else GONE }
    }

    override fun applyTo(c: LedConf) {
        bandPicker.set(0)
        first.setText(c.firstThreshold.toString())
        second.setText(c.secondThreshold.toString())
        loadCardInto(cardLow, c.chargeLower, c.lowerColor)
        loadCardInto(cardMid, c.chargeMiddle, c.middleColor)
        loadCardInto(cardHi, c.chargeUpper, c.upperColor)
        syncBand()
    }

    override fun collectFrom(c: LedConf) {
        c.firstThreshold = first.getInt(90)
        c.secondThreshold = second.getInt(95)
        cardLow.collect(c.chargeLower)
        cardLow.getColor()?.let { c.lowerColor = it }
        cardMid.collect(c.chargeMiddle)
        cardMid.getColor()?.let { c.middleColor = it }
        cardHi.collect(c.chargeUpper)
        cardHi.getColor()?.let { c.upperColor = it }
    }
}