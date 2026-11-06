# 设备与平台调研

> 结论性材料，不是过程记录。过程见 [timeline.md](timeline.md)。

## 1. BT505（绿联 55545）

| 项目 | 规格 |
|---|---|
| 角色 | **A2DP Sink**（接收器），不能当发射器；3.5 mm + 2×RCA 模拟输出 |
| 蓝牙 | 6.0，约 10 m，RF 最大约 4 dBm EIRP |
| 编解码 | **LDAC / AAC / SBC**；LDAC 最高 990 kbps、24bit/96kHz（Hi-Res 金标） |
| 协议 | A2DP / AVRCP / HSP / HFP |
| 多点 | 支持双 Source 同时配对 |
| 电池 | 300 mAh，约 15 h，USB-C 边充边用 |
| 地址 | `33:DB:8A:C7:E6:7A`（本机这台） |

### 实测出来的设备脾气

- **TX/RX 双模**：谁发起连接，SEP 角色就翻转。设备拨入 → SEID 52/54/55 全 SRC；我们拨出 → SEID 1/2/3 全 SNK（含 LDAC）。
- LDAC 能力位：速率 `44.1=0x20 / 48=0x10 / 88.2=0x08 / 96=0x04`，模式 `MONO=0x04 / Dual=0x02 / Stereo=0x01`；厂商 `0x012D`、codec `0x00AA`。
- 信令通道 MTU 只接受 **679**；AVDTP 应答爱迟到约 5 s，不催不回，但**不能换新 TL 重试**。
- 传输通道是**第二条 PSM 25**，不是 33。
- 指示灯：红蓝交替=配对中；蓝 10 s 一闪=已连接；青/AAC=蓝 3 s；**LDAC=绿 3 s**。
- AVRCP 音量（TG 侧）：能连，但回包不合法、`SetAbsoluteVolume` 不应答 —— 见 [timeline.md](timeline.md) 音量段。

## 2. Windows 平台限制（决定了架构）

| 事实 | 影响 |
|---|---|
| 原生 A2DP 只有 **SBC / AAC / aptX（部分）**，永远没有 LDAC | 必须自己发 LDAC |
| **没有 L2CAP 公开 API**（只有 RFCOMM/BLE） | 用户态写不了 A2DP → 必须内核 profile driver |
| A2DP 编解码协商硬编码在 `BthA2dp` 一线 | 要替换该 devnode 的 FDO |
| 配对/link key/加密由 bthport 管 | 省掉 SSP 实现 |
| 内核 profile driver 有**文档化 BRB 接口** | 可以合法地对内置蓝牙做 L2CAP |

## 3. 编码器

- **libldac**（AOSP `platform/external/libldac`，Sony 出品，Apache-2.0）：编码器完全开源，无授权费；解码器专有，但解码在 BT505 里，与我们无关。
- 参数：44.1/48/88.2/96 kHz；输入 S16/S24/S32/F32；EQMID HQ=990 / SQ=660 / MQ=330 kbps；每帧 128 采样（88.2/96k 为 256）；单帧最大 1024 B；L2CAP MTU ≥ 679。
- 实际用：HQ + 48 kHz + Stereo，每包 2 帧、673 B 负载。

## 4. 参考实现

| 项目 | 价值 |
|---|---|
| [A2DP-Windows-Bridge](https://github.com/SeiyaFunaokaJP/A2DP-Windows-Bridge) | dongle + WinUSB + BTstack 的用户态全栈；AVDTP/LDAC/RTP 打包细节的权威参考（媒体负载头就是从它的 `packetizer.rs` 找到的）。代码 Apache/MIT，内置 BTstack 个人非商业免费 |
| Alternative A2DP Driver（闭源付费） | 同样用内置蓝牙的内核驱动，黑盒参考 |
| Android AOSP（Bluetooth / libldac） | 协议与编码器的原始实现 |

## 5. 本机环境（复现用）

| 项 | 值 |
|---|---|
| Host | MYPC，Win11 Insider 10.0.29698 |
| VM | VMware Workstation 17.5.1，客户机 tiny11 / 26300.9457，`test@192.168.144.130`，testsigning=Yes |
| 蓝牙 | Intel `8087:0032` 整机直通给客户机 |
| 工具链 | VS 18 Community 的 MSVC 14.51.36231 + WDK 10.0.28000.0 |
| 驱动日志 | `C:\Users\Public\ldac-probe.log` |
