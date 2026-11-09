// ============================================================================
//  Driver.c — LdacA2dp 驱动入口 + 控制设备 + AVDTP/L2CAP 编排（WDM）
// ============================================================================
//  【2026-11-02 改造】从 KMDF 换成 WDM/PortCls。
//
//  为什么换：顶替 btha2dp.sys 之后必须自己给耳机建音频端点，否则设备管理器
//  里一片黄叹号、端点消失、"配对成功"永远等不到 —— 这是之前一路踩坑的病根。
//  端点由 PortCls 建（toy/VirtualSpeaker 已验证的骨架），BT 协商仍用原来的
//  Avdtp.c / BthL2cap.c（它们本来就只依赖 ntddk，不依赖 KMDF）。
//
//  职责边界（见 AGENTS.md）：
//    内核只做"和蓝牙协商 LDAC" + "把 PCM 交出去"：
//      1. 作为 BTHENUM A2DP 子设备的功能驱动（替掉 inbox btha2dp.sys）
//      2. 建 PortCls 端点（WaveRT render + Topology），见 KsAdapter.cpp
//      3. 控制设备 \\.\LdacCtl：AVDTP 信令、L2CAP 媒体转发、PCM 取出
//    编码/封帧/时钟全在用户态（host/）。
//
//  ⚠ IRP 派发表是**按驱动**的，不是按设备的。控制设备和 PortCls 的音频设备
//    共用同一张表，所以必须"保存原处理函数 + 按目标设备分流"（见文件末尾）。
// ============================================================================

#include <ntddk.h>
#include <initguid.h>
#include <portcls.h>
#include <wdmsec.h>      // IoCreateDeviceSecure：控制设备的 DACL 由我们自己定

#include "Log.h"
#include "BthL2cap.h"
#include "Avdtp.h"
#include "ldac_io.h"
#include "LdacBridge.h"
#include "Raii.h"

//  PortCls 动态端点：在位/不在位（定义在 KsAdapter.cpp，按 C++ 的 extern "C" 导出）。
//  这是本文件里唯一的 C++ 侧导入，所以就地声明，不拉 LdacKs.h（那是 C++ 头）。
extern "C" VOID LdacKsSetLinkPresent(_In_ BOOLEAN Present);

#define DEFAULT_BD_ADDR_STR "33:DB:8A:C7:E6:7A"
#define INT_SEID            1          // 本机源端点 SEID（惯例值）
#define OPEN_TIMEOUT_MS     15000

// --------------------------------------------------------- 构建标记 ---
//  热重载最容易踩的坑：磁盘上的 .sys 明明换了（哈希一致），日志格式却还是旧的
//  —— 因为内存里跑的仍是上一次加载的映像。所以让映像自己在 DriverEntry 第一行
//  自报编译时间，一眼就能判断"跑的到底是哪一版"，不用再靠 driverquery 的
//  Code(bytes) 去猜。
#define LDAC_BUILD_TAG      __DATE__ " " __TIME__

// ------------------------------------------------------------- 全局状态 ---
static PDEVICE_OBJECT g_CtlDevice = NULL;   // 控制设备（IOCTL 入口）
static BOOLEAN        g_CtlSymLink = FALSE; // \DosDevices\LdacCtl 是否已建（卸载时要删）
//  0 = LDAC，1 = SBC，2 = AAC。运行时由 IOCTL_LDAC_SET_CODEC 改，
//  注册表 Parameters\Codec（DWORD）作为开机默认。只影响下一次会话建立。
static ULONG          g_Codec = LDAC_CODEC_LDAC;
//  ⚠ 这三个是跨模块共享的全局（BthL2cap.cpp 以 extern "C" 引用），
//    所以定义也必须落在 extern "C" 里 —— 否则链接名对不上。
extern "C" {
BTH_L2CAP_CHAN g_SigChan;                   // AVDTP 信令通道（BthL2cap 断开时要清）
BTH_L2CAP_CHAN g_MediaChan;                 // AVDTP 媒体通道（同上）
LONG                  g_State = LDAC_STATE_IDLE;   // BthL2cap 断开时要复位
}
static LONG           g_LastErr = 0;
static ULONG          g_SigMtu = 0;
static ULONG          g_MediaMtu = 0;
static ULONG          g_RateBit = 0;
static ULONG          g_ModeBit = 0;
static ULONG          g_SinkSeid = 0;

//  累计发出的包数/字节数（v2 STATUS 字段）。两条发送路径共用：
//   - IOCTL_LDAC_SEND      （内核自己的 g_MediaChan）
//   - IOCTL_LDAC_L2CAP_SEND（用户态 AVDTP 的媒体槽位）
static volatile LONG  g_SentPackets = 0;
static volatile LONG  g_SentBytes = 0;

//  目标地址（BthBoot 解析出来后保存）。用户态走 L2CAP 原语时，
//  内核需要它来"开 PSM 25 前摘掉自己的 server、开完挂回去"。
static BTH_ADDR       g_TargetAddr = 0;

// PortCls 原来的派发函数（DriverEntry 里保存）
static PDRIVER_DISPATCH g_OrigCreate        = NULL;
static PDRIVER_DISPATCH g_OrigClose         = NULL;
static PDRIVER_DISPATCH g_OrigCleanup       = NULL;
static PDRIVER_DISPATCH g_OrigDeviceControl = NULL;

// ------------------------------------------------------- 卸映像（开发期）---
//  【为什么必须自己拆】IoCreateDevice 出来的设备对象会给驱动对象**加一次引用**。
//  控制设备是 DriverEntry 建的、永不删除，于是：
//      remove-device（唯一那个 PnP 设备没了）-> 引用计数还剩 1
//      -> I/O 管理器不调 DriverUnload、映像不卸载
//      -> PnP 重新枚举时 IoLoadDriver 发现"映像已在内存"，直接复用旧代码
//  表现就是：包换成新的、设备重枚举成功、ver 也变了，跑的还是旧驱动
//  （日志里连新加的打印都不出现）。所以留一个 IOCTL 主动把控制设备删掉，
//  引用计数归零后映像才真的卸载，下一次枚举才会从磁盘重新加载。
static VOID
LdacSelfDestructThread(_In_ PVOID Context);

// ------------------------------------------------------------ 小工具函数 ---
static UCHAR PickRate(_In_ UCHAR Rates, _In_ UCHAR Pref)
{
    if (Pref != 0 && (Rates & Pref)) return Pref;
    //  第一版只求"响"：默认 48k（Windows 混音常态），其次 44.1k。
    //  96k/88.2k 需要用户态重采样，等链路稳定后再开。
    if (Rates & LDAC_RATE_48000) return LDAC_RATE_48000;
    if (Rates & LDAC_RATE_44100) return LDAC_RATE_44100;
    if (Rates & LDAC_RATE_96000) return LDAC_RATE_96000;
    if (Rates & LDAC_RATE_88200) return LDAC_RATE_88200;
    return 0;
}

static UCHAR PickMode(_In_ UCHAR Modes)
{
    if (Modes & LDAC_CH_STEREO) return LDAC_CH_STEREO;
    if (Modes & LDAC_CH_DUAL)   return LDAC_CH_DUAL;
    if (Modes & LDAC_CH_MONO)   return LDAC_CH_MONO;
    return 0;
}

//  AAC 的 codec specific 是 8 字节（含 media type / codec type 两个字节）。
//  这里只负责"把能力集拆成字段"，不猜配置；选哪一档由调用方决定。
static BOOLEAN
ParseAacCaps(_In_ const PAVDTP_SEP Sep,
             _Out_ UCHAR* ObjType, _Out_ UCHAR* SfLow, _Out_ UCHAR* SfHigh,
             _Out_ UCHAR* ChMask, _Out_ BOOLEAN* Vbr, _Out_ ULONG* BitRate)
{
    ULONG i = 0;

    while (i + 1 < Sep->CapLen) {
        UCHAR        b0   = Sep->Caps[i];
        UCHAR        cat  = (UCHAR)(b0 & 0x3F);
        UCHAR        losc = (UCHAR)((b0 >> 6) & 0x03);
        ULONG        hdr  = (losc != 0) ? 1 : 2;
        ULONG        cln  = (losc != 0) ? losc : Sep->Caps[i + 1];
        const UCHAR* v;

        if (i + hdr + cln > Sep->CapLen) break;
        v = Sep->Caps + i + hdr;

        if (cat == AVDTP_CAT_MEDIA_CODEC && cln >= 8 && v[0] == 0x00 && v[1] == 0x02) {
            *ObjType = v[2];
            *SfLow   = v[3];
            *SfHigh  = v[4];
            *ChMask  = (UCHAR)(v[4] & 0x0C);
            *Vbr     = (BOOLEAN)((v[5] & 0x80) != 0);
            *BitRate = (((ULONG)(v[5] & 0x7F)) << 16) | ((ULONG)v[6] << 8) | v[7];
            return TRUE;
        }
        i += hdr + cln;
    }
    return FALSE;
}

static VOID SetState(_In_ LONG State, _In_ NTSTATUS Err)
{
    InterlockedExchange(&g_State, State);
    InterlockedExchange(&g_LastErr, (LONG)Err);

    //  ⚠ 这里**不能**再动 LdacKsSetLinkPresent。
    //  "链路在位"的含义是蓝牙设备连着，不是"正在放音"。
    //  2026-11-09 实测：原来写成 State==STREAMING，于是每次会话建立/停止都会
    //  把端点拆掉再装回来（日志里 ioctl START 后紧跟着 link: down），
    //  空闲一会儿音频设备就消失了 —— 表现就是"开机没声音，开关一次蓝牙才好"。
    //  端点生命周期只由 AddDevice / PnP remove 决定。
}

static VOID FillStatus(_Out_ PLDAC_STATUS S)
{
    RtlZeroMemory(S, sizeof(*S));
    S->Version   = LDAC_STATUS_VERSION;
    S->State     = g_State;
    S->LastError = g_LastErr;
    S->SigMtu    = g_SigMtu;
    S->MediaMtu  = g_MediaMtu;
    S->RateBit   = g_RateBit;
    S->ModeBit   = g_ModeBit;
    S->SinkSeid  = g_SinkSeid;
    S->Packets   = (ULONG)InterlockedCompareExchange(&g_SentPackets, 0, 0);
    S->Codec     = g_Codec;
    S->Bytes     = (ULONG)InterlockedCompareExchange(&g_SentBytes, 0, 0);
}

//  目标地址：注册表 Parameters\TargetBdAddr（REG_SZ，如 "33:DB:8A:C7:E6:7A"）。
//  原来是 WDF 版的 WdfRegistryOpenParametersRegistryKey，WDM 换回 Zw*。
static BOOLEAN
ReadTargetAddrFromRegistry(_Out_ CHAR Narrow[18])
{
    UNICODE_STRING  keyName, valName;
    OBJECT_ATTRIBUTES oa;
    HANDLE          hKey = NULL;
    NTSTATUS        st;
    UCHAR           buf[128];
    PKEY_VALUE_PARTIAL_INFORMATION pv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG           len = 0, i, chars;
    BOOLEAN         ok = FALSE;

    Narrow[0] = '\0';

    RtlInitUnicodeString(&keyName,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\LdacA2dp\\Parameters");
    InitializeObjectAttributes(&oa, &keyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    st = ZwOpenKey(&hKey, KEY_READ, &oa);
    if (!NT_SUCCESS(st)) {
        return FALSE;
    }

    RtlInitUnicodeString(&valName, L"TargetBdAddr");
    st = ZwQueryValueKey(hKey, &valName, KeyValuePartialInformation,
                         pv, sizeof(buf), &len);
    if (NT_SUCCESS(st) && pv->Type == REG_SZ && pv->DataLength >= 17 * sizeof(WCHAR)) {
        PWCHAR w = (PWCHAR)pv->Data;
        chars = pv->DataLength / sizeof(WCHAR);
        if (chars > 17) chars = 17;
        for (i = 0; i < chars; i++) {
            Narrow[i] = (CHAR)w[i];
        }
        Narrow[chars] = '\0';
        ok = TRUE;
    }
    ZwClose(hKey);
    return ok;
}

//  读 HKLM\SOFTWARE\LdacPanel 下的 DWORD。用于"用户态接管"这类全局开关，
//  和驱动 Parameters 里的调试参数分开存放（面板改的是这个键）。
static BOOLEAN
ReadDwordFromSoftwareKey(_In_ PCWSTR ValueName, _Out_ PULONG Out)
{
    UNICODE_STRING  keyName, valName;
    OBJECT_ATTRIBUTES oa;
    HANDLE          hKey = NULL;
    UCHAR           buf[128];
    PKEY_VALUE_PARTIAL_INFORMATION pv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG           len = 0;
    NTSTATUS        st;

    *Out = 0;
    RtlInitUnicodeString(&keyName, L"\\Registry\\Machine\\SOFTWARE\\LdacPanel");
    InitializeObjectAttributes(&oa, &keyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&hKey, KEY_READ, &oa))) return FALSE;

    RtlInitUnicodeString(&valName, ValueName);
    st = ZwQueryValueKey(hKey, &valName, KeyValuePartialInformation, pv, sizeof(buf), &len);
    ZwClose(hKey);
    if (!NT_SUCCESS(st) || pv->Type != REG_DWORD || pv->DataLength < sizeof(ULONG)) return FALSE;

    *Out = *(PULONG)pv->Data;
    return TRUE;
}

// ------------------------------------------------------------- 建流流程 ---
//  注册表里读一个 DWORD（读不到就返回 FALSE，调用方保留默认值）。
//  目前只用来读 Parameters\ChannelFlags，让「换 L2CAP 打开参数」不用重编驱动。
static BOOLEAN
ReadDwordFromRegistry(_In_ PCWSTR ValueName, _Out_ PULONG Out)
{
    UNICODE_STRING  keyName, valName;
    OBJECT_ATTRIBUTES oa;
    HANDLE          hKey = NULL;
    UCHAR           buf[128];
    PKEY_VALUE_PARTIAL_INFORMATION pv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    ULONG           len = 0;
    NTSTATUS        st;

    *Out = 0;
    RtlInitUnicodeString(&keyName,
        L"\\Registry\\Machine\\System\\CurrentControlSet\\Services\\LdacA2dp\\Parameters");
    InitializeObjectAttributes(&oa, &keyName,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&hKey, KEY_READ, &oa))) return FALSE;

    RtlInitUnicodeString(&valName, ValueName);
    st = ZwQueryValueKey(hKey, &valName, KeyValuePartialInformation, pv, sizeof(buf), &len);
    ZwClose(hKey);
    if (!NT_SUCCESS(st) || pv->Type != REG_DWORD || pv->DataLength < sizeof(ULONG)) return FALSE;

    *Out = *(PULONG)pv->Data;
    return TRUE;
}

static NTSTATUS
LdacSetup(_In_ BTH_ADDR Addr, _In_ UCHAR PrefRate)
{
    AVDTP_SEP  seps[AVDTP_MAX_SEPS];
    PAVDTP_SEP ldac = NULL;
    ULONG      count = 0;
    ULONG      i;
    UCHAR      rate, mode;
    BTH_ADDR   local = 0;
    NTSTATUS   st;

    if (g_LdacBthPdo == NULL) {
        ProbeLog("setup: no BTHENUM PDO yet");
        st = STATUS_NO_SUCH_DEVICE;
        goto fail;      // ⚠ 必须走 fail：否则状态机永远卡在 STARTING，
                        //   下一次 START 会一直返回 STATUS_DEVICE_BUSY。
    }

    //  ⚠ 绝不在 IOCTL 上下文里再调 BthL2capAttach(g_LdacBthPdo)。
    //  那个全局 PDO 只在 AddDevice 那一刻保证有效；设备被移除后它就是
    //  悬空指针，而这里过去拿它发 IRP_MN_QUERY_INTERFACE —— verifier 抓到的
    //  正是 0xC9「Invalid Device object passed to IoCallDriver」
    //  （栈：IoCallDriver <- BthL2capAttach+0xdd <- IOCTL），以及 0x35
    //  （StackSize 读自已释放的内存 = 0 -> 分配的 IRP 一个栈位置都没有）。
    //  接口只由 AddDevice -> LdacBthBoot 建立，这里只检查就绪状态。
    if (!BthL2capIsAttached()) {
        ProbeLog("setup: BT interface not attached (device gone or still coming up)");
        st = STATUS_DEVICE_NOT_READY;
        goto fail;
    }

    //  L2CAP 打开参数支持注册表覆盖：改完直接再跑一次 START 就生效，
    //  不用重编也不用重装（通道是每次 START 现开的）。
    {
        ULONG cf = 0;
        if (ReadDwordFromRegistry(L"ChannelFlags", &cf)) {
            g_L2capChannelFlags = cf;
            ProbeLog("setup: ChannelFlags <- registry 0x%X", cf);
        } else {
            ProbeLog("setup: ChannelFlags default 0x%X", g_L2capChannelFlags);
        }
        if (ReadDwordFromRegistry(L"RejectInbound", &cf)) {
            g_L2capRejectInbound = cf;
            ProbeLog("setup: RejectInbound <- registry %u", cf);
        }
        //  兼容老脚本：ForceSbc=1 等价于 Codec=1；Codec 优先。
        if (ReadDwordFromRegistry(L"ForceSbc", &cf)) {
            g_Codec = (cf != 0) ? LDAC_CODEC_SBC : LDAC_CODEC_LDAC;
            ProbeLog("setup: ForceSbc <- registry %u (Codec=%u)", cf, g_Codec);
        }
        if (ReadDwordFromRegistry(L"Codec", &cf)) {
            g_Codec = (cf > LDAC_CODEC_AAC) ? LDAC_CODEC_LDAC : cf;
            ProbeLog("setup: Codec <- registry %u", g_Codec);
        }
        //  逐包日志默认关：流媒体时每个 BRB 两条日志 = 每秒几百次内核文件写，
        //  而这个驱动是从 System 进程的内核线程里写的（见 KsAdapter 那段注释）。
        //  排查时才开：reg add ... /v VerboseLog /t REG_DWORD /d 1
        {
            ULONG vlog = 0;
            if (ReadDwordFromRegistry(L"VerboseLog", &vlog)) {
                g_VerboseLog = (vlog != 0);
            }
            ProbeLog("setup: VerboseLog %s", g_VerboseLog ? "ON" : "off");
        }
    }

    // 自检：BRB 提交路径通不通，看本机地址有没有值
    st = BthL2capGetLocalAddress(&local);
    if (NT_SUCCESS(st) && local != 0) {
        ProbeLog("self-test OK, local bd addr = 0x%X%08X",
                 (ULONG)(local >> 32), (ULONG)(local & 0xFFFFFFFF));
    } else {
        ProbeLog("self-test FAILED (st=0x%08X)", st);
    }

    //  ⚠ 这里原来有两发 BRB_L2CA_PING（L2CAP Echo），用来把"传输层不通"和
    //  "AVDTP 语义不对"切开。2026-11-03 实测：这个栈上 PING 永远不完成
    //  （bt 一直是 0xFF 哨兵、6s 超时），而同一时刻 AVDTP 数据收发完全正常
    //  —— 也就是说它在 bthport 上根本走不通，只会每次白等 6 秒 ×2。
    //  判据换成对数：只要收到过对端的 AVDTP 报文，传输层就是通的；
    //  真要看链路死活，看 indication 的 drops 计数更直接。

    // 上一次会话保留的信令通道：先清掉，免得 open 撞 "channel already exists"
    if (g_SigChan.Open) {
        BthL2capClose(&g_SigChan);
        ProbeLog("setup: closed kept signaling channel");
    }
    // 信令通道：优先接管开机时建立的常驻通道（设备此时已连上），
    // 否则再走"设备拨入 -> 8s 等待 -> 自己拨出"的老路。
    RtlZeroMemory(&g_SigChan, sizeof(g_SigChan));
    //  常驻通道机制已删除（内核不再自己拨号保活），一律现场建链。
    st = BthL2capAcquireSignaling(Addr, &g_SigChan);
    if (!NT_SUCCESS(st)) {
        ProbeLog("setup: acquire signaling failed 0x%08X", st);
        goto fail;
    }
    g_SigMtu = g_SigChan.Mtu;
    ProbeLog("setup: signaling up, handle=0x%X mtu=%u inbound=%u",
             (ULONG)(ULONG_PTR)g_SigChan.Handle, g_SigChan.Mtu,
             (ULONG)BthL2capIsInbound(g_SigChan.Handle));

    //  对端拨入说明它临时是 INT（sink 在"探源"）。ACP 通道上不能由我们发命令，
    //  所以只在这里串行应答它的一条 Discover，然后重开通道自己当 INT。
    //  绝不能在应答的同时另发命令——同通道并发提交 BRB 会把系统打复位。
    if (BthL2capIsInbound(g_SigChan.Handle)) {
        AVDTP_ACP_RESULT res;

        ProbeLog("setup: inbound channel -> ACP mode (peer is INT)");
        st = AvdtpServeAcp(&g_SigChan, &res);
        if (res.Started) {
            ProbeLog("setup: ===== ACP NEGOTIATED ldac=%u rate=0x%02X mode=0x%02X seid=%u =====",
                     (ULONG)res.IsLdac, (ULONG)res.RateBit, (ULONG)res.ModeBit,
                     (ULONG)res.IntSeid);
            g_RateBit  = res.RateBit;
            g_ModeBit  = res.ModeBit;
            g_SinkSeid = res.IntSeid ? res.IntSeid : 1;
            g_MediaMtu = 0;
            SetState(LDAC_STATE_STREAMING, STATUS_SUCCESS);
            return STATUS_SUCCESS;
        }
        //  注意这里不看 st 是否成功：ACP 循环正常收尾就是「等不到下一条命令」
        //  而超时返回（0xC00000B5），那不代表失败。只要探测被应答过
        //  （res.Discovered），就说明耳机认了我们是 A2DP source，接下来该由
        //  我们主动把协商做完。
        if (!res.Discovered) {
            //  对端拨了通道但一句话不说（UGREEN-BT505 实测就是这个行为）。
            //  它只是先把 L2CAP 建好，等 source 先发 Discover —— L2CAP 由谁建
            //  和 AVDTP 谁是 INT 是两回事。这里不再直接失败，就地复用这条
            //  inbound 通道自己当 INT 往下走（下面统一走 Discover/GetCaps）。
            ProbeLog("setup: ACP silent (st=0x%08X) -> Discover on inbound channel as INT", st);
            ProbeLog("setup: ACP mode ended st=0x%08X disc=%u cfg=%u open=%u start=%u",
                     st, res.Discovered, res.Configured, res.Opened, res.Started);
        } else {

        //  抓包实证（cold2.pcap）：BthA2dp 的传输通道是**设备自己拨进来的第二条
        //  PSM 25**。设备会继续在它自己拨的那条 ACL 上拨，所以这里**不关也不重开**，
        //  直接留着这条通道当 INT 用（应答 ACP 之后同一条通道上发命令是允许的）。
        ProbeLog("setup: keeping sink-dialed channel as initiator (mtu=%u)", g_SigChan.Mtu);
        }
    }

    // 协商：Discover → GetCaps → 找 Sony LDAC SEP
    st = AvdtpDiscover(&g_SigChan, seps, AVDTP_MAX_SEPS, &count);
    if (!NT_SUCCESS(st)) {
        //  这里不再打 Echo（那个 BRB 在这个栈上根本完不成），改用 drops 计数：
        //  drops 涨了 = 对端把通道拆了；没涨 = 通道还在，就是它不回。
        ProbeLog("setup: discover failed 0x%08X (drops=%d)", st, (LONG)g_L2capDrops);
        goto fail;
    }
    ProbeLog("setup: %u SEP(s), drops=%d", count, (LONG)g_L2capDrops);

    for (i = 0; i < count; i++) {
        //  SEP info 第二字节的 bit3 才是 TSEP（1=SNK）；之前用 &0x03 取，
        //  0x08 永远算成 SRC，日志里因此一直显示"全是 source"。
        st = AvdtpGetCapabilities(&g_SigChan, &seps[i]);
        if (!NT_SUCCESS(st)) {
            ProbeLog("setup: getcaps seid=%u failed 0x%08X", seps[i].Seid, st);
            continue;
        }
        //  把每个 SEP 的 codec 身份全打出来：一眼看出 sink 到底支持什么
        //  （SBC = media 0/codec 0；AAC = media 0/codec 2；LDAC = vendor 0x012D）。
        ProbeLog("setup: sep seid=%u media=%u codecType=%u vendor=0x%06X codecId=0x%04X hasCaps=%u",
                 (ULONG)seps[i].Seid, (ULONG)seps[i].MediaType, (ULONG)seps[i].CodecType,
                 seps[i].VendorId, (ULONG)seps[i].CodecId, (ULONG)seps[i].HasCodec);
        //  原始能力字节：写 SetConfiguration 时必须逐字节对齐（尤其 AAC 的 bitmask）。
        {
            ULONG c;
            CHAR  hex[80];
            ULONG n = 0, k;
            ULONG lim = seps[i].CapLen > 24 ? 24 : seps[i].CapLen;
            static const CHAR kHex[] = "0123456789ABCDEF";
            for (k = 0; k < lim && n + 3 < sizeof(hex); k++) {
                c = seps[i].Caps[k];
                hex[n++] = kHex[(c >> 4) & 0xF];
                hex[n++] = kHex[c & 0xF];
                hex[n++] = ' ';
            }
            hex[n] = 0;
            ProbeLog("setup:   caps(%u) %s", seps[i].CapLen, hex);
        }

        if (seps[i].HasCodec &&
            seps[i].VendorId == AVDTP_SONY_VENDOR_ID &&
            seps[i].CodecId == AVDTP_SONY_CODEC_LDAC &&
            ldac == NULL) {
            ldac = &seps[i];
        }
    }

    if (g_Codec == LDAC_CODEC_SBC) {
        //  对照实验：挑 SBC SEP（Media Codec = audio，codec type 0，非 vendor），
        //  用固定参数 48k / joint stereo / 16 blocks / 8 subbands / bitpool 2..38
        //  发 SetConfiguration。用户态必须用 --codec sbc 编码，否则帧对不上。
        PAVDTP_SEP sbcSep = NULL;
        for (i = 0; i < count; i++) {
            if (seps[i].HasCodec && seps[i].MediaType == 0 &&
                seps[i].CodecType == 0 && seps[i].VendorId == 0) {
                sbcSep = &seps[i];
                break;
            }
        }
        if (sbcSep == NULL) {
            ProbeLog("setup: no SBC SEP on this sink");
            st = STATUS_NOT_SUPPORTED;
            goto fail;
        }
        rate = LDAC_RATE_48000;          // 告诉用户态按 48k 编码
        mode = LDAC_CH_STEREO;
        ProbeLog("setup: SBC SEP seid=%u (force-sbc 对照实验)", sbcSep->Seid);
        st = AvdtpSetSbcConfiguration(&g_SigChan, sbcSep->Seid, INT_SEID,
                                      3 /*48k*/, 3 /*joint*/, 2, 38);
        if (!NT_SUCCESS(st)) {
            ProbeLog("setup: SET_CONFIGURATION(SBC) failed 0x%08X", st);
            goto fail;
        }
        ldac = sbcSep;                   // 后面的 Open/Start 复用这个 SEID
    } else if (g_Codec == LDAC_CODEC_AAC) {
        //  AAC：挑 Media Codec = audio / codec type 2 / 非 vendor 的 SEP。
        //  能力集是位掩码，配置必须选成单比特（spec 要求），否则有些 sink 直接回
        //  BAD_MEDIA_CODEC_... 。实测这台 UGREEN 上报：
        //    80 01 8C 82 00 00 = MPEG-2 LC / 44.1k+48k / mono+stereo / VBR / 128kbps
        PAVDTP_SEP aacSep = NULL;
        UCHAR      objType = 0, sfLo = 0, sfHi = 0, chMask = 0;
        BOOLEAN    vbr = FALSE;
        ULONG      bitRate = 0;

        for (i = 0; i < count; i++) {
            if (seps[i].HasCodec && seps[i].MediaType == 0 &&
                seps[i].CodecType == 2 && seps[i].VendorId == 0) {
                aacSep = &seps[i];
                break;
            }
        }
        if (aacSep == NULL) {
            ProbeLog("setup: no AAC SEP on this sink");
            st = STATUS_NOT_SUPPORTED;
            goto fail;
        }
        if (!ParseAacCaps(aacSep, &objType, &sfLo, &sfHi, &chMask, &vbr, &bitRate)) {
            ProbeLog("setup: AAC caps unparsable (seid=%u)", aacSep->Seid);
            st = STATUS_NOT_SUPPORTED;
            goto fail;
        }
        ProbeLog("setup: AAC SEP seid=%u obj=0x%02X sf=%02X/%02X ch=0x%02X vbr=%u br=%u",
                 aacSep->Seid, objType, sfLo, sfHi, chMask, (ULONG)vbr, bitRate);

        //  优先级：先满足用户态报上来的混音率（PrefRate），否则 48k > 44.1k。
        //  这条不是"偏好"而是硬约束：发送端不做重采样，协商率和混音率必须相等。
        if (PrefRate == LDAC_RATE_44100 && (sfLo & 0x01)) {
            sfLo = 0x01; sfHi = 0x00; rate = LDAC_RATE_44100;
        } else if (PrefRate == LDAC_RATE_48000 && (sfHi & 0x80)) {
            sfLo = 0x00; sfHi = 0x80; rate = LDAC_RATE_48000;
        } else if (sfHi & 0x80) {             // 48000
            sfLo = 0x00; sfHi = 0x80;
            rate = LDAC_RATE_48000;
        } else if (sfLo & 0x01) {             // 44100
            sfLo = 0x01; sfHi = 0x00;
            rate = LDAC_RATE_44100;
        } else {
            ProbeLog("setup: AAC has neither 48k nor 44.1k (sf=%02X/%02X)", sfLo, sfHi);
            st = STATUS_NOT_SUPPORTED;
            goto fail;
        }

        if (chMask & 0x04) { mode = LDAC_CH_STEREO; chMask = 0x04; }
        else if (chMask & 0x08) { mode = LDAC_CH_MONO; chMask = 0x08; }
        else {
            ProbeLog("setup: AAC channel mask unusable (0x%02X)", chMask);
            st = STATUS_NOT_SUPPORTED;
            goto fail;
        }

        //  对象类型也只留一个比特：优先 MPEG-4 LC(0x40)，其次 MPEG-2 LC(0x80)。
        if (objType & 0x40)      objType = 0x40;
        else if (objType & 0x80) objType = 0x80;
        else {
            ProbeLog("setup: AAC object type unusable (0x%02X)", objType);
            st = STATUS_NOT_SUPPORTED;
            goto fail;
        }

        vbr = FALSE;                          // 发送端是 CBR
        if (bitRate == 0) bitRate = 0x020000; // 兜底 128 kbps

        st = AvdtpSetAacConfiguration(&g_SigChan, aacSep->Seid, INT_SEID,
                                      objType, sfLo, sfHi, chMask, vbr, bitRate);
        if (!NT_SUCCESS(st)) {
            ProbeLog("setup: SET_CONFIGURATION(AAC) failed 0x%08X", st);
            goto fail;
        }
        ldac = aacSep;
    } else {

    if (ldac == NULL) {
        ProbeLog("setup: no Sony LDAC SEP on this sink");
        st = STATUS_NOT_SUPPORTED;
        goto fail;
    }

    rate = PickRate(ldac->Rates, PrefRate);
    mode = PickMode(ldac->Modes);
    if (rate == 0 || mode == 0) {
        ProbeLog("setup: LDAC caps unparsable (rates=0x%02X modes=0x%02X)",
                 ldac->Rates, ldac->Modes);
        st = STATUS_NOT_SUPPORTED;
        goto fail;
    }

    ProbeLog("setup: LDAC SEP seid=%u rate=0x%02X mode=0x%02X codecId=0x%04X",
             ldac->Seid, rate, mode, ldac->CodecId);

    st = AvdtpSetConfiguration(&g_SigChan, ldac, INT_SEID, rate, mode);
    if (!NT_SUCCESS(st)) {
        ProbeLog("setup: SET_CONFIGURATION failed 0x%08X", st);
        goto fail;
    }
    }

    st = AvdtpOpen(&g_SigChan, ldac->Seid);
    if (!NT_SUCCESS(st)) {
        ProbeLog("setup: OPEN failed 0x%08X", st);
        goto fail;
    }

    //  媒体传输永远是 PSM 33（PSM 25 是信令）。顺序：SetConfig -> Open -> 建 transport -> Start。
    //  （旧代码先连 PSM 25、失败才试 33，即使 Start 成功，RTP 也发进了信令通道。）
    RtlZeroMemory(&g_MediaChan, sizeof(g_MediaChan));
    //  Open 之后先停一下再开传输通道：实测 BthA2dp 在 Open 与 Start 之间
    //  隔了约 2.8s（中间正是它建传输通道 + 配置的时间）。我们原来 Open 完
    //  立刻连媒体、立刻 Start，sink 虽回 Accept 却始终不解码（LED 不进 LDAC 档）。
    {
        LARGE_INTEGER settle;
        settle.QuadPart = -1000000LL;       // 100ms（回连要快，原 300）
        KeDelayExecutionThread(KernelMode, FALSE, &settle);
    }

    st = BthL2capAcquireMedia(Addr, &g_MediaChan);
    if (!NT_SUCCESS(st)) {
        ProbeLog("setup: media channel (PSM %u) failed 0x%08X", AVDTP_MEDIA_PSM, st);
        goto fail;
    }
    g_MediaMtu = g_MediaChan.Mtu;
    ProbeLog("setup: media up, handle=0x%X mtu=%u",
             (ULONG)(ULONG_PTR)g_MediaChan.Handle, g_MediaChan.Mtu);

    //  传输通道建好后同样留一段 settle，再发 START（对齐 BthA2dp 的行为）。
    {
        LARGE_INTEGER settle2;
        settle2.QuadPart = -1500000LL;      // 150ms（回连要快，原 400）
        KeDelayExecutionThread(KernelMode, FALSE, &settle2);
    }

    //  START 不当作硬门槛：实测这台 sink 会对 Start 回 ResponseAccept，但紧接着
    //  对我们 nudge 出来的重复 Start 回 ResponseReject，而且也有整轮不回的情况。
    //  媒体通道已经 Open + Configure 完成，真正确认链路的是 RTP 能不能发出去，
    //  所以 Start 失败只记一条，继续进 STREAMING（业界实现普遍这么做）。
    st = AvdtpStart(&g_SigChan, ldac->Seid);
    if (!NT_SUCCESS(st)) {
        ProbeLog("setup: START not acked 0x%08X -> proceed to streaming anyway", st);
    } else {
        ProbeLog("setup: START acked");
    }

    g_RateBit  = rate;
    g_ModeBit  = mode;
    g_SinkSeid = ldac->Seid;
    SetState(LDAC_STATE_STREAMING, STATUS_SUCCESS);
    //  会话真的建立了 => 链路在位（jack 报"已插入"）。
    LdacKsSetLinkPresent(TRUE);
    ProbeLog("setup: ===== STREAMING (rate=0x%02X mode=0x%02X seid=%u mediaMtu=%u) =====",
             rate, mode, ldac->Seid, g_MediaMtu);
    return STATUS_SUCCESS;

fail:
    //  这里可能从很前面的位置跳进来（连通道都没建），所以先看句柄再关。
    if (g_MediaChan.Handle != NULL) BthL2capClose(&g_MediaChan);
    if (g_SigChan.Handle   != NULL) BthL2capClose(&g_SigChan);
    g_SigMtu = 0;
    g_MediaMtu = 0;
    SetState(LDAC_STATE_ERROR, st);
    //  连不上链路（典型是 sink 关机：open 直接 page timeout 0x04）=> jack 报未连接。
    //  端点本身不拆：PnP 节点还在，拆了设备会在系统里闪一下。
    LdacKsSetLinkPresent(FALSE);
    return st;
}

// ------------------------------------------------------- BT 侧早期初始化 ---
//  原来在 KMDF 的 EvtDeviceAdd 里做，现在由 KsAdapter 的 AddDevice 调进来。
extern "C" VOID
LdacBthBoot(_In_ PDEVICE_OBJECT Pdo)
{
    BTH_ADDR addr = 0;
    CHAR     s[18];
    NTSTATUS st;

    ProbeMark("bt-01-boot");

    //  幂等地挂 BDIF 接口。装成 root 设备（VM 里没有蓝牙节点）时这里会失败，
    //  端点照常能用，只是 START 会返回错误 —— 这是预期行为，不是 bug。
    st = BthL2capAttach(Pdo);
    if (!NT_SUCCESS(st)) {
        ProbeLog("BthBoot: BthL2capAttach failed 0x%08X (PDO is not a BT node)", st);
        return;
    }

    if (!ReadTargetAddrFromRegistry(s)) {
        RtlCopyMemory(s, DEFAULT_BD_ADDR_STR, sizeof(DEFAULT_BD_ADDR_STR));
    }
    if (NT_SUCCESS(BthParseAddress(s, &addr))) {
        g_TargetAddr = addr;
        //  用控制设备做 ReferenceObject：它跟驱动同生命周期，
        //  不像 A2DP 设备的 FDO 会随重连被删掉。
        BthL2capRegisterServer(g_CtlDevice ? g_CtlDevice : Pdo, addr);
        ProbeLog("BthBoot: L2CAP server registered for %s", s);
        ProbeMark("bt-02-server");
        //  ⚠ 动态端点的“链路在位”信号必须来自**蓝牙连接本身**，不能来自
        //  AVDTP 的 STREAMING —— 否则就是鸡生蛋（2026-11-09 实测卡在这一步）。
        //  AddDevice 只在 A2DP 子设备被枚举时调用，也就是远端设备真的连上了。
        //  设备节点到达不等于链路在位：端点在会话真正建起来时安装
        //  （见 KsAdapter.cpp 的 LdacKsSetLinkPresent）。
    } else {
        ProbeLog("BthBoot: bad target addr '%s', server not registered", s);
    }
}

//  2026-11-08 重构：内核里**不再有任何常驻线程或轮询**。
//  设备"已连接"的提示由用户态服务通过 START / L2CAP 原语建立。

//  ============================================================================
//  AVRCP 交还 Windows 自带的 BthAvrcpTg（2026-11-08 重构，见 docs/refactor-plan.md）
//
//  删掉自写 AVRCP 的理由：
//    - 它是**重复实现系统已有功能**：Windows 的 AVRCP profile 驱动（BthAvrcpTg）
//      独立绑定 AVRCP 的 BTHENUM 子设备（0x110E / PSM 23），和我们的 A2DP
//      profile 并行存在，删掉我们的不影响它。
//    - BT505 的 TG 本来就不守规范（回包 CR 位为 0、SetAbsoluteVolume 不应答），
//      我们那份实现实测从来没成功过，默认开关都是关的。
//    - 用户态/系统侧能做的事，内核不该再写一遍。
//  ============================================================================

//  ------------------------------------------------------------ 无常驻线程 ---
//  keeper 线程已删除（2026-11-08）：重连由用户态 supervisor 负责。
//  DisableKeeper / g_KeeperStop / g_KeeperThreadHandle 一并作废。

// --------------------------------------------------------- 卸映像（实现）---
static VOID
LdacSelfDestructThread(_In_ PVOID Context)
{
    PDEVICE_OBJECT ctl;
    LARGE_INTEGER  delay;
    UNICODE_STRING sym;

    UNREFERENCED_PARAMETER(Context);

    //  必须等触发它的那个 IOCTL 先完成、用户态也把句柄关掉，删设备才有意义。
    delay.QuadPart = -2000000LL;                 // 相对 200ms
    KeDelayExecutionThread(KernelMode, FALSE, &delay);

    //  0x139 的根：以前这里只置标志、睡 5 秒就拆通道。keeper / AVRCP 线程
    //  可能还活着、还拿着通道句柄并发提交 BRB，拆完它们继续写 —— 内核链表被写花。
    //  现在：先关门（新 BRB 全拒）→ join 线程 → 再等 BRB 归零 → 才拆通道。
    BthL2capBeginShutdown();
    {
        //  ⚠ 不要在这里 join 线程（KeWaitForSingleObject on a thread handle）。
        //  实测 0xA：自毁线程自己就在卸载路径上，拿到的对象指针是垃圾值
        //  （0xffffffff80000f38），直接 AV。关键保护是"关门"——新 BRB 已被
        //  SubmitBrb 拒绝——线程会在下一次循环自然退场，给一个有界窗口即可。
        LARGE_INTEGER kw;
        kw.QuadPart = -3000000LL;                  // 3 秒退场窗口
        KeDelayExecutionThread(KernelMode, FALSE, &kw);
    }
    ProbeLog("unload: tearing down L2CAP state (build %s)", LDAC_BUILD_TAG);

    //  0xCE 的正面解法：bthport 手里若还有我们的 IRP，映像绝不能消失。
    //  keeper 已停，这里最多等 10 秒（正常一次 BRB 几百毫秒内完成）。
    {
        NTSTATUS wi = BthL2capWaitIdle(10000);
        ProbeLog("unload: wait idle -> 0x%08X", wi);
    }

    //  1) 先摘 L2CAP 服务端：REGISTER_SERVER / REGISTER_PSM 会挂住 bthport 里的
    //     指示回调，回调里带着我们的函数指针。不摘就卸映像 = 回调打到已释放的
    //     代码页上。这一步必须在删控制设备之前做（它把控制设备当 ReferenceObject）。
    BthL2capUnregisterServer();
    //  拨入通道是独立的一条（OPEN_CHANNEL_RESPONSE），它同样带 ReferenceObject。
    //  只关 g_SigChan 不够：g_SigChan 可能只是它的副本，也可能根本没被 START 用过。
    BthL2capCloseInbound();
    if (g_SigChan.Open)   BthL2capClose(&g_SigChan);
    if (g_MediaChan.Open) BthL2capClose(&g_MediaChan);
    {
        LARGE_INTEGER settle;           // 给 bthport 一点时间释放 ReferenceObject
        settle.QuadPart = -3000000LL;   // 300ms
        KeDelayExecutionThread(KernelMode, FALSE, &settle);
    }

    //  2) 符号链接 + 控制设备。删掉控制设备 = 释放驱动对象的最后一次引用，
    //     引用计数归零后映像才会卸载。
    if (g_CtlSymLink) {
        RtlInitUnicodeString(&sym, L"\\DosDevices\\LdacCtl");
        IoDeleteSymbolicLink(&sym);
        g_CtlSymLink = FALSE;
    }
    ctl = (PDEVICE_OBJECT)InterlockedExchangePointer((PVOID volatile*)&g_CtlDevice, NULL);
    if (ctl != NULL) {
        ProbeLog("unload: control device deleted -> image may now unload");
        IoDeleteDevice(ctl);
    } else if (g_LogFn) {
        ProbeLog("unload: no control device (already gone)");
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

//  I/O 管理器真正卸载映像时进来。到这里引用计数已经是 0，说明设备对象全没了，
//  所以这里**不能**再碰任何设备/BRB（PDO 可能已经失效，提交 BRB 会直接蓝屏）。
static VOID
LdacUnload(_In_ PDRIVER_OBJECT DriverObject)
{
    UNREFERENCED_PARAMETER(DriverObject);

    if (g_CtlSymLink) {
        UNICODE_STRING sym;
        RtlInitUnicodeString(&sym, L"\\DosDevices\\LdacCtl");
        IoDeleteSymbolicLink(&sym);
        g_CtlSymLink = FALSE;
    }
    DbgPrint("[ldac] image unloaded (build %s)\n", LDAC_BUILD_TAG);
}

// ------------------------------------------- L2CAP 原语（AVDTP 搬到用户态） ---
//  内核从这里开始只做四件事：开一条 L2CAP 通道 / 发一段字节 / 收一段字节 / 关掉。
//  AVDTP 的 Discover/SetConfig/Open/Start、重试、退避，全部由用户态负责。
//  槽位号（1..4）是我们自己的句柄，和 bthport 的 L2CAP_CHANNEL_HANDLE 无关。
typedef struct _LDAC_L2CAP_SLOT {
    BTH_L2CAP_CHAN Chan;
    BOOLEAN        Used;
    USHORT         Psm;      // 关的时候要知道是不是 AVDTP 通道
} LDAC_L2CAP_SLOT;

static LDAC_L2CAP_SLOT g_L2capSlots[LDAC_L2CAP_MAX_SLOTS];

static LDAC_L2CAP_SLOT *
LdacSlotFromHandle(_In_ ULONG Handle)
{
    if (Handle == 0 || Handle > LDAC_L2CAP_MAX_SLOTS) return NULL;
    if (!g_L2capSlots[Handle - 1].Used) return NULL;
    return &g_L2capSlots[Handle - 1];
}

static ULONG
LdacAllocSlot(VOID)
{
    ULONG i;
    for (i = 0; i < LDAC_L2CAP_MAX_SLOTS; i++) {
        if (!g_L2capSlots[i].Used) {
            RtlZeroMemory(&g_L2capSlots[i], sizeof(g_L2capSlots[i]));
            g_L2capSlots[i].Used = TRUE;
            return i + 1;
        }
    }
    return 0;
}

static VOID
LdacCloseAllSlots(VOID)
{
    ULONG i;
    for (i = 0; i < LDAC_L2CAP_MAX_SLOTS; i++) {
        if (g_L2capSlots[i].Used) {
            if (g_L2capSlots[i].Chan.Open) BthL2capClose(&g_L2capSlots[i].Chan);
            RtlZeroMemory(&g_L2capSlots[i], sizeof(g_L2capSlots[i]));
        }
    }
}

//  目标地址：请求里带了就用请求的；全零则回落到注册表/内置默认。
static NTSTATUS
LdacResolveTarget(_In_opt_ const UCHAR Addr[6], _Out_ PBTH_ADDR Out)
{
    CHAR s[18];

    *Out = 0;
    if (Addr != NULL) {
        static const UCHAR zero[6] = { 0 };
        if (RtlCompareMemory(Addr, zero, 6) != 6) {
            *Out = ((BTH_ADDR)Addr[0] << 40) | ((BTH_ADDR)Addr[1] << 32) |
                   ((BTH_ADDR)Addr[2] << 24) | ((BTH_ADDR)Addr[3] << 16) |
                   ((BTH_ADDR)Addr[4] << 8)  |  (BTH_ADDR)Addr[5];
            return STATUS_SUCCESS;
        }
    }
    if (!ReadTargetAddrFromRegistry(s)) {
        RtlCopyMemory(s, DEFAULT_BD_ADDR_STR, sizeof(DEFAULT_BD_ADDR_STR));
    }
    return BthParseAddress(s, Out);
}
// ------------------------------------------------------------ IOCTL 派发 ---
//  METHOD_BUFFERED 下输入输出都走 Irp->AssociatedIrp.SystemBuffer，
//  所以原先 WdfRequestRetrieveInput/OutputBuffer 那两个调用直接消失。
static NTSTATUS
LdacHandleIoctl(_In_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);
    PVOID   buf    = Irp->AssociatedIrp.SystemBuffer;
    ULONG   inLen  = sp->Parameters.DeviceIoControl.InputBufferLength;
    ULONG   outLen = sp->Parameters.DeviceIoControl.OutputBufferLength;
    ULONG   code   = sp->Parameters.DeviceIoControl.IoControlCode;
    NTSTATUS st    = STATUS_SUCCESS;

    Irp->IoStatus.Information = 0;

    switch (code) {

    case IOCTL_LDAC_START:
    {
        BTH_ADDR addr = 0;
        CHAR     addrStr[18];
        UCHAR    prefRate = 0;

        //  用户态已经接管 AVDTP 时，内核 START 必须拒绝：
        //  否则会出现"两条 AVDTP 会话打同一个耳机"的诡异状态（用户态建链成功，
        //  内核又在另一边协商，两边互相把对端谈崩）。
        {
            ULONG um = 0;
            if (ReadDwordFromSoftwareKey(L"UserModeAvdtp", &um) && um != 0) {
                ProbeLog("ioctl START: refused -- user-mode AVDTP owns the session "
                         "(HKLM\\SOFTWARE\\LdacPanel\\UserModeAvdtp=1)");
                st = STATUS_NOT_SUPPORTED;
                break;
            }
        }

        if (g_State == LDAC_STATE_STREAMING) {
            ProbeLog("ioctl START: already streaming");
            break;
        }
        if (g_State == LDAC_STATE_STARTING) {
            st = STATUS_DEVICE_BUSY;
            break;
        }

        // 输入优先级：LDAC_START_REQ（7B，Addr 全零走注册表/默认）> 6B 裸地址
        if (buf != NULL && inLen >= sizeof(LDAC_START_REQ)) {
            const LDAC_START_REQ* r = (const LDAC_START_REQ*)buf;
            static const UCHAR zero[6] = { 0 };
            prefRate = r->RateBit;
            if (RtlCompareMemory(r->Addr, zero, 6) != 6) {
                addr = ((BTH_ADDR)r->Addr[0] << 40) | ((BTH_ADDR)r->Addr[1] << 32) |
                       ((BTH_ADDR)r->Addr[2] << 24) | ((BTH_ADDR)r->Addr[3] << 16) |
                       ((BTH_ADDR)r->Addr[4] << 8) | (BTH_ADDR)r->Addr[5];
                ProbeLog("ioctl START: target from caller = 0x%X%08X",
                         (ULONG)(addr >> 32), (ULONG)(addr & 0xFFFFFFFF));
            }
        } else if (buf != NULL && inLen >= 6) {
            const UCHAR* p = (const UCHAR*)buf;
            addr = ((BTH_ADDR)p[0] << 40) | ((BTH_ADDR)p[1] << 32) |
                   ((BTH_ADDR)p[2] << 24) | ((BTH_ADDR)p[3] << 16) |
                   ((BTH_ADDR)p[4] << 8) | (BTH_ADDR)p[5];
            ProbeLog("ioctl START: target from caller = 0x%X%08X",
                     (ULONG)(addr >> 32), (ULONG)(addr & 0xFFFFFFFF));
        }

        if (addr == 0) {
            if (!ReadTargetAddrFromRegistry(addrStr)) {
                RtlCopyMemory(addrStr, DEFAULT_BD_ADDR_STR, sizeof(DEFAULT_BD_ADDR_STR));
            }
            st = BthParseAddress(addrStr, &addr);
            if (!NT_SUCCESS(st)) {
                ProbeLog("ioctl START: bad target addr '%s'", addrStr);
                break;
            }
            ProbeLog("ioctl START: target %s", addrStr);
        }
        if (prefRate) {
            ProbeLog("ioctl START: preferred rate bit 0x%02X", prefRate);
        }

        SetState(LDAC_STATE_STARTING, STATUS_SUCCESS);
        st = LdacSetup(addr, prefRate);
        if (!NT_SUCCESS(st)) {
            st = g_State == LDAC_STATE_ERROR ? st : STATUS_UNSUCCESSFUL;
        }
        break;
    }

    case IOCTL_LDAC_STOP:
        ProbeLog("ioctl STOP");
        //  ⚠ 2026-11-08 改：以前 STOP **只关媒体通道、保留信令通道**，
        //  理由是"ACL 靠它维持，设备不会放完歌就掉线"。
        //
        //  这条理由在 keeper 删除后已经不成立，而且制造了一个真实 bug：
        //  耳机断电后那条通道已成死链、却仍持有 ACL 引用，系统因此一直显示
        //  "连接的音频"（截图核实：设备已关机，PnP 仍 OK、无 disconnect 指示）。
        //
        //  现在 STOP 语义是"彻底回到 IDLE"：两条通道都关，下次 START 现建。
        BthL2capClose(&g_MediaChan);
        BthL2capClose(&g_SigChan);

        //  用户态通过新原语开的通道也要一起关：STOP 的语义是"回到 IDLE"，
        //  留着半开的通道只会让下一次 START 撞上旧句柄。
        LdacCloseAllSlots();

        g_SigMtu = 0;
        g_MediaMtu = 0;
        SetState(LDAC_STATE_IDLE, STATUS_SUCCESS);
        //  停止播放不等于链路断开：端点保留（见 SetState 里的说明）。
        break;

    case IOCTL_LDAC_SET_CODEC:
        //  运行时选编解码器：0 = LDAC，1 = SBC。只影响下一次会话建立，
        //  调用方（面板服务）随后会 STOP + START 重建会话。
        if (buf == NULL || inLen < sizeof(ULONG)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        {
            ULONG v = *(PULONG)buf;
            g_Codec = (v > LDAC_CODEC_AAC) ? LDAC_CODEC_LDAC : v;
        }
        ProbeLog("ioctl SET_CODEC: %s", g_Codec == LDAC_CODEC_SBC ? "SBC" :
                                        g_Codec == LDAC_CODEC_AAC ? "AAC" : "LDAC");
        break;

    case IOCTL_LDAC_STATUS:
        if (buf == NULL || outLen < sizeof(LDAC_STATUS)) {
            st = STATUS_BUFFER_TOO_SMALL;
            break;
        }
        FillStatus((PLDAC_STATUS)buf);
        Irp->IoStatus.Information = sizeof(LDAC_STATUS);
        break;

    case IOCTL_LDAC_SEND:
    {
        ULONG len;

        if (g_State != LDAC_STATE_STREAMING) {
            st = STATUS_INVALID_DEVICE_STATE;
            break;
        }
        if (buf == NULL || inLen < LDAC_SEND_MIN) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        len = inLen;
        if (len > g_MediaMtu) {
            ProbeLog("ioctl SEND: %u > media MTU %u", len, g_MediaMtu);
            st = STATUS_INVALID_BUFFER_SIZE;
            break;
        }
        // 内核不做任何解释：用户态封好的 RTP+LDAC 原样转发
        st = BthL2capSend(&g_MediaChan, (const UCHAR*)buf, len);
        if (!NT_SUCCESS(st)) {
            ProbeLog("ioctl SEND: transfer failed 0x%08X (len=%u)", st, len);
        } else {
            InterlockedIncrement(&g_SentPackets);
            InterlockedExchangeAdd(&g_SentBytes, (LONG)len);
        }
        break;
    }

    case IOCTL_LDAC_L2CAP_OPEN:
    {
        PLDAC_L2CAP_OPEN_REQ  req;
        PLDAC_L2CAP_OPEN_RSP  rsp;
        BTH_ADDR              addr = 0;
        ULONG                 idx;
        USHORT                psm;
        NTSTATUS              ost;

        if (buf == NULL || inLen < sizeof(LDAC_L2CAP_OPEN_REQ) ||
            outLen < sizeof(LDAC_L2CAP_OPEN_RSP)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        req = (PLDAC_L2CAP_OPEN_REQ)buf;
        st = LdacResolveTarget(req->Addr, &addr);
        if (!NT_SUCCESS(st)) {
            ProbeLog("l2cap open: bad target 0x%08X", st);
            break;
        }
        //  ⚠ 先取出 psm：下面会原地覆写同一块 buffer（METHOD_BUFFERED），
        //    否则日志里会打成 0（踩过）。
        psm = (USHORT)req->Psm;

        idx = LdacAllocSlot();
        if (idx == 0) { st = STATUS_INSUFFICIENT_RESOURCES; break; }

        //  ⚠ 关键规避（与 BthL2capAcquireMedia 同源）：
        //  我们自己注册了 PSM 25 的 server 时，client open 会被 bthport
        //  **本地满足** —— 句柄有效、send 也"成功"，但空口上没有 Connection
        //  Request，对端根本收不到我们的 Discover。所以开 PSM 25 之前先摘掉
        //  server，开完立刻挂回去。
        {
            BOOLEAN hadServer = BthL2capIsServerRegistered();

            if (psm == AVDTP_SIGNAL_PSM && hadServer) {
                BthL2capUnregisterServer();
                if (g_LogFn) g_LogFn("l2cap open: server temporarily unregistered (psm 25)");
            }

            g_L2capSlots[idx - 1].Psm = psm;
        ost = BthL2capOpen(addr, psm, &g_L2capSlots[idx - 1].Chan);

            //  ⚠ 用户态接管 AVDTP 时**不再挂回 server**：
            //  设备会主动拨入 PSM 25，内核接住它（BthL2capAcceptWorker）但没人读，
            //  于是设备等我们回应、我们在自己开的通道上等它回应 —— 协议层死锁。
            //  用户态模式下让 server 保持注销，设备只能通过我们主动开的通道会话。
            if (psm == AVDTP_SIGNAL_PSM && hadServer &&
                g_CtlDevice != NULL && g_TargetAddr != 0) {
                ULONG um = 0;
                BOOLEAN userMode = ReadDwordFromSoftwareKey(L"UserModeAvdtp", &um) && um != 0;
                if (userMode) {
                    if (g_LogFn) g_LogFn("l2cap open: server left unregistered (user-mode AVDTP owns the session)");
                } else {
                    BthL2capRegisterServer(g_CtlDevice, g_TargetAddr);
                    if (g_LogFn) g_LogFn("l2cap open: server re-registered after open -> 0x%08X", ost);
                }
            }
        }

        //  注意：METHOD_BUFFERED 下输入输出同一块内存，这里可以原地覆写。
        rsp = (PLDAC_L2CAP_OPEN_RSP)buf;
        rsp->Handle = NT_SUCCESS(ost) ? idx : 0;
        rsp->Mtu    = (ULONG)g_L2capSlots[idx - 1].Chan.Mtu;
        rsp->Status = ost;
        Irp->IoStatus.Information = sizeof(LDAC_L2CAP_OPEN_RSP);
        ProbeLog("l2cap open: psm=%u handle=%lu mtu=%lu -> 0x%08X",
                 (ULONG)psm, rsp->Handle, rsp->Mtu, ost);
        if (!NT_SUCCESS(ost)) {
            RtlZeroMemory(&g_L2capSlots[idx - 1], sizeof(g_L2capSlots[idx - 1]));
            st = ost;
        }
        break;
    }

    case IOCTL_LDAC_L2CAP_SEND:
    {
        PLDAC_L2CAP_IO_REQ r;
        LDAC_L2CAP_SLOT   *slot;
        ULONG              len;

        if (buf == NULL || inLen < sizeof(LDAC_L2CAP_IO_REQ)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        r = (PLDAC_L2CAP_IO_REQ)buf;
        slot = LdacSlotFromHandle(r->Handle);
        if (slot == NULL) { st = STATUS_INVALID_HANDLE; break; }
        len = r->Length;
        if (len == 0 || len > inLen - sizeof(LDAC_L2CAP_IO_REQ)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        st = BthL2capSend(&slot->Chan, (PUCHAR)buf + sizeof(LDAC_L2CAP_IO_REQ), len);
        if (!NT_SUCCESS(st)) {
            ProbeLog("l2cap send: handle=%lu len=%lu -> 0x%08X", r->Handle, len, st);
        } else {
            InterlockedIncrement(&g_SentPackets);
            InterlockedExchangeAdd(&g_SentBytes, (LONG)len);
        }
        break;
    }

    case IOCTL_LDAC_L2CAP_RECV:
    {
        PLDAC_L2CAP_RECV_REQ r;
        LDAC_L2CAP_SLOT     *slot;
        ULONG                handle, waitMs, cap, got = 0;

        if (buf == NULL || inLen < sizeof(LDAC_L2CAP_RECV_REQ) || outLen == 0) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        r = (PLDAC_L2CAP_RECV_REQ)buf;
        slot = LdacSlotFromHandle(r->Handle);
        if (slot == NULL) { st = STATUS_INVALID_HANDLE; break; }

        //  先把请求字段读出来：接收会把这块 buffer 整个覆写成载荷。
        handle = r->Handle;
        waitMs = r->TimeoutMs;
        cap    = (outLen < LDAC_L2CAP_RECV_MAX) ? outLen : LDAC_L2CAP_RECV_MAX;

        st = BthL2capRecv(&slot->Chan, (PUCHAR)buf, cap, waitMs, &got);
        if (NT_SUCCESS(st)) {
            Irp->IoStatus.Information = got;
        } else if (g_VerboseLog) {
            ProbeLog("l2cap recv: handle=%lu -> 0x%08X", handle, st);
        }
        break;
    }

    case IOCTL_LDAC_L2CAP_CLOSE:
    {
        PLDAC_L2CAP_HANDLE_REQ r;
        LDAC_L2CAP_SLOT       *slot;

        if (buf == NULL || inLen < sizeof(LDAC_L2CAP_HANDLE_REQ)) {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        r = (PLDAC_L2CAP_HANDLE_REQ)buf;
        slot = LdacSlotFromHandle(r->Handle);
        if (slot == NULL) { st = STATUS_INVALID_HANDLE; break; }
        if (slot->Chan.Open) BthL2capClose(&slot->Chan);
        {
            BOOLEAN wasAvdtp = (slot->Psm == AVDTP_SIGNAL_PSM);
            ULONG   i;
            RtlZeroMemory(slot, sizeof(*slot));
            ProbeLog("l2cap close: handle=%lu", r->Handle);

            //  最后一条 AVDTP 通道关了 = 链路不在位 → 端点撤掉。
            if (wasAvdtp) {
                BOOLEAN anyAvdtp = FALSE;
                for (i = 0; i < LDAC_L2CAP_MAX_SLOTS; i++) {
                    if (g_L2capSlots[i].Used && g_L2capSlots[i].Psm == AVDTP_SIGNAL_PSM) {
                        anyAvdtp = TRUE;
                        break;
                    }
                }
            }
        }
        break;
    }
    case IOCTL_LDAC_UNLOAD:
    {
        //  开发期专用：拆干净 + 删控制设备，让映像能被卸载（见文件上方长注释）。
        //  不能在本派发里直接 IoDeleteDevice —— 当前 IRP 还挂在它上面。
        HANDLE th = NULL;
        st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                  LdacSelfDestructThread, NULL);
        if (NT_SUCCESS(st)) {
            ZwClose(th);
            ProbeLog("ioctl UNLOAD: self-destruct thread started");
        } else {
            ProbeLog("ioctl UNLOAD: PsCreateSystemThread failed 0x%08X", st);
        }
        break;
    }

    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    return st;
}

// ------------------------------------------------- 派发（控制设备 / PortCls 分流）---
#define LDAC_DONE(irp, status, info)                     \
    do {                                                 \
        (irp)->IoStatus.Status      = (status);          \
        (irp)->IoStatus.Information = (info);            \
        IoCompleteRequest((irp), IO_NO_INCREMENT);       \
    } while (0)

//  端点常驻后，这里没有任何闸门需要维护：我们不注册/注销子设备，
//  PortCls 的 filter 生命周期就不会和我们的操作重叠（0x3B 的根因已移除，
//  见 docs/lifecycle-audit.md）。
static NTSTATUS
LdacCtlCreate(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    if (DevObj == g_CtlDevice) {
        LDAC_DONE(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    }
    if (g_OrigCreate) return g_OrigCreate(DevObj, Irp);
    LDAC_DONE(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS
LdacCtlClose(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    if (DevObj == g_CtlDevice) {
        LDAC_DONE(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    }
    if (g_OrigClose) return g_OrigClose(DevObj, Irp);
    LDAC_DONE(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS
LdacCtlCleanup(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    if (DevObj == g_CtlDevice) {
        LDAC_DONE(Irp, STATUS_SUCCESS, 0);
        return STATUS_SUCCESS;
    }
    if (g_OrigCleanup) return g_OrigCleanup(DevObj, Irp);
    LDAC_DONE(Irp, STATUS_SUCCESS, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS
LdacCtlDeviceControl(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    if (DevObj == g_CtlDevice) {
        NTSTATUS st = LdacHandleIoctl(Irp);
        LDAC_DONE(Irp, st, Irp->IoStatus.Information);
        return st;
    }
    //  不是我们的控制设备 -> 一定是 PortCls 的 KS 属性请求，原样转回去
    if (g_OrigDeviceControl) return g_OrigDeviceControl(DevObj, Irp);
    LDAC_DONE(Irp, STATUS_INVALID_DEVICE_REQUEST, 0);
    return STATUS_INVALID_DEVICE_REQUEST;
}

// -------------------------------------------------- PnP 钩子：设备移除 ---
//  PortCls 装了 IRP_MJ_PNP 的派发，我们包一层：只在"设备被移除"这两个
//  minor 上先作废 L2CAP 接口，然后原样转发给 PortCls。
//
//  为什么必须有它：verifier 实测抓到
//    0xC9 Arg1=4 "Invalid Device object passed to IoCallDriver"
//    IoCallDriver <- SubmitBrb <- BthL2capUnregisterServer <- ... <- LdacKeeperThread
//  设备节点删掉后 g_Pdo 是悬空的，keeper 还在往里发 IRP；那一下就把内核
//  链表写花了，几秒后随机爆成 0x139_3。作废接口后 keeper 会自己停下。
static PDRIVER_DISPATCH g_OrigPnp = NULL;

static NTSTATUS
LdacPnpDispatch(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp)
{
    PIO_STACK_LOCATION sp = IoGetCurrentIrpStackLocation(Irp);

    if (sp->MinorFunction == IRP_MN_REMOVE_DEVICE ||
        sp->MinorFunction == IRP_MN_SURPRISE_REMOVAL) {
        ProbeLog("pnp: %s -> detaching L2CAP interface",
                 sp->MinorFunction == IRP_MN_REMOVE_DEVICE ? "REMOVE_DEVICE" : "SURPRISE_REMOVAL");
        //  先关用户态开的通道，再作废接口 —— 反过来的话 BthL2capClose 就没得用了。
        LdacCloseAllSlots();
        //  端点跟着设备一起走：拆掉当前这一份（如果有），并清理全局引用。
        //  PnP remove 一定是 PASSIVE，可以直接做 PortCls 操作。
        LdacKsOnDeviceRemove();
        BthL2capDetach();
    }

    if (g_OrigPnp != NULL) return g_OrigPnp(DevObj, Irp);
    Irp->IoStatus.Status = STATUS_INVALID_DEVICE_REQUEST;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_INVALID_DEVICE_REQUEST;
}

// -------------------------------------------------------------- 驱动入口 ---
extern "C" NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS       st;
    UNICODE_STRING devName, symName;

    g_LogFn = ProbeLog;

    //  动态端点的 create 闸门是纯原子标志，不需要初始化（见 LdacCtlCreate）。

    ProbeMark("de-01-entry");
    //  这一行是「跑的是哪一版」的唯一可信证据。日志里没有 build 行 = 内存里是旧映像。
    ProbeLog("===== LdacA2dp build [%s] =====", LDAC_BUILD_TAG);
    ProbeLog("===== DriverEntry: LdacA2dp (PortCls endpoint + AVDTP) =====");

    //  1) PortCls：装 PnP/电源派发，绑定我们的 AddDevice
    st = PcInitializeAdapterDriver(DriverObject, RegistryPath, LdacKsAddDevice);
    ProbeLog("PcInitializeAdapterDriver -> 0x%08X", st);

    //  包一层 PnP 派发：设备移除时先作废 L2CAP 接口（见 LdacPnpDispatch 注释）
    if (NT_SUCCESS(st)) {
        g_OrigPnp = (PDRIVER_DISPATCH)DriverObject->MajorFunction[IRP_MJ_PNP];
        DriverObject->MajorFunction[IRP_MJ_PNP] = LdacPnpDispatch;
        ProbeLog("pnp dispatch hooked (orig=%p)", g_OrigPnp);
    }
    if (!NT_SUCCESS(st)) {
        return st;
    }
    ProbeMark("de-02-portcls");

    //  2) 保存 PortCls 的派发，装我们自己的。表是按驱动的，
    //     所以处理器里必须按目标设备对象分流（见上面的 LdacCtl*）。
    g_OrigCreate        = DriverObject->MajorFunction[IRP_MJ_CREATE];
    g_OrigClose         = DriverObject->MajorFunction[IRP_MJ_CLOSE];
    g_OrigCleanup       = DriverObject->MajorFunction[IRP_MJ_CLEANUP];
    g_OrigDeviceControl = DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL];

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = LdacCtlCreate;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = LdacCtlClose;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = LdacCtlCleanup;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = LdacCtlDeviceControl;

    //  3) 控制设备：用户态服务的 IOCTL 入口（无 PnP，驱动加载即存在）
    //  DACL 显式指定：SYSTEM / Administrators 全权，Everyone 读写。
    //  不这么写就得靠"以 SYSTEM 跑计划任务"才能打开 —— 那是不该有的设计债。
    //  （默认 DACL 取决于驱动对象的继承，不可控；这里把它钉死。）
    RtlInitUnicodeString(&devName, L"\\Device\\LdacCtl");
    {
        UNICODE_STRING sddl;
        static const GUID devClass =
            { 0x8d1a9c42, 0x6e7b, 0x4e63, { 0x9a, 0x2f, 0x3c, 0x5d, 0x1b, 0x4e, 0x7f, 0x11 } };
        RtlInitUnicodeString(&sddl, L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)");
        st = IoCreateDeviceSecure(DriverObject, 0, &devName,
                                  FILE_DEVICE_UNKNOWN, FILE_DEVICE_SECURE_OPEN, FALSE,
                                  &sddl, &devClass, &g_CtlDevice);
    }
    if (!NT_SUCCESS(st)) {
        ProbeLog("IoCreateDevice(LdacCtl) failed 0x%08X", st);
        return st;
    }
    g_CtlDevice->Flags |= DO_BUFFERED_IO;
    g_CtlDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    RtlInitUnicodeString(&symName, L"\\DosDevices\\LdacCtl");
    st = IoCreateSymbolicLink(&symName, &devName);
    if (!NT_SUCCESS(st)) {
        ProbeLog("IoCreateSymbolicLink failed 0x%08X", st);
    } else {
        g_CtlSymLink = TRUE;
    }

    //  卸映像时进来收尾。没有这个回调，映像照样能卸，只是少了 SymLink 清理和一行日志。
    DriverObject->DriverUnload = LdacUnload;

    ProbeMark("de-03-ctldevice-ready");
    ProbeLog("control device ready: \\\\.\\LdacCtl");
    return STATUS_SUCCESS;
}
