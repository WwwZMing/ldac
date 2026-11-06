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

#include "Log.h"
#include "BthL2cap.h"
#include "Avdtp.h"
#include "Avrcp.h"
#include "ldac_io.h"
#include "LdacBridge.h"

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
static BOOLEAN        g_ForceSbc = FALSE;    // 注册表 Parameters\ForceSbc：走 SBC 对照实验
BTH_L2CAP_CHAN g_SigChan;                   // AVDTP 信令通道（BthL2cap 断开时要清）
BTH_L2CAP_CHAN g_MediaChan;                 // AVDTP 媒体通道（同上）
LONG                  g_State = LDAC_STATE_IDLE;   // BthL2cap 断开时要复位
static LONG           g_LastErr = 0;
static ULONG          g_SigMtu = 0;
static ULONG          g_MediaMtu = 0;
static ULONG          g_RateBit = 0;
static ULONG          g_ModeBit = 0;
static ULONG          g_SinkSeid = 0;

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

static VOID
LdacStartKeeper(_In_ BTH_ADDR Addr);

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

static VOID SetState(_In_ LONG State, _In_ NTSTATUS Err)
{
    InterlockedExchange(&g_State, State);
    InterlockedExchange(&g_LastErr, (LONG)Err);
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

    st = BthL2capAttach(g_LdacBthPdo);
    if (!NT_SUCCESS(st)) {
        ProbeLog("setup: BthL2capAttach failed 0x%08X", st);
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
        if (ReadDwordFromRegistry(L"ForceSbc", &cf)) {
            g_ForceSbc = (cf != 0);
            ProbeLog("setup: ForceSbc <- registry %u", g_ForceSbc);
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
    if (BthL2capTakeHeld(&g_SigChan)) {
        ProbeLog("setup: took kept signaling channel handle=0x%X mtu=%u",
                 (ULONG)(ULONG_PTR)g_SigChan.Handle, g_SigChan.Mtu);
        st = STATUS_SUCCESS;
    } else {
        st = BthL2capAcquireSignaling(Addr, &g_SigChan);
        if (!NT_SUCCESS(st)) {
            ProbeLog("setup: acquire signaling failed 0x%08X", st);
            goto fail;
        }
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
        if (seps[i].HasCodec &&
            seps[i].VendorId == AVDTP_SONY_VENDOR_ID &&
            seps[i].CodecId == AVDTP_SONY_CODEC_LDAC &&
            ldac == NULL) {
            ldac = &seps[i];
        }
    }

    if (g_ForceSbc) {
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
    return st;
}

// ------------------------------------------------------- BT 侧早期初始化 ---
//  原来在 KMDF 的 EvtDeviceAdd 里做，现在由 KsAdapter 的 AddDevice 调进来。
VOID
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
        //  用控制设备做 ReferenceObject：它跟驱动同生命周期，
        //  不像 A2DP 设备的 FDO 会随重连被删掉。
        BthL2capRegisterServer(g_CtlDevice ? g_CtlDevice : Pdo, addr);
        ProbeLog("BthBoot: L2CAP server registered for %s", s);
        ProbeMark("bt-02-server");
        LdacStartKeeper(addr);      // 自己拨号，让设备提示"已连接"
    } else {
        ProbeLog("BthBoot: bad target addr '%s', server not registered", s);
    }
}

// --------------------------------------------- 常驻拨号（让设备"已连接"）---
//  设备只有在有 L2CAP 连接时才提示已连接。这条连接以前由 user-mode 的 send
//  建立，所以"不跑 send = 设备不连"。这里在开机注册完 server 后自己拨一次，
//  并让通道常驻。
static BTH_ADDR g_TargetAddr;
static BOOLEAN  g_KeeperStarted = FALSE;

static volatile LONG g_KeeperStop = 0;

//  AVRCP 音量：每次"新连接"（以 g_L2capDrops 变化为界）做一次。
//  不能每 300ms 都发：SetAbsoluteVolume 是写操作，会把用户自己按的音量
//  又覆盖掉；只在会话刚建立时校一次。
static LONG  g_AvrcpDrops = -1;
static BOOLEAN g_AvrcpDone = FALSE;
static ULONG g_AvrcpTries = 0;      // 每次连接最多试 3 次，别刷日志/反复开关通道

static VOID
LdacAvrcpPushVolume(_In_ BTH_ADDR Addr)
{
    UCHAR    cur = 0xFF, ct = 0xFF;
    NTSTATUS st  = AvrcpSetVolume(Addr, 0x7F, &cur, &ct);

    ProbeLog("avrcp: result 0x%08X  before=%lu/127  ctype=0x%02X",
             st, (ULONG)cur, (ULONG)ct);

    //  只有对端真的回 ACCEPTED(0x09) 才算完成。失败就留着 g_AvrcpDone=FALSE，
    //  下一轮再试 —— 这条通道"能开但没人应答"是常态（对端忙/还没就绪）。
    if (NT_SUCCESS(st) && ct == 0x09) {
        g_AvrcpDrops = g_L2capDrops;
        g_AvrcpDone  = TRUE;
    }
}

//  ⚠ 必须独立线程：BthL2capOpen 内部要等 BRB 完成，最坏几秒。
//  之前放在 keeper 循环里，直接把回连逻辑堵死（实测断在 open 的轮询上）。
static VOID
LdacAvrcpThread(_In_ PVOID Context)
{
    LARGE_INTEGER d;
    UNREFERENCED_PARAMETER(Context);

    //  注册表开关 Parameters\AvrcpVolume（DWORD），默认 **关**：
    //  BT505 的 TG 不守规范 —— 回包 CR 位是 0、ctype 不在已知集合里，
    //  SetAbsoluteVolume 干脆不应答。失败重试只会刷日志、反复开/关 L2CAP，
    //  所以默认不跑；要做协议实验时把它置 1。
    {
        ULONG on = 0;
        if (!ReadDwordFromRegistry(L"AvrcpVolume", &on) || on == 0) {
            ProbeLog("avrcp: disabled (Parameters\\AvrcpVolume=0)");
            PsTerminateSystemThread(STATUS_SUCCESS);
            return;
        }
    }

    d.QuadPart = -5000000LL;                  // 5s
    while (!g_KeeperStop) {
        if (g_State == LDAC_STATE_STREAMING &&
            (!g_AvrcpDone || g_AvrcpDrops != g_L2capDrops) &&
            g_AvrcpTries < 3) {
            g_AvrcpTries++;
            LdacAvrcpPushVolume(g_TargetAddr);
        }
        KeDelayExecutionThread(KernelMode, FALSE, &d);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID
LdacKeeperThread(_In_ PVOID Context)
{
    LARGE_INTEGER d;
    UNREFERENCED_PARAMETER(Context);

    d.QuadPart = -3000000LL;           // 300ms（回连要快）

    //  常驻重试：设备可能晚于驱动开机，也可能中途断电。只要当前空闲
    //  （没在放歌、也没有持有通道），就再拨一次，保证"每次开关机都能连上"。
    while (!g_KeeperStop) {
        //  每轮确保 PSM25 的 server 注册还在（幂等：已注册直接返回）。
        //  任何一次 Unregister 漏了恢复，设备回连都会被拒（灯 5s 一闪）。
        if (g_CtlDevice != NULL)
            BthL2capRegisterServer(g_CtlDevice, g_TargetAddr);

        //  ERROR 也要重试：否则一次失败（设备没开/在重连）就永久卡死
        if (g_State == LDAC_STATE_ERROR) {
            ProbeLog("keeper: state was ERROR -> reset to IDLE");
            SetState(LDAC_STATE_IDLE, STATUS_SUCCESS);
        }
        if (g_State == LDAC_STATE_IDLE) {
            //  设备要看到【整套 A2DP 建立】（Discover -> SetConfig -> Open -> Start）
            //  才提示已连接并进入播放态；只开 L2CAP、只发 Discover 都不够。
            //  所以 keeper 直接把 LdacSetup 跑完，把会话保持在 STREAMING：
            //  用户态的 START 会看到 already streaming 直接返回，只负责推数据。
            NTSTATUS ss = LdacSetup(g_TargetAddr, 0);
            ProbeLog("keeper: full setup -> 0x%08X (state=%ld)", ss, g_State);
        }

        KeDelayExecutionThread(KernelMode, FALSE, &d);
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static VOID
LdacStartKeeper(_In_ BTH_ADDR Addr)
{
    HANDLE th = NULL;
    NTSTATUS st;

    if (g_KeeperStarted) return;
    g_TargetAddr = Addr;
    st = PsCreateSystemThread(&th, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                              LdacKeeperThread, NULL);
    if (NT_SUCCESS(st)) {
        HANDLE vth = NULL;
        g_KeeperStarted = TRUE;
        ZwClose(th);

        //  AVRCP 音量线程：独立于 keeper，失败不影响回连
        if (NT_SUCCESS(PsCreateSystemThread(&vth, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                                            LdacAvrcpThread, NULL))) {
            ZwClose(vth);
        }
        ProbeLog("keeper: thread started");
    } else {
        ProbeLog("keeper: PsCreateSystemThread failed 0x%08X", st);
    }
}

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

    g_KeeperStop = 1;          // 先停常驻拨号线程
    {
        LARGE_INTEGER kw; kw.QuadPart = -5000000LL;
        KeDelayExecutionThread(KernelMode, FALSE, &kw);
    }
    ProbeLog("unload: tearing down L2CAP state (build %s)", LDAC_BUILD_TAG);

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
        //  只关媒体通道，**保留信令通道**：ACL 靠它维持，设备就不会在
        //  "放完一首歌"之后掉线（掉线后它也不主动回连，用户听到的就是
        //  "断开以后连不回来"）。下次 START 会先关掉它再重开。
        BthL2capClose(&g_MediaChan);
        g_SigMtu = 0;
        g_MediaMtu = 0;
        SetState(LDAC_STATE_IDLE, STATUS_SUCCESS);
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

// -------------------------------------------------------------- 驱动入口 ---
NTSTATUS
DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    NTSTATUS       st;
    UNICODE_STRING devName, symName;

    g_LogFn = ProbeLog;

    ProbeMark("de-01-entry");
    //  这一行是「跑的是哪一版」的唯一可信证据。日志里没有 build 行 = 内存里是旧映像。
    ProbeLog("===== LdacA2dp build [%s] =====", LDAC_BUILD_TAG);
    ProbeLog("===== DriverEntry: LdacA2dp (PortCls endpoint + AVDTP) =====");

    //  1) PortCls：装 PnP/电源派发，绑定我们的 AddDevice
    st = PcInitializeAdapterDriver(DriverObject, RegistryPath, LdacKsAddDevice);
    ProbeLog("PcInitializeAdapterDriver -> 0x%08X", st);
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
    RtlInitUnicodeString(&devName, L"\\Device\\LdacCtl");
    st = IoCreateDevice(DriverObject, 0, &devName,
                        FILE_DEVICE_UNKNOWN, 0, FALSE, &g_CtlDevice);
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
