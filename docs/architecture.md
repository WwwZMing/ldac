# 架构

LDAC（330/660/990 kbps）与 AAC（128 kbps CBR）over A2DP，面向 Windows。
系统蓝牙栈只实现 SBC，且用户态无 L2CAP socket，故以内核驱动承担蓝牙传输。

## 1. 分层

| 层 | 形态 | 职责 |
|---|---|---|
| `LdacA2dp.sys` | WDM + PortCls，C/C++ 子集 | BTHENUM `AudioSink(0x110B)` 的 FDO；bthport 原语（open/send/recv/close）；AVDTP 信令；PortCls 渲染端点 |
| `LdacPanel.exe --service` | LocalSystem 服务 | 重连与退避、编解码器选择、发送端看护、本机 API（127.0.0.1:48620）|
| `ldac-send.exe` | 服务子进程 | WASAPI loopback 采集、编码、RTP 封帧、时钟与预缓冲、按样本时间轴发包 |
| `LdacPanel.exe` | 交互进程 | 状态显示、编解码器与码率切换、日志窗口 |

约束：

1. 内核不持有常驻线程，不含重试、退避、定时任务。策略全部在用户态服务。
2. 内核仅提供原子动作：开关通道、发送一包、读取状态。
3. 新增功能先判定：故障是否会触发 BugCheck。会则必须重新设计到用户态。

## 2. 数据通路

```
播放器 → 系统混音 → ldac-send --loopback 采集 → 编码 → RTP(+LATM/LDAC 帧)
                                                        │ IOCTL_LDAC_SEND
                            BthL2capSend → BRB ACL 传输 → L2CAP PSM 25
                                                        ↓
                                                    sink 解码
```

采集/编码线程只入队；`SenderThread` 按 `样本累计数 / 采样率` 的时间轴取包发送。
`IOCTL_LDAC_SEND` 为同步 BRB，不可放入编码循环。

## 3. 组件

| 文件 | 职责 |
|---|---|
| `kernel/LdacA2dp/Driver.cpp` | DriverEntry、控制设备 `\\.\LdacCtl`、IOCTL 分发、`LdacSetup` 会话主流程、SEP 选择 |
| `kernel/LdacA2dp/BthL2cap.cpp` | BTH 接口 attach/detach、PSM 25 server、通道开关、ACL 收发、媒体通道获取 |
| `kernel/LdacA2dp/Avdtp.c` | Discover / GetCapabilities / SetConfiguration(LDAC·SBC·AAC) / Open / Start |
| `kernel/LdacA2dp/KsAdapter.cpp` | PortCls 适配器、端点的动态安装与卸载、jack 上报 |
| `kernel/LdacA2dp/MiniportWaveRT.cpp`、`MiniportTopology.cpp` | 渲染 miniport、拓扑与 jack 描述 |
| `kernel/LdacA2dp/Raii.h` | `ScopeExit`、`ResourceGuard`、`IrpGuard`、`PoolGuard`、`ObjectRef`、`ComRef` |
| `kernel/LdacA2dp/Log.c` | 日志实现，行首 `[秒.毫秒]`，输出 `C:\Users\Public\ldac-probe.log` |
| `common/ldac_io.h` | IOCTL、`LDAC_STATUS`、编解码器常量（内核与用户态共享）|
| `host/ldac_send.c` | 采集环、编码循环、RTP 封装、节拍发送 |
| `host/aac_latm.cpp` | AAC-LC（Media Foundation MFT）与 LATM 封帧 |
| `host/wasapi_loop.cpp` | WASAPI loopback 采集 |
| `panel/LdacPanel/Core.cs` | supervisor、发送端生命周期、编解码器切换、状态快照 |
| `panel/LdacPanel/Ctl.cs`、`CtlL2cap.cs` | IOCTL 封装 |
| `panel/LdacPanel/Avdtp.cs` | 用户态 AVDTP（`UserModeAvdtp=1` 时的备用路径）|
| `panel/LdacPanel/ApiServer.cs`、`ApiClient.cs`、`Protocol.cs` | 本机 API：`status` / `start` / `stop` / `codec` / `eqmid` |

## 4. 会话建立（内核路径，默认）

```
IOCTL_LDAC_START
  1. 打开信令通道（PSM 25，client，经空口）
  2. Discover            → 3 个 SEP
  3. GetCapabilities ×3  → SBC(seid1) / AAC(seid2) / LDAC(seid3)
  4. SetConfiguration    → 按 Codec 选 SEP，回放能力元素，codec-specific 收为单比特
  5. Open                → Accept
  6. 打开媒体通道         → 自行发起第二条 PSM 25；发起前注销 server
  7. Start               → Accept，进入 STREAMING
```

应答轮询规则：本 BT 栈的 `ACL_TRANSFER IN` 在无数据时立即返回 `0xC00000B5`，不遵守 Timeout。
`AvdtpXact` 因此以墙上时间为预算（每轮 250 ms），两次 recv 间隔 `KeDelayExecutionThread(10 ms)`，最多 6 轮。
按次数轮询会在 5 ms 内耗尽并误判超时。

## 5. 编解码器

| | LDAC | AAC |
|---|---|---|
| SEP 识别 | vendor `0x012D`，codecId `0x00AA` | media 0，codecType 2 |
| SetConfiguration | 回放对端能力元素，采样率/声道收为单比特 | 8 字节定长，各字段收为单比特 |
| 采样率 | 44.1 / 48 / 88.2 / 96 kHz | 44.1 / 48 kHz，跟随系统混音率 |
| 码率 | 330 / 660 / 990 kbps（eqmid）| 128 kbps CBR |
| 媒体负载 | RTP + 1 字节帧数头 + LDAC 帧 | RTP + LATM AudioMuxElement，无帧数头 |
| 编码器 | libldac（vendor）| Media Foundation AAC MFT + 自实现 LATM |

不重采样：协商采样率必须等于系统混音率。

SBC：实现保留于驱动（`IOCTL_LDAC_SET_CODEC=1`），已从 UI 移除。原因：对端 bitpool 上限 38 与本机 2..38 档位不匹配，输出有杂音；需求方不再使用。

## 6. 端点生命周期

1. 端点的在位状态只跟随蓝牙链路：`BthL2capAttach/Detach` → `LdacKsSetLinkPresent()`。
2. 安装与卸载走 PortCls 动态路径，每次新建 `PcNewPort` + miniport；卸载时 `IUnregisterSubdevice` 后释放旧对象。
   复用同一 `WavePort` 会在下一次 create 时崩于 `portcls!CPortFilterWaveRT::Init`。
3. 安装必须由工作项在 `g_AdapterReady` 之后执行。
4. jack 的 `IsConnected` 跟随链路在位状态。

## 7. 内核实现约束

1. 不使用标准库、浮点、SSE、异常；容器仅定长数组。
2. 分配与引用一律经 `Raii.h` 守卫，不使用 goto-cleanup。
   例外：取不到 `ChannelHandle` 时的 BRB 不释放（协议栈可能仍在写入）。
3. ACL 传输缓冲必须为非分页内存；`BthL2capSend/Recv` 内部拷贝，调用方可传栈数组。
4. 事件对象只初始化一次；重复 `KeInitializeEvent` 会破坏等待链表。
5. INF 为纯 ASCII；源码 UTF-8 并以 `/utf-8` 编译。

## 8. AAC 的 LATM 布局

对端仅接受：每帧携带 StreamMuxConfig，`audioMuxVersion = 1`。字段顺序：

```
useSameStreamMux(1)              = 0
audioMuxVersion(1)               = 1
audioMuxVersionA(1)              = 0
taraBufferFullness               2 位“字节数-1” + 8 位值 = 0xFF
allStreamsSameTimeFraming(1)      = 1
numSubFrames-1(6)                = 0
numProgram-1(4)                  = 0
numLayer-1(3)                    = 0
ascLen                           2 位 + 8 位（48 kHz 立体声 LC = 22）
AudioSpecificConfig              AOT(5)=2, sfIndex(4), chCfg(4), frameLengthFlag, dependsOnCoreCoder, extensionFlag
填充                              对齐到字节边界（48 kHz 为 6 位）
frameLengthType(3)               = 0
latmBufferFullness(8)            = 192
otherDataPresent(1)              = 0
crcCheckPresent(1)               = 0
PayloadLengthInfo                每 255 字节一个 0xFF，末尾余数
PayloadMux                       原始 AAC AU
填充                              对齐到字节边界
```

48 kHz 立体声首帧起始字节：`47 FC 00 00 B0 8C 80 03 00`。

RTP 负载不含“帧数”头字节；SBC 与 LDAC 需要该字节。

## 9. 故障归因

| BugCheck | 现象 | 根因 | 修正 |
|---|---|---|---|
| `0x139` | wait-list 损坏 | 有人等待时重复 `KeInitializeEvent`；keeper 线程高频开关通道 | 事件只初始化一次；删除 keeper |
| `0xC9` | 无效设备对象 | 缓存已移除的 `g_LdacBthPdo` | 仅在 AddDevice 中 attach，Detach 清空 PDO |
| `0x35` | IRP 栈不足 | QI IRP 使用默认栈深 | `StackSize + 2`，钳制于 4..120 |
| `0x3B` | `CPortFilterWaveRT::Init` 空指针 | 卸载后复用 `WavePort` | 每次安装新建 port + miniport |
| 无 dump | 开机黑屏 | 在 `AddDevice` 或 adapter 未就绪时注册子设备 | 工作项 + `g_AdapterReady` 门控 |

## 10. 限制

1. 不重采样；协商采样率须等于系统混音率，否则发送端退出。
2. 切换编解码器会拆除 A2DP 会话，对端可能同时释放 ACL，重连需数秒至十余秒。
3. 单一 sink 验证：UGREEN BT505。其他设备需调整 SEP 选择与能力位。

## 11. 音量与 AVRCP

1. 本端点拓扑为 0 节点，上报 `hw=0`：Windows 音量条对采样做软件衰减，不改设备绝对音量。
2. BT505 支持 AVRCP 1.4 vendor-dependent `SetAbsoluteVolume`，对端回 `ACCEPTED`：

   ```
   host   20 11 0e 00 48 00 00 19 58 50 00 00 01 7f
   sink   22 11 0e 09 48 00 00 19 58 50 00 00 01 7f
   ```

   字段依次为 AVCTP 3 字节（TL / PT / CR 与 profile `0x110E`）、ctype、subunit `0x48`、
   opcode `0x00`（VENDOR DEPENDENT）、company `0x001958`、PDU `0x50`、packet type `0x00`、
   param length `0x0001`、volume（`0x7F` = 100%）。
3. AVRCP 1.0 风格帧（`48 50 <vol>`，缺 packet type 与 param length）对端不应答。
4. PSM 23 归系统 `BthAvrcpTg`；本驱动不实现 AVRCP（提交 `0e6b993`），端点音量与设备音量因此不同步。
5. 设备绝对音量须由系统 A2DP 端点或设备按键设为 100%；该端点的滑块此后只做软件衰减。

## 12. 发送端调度

1. 采集、编码、发包三线程均以 MMCSS「Pro Audio」+ `AVRT_PRIORITY_CRITICAL` 运行；
   `AvSetMmThreadCharacteristicsW` 失败时回退 `THREAD_PRIORITY_HIGHEST`。
2. 发包节拍 5.33 ms（每包 2 帧 × 128 样本 / 48 kHz）。线程被挤开 20–40 ms 时，
   空口速率掉到约 97 包/秒（开机后实测持续约 70 秒），之后恢复 187.5 包/秒。
3. 诊断字段：`[pkt N] ... t=<毫秒> gap=<最近 100 包最大发包间隔，微秒>`。
   `gap` 长期大于 20 ms 即调度问题，不是编码算力不足。
