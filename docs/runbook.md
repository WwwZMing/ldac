# 操作手册

> 唯一可信流程。历史上「发送端必须跑在 session 1」「音量靠 AVRCP」都已被证伪，
> 原因见 [timeline.md](timeline.md)。

## 0. 前置

- 驱动服务 `LdacA2dp` Running，端点出现：`扬声器 (LDAC A2DP Audio (Abu))`
- BT505 已配对；它开关机后 keeper 会自动回连（约 3 s）
- 客户机上有 `ldac-send.exe` 与 `epmeter.exe`（本机在 `C:\Users\test\`）

## 1. 放歌（唯一推荐姿势）

> 脚本在仓库根的 `scripts\`（`C:\Developer\ldac`）。

```powershell
cd C:\Developer\ldac
.\scripts\play-music.ps1 -Vol 70     # 起发送端（SYSTEM 计划任务，无窗口）
.\scripts\play-music.ps1 -Stop       # 停
```

然后任何播放器（QQ 音乐/浏览器）直接放，输出选我们的端点。

为什么是 SYSTEM 计划任务而不是 `Start-Process`：SSH 会话结束会带走子进程；
交互任务那个黑窗口一关就等于杀进程。SYSTEM 任务跑在 session 0，**没有窗口可关**。

## 2. 确认在跑

```powershell
Get-Process ldac-send | Select-Object Id,SessionId    # SYSTEM 任务 => SessionId 0
Get-Content C:\Users\test\play.log -Tail 5   # 应有 [pkt N] peak=...
```

`peak` 是本包最大的 16 位采样绝对值（32768 = 0 dBFS），音乐通常在 3000~20000 波动。

## 3. 对照实验

```powershell
ldac-send.exe C:\Users\test\tone48.wav                   # 固定 WAV，绕开系统混音
epmeter.exe --match LDAC --tone --sec 3                     # 端点自发声 + 回采（会响）
ldac-send.exe --loopback --match LDAC --codec sbc           # SBC 对照
ldac-send.exe --loopback --match LDAC --eqmid sq            # 660 kbps 对照
```

## 4. 音量

- **设备侧**是真正的输出级：用 BT505 按键按到 100%（它会记住）。
- **Windows 音量条**（端点 `hw=0`）是软件衰减，能用，但**不会告诉设备**。
- 结论：设备保持 100%，日常用 Windows 音量条。

## 5. 常见问题

| 现象 | 原因 | 处理 |
|---|---|---|
| 放歌没声 | 发送端没在跑 | `play-music.ps1`；看 `play.log` 有无 `[pkt` |
| 响一下就没声 | 发送端被 SSH 会话带走 | 用 `play-music.ps1` |
| 关黑窗口就没声 | 那是 `cmd` 宿主，关窗=杀进程 | 同上，SYSTEM 任务无窗口 |
| 声音明显小 | 设备绝对音量低（`hw=0`、无 AVRCP） | 设备按键按到 100% |
| 一卡一卡 | 包间隔抖动（VM/USB 直通、重传） | 已上采集环 + 发送线程 + 1 ms 定时器 |
| 设备关机后不回连 | keeper 卡住 / server 注册丢了 | 看 probe log 的 `keeper:` 与 `server ... registered` |
| 装完没端点 | PortCls 没起 / 节点没切过来 | `Get-PnpDevice` 看 A2DP 节点 service 是否 `LdacA2dp` |
| 装驱动报 1072 | 旧包被标记删除 | 不要在 `bt` 目标上 `delete-driver`，只 stage 新包 |

## 6. 排障看哪里

```
C:\Users\Public\ldac-probe.log     驱动日志：BthBoot / setup / STREAMING / send FAIL / avrcp
C:\Users\test\play.log             发送端：[pkt N] peak=… frames=… len=…
epmeter.exe --list                  端点、音量、hw 位
```

## 7. 已知遗留

AVRCP 绝对音量（对端不配合）；设备关机时 Windows 仍显示「配对中」；88.2/96 kHz 未验证；长稳未测。详见 [timeline.md](timeline.md)。
