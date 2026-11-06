# 架构（依据磁盘上的微软 INF + 二进制实测，不是猜的）

## 代码架构拆解（2026-11-02，针对"整天蓝屏"）

**根因一句话：业务逻辑（编码器）跑在崩溃即整机重启的地方。** 仓库里叠着三代内核栈：

| 代 | 文件 | 状态 |
|---|---|---|
| 1 ProbeDriver | `ProbeDriver.c` + `ProbeDriver.vcxproj` | 废弃（非 PnP sc create，早期探测） |
| 2 裸 KS + KMDF | `A2dpDriver.c`、`KsAudio.c` | vcxproj 已注释停用。死因：`KsCreateFilterFactory` 往 KMDF 的 DeviceExtension 写 KS header → 0x7E |
| 3 PortCls/WDM | `Adapter.cpp`、`MiniportWaveRT.cpp`、`MiniportTopology.cpp`、`CUnknown.cpp`、`Guids.cpp`、`A2dpTransport.c`、`LdacEncoder.c`（+libldac 内核编译） | **现役**，bin-a2dp 里装的就是它 |

蓝屏风险面（现役第 3 代内部）：

- **`LdacEncoder.c`（635 行）是最大风险面**：WaveRT 缓冲推进 + float→S16 位运算 + 编码系统线程。
  IRQL 加固做过，但任何一处漏掉就是 0x7E/0x1C6，代价是整机重启而非进程重启。
- **BRB 取消路径**（`BthL2cap.c` `SubmitBrb`）：超时后 `IoCancelIrp` 再等完成回调——这里曾绿过 0x44，
  模式本身是对的，但每个新调用点都要复刻这套纪律。
- **全局单例遍地**：`g_LogFn`（曾 NULL → 日志静默黑洞）、BRB 接口 `g_If`/`g_Pdo`、编码器全局锁、
  AVDTP 的 `g_Tl`/`g_SigOff`。单设备假设，无多实例/重入防护。
- **COM miniport**：`stdunk` 的 `DECLARE_STD_UNKNOWN` 在 outer=NULL 时 0x7E（已绕过，见 `LdacPortCls.h`）。

**结论（对齐 AGENTS.md 用户态优先定调）**：第 2、1 代整代删除归档；现役第 3 代拆掉上半身
（miniport 三件套 + 内核编码器全部退役），内核只留 `BthL2cap.c` + `Avdtp.c` 信令 + 新写一个
百行级 ACL 转发（IOCTL 收用户态编码帧 → BRB 发 PSM 33）；编码/封帧/时钟/采集全部移用户态。
AVDTP 媒体包构造（`AvdtpMediaBuildPacket`）是纯字节操作，一并搬用户态。
代价：一次用户态→内核 memcpy，990 kbps 满速仅约 124 KB/s。
取舍：没有内核音频端点后，系统不会多出一个"LDAC 扬声器"设备——采集走 WASAPI loopback。

## 结论：要当"功能驱动"，不是 filter

```
BTHENUM 子设备实例  BTHENUM\{0000110b-…}_VID&000105d6_PID&000a
├─ PDO           BthEnum/BthPort 提供
├─ FDO           ← 这一层我们要占
│                  inbox 是 btha2dp.sys（KMDF + KS filter）
│                  我们的 LdacA2dp.sys 要做同样的事
└─ 上层接口       KSCATEGORY_AUDIO（MMDevAPI 里的播放设备）
```

filter 只能“陪着” btha2dp，PSM 25 和音频端点仍归它，所以 `OPEN_CHANNEL` 拿到 `handle=0`。

## 三个已确证的事实

| 事实 | 证据 |
|---|---|
| btha2dp 是 **KMDF** | `btha2dp.sys` 含 `WdfLdr` / `WdfVersionBind` |
| btha2dp 用 **KS filter**，不是 PortCls | 含 `KsCreateFilterFactory`，不含 `PcNewMiniport`/`PortCls` |
| 抢设备靠 **HardwareID 排名** | 设备 HardwareID 含 `_VID&000105d6_PID&000a`，inbox INF 只匹配 CompatibleID |

## INF 要点（见 `src/LdacA2dp.inf`）

- `Class=MEDIA`，`ClassGUID={4d36e96c-…}`
- `Include=ks.inf, wdmaudio.inf` + `Needs=KS.Registration, WDMAUDIO.Registration`
- `AddInterface=%KSCATEGORY_AUDIO%` + `EP\0` 端点属性（**不能用 MSEP\0**，那是 inbox 保留）
- `KmdfService` 必须写在 `[<DDInstall>.NT.Wdf]` 段（写在 service-install 段会被 setupapi 静默忽略）。
  **但这只是规范问题，不是蓝屏主因** —— 见下面「绿屏归因」一节的实测反驳。
- INF 必须是**纯 ASCII**。setupapi / infverif 按 ANSI 读文件，UTF-8 中文注释会吞掉物理行，
  让段头"消失"，报出 `ERROR(1203) Section [...] not found` 这种灵异错误。
  判据：`infverif /u` 应当零输出。
- 匹配 `BTHENUM\{0000110b-…}_VID&xxxxxxxx_PID&yyyy` 而不是 `BTHENUM\{0000110b-…}`

## 绿屏归因（2026-10-31 实测，推翻了先前的猜测）

### 被证伪的假设

先前认为「INF 缺 `KmdfService` → WDFLDR 不绑定 → `WdfFunctions` 为 NULL → `DriverEntry` 首次
`WdfDriverCreate` 空指针跳转 → 0x7E」。**这个链条是错的**，实测反驳：

| 驱动 | 工作正常 | 有 `Services\<svc>\Wdf` 键 | 导入 `WdfVersionBind` |
|---|---|---|---|
| `ViGEmBus.sys`（第三方 KMDF） | 是 | **无** | 是（2 处） |
| `btha2dp.sys`（inbox KMDF） | 是 | **无** | 是（2 处） |
| `WudfPf.sys`（UMDF，非 KMDF） | 是 | 无 | 否 |

→ Win10+ 的 KMDF 框架版本来自编译期固化在映像里的 `WDF_BIND_INFO`（工程属性
`<KmdfVersion>1.35</KmdfVersion>` → `WdfVersion.h` 宏），由 `WdfVersionBind` 在运行时比对
`Wdf01000.sys`。**完全不查注册表，也不需要 co-installer**（微软文档：只面向 Win10 时无需分发 WDF）。

### 崩溃现场实测（`103126-5703-01.dmp`）

```
BugCheck  0x1000007E = 0x7E SYSTEM_THREAD_EXCEPTION_NOT_HANDLED
Param1    0xFFFFFFFFC0000005   STATUS_ACCESS_VIOLATION
Param2    0xFFFFF80525281BA7   出错指令地址
Param3/4  0xFFFF88401F3A6B78 / 0xFFFF88401F3A63A0
CONTEXT   Rip=Param2  Rsp=0xFFFF88401F3A6DB0  Rcx=0x1930E203D4AEE583
```

`Rcx` 是个彻底无效的地址，配合 `and byte ptr [rcx], al` 这类写操作，即"往垃圾指针写"。

**转储是 PAGEDUMP64（`PAGE`/`DU64`）不是 MDMP**，`C:\Windows\Minidump\*.dmp` 里的文件头是
`PAGEDU64`，用 minidump 解析器会直接报 `not a minidump`。自带脚本见 `scripts/`：
`pagedump.py`（头部）、`dumpctx.py`（找 CONTEXT）、`dumpstack.py`（扫内核地址重建栈）、
`disasm.py`（PE + .pdata + capstone 反汇编定位 RVA）。

### 尚未确定的部分

- `Rip` 归属模块未定。按 64KB 对齐推 `RVA 0x1ba7` 归属 `0xFFFFF80525280000`，但该 RVA 在
  `LdacA2dp.sys`（45,592 B 旧映像）和 `btha2dp.sys` 里**都不是指令边界**，故两者都可排除。
- 转储是 triage 型（声明 32 GB 内存，文件仅 6 MB），**不含模块列表**，无法直接反查基址。
- 已确认驱动确实被加载：转储内有 `LdacA2dp` 名字串，以及 `\??\C:\Users\Public\ldac-probe.log`
  的 UTF-16 常量（来自驱动 `.rdata`）。

### 因此采取的策略：分层上机

`src/A2dpDriver.c` 顶部 `LDAC_STAGE`（0/1/2）把加载路径切成三层，一次只开一层：

- **0** = 只 `WdfDeviceCreate` + 设备接口 + 日志。不建 KS 工厂、不起 AVDTP 线程。
- **1** = + `KsAudioCreate`
- **2** = + AVDTP 探查线程（BRB / L2CAP / Discover / SetConfig / Open / Start）

STAGE 0 若仍崩 → 问题在 WDF 骨架或环境；不崩 → 问题在我们的业务代码里。二分一次即可定位。


## 要写的代码（按依赖顺序）

> **⚠️ 2026-11-02 架构定调（详见 AGENTS.md）：用户态优先。**
> 只有 1–4 留在内核；第 5/6/7 项（编码器、封帧、时钟/欠载）**全部改为用户态实现**，
> 内核↔用户态允许额外一次内存拷贝。参考 `ref/alt-a2dp`（同为薄驱动 + 用户态服务分工）。

1. **KMDF 宿主**：`DriverEntry` → `WdfDriverCreate`（PnP）→ `EvtDeviceAdd` → `WdfDeviceCreate` ✅
2. **KS filter 工厂**：`KsCreateFilterFactory`，注册 render pin ✅
3. **BRB 层**（`BthL2cap.c`）：QI → `BRB_L2CA_OPEN_CHANNEL` → `BRB_L2CA_ACL_TRANSFER` 收发 ✅
4. **AVDTP 状态机**（`Avdtp.c`）：Discover / GetCapabilities / SetConfiguration / Open / Start ✅（待上机验证）
5. **LDAC 编码器**：libldac（AOSP，Apache-2.0），990/660/330 kbps ⬜ **→ 改用户态**
6. **媒体包封帧**：12B RTP 头 + 1B MP 头 + N×LDAC frame（128 sample/ch） ⬜ **→ 改用户态**
7. **时钟/欠载处理** ⬜ **→ 改用户态**

## 当前状态（2026-10-31）

- `bin-a2dp\x64\Debug\LdacA2dp\` 是**完整包**（inf + sys + cat 必须同目录；根目录那份缺 cat，
  拿根目录的 inf 去 pnputil 会静默失败）。
- 测试签名证书 `WDKTestCert erica` 已进 `LocalMachine\Root` + `TrustedPublisher`，
  指纹 `D59EFE33…F723E`，与 .sys 的签名者一致；`testsigning = Yes`。
- 只差管理员跑一次 `go-live.cmd`（自提权）+ 重启一次。
- 日志改写到 `C:\Users\Public\ldac-probe.log`：原来写 `\SystemRoot\Temp`（= Windows\Temp）
  ACL 只给管理员，非提权读不了，每轮迭代都要提权很痛苦。

### AVDTP 两处旧实现是错的（已改）

| 错法 | 正确 |
|---|---|
| 单包信令里塞了 2 字节长度字段 | AVDTP 信令单元是 `[头1B][信号ID1B][载荷]`，**没有长度字段**，长度由 L2CAP 交付长度决定 |
| 信号 ID 直接写死（Discover=0x00 / GetCaps=0x02 / SetConfig=0x03 / Open=0x06 / Start=0x07） | 网上有两套编号（Discover=0x00 或 0x01）。改为运行时 `AvdtpCalibrate()` 两种都试、把能用的偏移记住 |
| Media Codec 首字节 `mt=v[0]&0x0F / ct=v[0]>>4` | 高 4 位是 Media Type，低 4 位是 Codec Type |
| Vendor ID 当 1 字节 Codec ID 解析 | Vendor ID 4 octets + Codec ID **2 octets**，codec specific 在其后；且 Vendor ID 字节序各家不同，用**字节模式搜索** `00 00 01 2D` / `2D 01 00 00` 两种都试 |

`SetConfiguration` 的策略是**回放设备自己上报的能力元素**（只保留 Media Transport + Media Codec，
把 codec specific 的速率/声道改成单比特），这样即使我对 LDAC 参数布局的假设有偏差也不会构造出畸形报文。


## 已踩过的坑（别再犯）

| 坑 | 后果 |
|---|---|
| 手搓链接 KMDF 却缺 INF 的 `KmdfService` | `WdfFunctions` 为 NULL → 第一次派发 0x7E |
| 从外部打开设备接口再 QI BRB | radio FDO 返 `0xC00000BB`；降到 PDO 再 QI → 0x7E |
| 提交 BRB 只看 IRP 状态 | 真实结果在 `BRB->BrbHeader.Status` |
| 超时路径 `IoCancelIrp` + `IoFreeIrp` | bthport 之后又完成一次 → 0x44 双完成。**取消后必须等完成回调再释放** |
| 每次改一行就换镜像重启 | 用户一天重启 4 次。**先在本地编译+静态检查，攒够再上机** |

## 构建链已打通（VS 2026 BuildTools 18.11 + WDK 28000）

产物：`bin-a2dp\x64\Debug\` → `LdacA2dp.sys`（29KB）+ `.cat` + `.inf`，测试签名 + Signability 全过。
导入表实测：`ks.sys` + `WDFLDR.SYS` —— 确认是「KMDF 宿主 + KS filter 工厂」，与 btha2dp 同构。

### 构建侧的坑（都是硬坑，记下来别再撞）

| 坑 | 症状 | 解法 |
|---|---|---|
| 进程环境块里有 `Path` / `PATH` / `path` 三条 | `MSB6001 CL.exe 命令行开关无效`，堆栈落在 `ProcessStartInfo.get_EnvironmentVariables()` | .NET Framework 把它逐条 `Add` 进**大小写不敏感**字典 → 必炸。`SetEnvironmentVariable` 在 PowerShell 里删不掉（`$env:Path` 变空但块里仍是三条）。**只能 `Start-Process -UseNewEnvironment`**（子进程环境从注册表重建），见 `build-dk.ps1` |
| 源码 UTF-8 无 BOM + 中文注释 | `// 注释` 被 CP936 误读成以 `\` 结尾 → 续行吞掉下一行声明，报莫名的 `C2061/C2065 g_LogFn 未声明` | `ClCompile` 加 `/utf-8` |
| 内核里直接 `#include <ks.h>` | `ks.h(6143) BOOL` 未定义 → `ksmedia.h` 连报上百个 `C3646 未知重写说明符` | 照抄 `km\portcls.h` 的顺序：`<windef.h>` → `#define NOBITMAP` `<mmreg.h>` `<undef>` → `<ks.h>` → `<ksmedia.h>`。`BOOL/WINAPI/FLOAT/WAVEFORMATEX` 全在这两个头里 |
| 凭印象写 KS 结构体 | `C2440 int/size_t → 指针`、`C2078 初始值设定项太多` | 顺序必须照 `ks.h` 原文：`KSFILTER_DESCRIPTOR` 的 `Version` 在**第 3 位**（前面还有 `Dispatch`/`AutomationTable`，后面紧跟 `Flags`/`ReferenceGuid`）；`KSPIN_DESCRIPTOR_EX` 尾部还有 `AllocatorFraming`/`IntersectHandler`；`KSPIN_DISPATCH` 只有 **10** 个成员（无 `Cleanup`）；`PFNKSPINSETDATAFORMAT` 是 **5** 参数；`KsCreateFilterFactory` 是 **8** 参数 |
| C 文件与 C++ 文件混链 | `LNK2019 KsAudioCreate`、`LNK2019 ProbeLog`（提示里能看到 `?KsAudioCreate@@YAJ…` 修饰名） | `KsAudio.c` 必须按 C++ 编（ks.h 的 GUID 要 C++ 语义），所以跨语言的头里加 `extern "C"` |
| 漏 `ks.lib` | `__imp_KsCreateFilterFactory` 等 5 个全未解析 | vcxproj `<Link><AdditionalDependencies>ks.lib` |

