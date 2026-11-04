//==============================================================================
//  tupletMetronome / 核心音频逻辑
//  processBlock：DAW 时钟同步 + 任意 N 连音细分网格 + 极简内建 click 合成
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>

//==============================================================================
SubMetronomeAudioProcessor::SubMetronomeAudioProcessor()
    : AudioProcessor (juce::AudioProcessor::BusesProperties()
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    // 构造期一次性把参数缓存成 std::atomic<float>*。
    // getRawParameterValue() 返回的是「非归一化的实际值」（dB / Hz / N / 选项索引），
    // 之后音频线程只读原子值：不做 ValueTree 查找、不加锁、不分配内存。
    accentGainDb     = apvts.getRawParameterValue (SubMetronomeParamIDs::accentGain);
    beatGainDb       = apvts.getRawParameterValue (SubMetronomeParamIDs::beatGain);
    subGainDb        = apvts.getRawParameterValue (SubMetronomeParamIDs::subGain);
    accentPitchHz    = apvts.getRawParameterValue (SubMetronomeParamIDs::accentPitch);
    beatPitchHz      = apvts.getRawParameterValue (SubMetronomeParamIDs::beatPitch);
    subPitchHz       = apvts.getRawParameterValue (SubMetronomeParamIDs::subPitch);
    subdivisionParam = apvts.getRawParameterValue (SubMetronomeParamIDs::subdivision);
    waveformParam    = apvts.getRawParameterValue (SubMetronomeParamIDs::waveform);
}

//==============================================================================
void SubMetronomeAudioProcessor::prepareToPlay (double newSampleRate, int samplesPerBlock)
{
    sampleRate = juce::jmax (newSampleRate, 1.0);
    blockSize  = juce::jmax (samplesPerBlock, 1);
    resetRealtimeState();
}

void SubMetronomeAudioProcessor::releaseResources()
{
    resetRealtimeState();
}

void SubMetronomeAudioProcessor::resetRealtimeState() noexcept
{
    numEvents = 0;
    nextVoice = 0;

    for (auto& e : events)
        e = ClickEvent {};
    for (auto& v : voices)
        v = ClickVoice {};

    previous        = TransportSnapshot {};
    gridNeedsResync = true;
}

//==============================================================================
void SubMetronomeAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer,
                                              juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // 本插件是纯生成器：不处理输入，先把输出缓冲整体清零。
    buffer.clear();

    const int numSamples = buffer.getNumSamples();
    if (numSamples <= 0 || buffer.getNumChannels() <= 0)
        return;

    // ★ 每块只向宿主询问一次绝对时间：PPQ / BPM / 播放状态 / 拍号 / 小节起点。
    //   PPQ 本身是宿主 tempo map 的积分结果（1 PPQ = 1 个四分音符，与 BPM 无关），
    //   所以 BPM 渐变、变速、时间轴跳转都由宿主负责，插件无需相位累加器。
    const auto t = readTransport (getPlayHead(), sampleRate);

    if (! t.valid || ! t.isPlaying)
    {
        // DAW 暂停 / 停止 / Standalone 无 playhead：立即静音并清空未播完的 click
        resetRealtimeState();
        previous = t;   // 保留快照，避免恢复播放时被误判成「跳转」
        return;
    }

    // ---- 传输状态机：识别「停止→播放」「循环回绕」「时间轴定位」 ----
    const double expectedAdvance = (double) numSamples * previous.ppqPerSample;
    const double ppqDelta        = t.ppq - previous.ppq;
    const bool   needsResync     = gridNeedsResync
                               || ! previous.valid
                               || ! previous.isPlaying
                               || ppqDelta < -1e-6                                     // 回退 / 循环
                               || ppqDelta > juce::jmax (expectedAdvance * 8.0, 0.5);  // 跳转

    if (needsResync)
    {
        // 只清空发声池，绝不补发历史网格线（否则一次性喷出成百上千个 click）
        for (auto& v : voices)
            v = ClickVoice {};
        gridNeedsResync = false;
    }

    numEvents = 0;
    collectGridEvents (t, numSamples);   // 求 [p0, p1) 与细分网格的交点
    renderVoices (buffer);               // 按样本偏移启动短促 click

    previous = t;
}

//==============================================================================
void SubMetronomeAudioProcessor::collectGridEvents (const TransportSnapshot& t,
                                                   int numSamples) noexcept
{
    // ★ N =「一拍内几个音」，就是 subdivision 参数的实际整数值（1..maxNotesPerBeat）。
    //   N=2 八分、3 三连音、4 十六分、5 五连音、7 七连音……全走同一套网格算法。
    const int N = gridDivisionsForMode (
        juce::roundToInt (subdivisionParam->load (std::memory_order_relaxed)));

    const double p0 = t.ppq;
    const double p1 = p0 + (double) numSamples * t.ppqPerSample;  // 块内 BPM 线性近似
    constexpr double eps = 1e-9;   // 抵消 p0*N 恰为整数时的浮点误差

    // 网格线用整数索引 k 表示：ppq(k) = k / N。
    // 半开区间 [p0, p1) 保证相邻两块不会重复触发同一条网格线。
    const auto kFirst = (long long) std::ceil (p0 * (double) N - eps);
    const auto kLast  = (long long) std::ceil (p1 * (double) N - eps) - 1;

    // 小节首拍用整数取模判定，避免浮点比较
    const auto     kBar   = (long long) std::llround (t.barStartPpq * (double) N);
    const long long perBar = (long long) N * (long long) juce::jmax (t.numerator, 1);

    const int wave = juce::jlimit (0, 2,
        juce::roundToInt (waveformParam->load (std::memory_order_relaxed)));

    // 高 N（例如 32 连音）+ 高 BPM 时，相邻网格间隔可能短于默认 12 ms。
    // 按间隔的 80% 自适应缩短 click，避免相邻击打糊成一团。
    const double gridIntervalSamples = (1.0 / (double) N) / t.ppqPerSample;
    const int defaultClickSamples = juce::jmax (1,
        juce::roundToInt (sampleRate * (double) clickLengthMs / 1000.0));
    const int clickSamples = juce::jlimit (1, defaultClickSamples,
        juce::roundToInt (gridIntervalSamples * 0.8));

    for (auto k = kFirst; k <= kLast; ++k)
    {
        if (numEvents >= maxEventsPerBlock)
            break;

        // ★ 一拍被等分为 N 份：节点是 0/N, 1/N, ... (N-1)/N。
        //   永远用一次除法从整数 k 求出，绝不用 1/N 的小数近似累加（会累积成拖拍）。
        const double eventPpq = (double) k / (double) N;
        const int    offset   = juce::roundToInt ((eventPpq - p0) / t.ppqPerSample);

        // 恰好落在块尾的网格线交给下一块（下一块的 p0 正好命中它）
        if (offset < 0 || offset >= numSamples)
            continue;

        const int  subIndex  = (int) ((k % N + N) % N);   // 兼容负 PPQ
        const bool onQuarter = (subIndex == 0);           // k % N == 0 即整拍
        const bool isAccent  = onQuarter && ((k - kBar) % perBar == 0);

        // 首拍 / 正拍 / 细分拍：各自独立的音量与音高
        const float db = isAccent  ? accentGainDb->load (std::memory_order_relaxed)
                       : onQuarter ? beatGainDb->load (std::memory_order_relaxed)
                                   : subGainDb->load (std::memory_order_relaxed);
        const float hz = isAccent  ? accentPitchHz->load (std::memory_order_relaxed)
                       : onQuarter ? beatPitchHz->load (std::memory_order_relaxed)
                                   : subPitchHz->load (std::memory_order_relaxed);

        const float gain = juce::Decibels::decibelsToGain (db, -60.0f);
        if (gain <= 0.0f)      // -60 dB 视为静音，直接跳过
            continue;

        // events 天然按 offset 升序，renderVoices 顺序消费即可
        events[numEvents++] = { offset, gain, hz, wave, clickSamples };
    }
}

//==============================================================================
void SubMetronomeAudioProcessor::startVoice (const ClickEvent& e) noexcept
{
    auto& v = voices[(size_t) nextVoice];
    nextVoice = (nextVoice + 1) % maxVoices;   // 固定环形池，零分配

    v.active       = true;
    v.totalSamples = juce::jmax (1, e.lengthSamples);   // 自适应长度，随 N / BPM 变化
    v.samplesLeft  = v.totalSamples;
    v.phase        = 0.0;                     // 三种波形在 phase=0 时输出都为 0，天然无爆音
    v.phaseInc     = juce::MathConstants<double>::twoPi * (double) e.frequency / sampleRate;
    v.amplitude    = e.amplitude;
    v.waveform     = e.waveform;
}

void SubMetronomeAudioProcessor::renderVoices (juce::AudioBuffer<float>& buffer) noexcept
{
    const int numSamples = buffer.getNumSamples();
    auto* left  = buffer.getWritePointer (0);
    auto* right = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : nullptr;

    int eventIndex = 0;

    for (int s = 0; s < numSamples; ++s)
    {
        // 启动本样本到期的所有 click
        while (eventIndex < numEvents && events[eventIndex].sampleOffset <= s)
        {
            startVoice (events[eventIndex]);
            ++eventIndex;
        }

        float mixed = 0.0f;

        for (auto& v : voices)
        {
            if (! v.active)
                continue;

            // 二次衰减包络：不用 pow/exp，CPU 极低；首样本 1.0、末样本趋近 0
            const float envT = (float) v.samplesLeft / (float) v.totalSamples;
            const float env  = envT * envT;

            double wave = 0.0;
            switch (v.waveform)
            {
                case 0:  wave = std::sin (v.phase);                                        break; // Sine
                case 1:  wave = (2.0 / juce::MathConstants<double>::pi)
                              * std::asin (std::sin (v.phase));                           break; // Triangle
                case 2:  wave = std::max (0.0, std::sin (v.phase));                       break; // Click 半波脉冲
                default: wave = std::sin (v.phase);                                        break;
            }

            mixed += (float) wave * env * v.amplitude * 0.8f;   // 0.8 = 叠加 headroom

            v.phase += v.phaseInc;
            if (--v.samplesLeft <= 0)
                v.active = false;
        }

        const float out = juce::jlimit (-1.0f, 1.0f, mixed);
        left[s] += out;
        if (right != nullptr)
            right[s] += out;
    }

    numEvents = 0;   // 本块事件已消费完
}

//==============================================================================
// 状态持久化：DAW 工程保存 / 预设（参数自动化由宿主单独录制）
void SubMetronomeAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::MemoryOutputStream stream (destData, true);
    apvts.state.writeToStream (stream);
}

void SubMetronomeAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (data == nullptr || sizeInBytes <= 0)
        return;

    juce::MemoryInputStream stream (data, (size_t) sizeInBytes, false);
    const auto tree = juce::ValueTree::readFromStream (stream);
    if (tree.isValid())
        apvts.replaceState (tree);
}

//==============================================================================
juce::AudioProcessorEditor* SubMetronomeAudioProcessor::createEditor()
{
    return new SubMetronomeAudioProcessorEditor (*this);
}

//==============================================================================
// JUCE 插件入口：VST3 / Standalone wrapper 都通过它创建 Processor
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new SubMetronomeAudioProcessor();
}
