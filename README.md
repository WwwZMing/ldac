# LDAC A2DP for Windows

以内核驱动方式让 Windows 把系统音频以 **LDAC（330/660/990 kbps）** 或 **AAC（128 kbps CBR）**
推送给蓝牙音箱/耳机（A2DP Source）。

[![License](https://img.shields.io/badge/license-Apache--2.0-blue?style=flat-square)](LICENSE)
![Platform](https://img.shields.io/badge/platform-Windows%2010%20%2F%2011-0078D6?style=flat-square)
![Codec](https://img.shields.io/badge/codec-LDAC%20990%20%7C%20AAC-4c1?style=flat-square)

Windows 原生 A2DP 仅支持 SBC AAC等编码（缺少 LDAC 编码器），用户态亦无 L2CAP 接口。
因此以内核驱动承担 BTHENUM A2DP 子设备、L2CAP/AVDTP 与 PortCls 渲染端点，
采集与编码位于用户态。

> 仅在 UGREEN BT505上验证。其他 sink 需调整 SEP 选择与能力位，见 [docs/architecture.md](docs/architecture.md)。

## 特性

- LDAC 330 / 660 / 990 kbps，48 kHz 立体声。
- AAC 128 kbps CBR，48 kHz 立体声（Media Foundation 编码 + 自实现 LATM 封帧）。
- 系统音频（WASAPI loopback）或 WAV 文件作为输入。
- 播放器无需改动：设备中多出渲染端点「LDAC A2DP Audio」。
- 链路在位即安装端点，链路断开即卸载；重连与退避在用户态服务内完成。
- 编解码器切换在面板内完成，选择写入驱动注册表，重启后保持。
- 诊断仪表 `epmeter`：端点音量、硬件音量位、0 dBFS 单音与 loopback 回采。

## 架构

```
播放器 → Windows 混音器 → 【PortCls 端点】──loopback──▶ ldac-send.exe
                                                        │ 编码 + RTP
                                                        ▼
                                     LdacA2dp.sys（AVDTP / L2CAP）
                                                        │
                                     bthport → 蓝牙 → Sink（解码 + DAC）
```

| 层 | 内容 | 位置 |
|---|---|---|
| 内核（WDM + PortCls）| BTHENUM `AudioSink(0x110B)` FDO、BRB/L2CAP、AVDTP 信令、WaveRT 端点 | `kernel/LdacA2dp/` |
| 服务 | 重连与退避、编解码器选择、发送端看护、本机 API | `panel/LdacPanel/` |
| 发送端（C/C++）| WASAPI 采集、libldac / AAC 编码、RTP 封帧、发包节拍 | `host/` |

详见 [docs/architecture.md](docs/architecture.md)。

## 前置条件

| | 要求 |
|---|---|
| 系统 | Windows 11 x64，具备 BR/EDR 的蓝牙适配器（我只在Windows11测试过） |
| 设备 | 支持 LDAC 或 AAC 的 A2DP Sink（当前验证：BT505）|
| 构建 | Visual Studio 2026（MSVC 14.51）+ WDK 10.0.28000 |
| 运行 | `bcdedit /set testsigning on` 与自签名证书（驱动以测试签名加载）|

## 构建

```powershell
.\build.ps1 -Configuration Debug    # 驱动 + 发送端 + 编码自检
.\scripts\99-build-epmeter.ps1      # 仅编译诊断仪表
```

产物：`bin-a2dp\`（inf/sys/cat）、`bin-host\`（`ldac-send.exe`、`epmeter.exe`）、`bin-tests\`。

## 安装与使用

```powershell
.\scripts\48-install-ldac.ps1                       # 安装驱动（顶替 BthA2dp，绑定 A2DP 子设备）
.\scripts\50-install-service.ps1 -Exe <publish\LdacPanel.exe>   # 注册后台服务
sc start LdacA2dpSvc                                # 服务负责建链与拉起发送端
```

播放器中选择「LDAC A2DP Audio」输出。卸载：`scripts\47-uninstall-ldac.ps1`。
驱动日志：`C:\Users\Public\ldac-probe.log`。

## 配置

发送端：

```powershell
ldac-send.exe --loopback --match LDAC              # 系统音频
ldac-send.exe song.wav --codec aac                 # WAV 文件，AAC
ldac-send.exe --loopback --codec aac --rate 44100  # 指定协商采样率
ldac-send.exe --loopback --eqmid sq                # 660 kbps
ldac-send.exe --unload                             # 开发用：删除控制设备以卸载映像
```

驱动（`HKLM\SYSTEM\CurrentControlSet\Services\LdacA2dp\Parameters`）：

| 值 | 默认 | 作用 |
|---|---|---|
| `Codec` | 0 | 0 = LDAC，1 = SBC，2 = AAC |
| `TargetBdAddr` | — | 目标设备地址 |
| `ChannelFlags` | 0 | L2CAP open 的 `ChannelFlags` |
| `RejectInbound` | 0 | 非 0 时拒绝设备拨入的 PSM 25 |
| `VerboseLog` | 0 | 非 0 时逐 BRB 记录（写盘量大）|

## 已知限制

- 不做重采样：协商采样率须等于系统混音率。
- 端点无硬件音量节点（`hw=0`）：Windows 音量条为软件衰减，不同步音箱绝对音量；音箱音量需用其按键（或系统 A2DP 端点）设为 100%，详见 [docs/architecture.md](docs/architecture.md) 第 11 节。
- 88.2 / 96 kHz 未实播验证；长时间稳定性（>10 min）未测。
- 仅验证 BT505；其他设备需调整 AVDTP 能力位与 settle 参数。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/architecture.md](docs/architecture.md) | 架构：分层、数据通路、组件、会话建立、编解码器、端点生命周期、故障归因 |
| [docs/development.md](docs/development.md) | 开发流程：构建、部署、测试、抓包、排查顺序、已知缺陷 |

## 贡献

两类 issue 尤有价值：其他 A2DP Sink 的测试结果（设备型号 + `ldac-probe.log` + `epmeter --list`），
以及违反 [AGENTS.md](AGENTS.md) 中约束的实现缺陷。

## 许可与致谢

[Apache License 2.0](LICENSE)。第三方归属见 [NOTICE](NOTICE)。

- [libldac](https://android.googlesource.com/platform/external/libldac) — Sony LDAC 编码器（Apache-2.0）
- AOSP bluedroid SBC 编码器 — Broadcom / Google（Apache-2.0）
- [A2DP-Windows-Bridge](https://github.com/SeiyaFunaokaJP/A2DP-Windows-Bridge) — AVDTP/RTP 打包参考实现

---

[![](https://img.shields.io/badge/powered_by_dsh-4D6BFE?style=flat-square&logo=deepseek&logoColor=white)](https://github.com/deepseek-ai/deepseek-harness)
