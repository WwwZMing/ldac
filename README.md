# LDAC A2DP for Windows

让 Windows 用**自研内核驱动**把系统音频以 **LDAC 990 kbps** 推给蓝牙音箱 / 耳机（A2DP Source 角色）。

[![License](https://img.shields.io/badge/license-Apache--2.0-blue?style=flat-square)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078D6?style=flat-square)
![Codec](https://img.shields.io/badge/codec-LDAC%20990%20kbps-4c1?style=flat-square)

Windows 原生 A2DP 只支持 SBC / AAC / aptX，没有 LDAC；用户态也没有 L2CAP API。
所以这个项目在**内核**里放一个最小的 profile driver（BRB 直连 L2CAP + AVDTP 信令），
把**采集与编码**放在用户态。已在 UGREEN BT505 接收器上跑通：LDAC 48 kHz 立体声、设备开关机自动回连。

> 仅在一台设备（UGREEN BT505，`33:DB:8A:C7:E6:7A`）上验证过。别的 sink 可能需要调 SEP/能力位，见 [docs/](docs/)。

## 特性

- **LDAC 330 / 660 / 990 kbps**（默认 HQ 990），48 kHz 立体声，每包 2 帧
- **系统音频或 WAV**：WASAPI loopback 采集，或直接喂 WAV 做对照实验
- **任意播放器可用**：装好后多出一个普通渲染端点「扬声器 (LDAC A2DP Audio)」
- **会话在驱动里保活**：用户态发送端退出不拆链；设备开关机自动回连（约 3 s），PC 主动回连 < 1 s
- **SBC 通路**（`--codec sbc`）：出问题时用来区分「编码侧」还是「链路/设备侧」
- **诊断仪表** `epmeter`：读端点音量与硬件音量位，内置 0 dBFS 单音 + loopback 回采 dBFS

## 架构

```
播放器 ─▶ Windows 混音器 ─▶ 【我们的 PortCls 端点】──loopback──▶ ldac-send.exe
                                                                │ 编码 + RTP
                                                                ▼
                                     LdacA2dp.sys（AVDTP / L2CAP / keeper）
                                                                │
                                       bthport → 蓝牙 → Sink（LDAC 解码 + DAC）
```

| 层 | 内容 | 位置 |
|---|---|---|
| 内核（WDM + PortCls，C） | BTHENUM `AudioSink(0x110B)` 的 FDO、BRB/L2CAP、AVDTP 状态机、keeper 线程、WaveRT 端点 | `kernel/LdacA2dp/` |
| 用户态（C/C++） | WASAPI 采集、libldac / SBC 编码、RTP 打包、发包节拍、诊断仪表 | `host/` |

细节见 [docs/architecture.md](docs/architecture.md)。

## 前置条件

| | 要求 |
|---|---|
| 系统 | Windows 10/11 x64，带 BR/EDR 的蓝牙适配器 |
| 设备 | 支持 LDAC 的 A2DP Sink（当前只在 BT505 上验证） |
| 构建 | Visual Studio 2026 的 C++ 工具（MSVC 14.51）+ WDK 10.0.28000 |
| 运行 | `bcdedit /set testsigning on` + 自签名证书（驱动用测试签名加载） |

## 构建

```powershell
.\build.ps1 -Configuration Debug      # 驱动 + 发送端 + 编码自检（约 6 秒，直接 cl/link）
.\scripts\99-build-epmeter.ps1   # 只编诊断仪表
```

产物：`bin-a2dp\`（驱动的 inf/sys/cat）、`bin-host\`（`ldac-send.exe`、`epmeter.exe`）、`bin-tests\`。

## 安装与使用

```powershell
# 1) 安装驱动（管理员）。它会顶替 BthA2dp，绑到蓝牙耳机的 A2DP 子设备上
.\scripts\48-install-ldac.ps1

# 2) 起发送端（SYSTEM 计划任务，无窗口；SSH 断开也不受影响）
.\scripts\play-music.ps1 -Vol 70

# 3) 播放器里选「扬声器 (LDAC A2DP Audio)」放歌

# 停
.\scripts\play-music.ps1 -Stop
```

卸载：`scripts\47-uninstall-ldac.ps1`。驱动日志：`C:\Users\Public\ldac-probe.log`。

## 配置

**发送端**：

```powershell
ldac-send.exe --loopback --match LDAC              # 系统音频；--match 按端点名选设备
ldac-send.exe song.wav                             # WAV 模式
ldac-send.exe --loopback --codec sbc               # SBC 对照
ldac-send.exe --loopback --eqmid sq                # 660 kbps
ldac-send.exe --unload                             # 开发用：卸控制设备以卸映像
```

**驱动**（`HKLM\SYSTEM\CurrentControlSet\Services\LdacA2dp\Parameters`）：

| 值 | 默认 | 作用 |
|---|---|---|
| `TargetBdAddr` | — | 目标设备地址，如 `33:DB:8A:C7:E6:7A` |
| `ChannelFlags` | 0 | L2CAP open 的 `ChannelFlags` |
| `RejectInbound` | 0 | 非 0 = 拒绝设备拨入的 PSM 25 |
| `ForceSbc` | 0 | 非 0 = 强制 SBC |
| `AvrcpVolume` | 0 | 非 0 = 启用 AVRCP 音量控制器（实验性） |

## 已知限制

- **AVRCP 绝对音量**未打通：设备音量请用它的按键调到 100%，音量用 Windows 音量条（本端点 `hw=0`，是软件衰减）。
- 设备关机后 Windows 可能仍显示「配对中」（BTHENUM 子节点残留）。
- 88.2 / 96 kHz 未实播验证；长稳（>10 min）未测。
- 只在 BT505 上验证；换设备大概率要调 AVDTP 能力位与 settle 参数。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 架构：模块、数据流、线程与状态机、IOCTL、注册表开关 |
| [docs/runbook.md](docs/runbook.md) | 操作手册：安装、放歌、排障 |
| [docs/toolkit.md](docs/toolkit.md) | 工具箱：epmeter、探针日志、常用诊断配方 |
| [docs/timeline.md](docs/timeline.md) | 实验时间线：11-02 → 11-07 全过程 + 十次「以为 → 其实」 |
| [docs/research.md](docs/research.md) | 设备与平台事实：BT505 脾气、Windows 限制、编码器参数 |
| [experiments/README.md](experiments/README.md) | 实验稿（纯文字）：做过哪些实验、结论 |

## 贡献

欢迎 issue：尤其是**在其他 A2DP Sink 上的测试结果**（设备型号 + `ldac-probe.log` + `epmeter --list` 输出）。
提交代码前请先看 [AGENTS.md](AGENTS.md)（架构总纲与硬约束，很多是踩坑换来的）。

## 许可与致谢

[Apache License 2.0](LICENSE)，第三方归属见 [NOTICE](NOTICE)。

- [libldac](https://android.googlesource.com/platform/external/libldac) — Sony 的 LDAC 编码器（Apache-2.0）
- AOSP bluedroid 的 SBC 编码器 — Broadcom / Google（Apache-2.0）
- [A2DP-Windows-Bridge](https://github.com/SeiyaFunaokaJP/A2DP-Windows-Bridge) — AVDTP/RTP 打包细节的参考实现
- 本项目代码与文档由 **DeepSeek**（DeepSeek Harness 智能体）按作者的目标逐轮生成；需求、取舍与所有上机实验由作者负责

---

[![](https://img.shields.io/badge/powered_by_dsh-4D6BFE?style=flat-square&logo=deepseek&logoColor=white)](https://github.com/deepseek-ai/deepseek-harness)
