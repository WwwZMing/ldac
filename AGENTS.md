# AGENTS.md — 项目须知

LDAC（330/660/990 kbps）与 AAC（128 kbps CBR）over A2DP，面向 Windows。
系统蓝牙栈只实现 SBC，且用户态无 L2CAP socket，故以内核驱动承担蓝牙传输。

## 架构总纲

内核只负责与蓝牙交互，音频业务全部位于用户态。

| 层 | 内容 |
|---|---|
| 内核 `LdacA2dp.sys`（WDM + PortCls）| BTHENUM `AudioSink(0x110B)` 的 FDO、BRB/L2CAP 原语、PortCls 渲染端点。无常驻线程，无重连逻辑 |
| 服务 `LdacPanel.exe --service` | 重连与退避（`Core.Supervise`）、编解码器选择、发送端看护、本机 API |
| 发送端 `ldac-send.exe` | WASAPI 采集、LDAC/AAC 编码、RTP 封帧、时钟与预缓冲、发包节拍 |

硬性约束：

1. 策略、重试、退避、状态机推进、定时任务一律在用户态服务。
2. 内核只提供原子动作：开关通道、发送一包、读取状态；不得包含轮询或重试循环。
3. 新增功能先判定故障是否会触发 BugCheck；会则重新设计到用户态。

## 目录

```
kernel/LdacA2dp/   驱动：Driver.cpp（IOCTL/状态/会话）、BthL2cap.cpp（BRB/BTH）、
                   Avdtp.c（协商）、KsAdapter.cpp（动态端点）、Miniport*/、Log.c、LdacA2dp.inf
host/              发送端：ldac_send.c、aac_latm.cpp（AAC+LATM）、wasapi_loop、epmeter
common/            ldac_io.h —— IOCTL 与状态结构（内核/用户态共享）
panel/LdacPanel/   面板与服务（同一 exe，`--service` 为服务模式）
third_party/       libldac、aosp-sbc、compat（源码入库）
tests/             AVDTP 单元测试（含内核头 shim）与编码自检
scripts/           构建、安装、部署、抓包、日志脚本
certs/             自签名开发证书公钥
docs/              architecture.md（架构）、development.md（开发流程）
experiments/       实验记录（文本）
bin-* obj-*        构建产物（gitignore）
```

## 构建

```powershell
.\build.ps1                          # 驱动 + 发送端 + 编码自检
.\build.ps1 -Configuration Release
.\scripts\99-build-epmeter.ps1      # 仅编译 epmeter
bin-tests\Debug\ldac-enc-test.exe    # 编码自检
```

上机为高风险操作：一次只改一层，本地编译通过后再推客户机。

## 常用脚本

| 脚本 | 用途 |
|---|---|
| `scripts/48-install-ldac.ps1` | 真机：导入证书、stage 驱动包、绑定 A2DP 节点 |
| `scripts/47-uninstall-ldac.ps1`、`49-cleanup-ldac.ps1` | 卸载、清理残留 |
| `scripts/50-install-service.ps1` | 注册或启停 `LdacA2dpSvc` |
| `scripts/60-deploy-vm.ps1` | 客户机完整部署（`-VerifyOnly` 只检查）|
| `scripts/vm-install-ldac.ps1` | 客户机装驱动（`-Target uninstall` 回滚）|
| `scripts/vm-restore-inbox.ps1` | 删除我方驱动包，退回 inbox `btha2dp`（需重启）|
| `scripts/55-ldac-stop.ps1` | 发送 `IOCTL_LDAC_STOP`，强制回 IDLE |
| `scripts/56-capture-aac.ps1` | 抓包并放音（USBPcap）|
| `scripts/play-music.ps1` | 起停发送端（SYSTEM 计划任务）|

## 硬约束

1. 命令使用 PowerShell 7；git 例外。
2. INF 必须为纯 ASCII；源码 UTF-8 并以 `/utf-8` 编译。
3. 内核禁用浮点、SSE、标准库、异常；日志与格式化走 `Log.c`。
4. 日志写入 `C:\Users\Public\ldac-probe.log`；高 IRQL 仅 `DbgPrint`。
5. 用户态可执行文件必须使用发布版 CRT（`RuntimeLibrary=MultiThreadedDLL`）。
6. 临时文件置于 `log/`，不得堆放在根目录。
7. 驱动库中不得同时存在两个本项目的包；安装前先 `pnputil /enum-drivers` 清理。
8. `ldac-send.exe` 必须与 `LdacPanel.exe` 同目录。
9. 本机 `Microsoft_Bluetooth_AvrcpTransport` 节点会使 PSM 23 拨出在本地被吞；AVRCP 实验前禁用它，完成后恢复。
10. 发送端 `--match` 按端点名选择设备，session 0 亦可采集。
11. 单条命令控制在 2 分钟内；长流程拆分或置于后台。

## 部署判据（客户机）

全部满足方可视为部署完成：

1. `.inf`、`.sys`、`.cat`、`.cer` 四个文件均已推送。
2. 先 `-Target uninstall` 再 `-Target bt`；覆盖 `sys` 不会生效（`pnputil` 遇同版本跳过）。
3. 内核变更后已重启；日志 `===== LdacA2dp build [...] =====` 与构建时间一致。
4. `LdacA2dpSvc` 存在、`AUTO_START`、`RUNNING`；硬复位后须复查服务项。
5. `ldac-send.exe` 与 `LdacPanel.exe` 同目录。
6. `Parameters` 下的测试开关（`Codec`、`VerboseLog`、`ChannelFlags` 等）已显式设置或清除。
7. 已验证：sys 时间戳、节点 service、服务状态、发送端进程、API 端口。

设备卡死或需重启时由使用者告知，不编写轮询脚本探测状态。

## Git 约定

- 主分支 `main`。构建产物、日志、dump、`ref/`、`memory/` 不入库。
- `third_party` 以源码形式入库，不使用 submodule。
- 提交信息使用 ASCII。
