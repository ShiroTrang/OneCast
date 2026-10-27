#include "MeterComponent.h"
#include "Theme.h"

//==============================================================================
MeterComponent::MeterComponent()
{
    startTimerHz (30);
}

void MeterComponent::setLevel (float newLevelDb)
{
    currentLevel.store (juce::jlimit (minDb, maxDb, newLevelDb),
                        std::memory_order_relaxed);
}

//==============================================================================
void MeterComponent::timerCallback()
{
    const float target = currentLevel.load (std::memory_order_relaxed);

    // Smooth display level - fast attack, slow decay.
    if (target > displayLevel)
        displayLevel = target;
    else
        displayLevel = juce::jmax (displayLevel - decayRate, target);

    // Peak hold logic.
    if (target >= peakLevel)
    {
        peakLevel       = target;
        peakHoldCounter = peakHoldFrames;
    }
    else if (peakHoldCounter > 0)
    {
        --peakHoldCounter;
    }
    else
    {
        peakLevel = juce::jmax (peakLevel - decayRate, minDb);
    }

    repaint();
}

//==============================================================================
float MeterComponent::dbToNorm (float db)
{
    return juce::jlimit (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
}

juce::Colour MeterComponent::colourForNorm (float norm)
{
    // -60 dB .. -12 dB  =>  norm 0.0 .. ~0.727  =>  green
    // -12 dB ..  -3 dB  =>  norm ~0.727 .. ~0.864  =>  yellow
    //  -3 dB ..  +6 dB  =>  norm ~0.864 .. 1.0  =>  red

    const float greenEnd  = (-12.0f - minDb) / (maxDb - minDb);  // ~0.727
    const float yellowEnd = ( -3.0f - minDb) / (maxDb - minDb);  // ~0.864

    if (norm <= greenEnd)
        return juce::Colour (0xff00cc66);  // green

    if (norm <= yellowEnd)
    {
        const float t = (norm - greenEnd) / (yellowEnd - greenEnd);
        return juce::Colour (0xff00cc66).interpolatedWith (juce::Colour (0xffffcc00), t);
    }

    const float t = (norm - yellowEnd) / (1.0f - yellowEnd);
    return juce::Colour (0xffffcc00).interpolatedWith (juce::Colour (0xffff3333), t);
}

//==============================================================================
void MeterComponent::paint (juce::Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();

    // Reserve space at the bottom for the dB text label.
    const float textHeight = 16.0f;
    const auto meterArea = bounds.withTrimmedBottom (textHeight);

    // Background.
    g.setColour (Theme::wellBg);
    g.fillRoundedRectangle (meterArea, 2.0f);

    // Filled portion.
    const float norm   = dbToNorm (displayLevel);
    const float fillH  = meterArea.getHeight() * norm;
    const auto  fillR  = meterArea.withTop (meterArea.getBottom() - fillH);

    if (fillH > 0.0f)
    {
        // Draw with colour gradient - paint thin horizontal slices.
        const int slices = juce::jmax (1, static_cast<int> (fillH));
        const float sliceH = fillH / static_cast<float> (slices);

        for (int i = 0; i < slices; ++i)
        {
            const float y = fillR.getY() + static_cast<float> (i) * sliceH;
            const float sliceNorm = 1.0f - (y - meterArea.getY()) / meterArea.getHeight();
            g.setColour (colourForNorm (sliceNorm));
            g.fillRect (fillR.getX(), y, fillR.getWidth(), sliceH);
        }
    }

    // Peak hold indicator - a thin horizontal line.
    const float peakNorm = dbToNorm (peakLevel);
    if (peakNorm > 0.0f)
    {
        const float peakY = meterArea.getBottom() - meterArea.getHeight() * peakNorm;
        g.setColour (colourForNorm (peakNorm).brighter (0.3f));
        g.fillRect (meterArea.getX(), peakY, meterArea.getWidth(), 2.0f);
    }

    // Border around the meter.
    g.setColour (Theme::border);
    g.drawRoundedRectangle (meterArea, 2.0f, 1.0f);

    // dB text below the meter (this is the live level, not the gain setting).
    g.setColour (Theme::textSecondary);
    g.setFont (Theme::font (10.0f));

    juce::String dbText;
    if (displayLevel <= minDb)
        dbText = "-inf";
    else
        dbText = juce::String (displayLevel, 1) + " dB";

    g.drawText (dbText,
                juce::Rectangle<int> (static_cast<int> (meterArea.getX()),
                                      static_cast<int> (meterArea.getBottom()),
                                      static_cast<int> (meterArea.getWidth()),
                                      static_cast<int> (textHeight)),
                juce::Justification::centred, false);
}
