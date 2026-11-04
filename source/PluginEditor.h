#pragma once

#include <JuceHeader.h>
#include "PluginProcessor.h"
#include <memory>

//==============================================================================
/**
    极简 UI。

    所有控件通过 APVTS 的 Attachment 与参数双向绑定：
      * 拖动控件                -> 写入参数（DAW 自动化可覆盖）
      * 自动化 / 预设 / 工程恢复 -> 自动刷新控件显示
    Editor 不持有任何音频状态，也不读取音频线程的原子变量。
*/
class SubMetronomeAudioProcessorEditor : public juce::AudioProcessorEditor
{
public:
    explicit SubMetronomeAudioProcessorEditor (SubMetronomeAudioProcessor& p);
    ~SubMetronomeAudioProcessorEditor() override = default;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    // JUCE 9 中 SliderAttachment / ComboBoxAttachment 是 APVTS 的嵌套类型
    using SliderAttachment   = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ComboBoxAttachment = juce::AudioProcessorValueTreeState::ComboBoxAttachment;

    SubMetronomeAudioProcessor& processor;

    // ---- 控件必须先于 Attachment 声明，保证成员初始化顺序 ----
    juce::Slider accentGainSlider,  beatGainSlider,  subGainSlider;
    juce::Slider accentPitchSlider, beatPitchSlider, subPitchSlider;
    juce::Slider subdivisionSlider;   // 每拍细分数 N（整数 1..maxNotesPerBeat）
    juce::ComboBox waveformBox;

    juce::Label subdivisionLabel { {}, "NOTES / BEAT" };
    juce::Label accentGainLabel  { {}, "ACCENT VOL" };
    juce::Label beatGainLabel    { {}, "BEAT VOL" };
    juce::Label subGainLabel     { {}, "SUB VOL" };
    juce::Label accentPitchLabel { {}, "ACCENT Hz" };
    juce::Label beatPitchLabel   { {}, "BEAT Hz" };
    juce::Label subPitchLabel    { {}, "SUB Hz" };

    // 用 unique_ptr 延迟创建：waveform ComboBox 必须先 addItemList 再绑定，
    // 否则 ComboBoxParameterAttachment 用 getNumItems()-1 做映射会得到 -1。
    std::unique_ptr<SliderAttachment> subdivisionAttachment;
    std::unique_ptr<SliderAttachment> accentGainAttachment;
    std::unique_ptr<SliderAttachment> beatGainAttachment;
    std::unique_ptr<SliderAttachment> subGainAttachment;
    std::unique_ptr<SliderAttachment> accentPitchAttachment;
    std::unique_ptr<SliderAttachment> beatPitchAttachment;
    std::unique_ptr<SliderAttachment> subPitchAttachment;

    std::unique_ptr<ComboBoxAttachment> waveformAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubMetronomeAudioProcessorEditor)
};
