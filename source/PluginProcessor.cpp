//==============================================================================
//  tupletMetronome / 核心音频逻辑
//  processBlock：DAW 时钟同步 + 任意 N 连音细分网格 + 极简内建 click 合成
//==============================================================================

#include "PluginProcessor.h"
#include "PluginEditor.h"

#include <algorithm>
#include <cstdint>

namespace
{
    // 固定种子的 xorshift32 噪声：音频线程零分配，不依赖任何随机库
    inline float nextNoise (uint32_t& state) noexcept
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (state >> 8) * (2.0f / 16777216.0f) - 1.0f;   // [-1, 1)
    }

    // 波形参数 -> 实际合成类型。
    // "Drum Kit"（索引 3）不是单一波形：按拍位映射 kick / snare / hat，
    // 所以即使 beat 与 sub 都选 Drum Kit，也能得到一整套鼓组而不是同一种音色。
    inline int resolveWaveform (int paramChoice, bool isAccent, bool onQuarter) noexcept
    {
        if (paramChoice != 3)
            return paramChoice;                       // Sine / Triangle / Click 原样使用

        if (isAccent)
            return 3;                                 // 小节首拍 -> Kick
        return onQuarter ? 4 : 5;                     // 正拍 -> Snare，细分 -> Hat
    }
}

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
    subdivisionParam  = apvts.getRawParameterValue (SubMetronomeParamIDs::subdivision);
    beatWaveformParam = apvts.getRawParameterValue (SubMetronomeParamIDs::waveform);
    subWaveformParam  = apvts.getRawParameterValue (SubMetronomeParamIDs::subWaveform);
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

    // 波形按「正拍 / 细分拍」分开读取：首拍 accent 属于正拍，与 beat 共用选项。
    // 索引 0..2 = 合成波形，3 = Drum Kit（在事件生成时再映射到具体鼓件）
    const int beatWaveChoice = juce::jlimit (0, 3,
        juce::roundToInt (beatWaveformParam->load (std::memory_order_relaxed)));
    const int subWaveChoice  = juce::jlimit (0, 3,
        juce::roundToInt (subWaveformParam->load (std::memory_order_relaxed)));

    // 高 N（例如 32 连音）+ 高 BPM 时，相邻网格间隔可能短于默认 12 ms。
    // 按间隔的 80% 自适应缩短 click，避免相邻击打糊成一团。
    const double gridIntervalSamples = (1.0 / (double) N) / t.ppqPerSample;
    const int defaultClickSamples = juce::jmax (1,
        juce::roundToInt (sampleRate * (double) clickLengthMs / 1000.0));
    const int clickSamples = juce::jlimit (1, defaultClickSamples,
        juce::roundToInt (gridIntervalSamples * 0.8));

    // 鼓组音长：kick 需要下沉尾巴，hat 极短。仍按网格间隔的 95% 夹紧，
    // 避免密集细分时尾音叠成一团噪声。
    const int maxDrumSamples[3] = {
        juce::jmax (1, juce::roundToInt (sampleRate * (double) kickLengthMs  / 1000.0)),
        juce::jmax (1, juce::roundToInt (sampleRate * (double) snareLengthMs / 1000.0)),
        juce::jmax (1, juce::roundToInt (sampleRate * (double) hatLengthMs   / 1000.0))
    };
    const int drumLimitSamples = juce::jmax (1, juce::roundToInt (gridIntervalSamples * 0.95));

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

        // 正拍（含首拍）用 beat 选项，细分拍用 sub 选项；Drum Kit 再映射到 kick/snare/hat
        const int wave = resolveWaveform (onQuarter ? beatWaveChoice : subWaveChoice,
                                          isAccent, onQuarter);

        // 鼓件用固定音长（再按网格间隔夹紧），合成 click 用自适应短音长
        const int lengthSamples = wave >= 3
            ? juce::jlimit (1, maxDrumSamples[wave - 3], drumLimitSamples)
            : clickSamples;

        // events 天然按 offset 升序，renderVoices 顺序消费即可
        events[numEvents++] = { offset, gain, hz, wave, lengthSamples };
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
    v.phase        = 0.0;                     // 所有波形在 phase=0 时输出都为 0，天然无爆音
    v.phaseInc     = juce::MathConstants<double>::twoPi * (double) e.frequency / sampleRate;
    v.amplitude    = e.amplitude;
    v.waveform     = e.waveform;

    // 鼓组状态必须每次重置：voice 是环形复用的
    v.noiseLp    = 0.0f;
    v.pitchEnv   = 0.0f;
    v.pitchDecay = 0.0f;
    v.startHz    = e.frequency;
    v.rngState   = ((uint32_t) nextVoice + 1u) * 2654435761u;   // 每个 voice 不同噪声序列

    if (e.waveform == 3)   // Kick：以参数音高为参考，落到 60..260 Hz 的鼓体区间
    {
        v.startHz    = juce::jlimit (60.0f, 260.0f, e.frequency * 0.18f);
        v.pitchEnv   = 1.0f;
        v.pitchDecay = 1.0f / (float) v.totalSamples;
    }
    else if (e.waveform == 4)   // Snare：音调体固定在 180..400 Hz，噪声层负责"沙"感
    {
        v.phaseInc = juce::MathConstants<double>::twoPi
                   * (double) juce::jlimit (180.0f, 400.0f, e.frequency * 0.4f) / sampleRate;
    }
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
        const double twoPiOverSr = juce::MathConstants<double>::twoPi / sampleRate;

        for (auto& v : voices)
        {
            if (! v.active)
                continue;

            // 二次衰减包络：不用 pow/exp，CPU 极低；首样本 1.0、末样本趋近 0
            const float envT = (float) v.samplesLeft / (float) v.totalSamples;
            const float env  = envT * envT;

            float sample = 0.0f;

            if (v.waveform <= 2)
            {
                double wave = 0.0;
                switch (v.waveform)
                {
                    case 0:  wave = std::sin (v.phase);                                        break; // Sine
                    case 1:  wave = (2.0 / juce::MathConstants<double>::pi)
                                  * std::asin (std::sin (v.phase));                           break; // Triangle
                    default: wave = std::max (0.0, std::sin (v.phase));                       break; // Click 半波脉冲
                }

                sample = (float) wave * env;
            }
            else if (v.waveform == 3)   // Kick：正弦 + 快速音高下扫（鼓体冲击感）
            {
                const float hz = 45.0f + (v.startHz - 45.0f) * v.pitchEnv * v.pitchEnv;
                v.phaseInc = twoPiOverSr * (double) hz;
                sample = (float) std::sin (v.phase) * env;
                v.pitchEnv = juce::jmax (0.0f, v.pitchEnv - v.pitchDecay);
            }
            else if (v.waveform == 4)   // Snare：低通噪声 + 短音调体
            {
                v.noiseLp += 0.40f * (nextNoise (v.rngState) - v.noiseLp);
                const float body = (float) std::sin (v.phase);
                sample = (0.72f * v.noiseLp + 0.28f * body) * env;
            }
            else                        // Hat：高通噪声（噪声 - 低通残差）
            {
                const float n = nextNoise (v.rngState);
                v.noiseLp += 0.55f * (n - v.noiseLp);
                sample = (n - v.noiseLp) * 1.8f * env;
            }

            mixed += sample * v.amplitude * 0.8f;   // 0.8 = 叠加 headroom

            v.phase += v.phaseInc;
            if (--v.samplesLeft <= 0)
                v.active = false;
        }

        // VST3 为浮点总线，允许输出短时超过 0 dBFS —— 音量上限提到 +18 dB 后，
        // 只有放开这个上限，推高音量才不会在插件内部被削平。最终电平由宿主推子 /
        // 总线管理。±8 仅作防数值异常的安全网，常规参数下不会触发。
        const float out = juce::jlimit (-8.0f, 8.0f, mixed);
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
