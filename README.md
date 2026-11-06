# LDAC A2DP for Windows（绿联 BT505）

> **状态（2026-11-07）：可用。** Windows 把系统音频以 **LDAC 990 kbps 立体声**推到 BT505，
> 音量正常，设备开关机自动回连（约 3 s），PC 主动回连 < 1 s。
> 自用 + 开源学习项目，驱动自签名 + `testsigning` 加载。

## 现在长什么样

| 层 | 实现 | 位置 |
|---|---|---|
| 内核驱动 | `LdacA2dp.sys`（**WDM + PortCls，C**），绑在 BT505 的 BTHENUM `AudioSink(0x110B)` 节点上，顶替 `BthA2dp` | `kernel/LdacA2dp/` |
| 音频端点 | PortCls WaveRT 渲染端点「扬声器 (LDAC A2DP Audio (Abu))」 | `MiniportWaveRT.cpp` / `MiniportTopology.cpp` |
| L2CAP | 驱动内经 BRB 直接开通道（PSM 25 信令+媒体、PSM 23 AVRCP） | `BthL2cap.c` |
| AVDTP | 信令状态机在**驱动内**，keeper 线程自动跑完 Discover→GetCaps→SetConfig→Open→Start | `Avdtp.c`、`Driver.c` |
| AVRCP | 绝对音量控制器（默认关闭） | `Avrcp.c` |
| 编码/发送 | `ldac-send.exe`：WASAPI loopback（或 WAV）→ libldacBT / AOSP SBC → RTP → IOCTL | `host/ldac_send.c` |
| 诊断 | `epmeter.exe`：端点音量、硬件音量位、0 dBFS 单音回采 dBFS | `host/epmeter.cpp` |

```
播放器 ─▶ Windows 混音器 ─▶ 【我们的 PortCls 端点】──loopback──▶ ldac-send.exe
                                                                │ 编码 + RTP
                                                                ▼
                                     LdacA2dp.sys（AVDTP / L2CAP / keeper）
                                                                │
                                       bthport → 蓝牙 → BT505（LDAC 解码 + DAC）
```

## 目录

| 路径 | 内容 |
|---|---|
| `kernel/LdacA2dp/` | 驱动：`Driver.c`、`BthL2cap.c`、`Avdtp.c`、`Avrcp.c`、`Miniport*`、`Log.c` |
| `host/` | 发送端、WASAPI loopback、`epmeter` 仪表 |
| `third_party/` | `libldac`（Sony/AOSP）、`aosp-sbc`、MSVC 兼容层 |
| `common/` | 内核/用户态共享的 IOCTL 定义 |
| `scripts/` | **全部脚本**：构建、安装/卸载、部署、放歌 |
| `tests/` | AVDTP 单元测试 + 编码自检 |
| `certs/` | 自签名开发证书（公钥） |
| `docs/` | [架构](docs/architecture.md) · [操作](docs/runbook.md) · [工具箱](docs/toolkit.md) · [时间线](docs/timeline.md) · [调研](docs/research.md) |
| `experiments/` | 实验稿（纯文字）：做过哪些实验、结论、已删工具的用途备查 |
| `ARCH.md` | 蓝屏归因史、三代内核栈演进（历史） |
| `AGENTS.md` | 项目须知：架构总纲、硬约束、构建与验证（AI 助手从这里读起） |

## 构建

```powershell
.\build.ps1 -Configuration Debug      # 驱动 + 发送端 + 编码自检（直接 cl/link，不碰 MSBuild 任务链）
.\scripts\99-build-epmeter.ps1   # 只编仪表
```

需要 VS 2026 的 C++ 工具（MSVC 14.51.x）+ WDK 10.0.28000。产物 `bin-*\` 已 gitignore。

## 使用（客户机）

```powershell
# 安装/更新驱动（管理员）
.\scripts\48-install-ldac.ps1                             # 真机
.\scripts\vm-install-ldac.ps1 -Target bt -PkgDir <包目录>    # VM

# 放歌（SYSTEM 计划任务，无窗口，SSH 断开也不受影响）
.\scripts\play-music.ps1 -Vol 70
.\scripts\play-music.ps1 -Stop

# 诊断
bin-host\Debug\epmeter.exe --list
Get-Content C:\Users\Public\ldac-probe.log -Tail 20
```

## 许可与免责

| 项 | 说明 |
|---|---|
| `third_party/libldac` | Sony 经 AOSP 发布的 LDAC 编码器，**Apache-2.0**；原样 vendor，保留 `LICENSE` / `NOTICE` |
| `third_party/aosp-sbc` | Broadcom/Google 的 SBC 编码器（AOSP bluedroid），**Apache-2.0**，版权头保留在各 .c 里 |
| 本仓库自身 | 仅个人学习用；**不发布任何二进制**（没有签名的 .sys/.cat 分发） |
| 驱动加载 | 依赖 `bcdedit /set testsigning on` + 自签名证书，仅供本机研究 |
| 商标 | **LDAC 是 Sony 的商标**。此处仅用于说明互操作性，不声称通过 LDAC / Hi-Res Audio Wireless 认证，不使用其标识 |
| 不含 | 对微软或第三方二进制（`BthA2dp.sys`、商业 A2DP 驱动安装包等）的逆向材料，也未分发任何第三方 `.sys` |

换句话说：**代码是干净的**——自家驱动 + 两个 Apache-2.0 编码器 + 文档。

## 许可证

**Apache License 2.0**，见 [LICENSE](LICENSE)（含明确的专利授权条款）。选它是因为
vendor 进来的两个编码器都是 Apache-2.0，同一许可省掉混用问题。

| 组件 | 来源 | 许可 |
|---|---|---|
| `third_party/libldac` | Sony（经 AOSP 发布） | Apache-2.0，原样保留其 `LICENSE`/`NOTICE` |
| `third_party/aosp-sbc` | Broadcom / Google（AOSP bluedroid） | Apache-2.0，版权头在各 `.c` 内 |
| 本仓库其余代码与文档 | 作者 | Apache-2.0 |

第三方归属见 [NOTICE](NOTICE)。

## 谁写的

- **需求、取舍、所有上机实验**（配对、放歌、听感判断、要不要继续折腾）：作者本人。
- **代码与文档由 DeepSeek 编写**：在 DeepSeek Harness 智能体里，由 DeepSeek 模型按作者的口头目标逐轮生成
  —— 内核驱动、发送端、诊断仪表、构建/部署脚本、以及 `docs/` 下全部文档（含这份 README）。
  作者负责定目标、拍板与验收；AI 负责读代码、写补丁、跑实验、记录结论。
- 所以：**AI 生成的代码请自行审阅后再用于生产**。不过这个项目的每个结论都有实测数据撑着，
  过程与「以为 → 其实」的对照都在 [docs/timeline.md](docs/timeline.md) 里。

## 历史

2026-11-07 把 `ldac`（原文档仓库）与 `ldac-driver`（原源码仓库）合并成这一个仓库；
两边完整历史打包在 `C:\Developer\_reference\bundles\`（`git clone <bundle> <dir>` 可还原）。
更早的实验脚本在旧历史里，用途见 [experiments/README.md](experiments/README.md)。

---

[![](https://img.shields.io/badge/powered_by-dsh-4D6BFE?style=flat-square&logo=deepseek&logoColor=white)](https://github.com/deepseek-ai/deepseek-harness)
