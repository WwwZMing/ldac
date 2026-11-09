# 实验稿（文字版）

> 2026-11-07 清理：本目录原来有 90+ 个一次性脚本（vmrun 部署、dump 分析、
> 虚拟声卡玩具、几十个 ssh 上传/包装脚本）。**结论已沉淀到文字，脚本删掉**。
> 需要看原始脚本：旧历史打包在 `C:\Developer\_reference\bundles\ldac-driver-2026-11-07.bundle`，
> `git clone <bundle> old-driver` 之后 `git show pre-cleanup-2026-11-07:<路径>`。

## 一、VM 实验室（蓝牙 USB 直通）

| 目的 | 做法 | 结论 | 证据 |
|---|---|---|---|
| 让客户机拿到真 radio | VMware 把 Intel BT（`VID_8087&PID_0032`）直通给 guest | 可用；VMware 虚拟蓝牙不可用 | docs/timeline.md |
| 配对 | 系统设置里正常配对 | 配对/link key 完全交给 bthport，我们只做 L2CAP | docs/research.md |

## 二、挂载与 L2CAP（kill-switch）

- 目标：证明「能绑 devnode → 拿到 BRB → PING 通 → 打开 PSM 25」。**全部通过**。
- 关键教训：**本机注册的 L2CAP server 会吞掉自己的拨出连接**（PSM 25 与 PSM 23 各踩一次）。
  媒体通道打不开时，先看是不是自己的 server 满足了 open；拨出前先 `Unregister`。

## 三、AVDTP 协商

- 设备是 TX/RX 一体机：**谁发起，SEP 角色就反过来**。
  - 设备拨入 → SEID 52/54/55 全 SRC（`…d0 00 d8 00 dc 00`）
  - 主机拨出 → SEID 1/2/3 全 SNK，带 LDAC（`…04 08 08 08 0c 08`）
- LDAC 能力位：速率 `44.1=0x20 / 48=0x10 / 88.2=0x08 / 96=0x04`，模式 `MONO=0x04/Dual=0x02/Stereo=0x01`。
- 失败重试要带 `tl` 递增 + nudge 重发；对端会丢第一包。

## 四、出声（docs/timeline.md · 出声段）

- 全链路跑通但**没声音**，逐个排除采样率、编码、时钟、设备之后，真因是**媒体负载头少了 1 个字节**：
  RTP 头后面 1 字节，低 4 位 = 本包帧数（SBC/LDAC/Opus 都是这个规矩；AAC 不用）。
- 对照实验做法：`epmeter` 放 0 dBFS 单音 + 同端点 loopback 回采，一切以 dBFS 说话。

## 五、SBC 对照实验（`--codec sbc`）

- AOSP bluedroid 的 SBC 编码器以源码 vendor；用途是「协议无关」对照：
  声音小/卡顿时，用 SBC 走同一条驱动链路，能立刻区分是「编码侧」还是「链路/设备侧」。

## 六、回连（docs/timeline.md · 会话解耦段）

- 三层原因都修了：disconnect 只清一条通道 / Unregister 清掉了 addr+RefDo / ERROR 状态卡死不重试。
- 最终：keeper 线程 300 ms 轮询自己跑完整套 Setup，会话在驱动里保活。
- 实测：设备开关机 ~3 s；PC 主动回连 < 1 s（paging ~1 s 是物理层，压不掉）。

## 七、音量（docs/timeline.md · 音量段）

- 结论：设备绝对音量才是输出级；我们端点 `hw=0`，音量条只是软件衰减。
- 关键判据：**0 dBFS 单音进、0 dBFS 出** ⇒ Windows 没衰减（微软 A2DP 端点 `hw=1`，音量 100%→50% 采样不变）。
- AVRCP 控制器已实现但默认关：通道能开，但对端 TG 回包不合法、`SetAbsoluteVolume` 不应答。

## 八、已删除的实验工具（用途备查）

| 工具 | 用途 |
|---|---|
| `vmrun` 部署族（`*.upload.ps1` / `*.wrapper.ps1`） | 早期用 VMware GuestOps 推脚本、取输出；已被 SSH + scp 取代 |
| `toy/VirtualSpeaker` | 纯 WaveRT 虚拟发声端点玩具，用来单独验证 PortCls 端点；已被正式端点取代 |
| `attic/gen1..gen3` | 三代旧内核栈（ProbeDriver / 裸 KS-KMDF / PortCls 前身） |
| `log/*.etl`、`*.cab`、`*.dmp` | WPR 蓝牙抓包、崩溃 dump |
