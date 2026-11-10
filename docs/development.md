# 开发流程

## 0. 环境

- Windows、VS 2026（含 WDK `WindowsKernelModeDriver10.0`）、.NET 10 SDK。
- 测试机：本机，或 `C:\VMs\win11\win11.vmx`（客户机 `test@192.168.144.130`，密钥 `%USERPROFILE%\.ssh\id_ed25519_vm_test`）。
- 蓝牙适配器 Intel `VID_8087 PID_0032`；sink 为 UGREEN BT505（`33:DB:8A:C7:E6:7A`）。
- 客户机测试需 USB 透传（`usb.autoConnect.device0 = "vid:8087 pid:0032"`）；适配器回到宿主机需先给虚拟机断电。

## 1. 构建

```powershell
.\build.ps1                       # 驱动 + 发送端 + 编码自检
.\scripts\99-build-epmeter.ps1    # 仅编译 epmeter
bin-tests\Debug\ldac-enc-test.exe  # LDAC 编码自检
```

面板（GUI 与服务为同一 exe）：

```powershell
# publish 前须停止服务，否则 panel\publish\LdacPanel.exe 被占用，报 MSB4018
sc stop LdacA2dpSvc
dotnet publish -c Release -r win-x64 --self-contained true `
  -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true `
  -o panel\publish
Copy-Item bin-host\Debug\ldac-send.exe panel\publish\ -Force   # 发送端须与面板同目录
sc start LdacA2dpSvc
```

## 2. 部署

### 2.1 宿主

```powershell
.\scripts\48-install-ldac.ps1                 # 导入证书、stage 驱动包、仅在 A2DP 节点更新
.\scripts\47-uninstall-ldac.ps1 [-Reboot]     # 回滚至 inbox btha2dp
.\scripts\50-install-service.ps1 -Exe <publish\LdacPanel.exe>
```

### 2.2 客户机

```powershell
.\scripts\60-deploy-vm.ps1 -UserModeAvdtp 0 [-SkipReboot]
.\scripts\60-deploy-vm.ps1 -VerifyOnly        # 只检查，不改动
.\scripts\remote.ps1 -TimeoutSec 60 -Command "..."   # 在客户机执行命令
```

### 2.3 部署完成判据

全部满足方可视为部署完成：

1. 驱动包四个文件均已推送：`.inf`、`.sys`、`.cat`、`.cer`。
2. 先卸载再安装。`pnputil` 遇同版本会跳过，设备会继续运行旧映像。
3. 内核变更后已重启；日志 `===== LdacA2dp build [...] =====` 与构建时间一致。
4. `LdacA2dpSvc` 存在、`AUTO_START`、`RUNNING`；硬复位后须复查服务项是否丢失。
5. `ldac-send.exe` 与 `LdacPanel.exe` 同目录。
6. 注册表 `HKLM\SYSTEM\CurrentControlSet\Services\LdacA2dp\Parameters` 的
   `Codec`（0 LDAC / 1 SBC / 2 AAC）、`VerboseLog`、`TargetBdAddr` 已显式设置。
7. 已验证：sys 时间戳、节点 service、服务状态、发送端进程、API 端口。

设备卡死或需重启时由使用者告知，不编写轮询脚本探测状态。

## 3. 测试

### 3.1 纯音（不经采集）

```powershell
reg add 'HKLM\SYSTEM\CurrentControlSet\Services\LdacA2dp\Parameters' /v Codec /t REG_DWORD /d 2 /f
sc stop LdacA2dpSvc                                  # 服务会自动启动发送端，先停以避免单实例冲突
bin-host\Debug\ldac-send.exe log\tone48k20.wav --codec aac
bin-host\Debug\ldac-send.exe log\tone44k20.wav --codec aac --rate 44100   # 指定协商采样率
```

判据：输出中 `frames`、`queued`、`sent` 三者相等。WAV 模式按队列深度节流并在退出前等待队列排空；
否则仅播出一小段，不能作为编解码器判定依据。

### 3.2 系统音频（产品路径）

```powershell
sc start LdacA2dpSvc     # supervisor 负责 START 与启动发送端，--codec 取自内核 codec
```

### 3.3 编解码器与码率切换

面板下拉框 → 服务命令 `codec <name>` → `IOCTL_LDAC_SET_CODEC` + 写注册表 + 由 supervisor 重建会话。
命令行等价方式：

```powershell
# 本机 API：TCP 48620，一行命令一行 JSON
$c = New-Object Net.Sockets.TcpClient('127.0.0.1', 48620)
$s = $c.GetStream(); $w = New-Object IO.StreamWriter($s); $r = New-Object IO.StreamReader($s)
$w.WriteLine('codec aac'); $w.Flush(); $r.ReadLine()
```

切换期间中断 1~15 秒：拆除 A2DP 会话可能使对端释放 ACL，需重新寻呼。属设备行为。

### 3.4 强制回 IDLE

```powershell
.\scripts\55-ldac-stop.ps1      # 发送 IOCTL_LDAC_STOP，用于测试间清状态
```

### 3.5 灯色（BT505）

| 灯色 | 编解码器 |
|---|---|
| 深绿 | LDAC |
| 浅绿 | SBC |
| 深蓝 | AAC |

灯色仅反映协商与负载解析结果，不证明解码正常，只作辅助判据。

## 4. 抓包

驱动日志只证明 BRB 已提交，不证明空口字节。判定传输内容须抓包。

准备：Wireshark 安装时勾选 USBPcap（可选组件，默认不勾）。
`dumpcap` 无法打开 `\\.\USBPcap*`（Error 123），改用 `USBPcapCMD.exe`。

```powershell
$u = Start-Process 'C:\Program Files\USBPcap\USBPcapCMD.exe' -PassThru -NoNewWindow `
  -ArgumentList '-d','\\.\USBPcap1','-o','log\bt1.pcap','-A','--inject-descriptors'
Start-Sleep 3
bin-host\Debug\ldac-send.exe log\tone48k20.wav --codec aac
Stop-Process -Id $u.Id -Force      # 强杀会丢弃末尾数百毫秒数据
```

- `-A`：缺省时报 `empty capture`。
- `--inject-descriptors`：缺省时 Wireshark 无法识别蓝牙设备，协议仅解到 `usb` 层。
- 不确定蓝牙设备位于哪个 `USBPcapN` 时，同时抓取，按文件大小判断。

分析：

```powershell
$ts = 'C:\Program Files\Wireshark\tshark.exe'
& $ts -r log\bt1.pcap -q -z io,phs                          # bthci_acl → btl2cap → bta2dp
& $ts -r log\bt1.pcap -Y btavdtp -T fields -e _ws.col.Info   # 协商流程
& $ts -r log\bt1.pcap -Y bta2dp -c 1 -x                     # 媒体包原始字节
```

脚本：`scripts\56-capture-aac.ps1`（等待设备在线 → 抓包 → 放音 → 收尾）。

开机即发的复现：`scripts\61-boot-capture.ps1`（计划任务 `LdacBootCapture`，开机 +15 s 起、抓 300 s、SYSTEM、pwsh 7）。
落盘 `log\boot\<时间戳>-usbpcapN.pcap` 与 `-monitor.txt`；monitor 每秒一行
`sec state pkt peak pps kbps cpu sndcpu`，可直接看出空口包率与 CPU 抢占是否相关。

## 5. 排查顺序

1. 状态：`IOCTL_LDAC_STATUS` 或服务 `status`，关注 state、codec、seid、媒体 MTU、包计数。
2. 驱动日志：`C:\Users\Public\ldac-probe.log`。`VerboseLog=1` 逐 BRB 记录，写盘量大，排查后关闭。
3. 发送端日志：`C:\ProgramData\LdacPanel\sender.log`，每 100 包一行。
   包率判据：AAC 约 47 pps，LDAC 约 187 pps（990 kbps）；降至个位数说明发送端未运行。
4. 抓包：见第 4 节，将空口字节与用户态缓冲区逐字节比对。
5. 编码独立性验证：`ffmpeg -f loas -i <file>.latm -f s16le -` 能解出 PCM，即编码与封帧合法，问题不在编码侧。

## 6. 已知缺陷与规避

| 现象 | 原因 | 规避 |
|---|---|---|
| `0x139`、整机卡死 | 内核常驻线程与轮询 | 内核不设常驻线程，策略上移用户态 |
| `0x3B` | 卸载后复用 `WavePort` | 每次安装新建 port 与 miniport |
| 应答必然超时 | `BthL2capRecv` 忽略 Timeout | 按墙上时间 sleep 轮询 |
| 媒体通道无数据上行 | 本地 PSM 25 server 使 client open 被本地满足 | 发起前注销 server，完成后重新注册 |
| 协商成功但无声 | `RtpFlush` 在保护分支后仍无条件写 `Pkt[12]`，覆盖 LATM 首字节 | 仅在需要帧数头时写入 |
| 纯音只播出一小段 | WAV 模式未节流、未等待队列排空 | 按队列深度节流并等待排空 |
| `Test-Path '\\.\LdacCtl'` 恒为 false | 设备对象不支持该判定 | 用 `[IO.File]::Open` 打开判断 |
| publish 报 MSB4018 | 服务占用 `LdacPanel.exe` | 先停服务再 publish |
| 同一首歌突然变小、滑块不改变音箱音量 | 端点 `hw=0`，AVRCP 未实现，音箱绝对音量停在低值 | 用系统 A2DP 端点或设备按键把音箱推到 100% |
| 开机后约一分钟内声音断续/滋滋，之后自愈 | 发送端三线程默认优先级，开机后台任务把 5.33 ms 节拍挤开 20–40 ms，空口只剩 ~97 包/秒 | 三线程改 MMCSS「Pro Audio」+ `AVRT_PRIORITY_CRITICAL`（`host/ldac_send.c`）；日志 `gap=` 为最近 100 包最大发包间隔 |

## 7. 约定

- 提交信息使用 ASCII。
- 不入库：`bin-*`、`obj-*`、`panel/publish`、`log/`、`ref/`、`memory/`。
- `third_party` 以源码形式入库，不使用 submodule。
- 用户态可执行文件必须使用发布版 CRT（`RuntimeLibrary=MultiThreadedDLL`）。
- 单条命令执行时间控制在 2 分钟内；长流程拆分或置于后台。
