# 架构（已实现）

> 这份描述代码里真实存在的形态，不是设计稿。历史选型与偏差原因见 [timeline.md](timeline.md)。

## 1. 总体形态

```
播放器 ─▶ Windows 混音器 ─▶ 【我们的 PortCls 渲染端点】
                                │ WASAPI loopback
                                ▼
                  ldac-send.exe（用户态，SYSTEM 计划任务）
                    · 采集线程：loopback → 256 KB 字节环
                    · 主循环：S16 → libldacBT(HQ 990k) / AOSP SBC
                    · RTP 打包：1 B 媒体负载头 + 2 帧 LDAC = 673 B
                    · 发送线程：按采样时间轴节拍，队列 >40 丢旧包
                                │ IOCTL_LDAC_SEND
  ───────────── 用户态 / 内核边界 ─────────────
                                ▼
                  LdacA2dp.sys（WDM + PortCls，C）
                    Driver.c    控制设备 \\.\LdacCtl、IOCTL、线程、状态机
                    BthL2cap.c  BRB 封装（open/send/recv/PSM server）
                    Avdtp.c     AVDTP 信令（Discover→…→Start）
                    Avrcp.c     AVRCP 绝对音量（默认关）
                    Miniport*   WaveRT 渲染端点 + 0 节点拓扑
                                │ BRB_L2CA_ACL_TRANSFER
                                ▼
                  bthport.sys → Intel 蓝牙 → BT505（Sink，LDAC 解码 + DAC）
```

关键点：**连接与会话在驱动里**，不在用户态。用户态只管把编码后的包发下去；
它退出也不会拆链（`IOCTL_LDAC_STOP` 只关媒体通道）。

## 2. 内核模块（`kernel/LdacA2dp/`）

| 文件 | 职责 |
|---|---|
| `Driver.c` | DriverEntry（控制设备、日志）、AddDevice（PortCls adapter、`BthL2capAttach`、注册 PSM25 server）、IOCTL 分发、**keeper 线程**、状态机、注册表参数 |
| `BthL2cap.c` | 向 PDO 发 `IRP_MN_QUERY_INTERFACE` 取 `BTH_PROFILE_DRIVER_INTERFACE`；`SubmitBrb`（同步，5 s）；通道 open/close/send/recv/ping；PSM 25 server 与 inbound 回调；常驻信令通道 hold/take/give-back |
| `Avdtp.c` | AVDTP：Discover / GetCapabilities / SetConfiguration / Open / Start；SEP 与能力解析（LDAC `0x012D/0x00AA`、速率/模式位）；重试与 nudge |
| `Avrcp.c` | AVRCP CT（PSM 23 / AVCTP）：GetCapabilities、RegisterNotification、SetAbsoluteVolume。**默认关闭**（`AvrcpVolume=1` 才跑） |
| `MiniportWaveRT.cpp` | WaveRT 渲染端点：混音格式 48 kHz/2ch/32f；声明支持 44.1/48/88.2/96、16/32 位 |
| `MiniportTopology.cpp` | 拓扑 **0 节点** → 端点没有硬件音量、没有 AVRCP 音量通路（见 [timeline.md](timeline.md) 音量段） |
| `KsAdapter.cpp` / `CUnknown.cpp` | PortCls adapter 胶水 |
| `Log.c` | `ProbeLog`（DbgPrint + `C:\Users\Public\ldac-probe.log`；仅 PASSIVE_LEVEL 落盘）、`ProbeMark` |

## 3. 用户态模块（`host/`）

| 文件 | 职责 |
|---|---|
| `ldac_send.c` | `--addr/--eqmid/--gain/--codec/--match/--loopback/--unload`、WAV 模式、采集线程 + 256 KB 环、编码主循环、RTP 打包、发包队列 + 发送线程、`timeBeginPeriod(1)`、150 ms 预缓冲、每 100 包打 `[pkt N] peak=…` |
| `wasapi_loop.cpp` | WASAPI loopback；**按端点名选设备**（`--match`），所以能从 session 0（SSH / SYSTEM）跑 |
| `epmeter.cpp` | 仪表：枚举渲染端点（音量 / 静音 / **是否有硬件音量**）、按名选端点、内置 0 dBFS 单音 + 同端点回采 dBFS |

## 4. 线程与状态机（内核）

| 线程 | 周期 | 干什么 |
|---|---|---|
| keeper | 300 ms | ① 保证 PSM25 server 注册还在；② `ERROR` → 复位 `IDLE`；③ `IDLE` 时跑整套 `LdacSetup`，把会话维持在 `STREAMING` |
| AVRCP | 5 s（默认不启动） | 会话在跑且本世代没成功过时，读设备音量并推 `SetAbsoluteVolume(0x7F)`，每次连接最多 3 次 |
| PortCls 流线程 | 由 KS | 端点缓冲读写、`SetState` |

```
        ┌────────┐  LdacSetup 成功   ┌───────────┐  IOCTL_START  ┌───────────┐
   ───▶ │  IDLE  │ ────────────────▶ │ STREAMING │ ◀──────────── │ STARTING  │
        └────────┘                   └───────────┘               └───────────┘
             ▲                              │ 对端拆链/失败             │
             │        keeper 复位           ▼                          │
             └─────────────────────────── ┌────────┐ ◀─────────────────┘
                                           │ ERROR  │
                                           └────────┘
```

`LDAC_STATUS`（`IOCTL_LDAC_STATUS`）：`State / LastError / SigMtu / MediaMtu / RateBit / ModeBit / SinkSeid`。

## 5. 接口与开关

**IOCTL**（`\\.\LdacCtl`）：`START`、`STOP`（只关媒体通道）、`STATUS`、`SEND`、`UNLOAD`（开发用）。

**注册表**（`HKLM\SYSTEM\CurrentControlSet\Services\LdacA2dp\Parameters`）：

| 值 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `TargetBdAddr` | REG_SZ | — | 目标地址，如 `33:DB:8A:C7:E6:7A` |
| `ChannelFlags` | DWORD | 0 | L2CAP open 的 `ChannelFlags`，换值不用重编 |
| `RejectInbound` | DWORD | 0 | 非 0 = 拒绝设备拨入的 PSM 25 |
| `ForceSbc` | DWORD | 0 | 非 0 = 走 SBC 对照实验 |
| `AvrcpVolume` | DWORD | 0 | 非 0 = 启用 AVRCP 音量控制器 |

## 6. 数据面节拍（实测）

- 端点混音格式 **48000 Hz / 2ch / 32-bit float**；
- LDAC HQ → 990 kbps；48 kHz 下每包 2 帧、负载 673 B；
- 采集环 256 KB（约 200 ms），预缓冲 150 ms，队列上限 40 包；
- `timeBeginPeriod(1)` 避免 1 ms 节拍被 15.6 ms 系统 tick 放大。
