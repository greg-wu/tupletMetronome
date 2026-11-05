# tupletMetronome

**English summary:** A minimal VST3 metronome locked to your DAW's transport. Set *Notes Per Beat* to any integer from 1 to 32 to divide each quarter note into equal clicks — 3 for triplets, 5 for quintuplets, 7 for septuplets, and so on. Accent, beat and subdivision hits each have independent volume (dB) and pitch (Hz). Built with JUCE 9 / C++17; the audio thread performs zero allocations.

一个极简、低 CPU 占用、**与 DAW 时钟严格同步**的任意连音细分节拍器 VST3 插件，基于 JUCE 9 / C++17。

不内置自由运行的时钟：插件每个音频块向宿主询问一次绝对 PPQ，再求「本块覆盖的 PPQ 区间」与「细分网格」的交点。因此 BPM 渐变、变速、循环、时间轴跳转、拍号变化都自动跟随宿主，无需手动对齐。

## 特性

- **宿主时钟同步**：通过 `AudioPlayHead` 读取 BPM、PPQ、播放状态、拍号与小节起点；DAW 播放即打拍，暂停/停止立即静音。
- **任意连音**：`NOTES / BEAT` 参数直接定义「一拍内几个音」，范围 1–32。
  - `1` 四分音符 · `2` 八分音符 · `3` 八分三连音 · `4` 十六分 · `5` 五连音 · `7` 七连音 · … 任意 `N` 连音
  - 所有 `N` 共用同一套网格算法，没有针对三连音的特殊分支。
- **三组独立控制**：首拍（小节第一拍）/ 正拍 / 细分拍各自独立音量（dB）与音高（Hz）。
- **三种极简内建波形**：Sine、Triangle、Click（半波脉冲），无采样、无外部资源。
- **全参数 APVTS**：DAW 自动化、工程保存、预设开箱可用。
- **音频线程零分配、零锁**：参数用 `std::atomic<float>` 缓存，事件与发声池用固定长度 `std::array`；短促包络，CPU 占用可忽略。
- **自适应击打长度**：高 `N` 或高 BPM 时自动缩短 click，避免相邻击打糊成一团。

## 核心算法

以 PPQ（1 PPQ = 1 个四分音符）为唯一时间基准。设 `N` 为每拍细分数，任意一条网格线的位置用整数索引 `k` 表示：

```
ppq(k) = k / N
```

设本块起点 PPQ 为 `p0`，`q = bpm / (60 * sampleRate)`（每个样本对应多少四分音符），本块覆盖 `[p0, p1)`，其中 `p1 = p0 + numSamples * q`。落在块内的网格索引为：

```
kFirst = ceil(p0 * N - eps)
kLast  = ceil(p1 * N - eps) - 1
offset = round((k / N - p0) / q)
```

- 半开区间 + `eps`（`1e-9`）保证相邻两块不会重复触发或漏触发同一条网格线。
- 节点永远由**一次整数除法** `k / N` 得到，绝不用 `1/N` 的小数近似累加，因此不存在累积漂移。
- 事件分类用整数取模：`k % N == 0` 为正拍，其余为细分拍；`(k - kBar) % (N * 拍号分子) == 0` 为小节首拍。
- 停止、循环回绕、定位跳转会触发重同步，只清空发声池，不补发历史网格线。

## 参数

| 参数 | 范围 | 默认 | 说明 |
| --- | --- | --- | --- |
| `Accent Gain` | -60 … +18 dB | 0 dB | 小节首拍音量 |
| `Beat Gain` | -60 … +18 dB | -6 dB | 其余正拍音量 |
| `Sub Gain` | -60 … +18 dB | -12 dB | 细分拍音量 |
| `Accent Pitch` | 200 … 2400 Hz | 1200 Hz | 首拍音高 |
| `Beat Pitch` | 200 … 2400 Hz | 800 Hz | 正拍音高 |
| `Sub Pitch` | 200 … 2400 Hz | 600 Hz | 细分拍音高 |
| `Notes Per Beat` | 1 … 32（整数） | 3 | 一拍等分为几个音 |
| `Waveform` | Sine / Triangle / Click | Sine | 内建波形 |

`-60 dB` 视为静音，直接跳过击打。上限 `maxNotesPerBeat` 定义在 `source/PluginProcessor.h`，改一处即可整体调整（参数范围、夹紧、界面文案会同步变化）。

音量上限为 `+18 dB`：VST3 使用浮点总线，插件输出允许短时超过 0 dBFS —— 否则推高音量只会在插件内部被削平，听感上没有变化。最终电平请用宿主推子 / 总线管理。插件内部仅保留 `±8` 的安全上限防止数值异常；默认 `0 / -6 / -12 dB` 的听感保持不变。

## 构建

### 要求

| 组件 | 版本（本项目验证环境） |
| --- | --- |
| JUCE | 9.0.5（Git submodule，已内置 VST3 SDK，无需单独下载） |
| CMake | ≥ 3.22（验证于 4.4.4） |
| C++ | C++17 |
| 编译器 | MSVC 19.51 / VS Build Tools 2026，或等价的 Clang / GCC |
| Windows SDK | 10.0.26100.0 |

JUCE 以 submodule 形式引入，**不把框架源码复制进本仓库**。

```powershell
git clone --recurse-submodules https://github.com/greg-wu/tupletMetronome.git
cd tupletMetronome

cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release --target tupletMetronome_VST3 -- /m
```

若已 clone 但没带子模块：

```powershell
git submodule update --init --depth 1
```

Linux / macOS 换用对应生成器即可（例如 `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release`），CMakeLists 里只有 `if(MSVC) /utf-8` 是平台相关项。

### 产物

```
build/tupletMetronome_artefacts/Release/VST3/tupletMetronome.vst3/   ← 整个文件夹是插件 bundle
```

把它加入 DAW 的 VST3 扫描路径，或复制到系统插件目录（Windows 需管理员权限）：

```powershell
Copy-Item -Recurse -Force `
  'build\tupletMetronome_artefacts\Release\VST3\tupletMetronome.vst3' `
  'C:\Program Files\Common Files\VST3\'
```

## 使用

1. 在 DAW 中加载 `tupletMetronome`（VST3 / Tools 类别）到一个音轨。
2. 让 DAW 处于**播放**状态；停止/暂停时插件静音，这是设计要求。
3. 调整 `NOTES / BEAT`：`3` 为八分三连音，`5` / `7` 为五、七连音，`32` 为极限密度。
4. 用三组音量/音高区分首拍、正拍与细分拍；所有参数都可被 DAW 自动化。

## 已知限制

- `N` 的单位是「**每个四分音符（1 PPQ）`N` 个音**」。在 4/4、3/4 下等同于「一拍几个音」；若要在 6/8、12/8 下按记谱的一拍定义，需要读 `TimeSignature.denominator` 做 `N * 4 / denominator` 换算，当前未实现。
- Standalone 构建目标没有宿主 `AudioPlayHead`，按设计会静音；要听声音请在 DAW 中加载 VST3。
- 默认输出为立体声，`isBusesLayoutSupported()` 同时接受单声道。
- 小节首拍依赖宿主提供 `getPpqPositionOfLastBarStart()`；不提供时退化为「以 PPQ 0 为起点、4/4」。

## 目录结构

```
.
├── CMakeLists.txt              # VST3 + Standalone 目标、/utf-8
├── .gitmodules                 # JUCE 9.0.5 submodule
├── external/JUCE/              # Git submodule（不在本仓库内复制源码）
└── source/
    ├── PluginProcessor.h       # APVTS 参数、PlayHead 快照、固定大小事件/发声池
    ├── PluginProcessor.cpp     # 网格求交、传输状态机、click 合成、状态持久化
    ├── PluginEditor.h
    └── PluginEditor.cpp        # Slider / ComboBox 与 APVTS 绑定
```

## 许可证

本仓库内的插件源码按 [MIT](LICENSE) 授权。

**注意**：插件依赖 [JUCE 9](https://juce.com/) 框架，JUCE 受其自身的 [JUCE 9 EULA](https://juce.com/legal/juce-9-licence/) 或 AGPLv3 约束。本仓库的 MIT 许可仅覆盖本仓库内的代码，不覆盖 JUCE；二进制分发、商业使用等场景请自行确认符合 JUCE 的许可要求。VST3 相关商标与 SDK 亦受 Steinberg 相应条款约束。
