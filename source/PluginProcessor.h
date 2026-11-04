#pragma once

#include <JuceHeader.h>   // 若使用细粒度 CMake 模块，可替换为具体 juce_* 头文件
#include <array>
#include <atomic>
#include <cmath>

//==============================================================================
// 参数 ID：Editor、APVTS、自动化、预设保存全部使用同一份常量，避免字符串拼写错误。
namespace SubMetronomeParamIDs
{
    inline constexpr const char* accentGain  = "accentGain";   // dB, -60 .. +6
    inline constexpr const char* beatGain    = "beatGain";     // dB
    inline constexpr const char* subGain     = "subGain";      // dB
    inline constexpr const char* accentPitch = "accentPitch";  // Hz
    inline constexpr const char* beatPitch   = "beatPitch";    // Hz
    inline constexpr const char* subPitch    = "subPitch";     // Hz
    inline constexpr const char* subdivision = "subdivision";  // 每拍细分数 N（整数 1..maxNotesPerBeat）
    inline constexpr const char* waveform    = "waveform";     // 0=Sine, 1=Triangle, 2=Click
}

//==============================================================================
/**
    tupletMetronome —— 与 DAW 传输同步的任意细分节拍器。

    核心设计（实现见 PluginProcessor.cpp）：
      * 插件内部不维护自由运行的相位累加器。每个 processBlock 从 AudioPlayHead
        读取绝对 PPQ，求「本音频块覆盖的 PPQ 区间」与「细分网格」的交点。
        这样可自动跟随宿主的 tempo map / 渐变 / 跳转 / 循环 / 拍号变化。
      * 网格线用整数索引 k 表示：ppq = k / N，其中 N =「一拍内几个音」，
        由整数参数直接给定（1..maxNotesPerBeat）。N=2 八分音符、N=3 八分三连音、
        N=4 十六分、N=5/7/9... 任意 x 连音，全部走同一套算法。
        节点永远用一次除法从整数 k 求出，绝不用 0.333 之类的小数累加（会漂移）。
      * 音频线程零分配、零锁、零字符串查找：参数用 atomic 缓存，
        事件与人声池（voice pool）用固定长度 std::array。
*/
class SubMetronomeAudioProcessor : public juce::AudioProcessor
{
public:
    //==========================================================================
    /** 一拍内最多可定义的细分音数量；改这一处即可整体调整上限（逻辑与 UI 共用）。 */
    static constexpr int maxNotesPerBeat = 32;

    SubMetronomeAudioProcessor();
    ~SubMetronomeAudioProcessor() override = default;

    //---- AudioProcessor 生命周期 ---------------------------------------------
    void prepareToPlay (double newSampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using AudioProcessor::processBlock;   // 避免隐藏 AudioBuffer<double> 重载 (JUCE 9)

    bool isBusesLayoutSupported (const juce::AudioProcessor::BusesLayout& layouts) const override
    {
        const auto& out = layouts.getMainOutputChannelSet();
        return out == juce::AudioChannelSet::mono()
            || out == juce::AudioChannelSet::stereo();
    }

    //---- Editor / 插件元数据 --------------------------------------------------
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                     { return true; }

    const juce::String getName() const override         { return "tupletMetronome"; }
    bool acceptsMidi()  const override                  { return false; }
    bool producesMidi() const override                  { return false; }
    bool isMidiEffect() const override                  { return false; }

    // 停止时立即静音并清空 voice，因此不需要尾音时间
    double getTailLengthSeconds() const override        { return 0.0; }

    //---- Programs（单预设即可，状态由 APVTS 保存） -----------------------------
    int getNumPrograms() override                       { return 1; }
    int getCurrentProgram() override                    { return 0; }
    void setCurrentProgram (int) override               {}
    const juce::String getProgramName (int) override    { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    //---- 状态持久化：DAW 工程保存 / 预设 ----------------------------------------
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    //---- APVTS 参数布局（JUCE 9 推荐的 ...Attributes 构造） ---------------------
    // 本工程 submodule 锁定 JUCE 9.0.5：旧版「带 label」的构造已被 [[deprecated]]，
    // 改用 AudioParameterFloatAttributes{}.withLabel(...) 保持零警告。
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
    {
        juce::AudioProcessorValueTreeState::ParameterLayout layout;

        const auto makeDb = [] (const char* id, const juce::String& name, float defaultValue)
        {
            return std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { id, 1 }, name,
                juce::NormalisableRange<float> { -60.0f, 6.0f, 0.1f },
                defaultValue,
                juce::AudioParameterFloatAttributes {}.withLabel ("dB"));
        };

        const auto makeHz = [] (const char* id, const juce::String& name, float defaultValue)
        {
            return std::make_unique<juce::AudioParameterFloat> (
                juce::ParameterID { id, 1 }, name,
                juce::NormalisableRange<float> { 200.0f, 2400.0f, 1.0f },
                defaultValue,
                juce::AudioParameterFloatAttributes {}.withLabel ("Hz"));
        };

        // 三组独立音量：首拍 / 正拍 / 细分拍
        layout.add (makeDb (SubMetronomeParamIDs::accentGain, "Accent Gain",   0.0f));
        layout.add (makeDb (SubMetronomeParamIDs::beatGain,   "Beat Gain",    -6.0f));
        layout.add (makeDb (SubMetronomeParamIDs::subGain,    "Sub Gain",    -12.0f));

        // 三组独立音高：直接用频率（Hz），避免半音换算带来的额外开销
        layout.add (makeHz (SubMetronomeParamIDs::accentPitch, "Accent Pitch", 1200.0f));
        layout.add (makeHz (SubMetronomeParamIDs::beatPitch,   "Beat Pitch",    800.0f));
        layout.add (makeHz (SubMetronomeParamIDs::subPitch,    "Sub Pitch",     600.0f));

        // ★ 每拍细分数 N：整数参数，1..maxNotesPerBeat。
        //   N=1 四分音符、2 八分、3 八分三连音、4 十六分、5/7/9... 任意 x 连音。
        //   默认 3 保留三连音作为开箱即用的主打手感。
        //   getRawParameterValue() 返回的就是这个整数本身（非归一化值）。
        layout.add (std::make_unique<juce::AudioParameterInt> (
            juce::ParameterID { SubMetronomeParamIDs::subdivision, 1 },
            "Notes Per Beat", 1, maxNotesPerBeat, 3));

        // 内建极简波形（UI 可省略此控件，由代码或自动化控制）
        layout.add (std::make_unique<juce::AudioParameterChoice> (
            juce::ParameterID { SubMetronomeParamIDs::waveform, 1 }, "Waveform",
            juce::StringArray { "Sine", "Triangle", "Click" }, 0));

        return layout;
    }

    /** APVTS 必须是 public 成员：Editor 的 SliderAttachment 需要引用它。 */
    juce::AudioProcessorValueTreeState apvts;

    //==========================================================================
    //---- AudioPlayHead 读取逻辑（内联，processBlock 每块只调用一次） ------------
    struct TransportSnapshot
    {
        bool   valid        = false;   // getPosition() 是否成功且字段齐全
        bool   isPlaying    = false;   // DAW 是否正在播放（非播放即静音）
        double bpm          = 120.0;   // 块起点的瞬时 BPM
        double ppq          = 0.0;     // 块起点 PPQ，单位 = 四分音符
        double barStartPpq  = 0.0;     // 本小节起点 PPQ，用于判定首拍 accent
        int    numerator    = 4;       // 拍号分子（缺省 4/4）
        double ppqPerSample = 0.0;     // 块内 PPQ -> sample 的换算系数
    };

    static TransportSnapshot readTransport (juce::AudioPlayHead* playHead,
                                            double sampleRate) noexcept
    {
        TransportSnapshot s;

        // 独立运行（Standalone）或无传输信息时，getPosition() 返回空 Optional
        if (playHead == nullptr)
            return s;

        // JUCE 9 返回 juce::Optional（JUCE 8 是 std::optional）。
        // 只使用 operator bool / operator* / operator->，两种版本都能编译。
        const auto pos = playHead->getPosition();
        if (! pos)
            return s;

        const auto bpm = pos->getBpm();
        const auto ppq = pos->getPpqPosition();
        if (! bpm || ! ppq || *bpm <= 0.0)
            return s;

        s.valid       = true;
        s.isPlaying   = pos->getIsPlaying();
        s.bpm         = *bpm;
        s.ppq         = *ppq;

        // 某些精简宿主不提供小节信息：退化为「以 PPQ 0 为起点、4/4」
        const auto barStart = pos->getPpqPositionOfLastBarStart();
        s.barStartPpq = barStart ? *barStart : 0.0;

        const auto sig = pos->getTimeSignature();
        if (sig && sig->numerator > 0)
            s.numerator = sig->numerator;

        // 1 sample 对应多少个四分音符；BPM 越高该值越大
        s.ppqPerSample = s.bpm / (60.0 * juce::jmax (sampleRate, 1.0));
        return s;
    }

    /** 参数值 -> 每拍细分数 N（参数本身就是 N，只需夹紧到合法范围）。 */
    static int gridDivisionsForMode (int notesPerBeat) noexcept
    {
        return juce::jlimit (1, maxNotesPerBeat, notesPerBeat);
    }

private:
    //==========================================================================
    //---- RT 安全的参数缓存（构造时由 getRawParameterValue 绑定） ----------------
    std::atomic<float>* accentGainDb     = nullptr;
    std::atomic<float>* beatGainDb       = nullptr;
    std::atomic<float>* subGainDb        = nullptr;
    std::atomic<float>* accentPitchHz    = nullptr;
    std::atomic<float>* beatPitchHz      = nullptr;
    std::atomic<float>* subPitchHz       = nullptr;
    std::atomic<float>* subdivisionParam = nullptr;
    std::atomic<float>* waveformParam    = nullptr;

    //---- 固定长度的音频线程数据结构：绝不 new / 绝不加锁 ------------------------
    struct ClickEvent
    {
        int   sampleOffset  = 0;      // 相对本块起点的样本偏移
        float amplitude     = 0.0f;   // 线性增益（已由 dB 转换）
        float frequency     = 0.0f;
        int   waveform      = 0;
        int   lengthSamples = 0;      // 自适应 click 长度：高 N / 高 BPM 时自动缩短
    };

    struct ClickVoice
    {
        bool   active       = false;
        int    samplesLeft  = 0;
        int    totalSamples = 0;
        double phase        = 0.0;
        double phaseInc     = 0.0;   // 2*pi*freq/sampleRate
        float  amplitude    = 0.0f;
        int    waveform     = 0;
    };

    static constexpr int maxEventsPerBlock = 128;  // N=32 时单块事件数仍远小于此
    static constexpr int maxVoices        = 32;
    static constexpr int clickLengthMs    = 12;   // 默认包络上限，高 N 时按比例缩短

    double sampleRate   = 44100.0;
    int    blockSize    = 0;

    // 上一块的传输快照：用于识别「停止 -> 播放」「循环 / 定位跳转」，
    // 防止一次性补发成百上千个 click。
    TransportSnapshot previous {};
    bool gridNeedsResync = true;

    std::array<ClickEvent, maxEventsPerBlock> events {};
    int numEvents = 0;

    std::array<ClickVoice, maxVoices> voices {};
    int nextVoice = 0;

    //---- 音频线程实现 ---------------------------------------------------------
    void resetRealtimeState() noexcept;
    void collectGridEvents (const TransportSnapshot& t, int numSamples) noexcept;
    void startVoice (const ClickEvent& e) noexcept;
    void renderVoices (juce::AudioBuffer<float>& buffer) noexcept;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SubMetronomeAudioProcessor)
};
