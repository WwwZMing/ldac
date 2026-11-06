# 实验时间线

> 2026-11-02 → 11-07，从「想做」到「能用」。每条都写清楚：当时以为是什么、实际是什么、证据在哪。
> 结论性的东西在 [architecture.md](architecture.md) 与 [runbook.md](runbook.md)，这里只记过程。

## 里程碑快照

| 阶段 | 结论 |
|---|---|
| Phase 0 可行性 | ✅ 设备本身 LDAC 正常 |
| Phase 0.5 挂载 kill-switch | ✅ 绑 `AudioSink(0x110B)`、拿 BRB、PING 通、开 PSM 25 |
| KM1 L2CAP 管道 | ✅ 收发正常；踩到「本机 server 吞掉自己拨出」 |
| KM2 caps 查询 | ⚠️ 改道：没走 SDP，用 AVDTP `GetCapabilities` |
| KM3 SBC 出声 | ✅ |
| KM4 LDAC 990 kbps | ✅ |
| KM5 音频管线 | 🟡 可用；长稳未测 |
| KM6 AVRCP / UI | 🟡 AVRCP 实现但默认关；无 UI |

## 11-02 · 三代内核栈与蓝屏

起点：编码器写在**内核**里，一天蓝屏好几次。

| 代 | 形态 | 死因/去向 |
|---|---|---|
| gen1 | `ProbeDriver`（`sc create` 非 PnP） | 早期探测用，废弃 |
| gen2 | 裸 KS + KMDF | `KsCreateFilterFactory` 往 KMDF 的 DeviceExtension 写 KS header → `0x7E` |
| gen3 | **PortCls / WDM**（现役） | 崩溃面被压缩到通道层 |

**结论**：业务逻辑不能放在崩溃即整机重启的地方 → 编码器搬到用户态，内核只留 L2CAP 通道。
详细归因（含每代代码位置）在驱动仓库的 `ARCH.md`。

崩溃签名留档：当时留下的 5 个 dump 里主要是 **`BUGCHECK_CODE 0x35`**（IRP/BRB 生命周期，
即「BRB 还在被协议栈写就被释放」那一类），最终靠**同步提交 + 拿不到句柄时有意泄漏 BRB** 解决。
dump、WinDbg 日志和 300 多个一次性脚本已删除（git 历史里可查）。

## 11-03 · 调研与选型

- BT505 规格与 Windows 平台限制见 [research.md](research.md)。
- 三条路线：① 专用 dongle + WinUSB + BTstack（A2DP-Windows-Bridge 路线）② 内核驱动替换 A2DP codec ③ 直接用现成软件。
- 选 ②（用内置蓝牙），因为目标是「不用额外硬件」；① 保留为参考实现与退路。

**当时的实现计划 vs 实际落地**：

| 计划 | 实际 | 原因 |
|---|---|---|
| KMDF profile driver，MSBuild 构建 | WDM + PortCls，直接 `cl`/`link` | WDK 10.0.28000 的 Build.Tasks 是给 VS2026 的（18.0），targets 却按 `VisualStudioVersion=17.0` 找 `...17.0.dll` → MSB4062 |
| 内核 Rust（windows-drivers-rs） | C | `bthddi.h` 的 BRB union/回调 bindgen 成本太高，先跑通链路 |
| 用户态协议引擎（AVDTP 在服务里） | AVDTP 在驱动里 | BRB 异步，用户态持有 pending IRP 太复杂；且会话放驱动里用户态退出才不拆链 |
| 虚拟声卡 + 单应用采集 | PortCls WaveRT 端点 + loopback | 端点就是普通渲染设备，任何播放器都能选；编码器在用户态，崩了不蓝屏 |

## 11-04 · VM 蓝牙直通

- VMware 虚拟蓝牙不可用 → Intel `8087:0032` **整机直通**（`usb.vbluetooth=FALSE` + `autoConnect vid:8087 pid:0032`）。
- 配对完全交给 Windows（link key 由 bthport 管），我们只做 L2CAP。
- 环境坑：VMware 首次 attach 的模态框会让 vmx 卡在 `MSG in progress`；`Import-Certificate` 写 TrustedPublisher 在 tiny11 上被拒，改用 `certutil -addstore`；换 A2DP 驱动后必须重启端点才重新枚举；抓建链要先把 `bthserv` 设 manual。

## 11-05 白天 · Phase 0.5：能不能挂上去

目标：证明「绑 devnode → 拿到 BRB → PING 通 → 打开 PSM 25」。**四步全过**。

- 绑定形态：**替换 `BthA2dp`，做 `AudioSink(0x110B)` 的 FDO**（upper filter 会与 BthA2dp 抢 SEP）。
- 构建：MSB4062 → `build.ps1` 直接调 `cl`/`link`，不碰 MSBuild 任务链。

## 11-05 夜 · LDAC 协商打通（一晚上七个坑）

| # | 坑 | 真相 |
|---|---|---|
| 1 | 信令通道 MTU 1500，Discover 零应答 | 设备只接受 **679**（与它自己 Configure Request 一致） |
| 2 | 超时后换新 TL 重试，应答全部错位 | 要**同 TL nudge 重发**（设备应答稳定晚约 5 s，不催不回） |
| 3 | 「接受迟到应答」 | 必须严格按 TL 匹配，否则 SEID 与 caps 整体串位，带 LDAC 的 SEID 3 被跳过 |
| 4 | SEID 2 拿到 SEID 1 的 SBC 能力 | 设备会重发上一条应答，**GetCapabilities 要去重** |
| 5 | 媒体传输通道开在 PSM 33，无应答 | 抓微软栈冷启动确认：传输通道是**第二条 PSM 25** |
| 6 | `START` 不被 ack | Open 与 Start 之间要 settle（微软栈隔约 2.8 s；我们 300 ms + 150 ms 够用） |
| 7 | 时间轴只有 1/6 速 | `ldacBT_encode` 一次吐整包多帧，RTP 时间戳要按**真实帧数**推进 |

结果：Discover/GetCaps/SetConfig/Open/Start 全 Accept，媒体流 330 kbps 稳定 —— **但没声音，灯停在 AAC**。

## 11-05 深夜 → 11-06 凌晨 · 出声

**真因：A2DP 的 RTP 负载不是裸 codec 帧** —— RTP 头后面还有 1 字节媒体负载头，低 4 位是本包帧数（SBC/LDAC/Opus 都这样；AAC 用 RFC 3640 AU 头，所以微软栈天生正确）。

```
[ RTP 12B ][ media header 1B ][ codec frame(s) … ]
                 └ 低 4 位 = 帧数
```

顺带查清两件事：

- **本机 server 会吞掉自己的拨出**：我们在 PSM 25 上注册了 server，导致后续 client open 被 bthport 本地满足（句柄有效、send 成功、空口上没有第二条 Connection Request）。修法：拨出前先 `Unregister`。PSM 23 上后来又踩了一次。
- **BT505 是 TX/RX 双模，SEP 表随发起方向翻转**：设备拨入 → SEID 52/54/55 全 **SRC**；我们拨出 → SEID 1/2/3 全 **SNK**（含 LDAC）。对照微软栈时必须区分方向。

实测：`SET_CONFIGURATION acp=3 int=1`、`media up mtu=679`、`START acked`、`1009.5 kbps / 6.0 s`。

## 11-06 · 卡顿治理

- 上：256 KB 采集环 + 专用发送线程 + `timeBeginPeriod(1)` + 150 ms 预缓冲。
- **误判**：以为是异步 BRB 导致抖动，实测无关，回退收效甚微的那套改动。

## 11-06 晚 → 11-07 · 连接与会话解耦

旧形态：`ldac-send` START 建链、退出 STOP 全拆 → 不跑 send 设备就不连、掉线后不回连。
改成：**驱动 keeper 线程**每 300 ms 检查，空闲就自己把整套 A2DP 建起来并保持 `STREAMING`；用户态 START 看到 already streaming 直接返回。

回连失败的三层原因：

1. 断开指示只清了 `g_Inbound`，另外三条通道还标着 Open（句柄已失效）→ 之后全 `0xC000009D`。修：按 handle 清四条通道 + 状态复位。
2. `UnregisterServer` 顺手把 `g_SrvAddr`/`g_RefDo` 清零 → 再注册静默失败 → 设备回连没人监听（灯 5 s 一闪）。修：注销只清句柄。
3. 一次失败进 `ERROR` 就永久摆烂。修：每轮把 `ERROR` 复位成 `IDLE`。

耗时压缩（keeper 3000→300 ms、ACP 首等 800→300、nudge 2000→800、settle 300/400→100/150、信令重开 500→200）后：

| 场景 | 实测 |
|---|---|
| 设备开关机回连 | **约 3 s**（paging ~1 s 属物理层 + AVDTP 七次往返） |
| PC 主动回连 | **< 1 s** |

## 11-07 · 音量（从「耳朵」到「数字」）

- 造了 `epmeter.exe`：读端点音量与 **hw 位**，还能放 0 dBFS 单音并同端点回采。
- 实测对照：

| 环境 | 端点 | vol 100% 回采 | vol 50% 回采 |
|---|---|---|---|
| 微软 A2DP | **hw=1** | −0.11 dBFS | **−0.11 dBFS（不变）** |
| 我们的驱动 | **hw=0** | −0.01 dBFS | −10.52 dBFS |

- 结论：微软把音量走 AVRCP 交给设备，**采样一个 bit 都不衰减**；我们只能软件衰减，而且从不告诉设备 → 设备停在 9/127（≈ −21 dB）。设备被推到 100% 后主观立刻正常。
- 早前「峰值 12000 = −8.7 dBFS，电平正常」的判据是错的：那是音乐本身的电平；用 0 dBFS 单音才看得出真相。
- AVRCP 控制器实现了（PSM 23/AVCTP），两条拦路虎：① 本机 `Microsoft_Bluetooth_AvrcpTransport` 节点会把我们的拨出**在本地吞掉**（`bt=0xFF`，8 s 超时），禁用后立刻能连；② 对端 TG 回包不合法（CR 位为 0、ctype 不在规范集合），`SetAbsoluteVolume` 完全不应答。→ 默认关闭（`AvrcpVolume`）。

## 11-07 · 自动化放歌（三次翻车）

| 现象 | 真相 |
|---|---|
| 响一下就没声 | `Start-Process` 的子进程被 SSH 会话带走 |
| 关掉黑窗口就没声 | 交互任务的控制台窗口就是 `cmd` 宿主，关它=杀进程 |
| 换机器后 exe 一启动就死 | Debug 配置 `/MDd` → 依赖 `VCRUNTIME140D.dll`（客户机没有） |

最终：`play-music.ps1` 用 **SYSTEM 计划任务 + `cmd` 重定向**，没有可见窗口；发送端 `--match` 按端点名选，session 0 也能采到。

## 11-07 · 文档与仓库重构

- 文档：16 篇编号文档 → 5 篇按用途命名（本文 + architecture / runbook / toolkit / research）。
- 仓库：~85 MB / 300+ 文件 → 0.9 MB / 115 文件。删掉日志、ETL、崩溃 dump、三代旧内核栈、反汇编工具链、90+ 一次性脚本；`ref/` 移出仓库到 `C:\Developer\_reference\`；实验精华压成驱动仓库的 `experiments/README.md`。

## 十次「以为 → 其实」

| 当时的判断 | 真相 |
|---|---|
| 没声音是编码/时序/采样率问题 | 媒体负载头少 1 字节 |
| 媒体通道打不开是设备不支持 | 我们自己注册的 server 在本地满足了连接 |
| 设备只接受 MTU 1500 之外的怪值 | 是 679，和它自己请求的一致 |
| 传输走 PSM 33 | 第二条 PSM 25 |
| 断开后要重启才能连 | 三条通道没清干净 + 注销把地址清了 + ERROR 不重试 |
| 声音小是源电平 | 音乐本来就是这个电平；差的是设备绝对音量 |
| 音量条能把音量传给设备 | 我们端点 `hw=0`，纯软件衰减 |
| 发送端必须跑在 session 1 | 只是默认端点选错，`--match` 一上 session 0 也行 |
| 黑窗口是播放的条件 | 它只是进程宿主，关窗=杀进程 |
| 一卡一卡是异步 BRB 的错 | 与 BRB 无关；采集环 + 发送线程才是解法 |

## 现在还剩什么

1. AVRCP 绝对音量（需手机侧 btsnoop 对齐字节）；2. 设备关机时 Windows 仍显示「配对中」；
3. 88.2/96 kHz 未验证；4. 长稳未测（连 10 分钟都还没跑）。
