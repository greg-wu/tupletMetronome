#include "PluginEditor.h"

namespace
{
    constexpr int editorWidth   = 620;
    constexpr int editorHeight  = 452;   // 比旧版多一行波形选择
    constexpr int margin        = 22;
    constexpr int titleHeight   = 48;
    constexpr int controlHeight = 30;
    constexpr int rowHeight     = 126;
}

//==============================================================================
SubMetronomeAudioProcessorEditor::SubMetronomeAudioProcessorEditor (SubMetronomeAudioProcessor& p)
    : juce::AudioProcessorEditor (p),
      processor (p)
{
    // ---- 旋钮外观：Attachment 会自动把参数范围/初值/步进灌进 Slider，无需 setRange ----
    juce::Slider* sliders[] = { &accentGainSlider,  &beatGainSlider,  &subGainSlider,
                                &accentPitchSlider, &beatPitchSlider, &subPitchSlider };
    juce::Label*  labels[]  = { &accentGainLabel,   &beatGainLabel,   &subGainLabel,
                                &accentPitchLabel,  &beatPitchLabel,  &subPitchLabel };

    for (auto* s : sliders)
    {
        s->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        s->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 68, 20);
        s->setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff5ec8d8));
        addAndMakeVisible (*s);
    }

    for (auto* l : labels)
    {
        l->setJustificationType (juce::Justification::centred);
        l->setColour (juce::Label::textColourId, juce::Colour (0xff9fb0c0));
        addAndMakeVisible (*l);
    }

    // ---- 每拍细分数 N：水平整数滑块，1..maxNotesPerBeat，任意 x 连音 ----
    subdivisionSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    subdivisionSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 48, 24);
    subdivisionSlider.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xff5ec8d8));
    addAndMakeVisible (subdivisionSlider);

    subdivisionLabel.setJustificationType (juce::Justification::centredLeft);
    subdivisionLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9fb0c0));
    addAndMakeVisible (subdivisionLabel);

    // ---- beat / sub 两个波形下拉框：先填条目，再创建 Attachment（见头文件注释）----
    const juce::StringArray waveformNames { "Sine", "Triangle", "Click", "Drum", "Cymbal" };

    for (auto* box : { &beatWaveBox, &subWaveBox })
    {
        box->addItemList (waveformNames, 1);
        box->setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff232a33));
        box->setColour (juce::ComboBox::textColourId,       juce::Colour (0xffe9f1f7));
        box->setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff3a4553));
        box->setColour (juce::ComboBox::arrowColourId,      juce::Colour (0xff9fb0c0));
        addAndMakeVisible (*box);
    }

    for (auto* l : { &beatWaveLabel, &subWaveLabel })
    {
        l->setJustificationType (juce::Justification::centredLeft);
        l->setColour (juce::Label::textColourId, juce::Colour (0xff9fb0c0));
        addAndMakeVisible (*l);
    }

    // ---- 与 APVTS 绑定：自动化 / 预设 / 参数改变全部双向同步 ----
    auto& state = processor.apvts;

    subdivisionAttachment = std::make_unique<SliderAttachment> (
        state, SubMetronomeParamIDs::subdivision, subdivisionSlider);

    accentGainAttachment = std::make_unique<SliderAttachment> (
        state, SubMetronomeParamIDs::accentGain, accentGainSlider);
    beatGainAttachment = std::make_unique<SliderAttachment> (
        state, SubMetronomeParamIDs::beatGain, beatGainSlider);
    subGainAttachment = std::make_unique<SliderAttachment> (
        state, SubMetronomeParamIDs::subGain, subGainSlider);

    accentPitchAttachment = std::make_unique<SliderAttachment> (
        state, SubMetronomeParamIDs::accentPitch, accentPitchSlider);
    beatPitchAttachment = std::make_unique<SliderAttachment> (
        state, SubMetronomeParamIDs::beatPitch, beatPitchSlider);
    subPitchAttachment = std::make_unique<SliderAttachment> (
        state, SubMetronomeParamIDs::subPitch, subPitchSlider);

    beatWaveAttachment = std::make_unique<ComboBoxAttachment> (
        state, SubMetronomeParamIDs::waveform, beatWaveBox);

    subWaveAttachment = std::make_unique<ComboBoxAttachment> (
        state, SubMetronomeParamIDs::subWaveform, subWaveBox);

    setSize (editorWidth, editorHeight);
}

//==============================================================================
void SubMetronomeAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff171b21));

    g.setColour (juce::Colour (0xffe9f1f7));
    g.setFont (22.0f);
    g.drawText ("tupletMetronome", 0, 8, getWidth(), 30,
                juce::Justification::centred, true);

    g.setFont (11.0f);
    g.setColour (juce::Colour (0xff728294));
    const auto info = juce::String ("DAW synced  -  1..")
                    + juce::String (SubMetronomeAudioProcessor::maxNotesPerBeat)
                    + " notes per beat  -  independent beat / sub waveforms";
    g.drawText (info, 0, 34, getWidth(), 16, juce::Justification::centred, true);
}

void SubMetronomeAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (margin);
    area.removeFromTop (titleHeight);

    // 行 1：标签 + 每拍细分数滑块
    auto notesRow = area.removeFromTop (controlHeight);
    subdivisionLabel.setBounds (notesRow.removeFromLeft (108));
    subdivisionSlider.setBounds (notesRow);

    area.removeFromTop (12);

    // 行 2：正拍波形 / 细分拍波形，左右各半
    auto waveRow = area.removeFromTop (controlHeight);
    auto leftHalf = waveRow.removeFromLeft ((waveRow.getWidth() - 12) / 2);
    waveRow.removeFromLeft (12);

    beatWaveLabel.setBounds (leftHalf.removeFromLeft (92));
    beatWaveBox.setBounds (leftHalf);

    subWaveLabel.setBounds (waveRow.removeFromLeft (92));
    subWaveBox.setBounds (waveRow);

    area.removeFromTop (16);

    const int colWidth = area.getWidth() / 3;

    const auto placeColumn = [] (juce::Rectangle<int> row, int col, int width,
                                 juce::Label& label, juce::Slider& slider)
    {
        for (int i = 0; i < col; ++i)
            row.removeFromLeft (width);

        auto cell = row.removeFromLeft (width).reduced (8, 0);
        label.setBounds (cell.removeFromTop (20));
        slider.setBounds (cell);
    };

    auto levelRow = area.removeFromTop (rowHeight);
    placeColumn (levelRow, 0, colWidth, accentGainLabel,  accentGainSlider);
    placeColumn (levelRow, 1, colWidth, beatGainLabel,    beatGainSlider);
    placeColumn (levelRow, 2, colWidth, subGainLabel,     subGainSlider);

    area.removeFromTop (12);

    auto pitchRow = area.removeFromTop (rowHeight);
    placeColumn (pitchRow, 0, colWidth, accentPitchLabel, accentPitchSlider);
    placeColumn (pitchRow, 1, colWidth, beatPitchLabel,   beatPitchSlider);
    placeColumn (pitchRow, 2, colWidth, subPitchLabel,    subPitchSlider);
}
