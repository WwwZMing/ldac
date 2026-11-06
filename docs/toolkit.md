# 工具箱

> 目的：把「听感」换成「数字」。结论都应该能用这套工具复现。

## 1. epmeter.exe —— 端点音量 / 电平仪表

源码 `host/epmeter.cpp`，构建 `scripts/99-build-epmeter.ps1`。

```powershell
epmeter --list                                  # 端点 + 音量 + mute + hw 位
epmeter --match LDAC --vol 70 --sec 0.1         # 只改音量（不出声）
epmeter --match LDAC --sec 5                    # 只回采 5 秒
epmeter --match LDAC --tone --sec 3             # 内置 0 dBFS/997Hz 单音 + 回采（会响）
epmeter --match LDAC --vol 100 --tone --sec 2   # 满幅（很吵）
```

| 字段 | 含义 |
|---|---|
| `hw=1` / `hw=0` | 有没有**硬件音量节点**。`hw=1` = 音量交给设备（AVRCP），采样不衰减；`hw=0` = 音量条只是软件乘在采样上 |
| `vol=` | 端点音量标量（Windows 曲线非线性：50% ≈ −10.5 dB，实测） |
| `peak / rms dBFS` | 回采峰值/有效值。0 dBFS 单音 + 音量 100% 应接近 −0.0x dBFS |

判据：**0 dBFS 进、0 dBFS 出 ⇒ Windows 没在衰减**。

## 2. 驱动探针日志

`C:\Users\Public\ldac-probe.log`（`ProbeLog`：PASSIVE_LEVEL 才落盘，高 IRQL 只进 DbgPrint）。

```
BthBoot: L2CAP server registered for …      ← 拿到 radio、注册 PSM25 成功
keeper: full setup -> 0x00000000 (state=2)   ← 一整套 A2DP 建链成功
setup: ===== STREAMING (rate=… mode=… seid=…)
open poll: … ms psm=25/23 …                  ← L2CAP 打开结果（23 = AVRCP）
recv FAIL: st=0xC00000B5                      ← 收超时（常见，nudge 会重发）
```

`C:\Users\Public\ldac-mark-*.txt` 是 `ProbeMark` 埋的时间点，用来定位卡在哪一步。

## 3. 发送端

```powershell
ldac-send.exe --loopback --match LDAC              # 系统混音 → LDAC
ldac-send.exe tone48.wav                           # 固定 WAV
ldac-send.exe --loopback --match LDAC --codec sbc  # SBC 对照
ldac-send.exe --loopback --match LDAC --eqmid sq   # 660 kbps 对照
ldac-send.exe --unload                             # 开发用：卸控制设备以卸映像
```

输出 `[pkt N] peak=… frames=… len=…`（每 100 包一行，走 **stderr**）。

## 4. 注册表开关

含义见 [architecture.md](architecture.md) §5：`TargetBdAddr`、`ChannelFlags`、`RejectInbound`、`ForceSbc`、`AvrcpVolume`。

## 5. 构建

```powershell
.\build.ps1 -Configuration Debug              # 驱动 + 发送端 + 编码自检
.\scripts\99-build-epmeter.ps1         # 只有仪表
```

## 6. 工具坑（别再踩）

| 坑 | 表现 | 处理 |
|---|---|---|
| Debug CRT | 客户机上 exe 一启动就死、连 usage 都不打 | 工程已强制 `RuntimeLibrary=MultiThreadedDLL` |
| `Test-Path` 查设备路径 | 设备在却返回 False | 用 `CreateFile` 试开 |
| PowerShell 嵌套引号 | `$_` 被宿主吃掉 | 写成 `.ps1` 再 scp，别拼一行 ssh |
| `Get-ChildItem -Depth N` | 深层文件找不到 | 用 `es.exe` 或不限深度 |
| 覆盖运行中的 exe | scp Failure | 先 `play-music.ps1 -Stop` |
