# AGENTS.md — 项目须知（AI 助手从这里读起）

> LDAC A2DP for Windows：把系统音频以 LDAC（330/660/990 kbps）推到蓝牙音箱/耳机。
> Windows 蓝牙栈没有 LDAC 编码器，用户态也拿不到 L2CAP socket，所以有一个内核驱动存在。

## 架构总纲

**内核只做「和蓝牙说话」，音频业务全在用户态。**

| 层 | 放什么 |
|---|---|
| 内核 `LdacA2dp.sys`（**WDM + PortCls，C**） | BTHENUM `AudioSink(0x110B)` 的 FDO、BRB/L2CAP 通道、**AVDTP 信令状态机**、keeper 保活线程、PortCls 渲染端点 |
| 用户态 `ldac-send.exe` | WASAPI 采集、**LDAC/SBC 编码**、RTP 封帧、时钟与预缓冲、发包节拍 |

判断标准：**这段代码崩了会蓝屏吗？** 会 → 想办法挪到用户态；不会 → 也先想想能不能挪。

会话（AVDTP 建链与保持）放驱动里是刻意的：用户态退出不能拆链，设备回连才稳定。

## 目录

```
kernel/LdacA2dp/   驱动：Driver.c（IOCTL/线程/状态机）、BthL2cap.c（BRB）、
                   Avdtp.c、Avrcp.c、MiniportWaveRT/Topology、Log.c、LdacA2dp.inf
host/              ldac_send.c（发送端）、wasapi_loop.{h,cpp}、epmeter.cpp（仪表）
common/            ldac_io.h —— 内核/用户态共享的 IOCTL 与状态结构
third_party/       libldac（AOSP，Apache-2.0，unity build）、aosp-sbc、compat/
tests/             AVDTP 单元测试（含内核头 shim）+ 编码自检
scripts/           全部脚本（见下表）
certs/             自签名开发证书公钥
docs/              架构 / 操作 / 工具箱 / 时间线 / 调研（逆向笔记不入库，见 .gitignore）
experiments/       实验稿（纯文字）
ARCH.md            蓝屏归因史（历史）
bin-* obj-*        构建产物（gitignore）
```

## 构建与验证（不需要 IDE）

```powershell
.\build.ps1                            # 一键：驱动 + 发送端 + 编码自检（约 6 秒）
.\build.ps1 -Configuration Release
.\scripts\99-build-epmeter.ps1       # 只编 epmeter
bin-tests\Debug\ldac-enc-test.exe    # 编码自检
```

上机是危险动作：一次只改一层，先本地编译通过再推客户机。

## 常用脚本

| 脚本 | 用途 |
|---|---|
| `scripts/48-install-ldac.ps1` | 真机：stage 驱动包 + 绑到 BTHENUM A2DP 子设备 |
| `scripts/47-uninstall-ldac.ps1`、`49-cleanup-ldac.ps1` | 卸载 / 清理残留 |
| `scripts/vm-install-ldac.ps1 -Target bt -PkgDir <dir>` | 客户机版安装（`-Target uninstall` 回滚） |
| `scripts/vm-restore-inbox.ps1` | 删我们的包，退回 inbox `btha2dp`（需重启） |
| `scripts/play-music.ps1 -Vol 70` / `-Stop` | 起/停发送端（SYSTEM 计划任务，无窗口） |
| `scripts/102-vm-push-ldac.ps1` | 主机侧：把驱动包推进客户机 |

## 硬约束（踩过的坑）

- **命令用 PowerShell 7**；git 例外。
- INF 必须**纯 ASCII**；源码 UTF-8 + `/utf-8`（中文注释被 CP936 误读会吞行）。
- **内核里禁浮点/SSE**（编码器放用户态的理由之一）。
- 内核无 CRT：别用标准库，格式化与日志走 `Log.c` 的最小实现。
- 日志写 `C:\Users\Public\ldac-probe.log`（可读），别写 `Windows\Temp`；高 IRQL 只落 DbgPrint。
- **用户态 exe 必须用发布版 CRT**：`ldac-send.vcxproj` 已强制 `RuntimeLibrary=MultiThreadedDLL`。`/MDd` 编出来的 exe 在客户机上加载即失败（缺 `VCRUNTIME140D.dll`）。
- 新产生的日志/dump/临时文件放 `log/` 或干脆不落盘，别堆根目录。
- **驱动库绝不允许同时存在两个我们的包**（PnP 挑哪个不确定）。装新包前先 `pnputil /enum-drivers` 逐个 `delete-driver /uninstall /force`；`scripts/48-install-ldac.ps1` 内置清理。
- 本机 `Microsoft_Bluetooth_AvrcpTransport` 节点会让我们的 PSM 23 拨出**在本地被吞掉**：做 AVRCP 实验前先禁用它，用完 `Enable-PnpDevice` 恢复。
- 发送端 `--match` 按端点名选设备，所以 session 0（SSH / SYSTEM 任务）也能采集，不要再依赖「必须在用户会话里跑」这个旧结论。

## Git 约定

- 主分支 `main`。构建产物/日志/dump/`ref/`/`memory/` 永不提交（已配 .gitignore）。
- third_party 一律 vendor 入库（不用 submodule）。
- 提交信息用 ASCII。
