// ============================================================================
//  BthL2cap.c — BRB 接口 + L2CAP 客户端（AVDTP 传输层）
// ============================================================================
//  流程：
//    1. BthL2capAttach(Pdo)  —— 对 BTHENUM 子设备的 PDO 发 IRP_MN_QUERY_INTERFACE，
//       拿到 BTH_PROFILE_DRIVER_INTERFACE（里面有 BthAllocateBrb/BthFreeBrb）
//    2. BthL2capOpen(addr, 25) —— BRB_L2CA_OPEN_CHANNEL，提交给 PDO
//    3. BthL2capSend / BthL2capRecv —— BRB_L2CA_ACL_TRANSFER，写/读
//    4. BthL2capClose —— BRB_L2CA_CLOSE_CHANNEL
//
//  提交方式：IRP_MJ_INTERNAL_DEVICE_CONTROL + IOCTL_INTERNAL_BTH_SUBMIT_BRB，
//  BRB 指针放在 Parameters.Others.Argument1（bthddi.h 顶部注释的约定）。
// ============================================================================

#include "BthL2cap.h"

PPROBE_LOG g_LogFn = NULL;

static BTH_PROFILE_DRIVER_INTERFACE g_If;
static BOOLEAN                      g_IfReady = FALSE;
static PDEVICE_OBJECT               g_Pdo = NULL;

//  回调用的 ReferenceObject（REGISTER_SERVER 时记下来的控制设备）。
//  客户端 open 也要用它 —— 见 BthL2capOpen 里那段说明，bthddi 明确写
//  "If a callback is requested, this parameter is not optional"。
static PDEVICE_OBJECT               g_RefDo = NULL;   // 注册时用的 ReferenceObject

#define L2CAP_MTU_DEFAULT 1500
//  signaling 通道用 679：和 BthA2dp 请求的、以及 TWS/sink 自己 Configure Request
//  里声明的一致。AVDTP 的信令帧很小，1500 只是"想要更大"，实测该 sink 在 MTU
//  1500 的通道上对 Discover 完全不理（抓包 ms3.pcap 对照），所以对齐成 679。
#define L2CAP_MTU_SIGNALING 679
#define OPEN_TIMEOUT_MS   15000

//  文件后段也有同样的 #ifndef 兜底；这里提前定义，BthL2capOpen 里判断 PSM 要用。
#ifndef AVDTP_SIGNAL_PSM
#define AVDTP_SIGNAL_PSM 25
#endif

//  indication 回调要在这里前向声明：BthL2capOpen（客户端通道）也要注册它，
//  而它的实现在文件后半段（服务端那段）。
static VOID
LdacL2capIndication(_In_opt_ PVOID Context,
                    _In_ INDICATION_CODE Code,
                    _In_ PINDICATION_PARAMETERS Params);

//  对端拆链的次数。回调里累加，ACP 探测完之后用它判断「手里那条拨入通道
//  是不是已经死了」—— 之前 CallbackFlags=0 收不到任何 indication，
//  对端断了我们还拿着死 handle 发数据，只会拿到 0xC000009D。
volatile LONG g_L2capDrops = 0;

//  打开通道时用的 CF_LINK_* 组合。默认 CF_LINK_NOTHING（不对链路提安全要求）。
//  留成可改的理由：有些 sink 只在对端**要求加密/鉴权**时才把 AVDTP 当回事，
//  而对端到底卡不卡这一位，只能靠一次次换值上机验证。
//  Driver.c 会在 START 之前按注册表 Parameters\ChannelFlags 覆盖它，
//  所以试不同组合不用重编驱动、也不用重装（改注册表 -> 再跑一次 START 即可）。
ULONG g_L2capChannelFlags = CF_LINK_NOTHING;

//  要不要拒绝对端拨进来的 PSM 25 连接。默认 0 = 接（原行为）。
//  做成开关的理由：耳机拨进来当 INT 发一条 Discover 之后就沉默，把对端留在
//  「半个会话」状态里。而 btha2dp 那边是先 SDP/AVRCP 再 A2DP，很可能根本
//  不接这种拨入。想验证「是不是这条拨入把对端搞哑了」，就得能一键改成拒。
//  运行时从 Parameters\RejectInbound 读，改完直接再跑 START。
ULONG g_L2capRejectInbound = 0;

//  客户端通道该收哪些 indication，是硬要求不是可选项（bthddi 注释写得很清楚）：
//    CALLBACK_CONFIG_QOS    不设 -> 对端在 config request 里带 QOS 就**直接拆链**
//    CALLBACK_CONFIG_EXTRA_IN 不设 -> 对端的 extra 选项被当未知选项拒掉
//    CALLBACK_RECONFIG      不设 -> 对端要求重配就拆链
//    CALLBACK_DISCONNECT    不设 -> 断链我们完全不知情（这是最要命的）
#define L2CAP_CB_FLAGS  (CALLBACK_DISCONNECT     | \
                         CALLBACK_CONFIG_QOS     | \
                         CALLBACK_CONFIG_EXTRA_IN| \
                         CALLBACK_RECONFIG)

// ------------------------------------------------------------- QI 取接口 ---
static NTSTATUS
QiComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent((PKEVENT)Context, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

static const GUID g_ProfileIfGuid =
    { 0x94a59aa8, 0x4383, 0x4286, { 0xaa, 0x4f, 0x34, 0xa1, 0x60, 0xf4, 0x00, 0x04 } };

NTSTATUS
BthL2capAttach(_In_ PDEVICE_OBJECT BthPdo)
{
    NTSTATUS           st;
    PIRP               irp;
    PIO_STACK_LOCATION sp;
    KEVENT             event;

    //  同一个 PDO 才复用接口。A2DP 设备会被反复枚举（重连/重配/disable-enable），
    //  换了 PDO 还继续用旧接口 = 拿着已删除设备的函数指针发 BRB。
    if (g_IfReady && g_Pdo == BthPdo) return STATUS_SUCCESS;
    if (BthPdo == NULL) return STATUS_INVALID_PARAMETER;

    g_IfReady = FALSE;       // PDO 变了，旧接口作废

    RtlZeroMemory(&g_If, sizeof(g_If));
    g_If.Interface.Size    = sizeof(BTH_PROFILE_DRIVER_INTERFACE);
    g_If.Interface.Version = BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI;

    KeInitializeEvent(&event, NotificationEvent, FALSE);

    irp = IoAllocateIrp(BthPdo->StackSize, FALSE);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_PNP;
    sp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    sp->Parameters.QueryInterface.InterfaceType = (PGUID)&g_ProfileIfGuid;
    sp->Parameters.QueryInterface.Size          = sizeof(BTH_PROFILE_DRIVER_INTERFACE);
    sp->Parameters.QueryInterface.Version       = BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI;
    sp->Parameters.QueryInterface.Interface     = (PINTERFACE)&g_If;
    sp->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoSetCompletionRoutine(irp, QiComplete, &event, TRUE, TRUE, TRUE);

    st = IoCallDriver(BthPdo, irp);
    if (st == STATUS_PENDING) {
        KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
        st = irp->IoStatus.Status;
    }
    IoFreeIrp(irp);

    if (!NT_SUCCESS(st)) {
        if (g_LogFn) g_LogFn("BthL2capAttach: QI failed 0x%08X", st);
        return st;
    }
    if (g_If.BthAllocateBrb == NULL || g_If.BthFreeBrb == NULL) {
        if (g_LogFn) g_LogFn("BthL2capAttach: interface incomplete");
        return STATUS_INVALID_DEVICE_STATE;
    }

    g_Pdo = BthPdo;
    g_IfReady = TRUE;
    if (g_LogFn) {
        g_LogFn("BthL2capAttach OK: version=0x%04X size=%u",
                g_If.Interface.Version, g_If.Interface.Size);
    }
    return STATUS_SUCCESS;
}

// --------------------------------------------------------- 提交 BRB 同步等 ---
static NTSTATUS
SubmitBrb(_In_ PBRB Brb, _In_ ULONG TimeoutMs)
{
    NTSTATUS           st;
    NTSTATUS           iocall;
    PIRP               irp;
    PIO_STACK_LOCATION sp;
    KEVENT             event;
    LARGE_INTEGER      timeout;

    if (g_Pdo == NULL) return STATUS_INVALID_DEVICE_STATE;

    //  0x35（NO_MORE_IRP_STACK_LOCATIONS）的老根：
    //  这里原来按 g_Pdo->StackSize 精确分配。bthport 收到
    //  IOCTL_INTERNAL_BTH_SUBMIT_BRB 后会把 IRP 再往下转发一层，
    //  栈位置刚好用完 -> 0x35。多给两层；PDO 正在移除时 StackSize 可能
    //  是 0/无效，至少给 4，避免写越界。
    {
        int stackSize = (int)g_Pdo->StackSize + 2;
        if (stackSize < 4) stackSize = 4;
        if (stackSize > 120) stackSize = 120;

        KeInitializeEvent(&event, NotificationEvent, FALSE);
        irp = IoAllocateIrp((CCHAR)stackSize, FALSE);
        if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;

        if (g_LogFn) {
            g_LogFn("brb enter: type=0x%04X stack=%d pdo=%p irp=%p",
                    Brb ? Brb->BrbHeader.Type : 0, stackSize, g_Pdo, irp);
        }
    }

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    sp->Parameters.Others.Argument1 = Brb;      // 偏移 0
    sp->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_BTH_SUBMIT_BRB;  // 偏移 8

    irp->IoStatus.Status = STATUS_PENDING;
    IoSetCompletionRoutine(irp, QiComplete, &event, TRUE, TRUE, TRUE);

    iocall = IoCallDriver(g_Pdo, irp);
    st = iocall;
    if (st == STATUS_PENDING) {
        // 相对时间：负数，单位 100ns
        timeout.QuadPart = -((LONGLONG)TimeoutMs * 10000);
        st = KeWaitForSingleObject(&event, Executive, KernelMode, FALSE,
                                   (TimeoutMs == 0) ? NULL : &timeout);
        if (st == STATUS_TIMEOUT) {
            // 绝不能 cancel 完立刻 IoFreeIrp：下层可能还会完成这个 IRP，
            // 那会导致 MULTIPLE_IRP_COMPLETE_REQUESTS (0x44) —— 已经绿过一次。
            // 正确做法：先取消，再等完成回调真正跑完，然后各自释放。
            IoCancelIrp(irp);
            KeWaitForSingleObject(&event, Executive, KernelMode, FALSE, NULL);
            //  ⚠ 这里必须换成 0xC00000B5，不能留 STATUS_TIMEOUT。
            //  STATUS_TIMEOUT 是 0x00000102 —— **正数**，NT_SUCCESS() 对它返回
            //  TRUE。留原值会让调用方把"我们等超时了"当成"BRB 成功了"，
            //  实测就是这么误报出 `ping OK respLen=0` 的（其实是彻底超时）。
            st = STATUS_IO_TIMEOUT;
        } else {
            st = irp->IoStatus.Status;
        }
    }
    if (g_LogFn) {
        //  实测：BRB_L2CA_OPEN_CHANNEL 的提交 IRP 会**立刻**以 SUCCESS 完成，
        //  而真正的结果（ChannelHandle / OutResults）稍后才写进 BRB。
        //  这两者必须分开看，否则就是"假成功"。
        g_LogFn("brb submit: iocall=0x%08X irp=0x%08X brb=0x%08X bt=0x%02X",
                iocall, irp->IoStatus.Status,
                Brb ? Brb->BrbHeader.Status : 0,
                Brb ? Brb->BrbHeader.BtStatus : 0);
    }
    IoFreeIrp(irp);

    // 关键：bthport 把真实结果放在 BRB 头里，IRP 状态可能仍是 SUCCESS
    if (NT_SUCCESS(st) && Brb != NULL && !NT_SUCCESS(Brb->BrbHeader.Status)) {
        st = Brb->BrbHeader.Status;
    }
    return st;
}

// ------------------------------------------------------------- 地址解析 ---
NTSTATUS
BthParseAddress(_In_ PCSTR Str, _Out_ PBTH_ADDR Out)
{
    BTH_ADDR v = 0;
    ULONG    nibbles = 0;

    if (Str == NULL || Out == NULL) return STATUS_INVALID_PARAMETER;

    while (*Str && nibbles < 12) {
        CHAR c = *Str++;
        ULONG d;
        if (c == ':' || c == '-') continue;
        if (c >= '0' && c <= '9')      d = (ULONG)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (ULONG)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (ULONG)(c - 'A' + 10);
        else return STATUS_INVALID_PARAMETER;

        v = (v << 4) | d;
        nibbles++;
    }
    if (nibbles != 12) return STATUS_INVALID_PARAMETER;

    *Out = v;
    return STATUS_SUCCESS;
}

// ------------------------------------------------------------- 自检 BRB ---
//  L2CAP 层 Echo。这是唯一能把「链路/L2CAP 层通不通」和「AVDTP 语义对不对」
//  切开的探针：ping 通了就证明数据真的能到对端、对端也真的回了，
//  那之后 recv 读不到东西就只能是上层（耳机不认我们的 AVDTP 请求）。
NTSTATUS
BthL2capPing(_In_ BTH_ADDR Addr, _Out_ PULONG RespLen)
{
    NTSTATUS st;
    PBRB     brb;

    *RespLen = 0;
    if (!g_IfReady) return STATUS_INVALID_DEVICE_STATE;

    brb = g_If.BthAllocateBrb(BRB_L2CA_PING, BTHL2CAP_TAG);
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    brb->BrbL2caPing.BtAddress         = Addr;
    brb->BrbL2caPing.PingRequestLength = 4;
    brb->BrbL2caPing.PingRequestData[0] = 'L';
    brb->BrbL2caPing.PingRequestData[1] = 'D';
    brb->BrbL2caPing.PingRequestData[2] = 'A';
    brb->BrbL2caPing.PingRequestData[3] = 'C';

    st = SubmitBrb(brb, 6000);
    if (NT_SUCCESS(st)) {
        *RespLen = brb->BrbL2caPing.PingResponseLength;
        if (g_LogFn) {
            g_LogFn("ping OK: respLen=%u ok0=%u type=MIRROR?0x%02X",
                    *RespLen,
                    (ULONG)((*RespLen >= 1) ? brb->BrbL2caPing.PingResponseData[0] : 0),
                    (ULONG)((*RespLen >= 2) ? brb->BrbL2caPing.PingResponseData[1] : 0));
        }
    } else if (g_LogFn) {
        g_LogFn("ping FAIL: st=0x%08X brb=0x%08X bt=0x%02X",
                st, brb->BrbL2caPing.Hdr.Status, brb->BrbL2caPing.Hdr.BtStatus);
    }
    g_If.BthFreeBrb(brb);
    return st;
}

NTSTATUS
BthL2capGetLocalAddress(_Out_ PBTH_ADDR Out)
{
    NTSTATUS st;
    PBRB     brb;

    *Out = 0;
    if (!g_IfReady) return STATUS_INVALID_DEVICE_STATE;

    brb = g_If.BthAllocateBrb(BRB_HCI_GET_LOCAL_BD_ADDR, BTHL2CAP_TAG);
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    st = SubmitBrb(brb, 5000);
    if (NT_SUCCESS(st)) {
        *Out = brb->BrbGetLocalBdAddress.BtAddress;
    } else if (g_LogFn) {
        g_LogFn("local addr FAIL: status=0x%08X btStatus=0x%02X",
                brb->BrbGetLocalBdAddress.Hdr.Status,
                brb->BrbGetLocalBdAddress.Hdr.BtStatus);
    }
    g_If.BthFreeBrb(brb);
    return st;
}

// ------------------------------------------------------------- 打开通道 ---
NTSTATUS
BthL2capOpenEx(_In_ BTH_ADDR Addr, _In_ USHORT Psm, _Out_ PBTH_L2CAP_CHAN Chan,
              _In_ ULONG PollMs)
{
    NTSTATUS st;
    PBRB     brb;
    USHORT   mtu;

    if (!g_IfReady) return STATUS_INVALID_DEVICE_STATE;
    if (Chan == NULL) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Chan, sizeof(*Chan));
    Chan->BtAddress = Addr;

    //  signaling 通道跟 BthA2dp / sink 自己声明的一样用 679；
    //  媒体通道维持 1500（RTP 包需要更大）。
    mtu = (Psm == AVDTP_SIGNAL_PSM) ? L2CAP_MTU_SIGNALING : L2CAP_MTU_DEFAULT;

    brb = g_If.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL, BTHL2CAP_TAG);
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    brb->BrbL2caOpenChannel.BtAddress    = Addr;
    brb->BrbL2caOpenChannel.Psm          = Psm;
    brb->BrbL2caOpenChannel.ChannelFlags = g_L2capChannelFlags;

    brb->BrbL2caOpenChannel.ConfigOut.Flags = CFG_MTU;
    brb->BrbL2caOpenChannel.ConfigOut.Mtu.Min       = 48;
    brb->BrbL2caOpenChannel.ConfigOut.Mtu.Preferred = mtu;
    brb->BrbL2caOpenChannel.ConfigOut.Mtu.Max       = mtu;

    brb->BrbL2caOpenChannel.ConfigIn.Flags = CFG_MTU;
    brb->BrbL2caOpenChannel.ConfigIn.Mtu.Min       = 48;
    brb->BrbL2caOpenChannel.ConfigIn.Mtu.Preferred = mtu;
    brb->BrbL2caOpenChannel.ConfigIn.Mtu.Max       = mtu;

    //  这里原来写的是「不收 indication：AVDTP 是请求/应答，用读超时模式即可」——
    //  那是错的。不注册 CALLBACK_CONFIG_QOS 时 bthddi 会**主动拆链**
    //  （远端 config request 只要带 QOS 就断），而 CALLBACK_DISCONNECT 不设
    //  则对端断开我们毫无察觉。数据仍走就绪模式（ACL_TRANSFER IN）读，
    //  所以故意**不设** CALLBACK_RECV_PACKET，两套模式不混用。
    brb->BrbL2caOpenChannel.CallbackFlags    = L2CAP_CB_FLAGS;
    brb->BrbL2caOpenChannel.Callback         = LdacL2capIndication;
    brb->BrbL2caOpenChannel.CallbackContext  = NULL;
    //  ⚠ ReferenceObject 不是可选项！bthddi.h 对 BRB_L2CA_OPEN_CHANNEL 的这一
    //  字段原话："Object to be passed to ObReferenceObject, ObDereferenceObject.
    //  Note: If a callback is requested, this parameter is not optional."
    //  之前留 NULL 的后果（2026-11-03 实测）：只要 CallbackFlags 非 0，
    //  bthport 就在参数校验阶段**同步**把整个 BRB 打回 0xC000000D
    //  （STATUS_INVALID_PARAMETER，iocall 直接是失败值、根本不 pending），
    //  于是"重新拨一条干净通道"永远失败。CallbackFlags=0 时反而不校验这一项，
    //  所以旧代码能 open 成功 —— 这也是这个 bug 藏了这么久的原因。
    brb->BrbL2caOpenChannel.ReferenceObject  = (g_RefDo != NULL) ? (PVOID)g_RefDo : (PVOID)g_Pdo;
    brb->BrbL2caOpenChannel.IncomingQueueDepth = 4;

    st = SubmitBrb(brb, OPEN_TIMEOUT_MS);

    //  ========================================================================
    //  BRB_L2CA_OPEN_CHANNEL 是**异步**的。
    //  提交 IRP 会立刻以 SUCCESS 完成（bthport 只是"收下了"），真正的
    //  ChannelHandle / OutResults 稍后由协议栈写回 BRB。
    //
    //  原来的代码在 SubmitBrb 成功后直接取 ChannelHandle —— 拿到的是 0，
    //  却当成成功返回，后面 AVDTP 就拿 0 句柄发数据 -> 0xC000000D -> 全线失败。
    //  实测日志：handle=0x0 mtu=0 brbStatus=0x00000000 btStatus=0xFF
    //  （0xFF 是刚 BthAllocateBrb 出来时的哨兵值，说明一个字都没被写。）
    //
    //  所以这里轮询等结果。而且**拿不到句柄时绝对不能 BthFreeBrb** ——
    //  协议栈还可能往这块内存里写，释放了就是 use-after-free（0x35 那个老坑）。
    //  宁可泄漏一个 BRB。
    //  ========================================================================
    {
        LARGE_INTEGER iv;
        ULONG i = 0;
        LONGLONG t0 = (LONGLONG)KeQueryInterruptTime();   // 100ns
        LONGLONG budget = (LONGLONG)PollMs * 10000LL;

        //  ⚠ 这里必须按**墙上时间**收敛，不能按次数：
        //  KeDelayExecutionThread 的 1ms 相对延时会被系统 tick（默认 15.6ms）
        //  放大，8000 次名义上 8 秒、实测能把调用线程压两分钟。
        //  AVRCP 探测因此把 keeper 线程堵死过一次（回连全停）。
        iv.QuadPart = -10000LL;                 // 相对 1ms
        for (;;) {
            if (brb->BrbL2caOpenChannel.ChannelHandle != NULL) break;
            //  BtStatus 离开 0xFF 说明协议栈已经有结论了（成功或失败）
            if (brb->BrbL2caOpenChannel.Hdr.BtStatus != 0xFF &&
                brb->BrbL2caOpenChannel.Hdr.BtStatus != 0x00) break;
            if (!NT_SUCCESS(brb->BrbL2caOpenChannel.Hdr.Status) &&
                brb->BrbL2caOpenChannel.Hdr.Status != 0) break;
            if ((LONGLONG)KeQueryInterruptTime() - t0 >= budget) break;
            KeDelayExecutionThread(KernelMode, FALSE, &iv);
            i++;
        }

        if (g_LogFn) {
            //  Response/Psm 在 bthddi 里是同一个 union 位置：主动 open 时装的是
            //  Psm，只有回 OPEN_CHANNEL_RESPONSE 时才装 Response/ResponseStatus。
            //  之前这里打 Response，读到的其实就是 Psm，标签写错了。
            g_LogFn("open poll: %u ms  handle=0x%X mtu=%u outMtu=%u brb=0x%08X bt=0x%02X psm=%u respSt=%u cf=0x%X",
                    i, (ULONG)(ULONG_PTR)brb->BrbL2caOpenChannel.ChannelHandle,
                    brb->BrbL2caOpenChannel.OutResults.Params.Mtu,
                    brb->BrbL2caOpenChannel.InResults.Params.Mtu,
                    brb->BrbL2caOpenChannel.Hdr.Status,
                    brb->BrbL2caOpenChannel.Hdr.BtStatus,
                    (ULONG)brb->BrbL2caOpenChannel.Psm,
                    (ULONG)brb->BrbL2caOpenChannel.ResponseStatus,
                    g_L2capChannelFlags);
        }

        if (brb->BrbL2caOpenChannel.ChannelHandle == NULL) {
            //  没拿到句柄。可能是失败，也可能还在路上 —— 两种情况都不释放 BRB。
            if (NT_SUCCESS(st)) {
                st = NT_SUCCESS(brb->BrbL2caOpenChannel.Hdr.Status)
                         ? STATUS_IO_TIMEOUT
                         : brb->BrbL2caOpenChannel.Hdr.Status;
            }
            if (g_LogFn) {
                g_LogFn("open FAILED: st=0x%08X (BRB leaked on purpose)", st);
            }
            return st;
        }

        Chan->Handle = brb->BrbL2caOpenChannel.ChannelHandle;
        Chan->Mtu    = brb->BrbL2caOpenChannel.OutResults.Params.Mtu;
        Chan->Open   = TRUE;
        if (g_LogFn) {
            //  Flags 只有 CFG_MTU(0x1) 才说明「只协商了 MTU」，是我们请求的样子；
            //  如果对端还塞了 FLUSHTO/QOS，这里会多出 bit，说明它要求的比我们给的多。
            g_LogFn("open OK: handle=0x%X mtu=%u outFlags=0x%X inFlags=0x%X outMtu=%u inMtu=%u",
                    (ULONG)(ULONG_PTR)Chan->Handle, Chan->Mtu,
                    brb->BrbL2caOpenChannel.OutResults.Params.Flags,
                    brb->BrbL2caOpenChannel.InResults.Params.Flags,
                    brb->BrbL2caOpenChannel.OutResults.Params.Mtu,
                    brb->BrbL2caOpenChannel.InResults.Params.Mtu);
        }
    }

    g_If.BthFreeBrb(brb);
    return STATUS_SUCCESS;
}

//  默认预算 8 秒（A2DP 信令/传输都走这条；paging 慢的时候要留够）
NTSTATUS
BthL2capOpen(_In_ BTH_ADDR Addr, _In_ USHORT Psm, _Out_ PBTH_L2CAP_CHAN Chan)
{
    return BthL2capOpenEx(Addr, Psm, Chan, 8000);
}

// --------------------------------------------------------------- 发送 ---
//  ===========================================================================
//  ACL 传输的 Buffer 必须是**非分页内存**。
//
//  bthddi 的约定是 Buffer 或 BufferMDL 二选一，且内存要能被 HCI 层在
//  DISPATCH_LEVEL / DPC 里直接访问。调用方（Avdtp.c）传的是 `UCHAR pkt[1600]`
//  这种栈上数组 —— 内核栈是可分页的，换页之后协议栈再碰就是
//  PAGE_FAULT_IN_NONPAGED_AREA。所以这里统一自己拷一份非分页副本。
//  ===========================================================================
static PVOID
AclAlloc(_In_ ULONG Len)
{
    if (Len == 0) return NULL;
    return ExAllocatePoolWithTag(NonPagedPoolNx, (SIZE_T)Len, BTHL2CAP_TAG);
}

NTSTATUS
BthL2capSend(_In_ PBTH_L2CAP_CHAN Chan, _In_reads_(Len) const UCHAR* Buf, _In_ ULONG Len)
{
    NTSTATUS st;
    PBRB     brb;
    PUCHAR   tmp;

    if (!g_IfReady || Chan == NULL || !Chan->Open) return STATUS_INVALID_DEVICE_STATE;
    if (Buf == NULL || Len == 0) return STATUS_INVALID_PARAMETER;

    tmp = (PUCHAR)AclAlloc(Len);
    if (tmp == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(tmp, Buf, Len);

    brb = g_If.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, BTHL2CAP_TAG);
    if (brb == NULL) {
        ExFreePoolWithTag(tmp, BTHL2CAP_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    brb->BrbL2caAclTransfer.BtAddress     = Chan->BtAddress;
    brb->BrbL2caAclTransfer.ChannelHandle = Chan->Handle;
    brb->BrbL2caAclTransfer.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    brb->BrbL2caAclTransfer.BufferSize    = Len;
    brb->BrbL2caAclTransfer.Buffer        = tmp;
    brb->BrbL2caAclTransfer.BufferMDL     = NULL;
    brb->BrbL2caAclTransfer.Timeout       = 0;

    st = SubmitBrb(brb, 5000);
    if (!NT_SUCCESS(st) && g_LogFn) {
        g_LogFn("send FAIL: status=0x%08X btStatus=0x%02X handle=0x%X len=%u",
                brb->BrbL2caAclTransfer.Hdr.Status, brb->BrbL2caAclTransfer.Hdr.BtStatus,
                (ULONG)(ULONG_PTR)Chan->Handle, Len);
    }
    g_If.BthFreeBrb(brb);
    ExFreePoolWithTag(tmp, BTHL2CAP_TAG);
    return st;
}

// --------------------------------------------------------------- 接收 ---
NTSTATUS
BthL2capRecv(_In_ PBTH_L2CAP_CHAN Chan, _Out_writes_(Cap) UCHAR* Buf,
             _In_ ULONG Cap, _In_ ULONG TimeoutMs, _Out_ PULONG Got)
{
    NTSTATUS st;
    PBRB     brb;
    PUCHAR   tmp;
    ULONG    got = 0;

    *Got = 0;
    if (!g_IfReady || Chan == NULL || !Chan->Open) return STATUS_INVALID_DEVICE_STATE;
    if (Buf == NULL || Cap == 0) return STATUS_INVALID_PARAMETER;

    tmp = (PUCHAR)AclAlloc(Cap);
    if (tmp == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    brb = g_If.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, BTHL2CAP_TAG);
    if (brb == NULL) {
        ExFreePoolWithTag(tmp, BTHL2CAP_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    brb->BrbL2caAclTransfer.BtAddress     = Chan->BtAddress;
    brb->BrbL2caAclTransfer.ChannelHandle = Chan->Handle;
    brb->BrbL2caAclTransfer.TransferFlags =
        ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK | ACL_TRANSFER_TIMEOUT;
    brb->BrbL2caAclTransfer.BufferSize    = Cap;
    brb->BrbL2caAclTransfer.Buffer        = tmp;
    brb->BrbL2caAclTransfer.BufferMDL     = NULL;
    brb->BrbL2caAclTransfer.Timeout       = (LONGLONG)TimeoutMs;

    if (g_LogFn) {
        //  提交前把 bthport 真正会看到的参数原样记一遍。
        //  出问题时这几行是唯一能区分「我们填错了」和「bthport 拒绝了」的证据。
        g_LogFn("recv submit: type=0x%04X len=%u addr=%p handle=0x%X flags=0x%X size=%u to=%d cap=%u",
                (ULONG)brb->BrbL2caAclTransfer.Hdr.Type,
                (ULONG)brb->BrbL2caAclTransfer.Hdr.Length,
                (PVOID)(ULONG_PTR)Chan->BtAddress,
                (ULONG)(ULONG_PTR)Chan->Handle,
                brb->BrbL2caAclTransfer.TransferFlags,
                brb->BrbL2caAclTransfer.BufferSize,
                (LONG)brb->BrbL2caAclTransfer.Timeout,
                Cap);
    }

    st = SubmitBrb(brb, TimeoutMs + 2000);
    if (NT_SUCCESS(st)) {
        //  WDK：BufferSize 是 [IN/OUT]，完成时被更新为实际读到的字节数；
        //  RemainingBufferSize 只在 buffer underrun 时有意义（否则留着 0）。
        //  用 Cap - RemainingBufferSize 会在成功路径上恒等于 Cap（读到一屏垃圾）。
        got = brb->BrbL2caAclTransfer.BufferSize;
        if (got > Cap) got = Cap;
        RtlCopyMemory(Buf, tmp, got);
        *Got = got;
    } else if (g_LogFn) {
        //  读失败时把 bthport 自己的判词全部打出来。只往上抛 NTSTATUS
        //  等于把"设备错误"这四个字扔给调用方，查不动。
        //  注意：格式化器里 l 修饰符按 64 位取参，所以 32 位字段一律用不带 l 的 %u/%d。
        g_LogFn("recv FAIL: st=0x%08X brb=0x%08X bt=0x%02X cap=%u inBufSz=%u rem=%u handle=0x%X flags=0x%X to=%d",
                st,
                brb->BrbL2caAclTransfer.Hdr.Status,
                brb->BrbL2caAclTransfer.Hdr.BtStatus,
                Cap,
                brb->BrbL2caAclTransfer.BufferSize,
                brb->BrbL2caAclTransfer.RemainingBufferSize,
                (ULONG)(ULONG_PTR)Chan->Handle,
                brb->BrbL2caAclTransfer.TransferFlags,
                (LONG)brb->BrbL2caAclTransfer.Timeout);
    }
    g_If.BthFreeBrb(brb);
    ExFreePoolWithTag(tmp, BTHL2CAP_TAG);
    return st;
}

// ===================================================== 服务端：接住拨入 ===
//  设备（JieLi 等）连接后会主动拨 PSM 25。必须接住它的握手——不接住的话，
//  它的入站请求被其它持有通配 PSM 25 的驱动（如 inbox btha2dp）晾掉，
//  我们再主动 page 它只会撞 page timeout（HCI status 0x04，HCI 抓包实锤）。

static L2CAP_SERVER_HANDLE g_SrvHandle = NULL;
static BTH_ADDR            g_SrvAddr   = 0;      // 注册时用的远端地址（注销必须原样传回）
static PDEVICE_OBJECT      g_WorkDo    = NULL;
static BTH_L2CAP_CHAN      g_Inbound;            // 设备拨入的第 1 条（AVDTP 信令）
static KEVENT              g_InboundEvent;
//  实测 UGREEN-BT505 会**自己拨两条 PSM 25**：第一条当 AVDTP 信令，第二条就是
//  A2DP 的传输通道（抓 cold2.pcap：四条 Connection Request 方向全是 IN）。
//  BthA2dp 也是直接用这条拨入通道发 RTP，而不是自己开 client 通道 ——
//  我们原来自己 open 的那条，sink 接了但根本不往里收数据（数据进黑洞、静音）。
static BTH_L2CAP_CHAN      g_Inbound2;           // 设备拨入的第 2 条（传输通道）
static KEVENT              g_Inbound2Event;
static BOOLEAN             g_SrvInit = FALSE;

struct ACCEPT_WORK {
    PIO_WORKITEM         Item;
    BTH_ADDR             Addr;
    L2CAP_CHANNEL_HANDLE Handle;
};
static struct ACCEPT_WORK g_Accept;

static VOID
LdacL2capIndication(_In_opt_ PVOID Context,
                    _In_ INDICATION_CODE Code,
                    _In_ PINDICATION_PARAMETERS Params);

#ifndef AVDTP_SIGNAL_PSM
#define AVDTP_SIGNAL_PSM 25
#endif

// --------------------------------------------------------------- 关闭 ---
VOID
BthL2capClose(_In_ PBTH_L2CAP_CHAN Chan)
{
    NTSTATUS st;
    PBRB     brb;

    if (!g_IfReady || Chan == NULL || !Chan->Open) return;

    // 若关的是拨入通道，同步服务端状态
    if (g_SrvInit && g_Inbound.Open && Chan->Handle == g_Inbound.Handle) {
        g_Inbound.Open   = FALSE;
        g_Inbound.Handle = NULL;
    }

    brb = g_If.BthAllocateBrb(BRB_L2CA_CLOSE_CHANNEL, BTHL2CAP_TAG);
    if (brb != NULL) {
        brb->BrbL2caCloseChannel.BtAddress     = Chan->BtAddress;
        brb->BrbL2caCloseChannel.ChannelHandle = Chan->Handle;
        st = SubmitBrb(brb, 5000);
        UNREFERENCED_PARAMETER(st);
        g_If.BthFreeBrb(brb);
    }
    Chan->Open = FALSE;
    Chan->Handle = 0;
}

//  拨入通道 = OPEN_CHANNEL_RESPONSE 建立的，带 ReferenceObject（控制设备）。
//  只注销 server 不关它，引用会挂在控制设备上：设备对象删不掉、驱动对象
//  引用不归零、映像永远不卸载（热重载"换了包跑旧码"的根）。
VOID
BthL2capCloseInbound(VOID)
{
    if (g_Inbound.Open) {
        if (g_LogFn)
            g_LogFn("close inbound: handle=0x%X", (ULONG)(ULONG_PTR)g_Inbound.Handle);
        BthL2capClose(&g_Inbound);
    }
}

// ===================================================== 服务端：接住拨入 ===
//  （全局变量与回调实现见上方；此处为注册/接受/取通道的逻辑）

// 系统工作项（PASSIVE_LEVEL）：异步发 OPEN_CHANNEL_RESPONSE 接受拨入。
// 绝不能在 indication 回调里同步等 BRB——那会卡死整个蓝牙栈线程。
static VOID
LdacAcceptWorker(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    PBRB         brb;
    NTSTATUS     st;
    PIO_WORKITEM item = g_Accept.Item;

    UNREFERENCED_PARAMETER(Context);

    g_Accept.Item = NULL;

    if (!g_IfReady || item == NULL) {
        if (item) IoFreeWorkItem(item);
        return;
    }

    brb = g_If.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL_RESPONSE, BTHL2CAP_TAG);
    if (brb != NULL) {
        brb->BrbL2caOpenChannel.ChannelHandle  = g_Accept.Handle; // = indication 的 ConnectionHandle
        brb->BrbL2caOpenChannel.BtAddress      = g_Accept.Addr;
        //  拒的模式下直接回一个负面结果，让对端的 AVDTP 干干净净地失败，
        //  而不是"拨进来了、说了半句话、然后两边都卡住"。
        brb->BrbL2caOpenChannel.Response       = g_L2capRejectInbound
                                                   ? CONNECT_RSP_RESULT_NO_RESOURCES
                                                   : CONNECT_RSP_RESULT_SUCCESS; // union 覆盖 Psm
        brb->BrbL2caOpenChannel.ChannelFlags   = 0;
        brb->BrbL2caOpenChannel.ConfigIn.Flags = 0;   // 全盘接受远端配置
        brb->BrbL2caOpenChannel.CallbackFlags  = L2CAP_CB_FLAGS;
        brb->BrbL2caOpenChannel.Callback       = LdacL2capIndication;
        brb->BrbL2caOpenChannel.CallbackContext = NULL;
        brb->BrbL2caOpenChannel.ReferenceObject = DeviceObject;
        brb->BrbL2caOpenChannel.IncomingQueueDepth = 10;

        st = SubmitBrb(brb, 5000);
        if (NT_SUCCESS(st) && g_L2capRejectInbound) {
            if (g_LogFn)
                g_LogFn("inbound REJECTED on purpose (RejectInbound=1) addr=0x%X%08X",
                        (ULONG)(g_Accept.Addr >> 32), (ULONG)(g_Accept.Addr & 0xFFFFFFFF));
        } else if (NT_SUCCESS(st)) {
            PBTH_L2CAP_CHAN slot;
            PKEVENT         evt;

            //  第一条 = 信令；第二条 = 传输通道（sink 自己拨进来的）
            if (!g_Inbound.Open) {
                slot = &g_Inbound;
                evt  = &g_InboundEvent;
            } else {
                slot = &g_Inbound2;
                evt  = &g_Inbound2Event;
            }
            slot->BtAddress = g_Accept.Addr;
            slot->Handle    = brb->BrbL2caOpenChannel.ChannelHandle;
            slot->Mtu       = (USHORT)brb->BrbL2caOpenChannel.OutResults.Params.Mtu;
            slot->Open      = TRUE;
            KeSetEvent(evt, IO_NO_INCREMENT, FALSE);
            if (g_LogFn)
                g_LogFn("inbound ACCEPTED (%s): handle=0x%p mtu=%u addr=0x%X%08X",
                        (slot == &g_Inbound) ? "signaling" : "transport",
                        (ULONG)(ULONG_PTR)slot->Handle, slot->Mtu,
                        (ULONG)(g_Accept.Addr >> 32), (ULONG)(g_Accept.Addr & 0xFFFFFFFF));
        } else if (g_LogFn) {
            g_LogFn("inbound accept FAILED 0x%08X btStatus=0x%02X", st,
                    brb->BrbL2caOpenChannel.Hdr.BtStatus);
        }
        g_If.BthFreeBrb(brb);
    }
    IoFreeWorkItem(item);
}

//  Driver.c 里的会话通道（断开指示里要按 handle 清掉）
extern BTH_L2CAP_CHAN g_SigChan;
extern BTH_L2CAP_CHAN g_MediaChan;
extern LONG           g_State;        // 0 = IDLE（keeper 靠它决定要不要重建会话）

//  ---------------------------------------------------------- 常驻信令通道 ---
//  设备只在"有 L2CAP 连接建立"时才提示已连接。之前这条连接由 user-mode 的
//  send 打开，所以 send 不跑 = 设备永远不连。这里改成驱动自己拨号并一直持有，
//  发送器启动时直接接管它（Take）。
static BTH_L2CAP_CHAN g_HoldChan;

NTSTATUS
BthL2capHoldSignaling(_In_ BTH_ADDR Addr)
{
    NTSTATUS st;

    if (g_HoldChan.Open) {
        if (g_LogFn) g_LogFn("keeper: already holding handle=0x%X",
                             (ULONG)(ULONG_PTR)g_HoldChan.Handle);
        return STATUS_SUCCESS;
    }

    RtlZeroMemory(&g_HoldChan, sizeof(g_HoldChan));
    st = BthL2capOpen(Addr, AVDTP_SIGNAL_PSM, &g_HoldChan);
    if (g_LogFn)
        g_LogFn("keeper: hold signaling -> 0x%08X handle=0x%X mtu=%u",
                st, (ULONG)(ULONG_PTR)g_HoldChan.Handle, g_HoldChan.Mtu);

    //  拨号会建立 ACL 并让设备提示"已连接"；失败也不要紧，server 还在，
    //  设备自己回连时我们照样能接。
    return st;
}

VOID
BthL2capGiveBackHeld(_In_ PBTH_L2CAP_CHAN Chan)
{
    if (Chan == NULL || !Chan->Open) return;
    if (g_HoldChan.Open) return;                 // 已经有就不覆盖
    g_HoldChan = *Chan;
    if (g_LogFn) g_LogFn("keeper: channel given back, handle=0x%X",
                         (ULONG)(ULONG_PTR)g_HoldChan.Handle);
}

BOOLEAN
BthL2capIsHolding(VOID)
{
    return g_HoldChan.Open ? TRUE : FALSE;
}

BOOLEAN
BthL2capTakeHeld(_Out_ PBTH_L2CAP_CHAN Out)
{
    if (!g_HoldChan.Open) return FALSE;
    *Out = g_HoldChan;
    RtlZeroMemory(&g_HoldChan, sizeof(g_HoldChan));
    if (g_LogFn) g_LogFn("keeper: signaling handed over, handle=0x%X",
                         (ULONG)(ULONG_PTR)Out->Handle);
    return TRUE;
}

// L2CAP 指示回调：可能在 DISPATCH_LEVEL，绝不阻塞
static VOID
LdacL2capIndication(_In_opt_ PVOID Context,
                    _In_ INDICATION_CODE Code,
                    _In_ PINDICATION_PARAMETERS Params)
{
    UNREFERENCED_PARAMETER(Context);

    switch (Code) {
    case IndicationRemoteConnect:
        if (g_LogFn)
            g_LogFn("indication: REMOTE CONNECT addr=0x%X%08X handle=0x%p psm=%u",
                    (ULONG)(Params->BtAddress >> 32),
                    (ULONG)(Params->BtAddress & 0xFFFFFFFF),
                    (ULONG)(ULONG_PTR)Params->ConnectionHandle,
                    (ULONG)Params->Parameters.Connect.Request.PSM);
        // 记下参数，排队异步接受（同一时刻只处理一个 pending）
        if (g_Accept.Item == NULL && g_WorkDo != NULL) {
            g_Accept.Addr   = Params->BtAddress;
            g_Accept.Handle = Params->ConnectionHandle;
            g_Accept.Item   = IoAllocateWorkItem(g_WorkDo);
            if (g_Accept.Item != NULL) {
                IoQueueWorkItem(g_Accept.Item, LdacAcceptWorker,
                                DelayedWorkQueue, NULL);
            }
        } else if (g_LogFn) {
            g_LogFn("indication: connect IGNORED (busy/none)");
        }
        break;

    case IndicationRemoteDisconnect:
        //  这条之前根本收不到（CallbackFlags=0），于是「对端已经拆链」这件事
        //  在我们这边完全不可见：g_Inbound.Open 还是 TRUE，handle 照样往下用，
        //  下一次 send 就是 0xC000009D（STATUS_DEVICE_NOT_CONNECTED）。
        InterlockedIncrement(&g_L2capDrops);
        if (g_LogFn)
            g_LogFn("indication: DISCONNECT handle=0x%p reason=%d closeNow=%u drops=%d",
                    (ULONG)(ULONG_PTR)Params->ConnectionHandle,
                    (LONG)Params->Parameters.Disconnect.Reason,
                    (ULONG)Params->Parameters.Disconnect.CloseNow,
                    (LONG)g_L2capDrops);
        //  所有通道都要按 handle 清！只清 g_Inbound 不够：设备断开后
        //  g_SigChan / g_MediaChan 还标着 Open，句柄已经失效，后续 open/send
        //  直接用死句柄 -> 0xC000009D，只能重启恢复（"断开后再也连不上"）。
        {
            PBTH_L2CAP_CHAN chans[4];
            ULONG i;
            chans[0] = &g_Inbound;
            chans[1] = &g_Inbound2;
            chans[2] = &g_SigChan;
            chans[3] = &g_MediaChan;
            for (i = 0; i < 4; i++) {
                if (chans[i]->Open && chans[i]->Handle == Params->ConnectionHandle) {
                    chans[i]->Open   = FALSE;
                    chans[i]->Handle = NULL;
                    if (g_LogFn) g_LogFn("indication: cleared %u-th channel (handle=0x%p)",
                                         i, (ULONG_PTR)Params->ConnectionHandle);
                }
            }
            //  ⚠ 状态必须一起复位！否则 keeper 看到还是 STREAMING 就不再重建，
            //  设备关机再开机后就再也连不上（只能重启 VM）。
            InterlockedExchange(&g_State, 0);
            if (g_LogFn) g_LogFn("indication: state -> IDLE (keeper may rebuild)");
        }
        break;

    case IndicationRemoteConfigRequest:
        //  远端（耳机）发来的 L2CAP 配置请求。带 QOS 时如果不设
        //  CALLBACK_CONFIG_QOS，bthddi 会直接拆链。
        if (g_LogFn)
            g_LogFn("indication: CONFIG REQ handle=0x%p askedFlags=0x%X resp=%u",
                    (ULONG)(ULONG_PTR)Params->ConnectionHandle,
                    Params->Parameters.ConfigRequest.RequestedParams.Flags,
                    (ULONG)Params->Parameters.ConfigRequest.Response);
        break;

    case IndicationRemoteConfigResponse:
        if (g_LogFn)
            g_LogFn("indication: CONFIG RSP handle=0x%p resp=%u rejectedFlags=0x%X unknown=%u",
                    (ULONG)(ULONG_PTR)Params->ConnectionHandle,
                    (ULONG)Params->Parameters.ConfigResponse.Response,
                    Params->Parameters.ConfigResponse.RejectedParams.Flags,
                    Params->Parameters.ConfigResponse.NumUnknownTypes);
        break;

    case IndicationRecvPacket:
        //  只有设了 CALLBACK_RECV_PACKET 才会来；这里留着当「模式混了」的报警。
        if (g_LogFn)
            g_LogFn("indication: RECV PACKET handle=0x%p len=%u qlen=%u",
                    (ULONG)(ULONG_PTR)Params->ConnectionHandle,
                    Params->Parameters.RecvPacket.PacketLength,
                    Params->Parameters.RecvPacket.TotalQueueLength);
        break;

    default:
        // AddReference/ReleaseReference：引用已由 ReferenceObject 托管
        break;
    }
}

NTSTATUS
BthL2capRegisterServer(_In_ PDEVICE_OBJECT RefDevice, _In_ BTH_ADDR RemoteAddr)
{
    PBRB     brb;
    NTSTATUS st;

    if (!g_IfReady) return STATUS_INVALID_DEVICE_STATE;

    //  RefDevice 应当传「控制设备」（DriverEntry 创建，生命周期等于驱动），
    //  不要传 A2DP 设备的 FDO——那个对象会随设备重枚举被删除，回调就投递到
    //  已释放的对象上（表现为「注册成功但永远接不到拨入」）。
    if (g_SrvHandle != NULL) {
        if (g_RefDo == RefDevice) {
            if (g_LogFn) g_LogFn("server already registered, skip duplicate (srv=0x%p)",
                                 (ULONG)(ULONG_PTR)g_SrvHandle);
            return STATUS_SUCCESS;
        }
        //  reference 换了：旧注册绑在失效对象上，先摘掉再重挂
        if (g_LogFn) g_LogFn("server reference changed, re-registering");
        BthL2capUnregisterServer();
    }
    g_RefDo = RefDevice;

    g_WorkDo = RefDevice;
    RtlZeroMemory(&g_Inbound, sizeof(g_Inbound));
    KeInitializeEvent(&g_InboundEvent, NotificationEvent, FALSE);
    RtlZeroMemory(&g_Inbound2, sizeof(g_Inbound2));
    KeInitializeEvent(&g_Inbound2Event, NotificationEvent, FALSE);
    g_SrvInit = TRUE;

    // 「特定远端 + PSM」注册：通配 PSM 25 可能被 inbox btha2dp 占着，
    // 特定配对可与其它驱动的通配注册共存（MS 文档明确允许）
    brb = g_If.BthAllocateBrb(BRB_L2CA_REGISTER_SERVER, BTHL2CAP_TAG);
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    brb->BrbL2caRegisterServer.BtAddress   = RemoteAddr;
    brb->BrbL2caRegisterServer.PSM         = AVDTP_SIGNAL_PSM;
    brb->BrbL2caRegisterServer.IndicationFlags = 0;
    brb->BrbL2caRegisterServer.IndicationCallback = LdacL2capIndication;
    brb->BrbL2caRegisterServer.IndicationCallbackContext = NULL;
    brb->BrbL2caRegisterServer.ReferenceObject = RefDevice;

    st = SubmitBrb(brb, 5000);
    if (NT_SUCCESS(st)) {
        g_SrvAddr   = RemoteAddr;          // 注销时要原样传回，别丢
        g_SrvHandle = brb->BrbL2caRegisterServer.ServerHandle;
        if (g_LogFn)
            g_LogFn("server registered: specific remote PSM %u, srv=0x%p",
                    AVDTP_SIGNAL_PSM, (ULONG)(ULONG_PTR)g_SrvHandle);

        //  关键第二步：REGISTER_SERVER 只是挂回调，还得把 PSM 本身登记给栈。
        //  不登记的话栈会把该 PSM 的所有入站连接当「未知 PSM」拒掉，回调永远不响。
        //  MS 文档 Accepting L2CAP Connections 原话：
        //    "...it should then build and send a BRB_REGISTER_PSM request so the
        //     Bluetooth driver stack will accept connections from the PSM."
        {
            PBRB     psmBrb;
            NTSTATUS stPsm;

            psmBrb = g_If.BthAllocateBrb(BRB_REGISTER_PSM, BTHL2CAP_TAG);
            if (psmBrb == NULL) {
                st = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                psmBrb->BrbPsm.Psm = AVDTP_SIGNAL_PSM;
                stPsm = SubmitBrb(psmBrb, 5000);
                g_If.BthFreeBrb(psmBrb);
                if (NT_SUCCESS(stPsm)) {
                    if (g_LogFn) g_LogFn("PSM %u registered with stack", AVDTP_SIGNAL_PSM);
                } else {
                    if (g_LogFn) g_LogFn("register PSM %u FAILED 0x%08X",
                                         AVDTP_SIGNAL_PSM, stPsm);
                    st = stPsm;
                }
            }
        }
    } else if (g_LogFn) {
        g_LogFn("register server FAILED 0x%08X", st);
    }
    g_If.BthFreeBrb(brb);
    return st;
}

VOID
BthL2capUnregisterServer(VOID){
    PBRB     brb;
    NTSTATUS st;

    if (!g_SrvInit || !g_IfReady) {
        if (g_LogFn) g_LogFn("unregister: skipped (srvInit=%u ifReady=%u)",
                             (ULONG)g_SrvInit, (ULONG)g_IfReady);
        return;
    }

    brb = g_If.BthAllocateBrb(BRB_L2CA_UNREGISTER_SERVER, BTHL2CAP_TAG);
    if (brb != NULL) {
        //  ⚠ 必须用**注册时的那个远端地址**，不能填 0。
        //  注册是「特定远端 + PSM」，注销时 bthport 要按 (地址, PSM) 找那条记录；
        //  填 0 找不到 -> 注销失败（而且它不一定报错），服务端注册连同它持有的
        //  ReferenceObject 引用一起留着 -> 控制设备删了也卸不掉驱动映像。
        //  这正是 2026-11-03 热重载"闸门"卡住的直接原因。
        brb->BrbL2caUnregisterServer.BtAddress    = g_SrvAddr;
        brb->BrbL2caUnregisterServer.ServerHandle = g_SrvHandle;
        brb->BrbL2caUnregisterServer.Psm          = AVDTP_SIGNAL_PSM;
        st = SubmitBrb(brb, 5000);
        if (g_LogFn)
            g_LogFn("unregister server: st=0x%08X brb=0x%08X addr=0x%X%08X",
                    st, brb->BrbL2caUnregisterServer.Hdr.Status,
                    (ULONG)(g_SrvAddr >> 32), (ULONG)(g_SrvAddr & 0xFFFFFFFF));
        g_If.BthFreeBrb(brb);
    }

    //  对应 RegisterServer 里那次 BRB_REGISTER_PSM
    brb = g_If.BthAllocateBrb(BRB_UNREGISTER_PSM, BTHL2CAP_TAG);
    if (brb != NULL) {
        brb->BrbPsm.Psm = AVDTP_SIGNAL_PSM;
        st = SubmitBrb(brb, 5000);
        if (g_LogFn) g_LogFn("unregister PSM %u: st=0x%08X", AVDTP_SIGNAL_PSM, st);
        g_If.BthFreeBrb(brb);
    }

    //  ⚠ 只清句柄，**保留 g_SrvAddr / g_RefDo**！
    //  我们开媒体通道前会先 UnregisterServer，开完再 RegisterServer 挂回去。
    //  如果这里把地址和引用清零，重新注册拿到的是 (NULL, 0) —— 静默失败，
    //  server 永久消失，设备回连时没人监听（灯 5 秒一闪，永远连不上）。
    g_SrvHandle = NULL;
    g_SrvInit   = FALSE;
}

BOOLEAN
BthL2capIsInbound(_In_ L2CAP_CHANNEL_HANDLE Handle)
{
    return (g_Inbound.Open && Handle != NULL && g_Inbound.Handle == Handle) ? TRUE : FALSE;
}

NTSTATUS
BthL2capAcquireSignaling(_In_ BTH_ADDR Addr, _Out_ PBTH_L2CAP_CHAN Chan)
{
    NTSTATUS     st;
    LARGE_INTEGER to;

    // 1) 设备已经拨进来了（驱动加载早于/伴随设备连接时常见）
    if (g_Inbound.Open && g_Inbound.BtAddress == Addr) {
        *Chan = g_Inbound;
        if (g_LogFn)
            g_LogFn("signaling: using inbound channel (mtu=%u)", g_Inbound.Mtu);
        return STATUS_SUCCESS;
    }

    // 2) 等它拨（连接后几秒内必来；「连接成功」提示音就是这时响的）
    if (g_SrvInit) {
        KeClearEvent(&g_InboundEvent);
        to.QuadPart = -(LONGLONG)8000 * 10000;   // 8s
        KeWaitForSingleObject(&g_InboundEvent, Executive, KernelMode, FALSE, &to);
        // 无论 wait 结果如何都复查：indication 可能在 clear 前刚到
        if (g_Inbound.Open && g_Inbound.BtAddress == Addr) {
            *Chan = g_Inbound;
            if (g_LogFn)
                g_LogFn("signaling: inbound channel arrived (mtu=%u)", g_Inbound.Mtu);
            return STATUS_SUCCESS;
        }
        if (g_LogFn) g_LogFn("signaling: no inbound dial, falling back to outbound");
    }

    // 3) 回退：主动拨出
    st = BthL2capOpen(Addr, AVDTP_SIGNAL_PSM, Chan);
    return st;
}

//  取传输通道：**优先用设备自己拨进来的第二条 PSM 25**（抓包实证：sink 主动发起），
//  等不到才退回自己拨。自己拨的那条 sink 虽然回 Success，但不会往里收 RTP。
NTSTATUS
BthL2capAcquireMedia(_In_ BTH_ADDR Addr, _Out_ PBTH_L2CAP_CHAN Chan)
{
    LARGE_INTEGER to;
    NTSTATUS      st;

    RtlZeroMemory(Chan, sizeof(*Chan));

    if (g_Inbound2.Open && g_Inbound2.BtAddress == Addr) {
        *Chan = g_Inbound2;
        if (g_LogFn)
            g_LogFn("transport: using sink-dialed channel (mtu=%u)", g_Inbound2.Mtu);
        return STATUS_SUCCESS;
    }

    if (g_SrvInit) {
        KeClearEvent(&g_Inbound2Event);
        to.QuadPart = -(LONGLONG)6000 * 10000;      // 6s
        KeWaitForSingleObject(&g_Inbound2Event, Executive, KernelMode, FALSE, &to);
        if (g_Inbound2.Open && g_Inbound2.BtAddress == Addr) {
            *Chan = g_Inbound2;
            if (g_LogFn)
                g_LogFn("transport: sink-dialed channel arrived (mtu=%u)", g_Inbound2.Mtu);
            return STATUS_SUCCESS;
        }
    }

    if (g_LogFn) g_LogFn("transport: no inbound transport, falling back to outbound open");

    //  ⚠ 关键：本地还挂着 PSM 25 的 server 时，这次 client open 会被 bthport
    //  **本地满足**（接到自己的 server 上）——空口上没有任何 Connection Request，
    //  拿到的 handle 有效、send 也"成功"，但数据根本不上链路。
    //  实测（2026-11-05）：抓包从会话开头录，全程只有 1 条 Connection Request
    //  （信令 PSM 25），媒体通道那条从来没出现过，设备自然永远不出声。
    //  所以这里先把 server 注销，让 open 真正走空口。
    BthL2capUnregisterServer();

    st = BthL2capOpen(Addr, AVDTP_SIGNAL_PSM, Chan);

    //  ⚠ 无论如何都要把 server 挂回去！只在成功时挂的话，一旦这次 open 失败
    //  （设备刚断/在重连），server 就永久没了，之后设备再怎么回连都没人监听
    //  —— 表现就是"断一次以后再也连不上，只能重启"。
    if (g_SrvAddr != 0 && g_RefDo != NULL) {
        NTSTATUS rs = BthL2capRegisterServer(g_RefDo, g_SrvAddr);
        if (g_LogFn)
            g_LogFn("server re-registered after media open attempt: 0x%08X (open=0x%08X)",
                    rs, st);
    }
    return st;
}

// ---------------------------------------------------------------------------
//  重建信令通道
//
//  实测行为：耳机主动拨入 PSM 25 -> 发一条 Discover 探我们 -> 我们应答 ->
//  它就把这条通道丢掉了（紧接着 send 就是 0xC000009D STATUS_DEVICE_NOT_CONNECTED）。
//  也就是说「拨入通道」只够应答一次探测；真正的 AVDTP 协商必须由 source
//  自己拨出一条干净的通道来做（这点和安卓一致：协商永远是 source 发起）。
//
//  必须先显式 CLOSE 旧 handle：不关的话 bthport 认为 PSM 25 上还挂着我们的
//  通道，主动 open 会直接撞 "channel already exists"。关完再 open，失败重试。
// ---------------------------------------------------------------------------
NTSTATUS
BthL2capReopenSignaling(_In_ BTH_ADDR Addr, _Out_ PBTH_L2CAP_CHAN Chan)
{
    NTSTATUS      st = STATUS_IO_TIMEOUT;
    LARGE_INTEGER d;
    ULONG         i;

    BthL2capClose(Chan);
    RtlZeroMemory(Chan, sizeof(*Chan));

    d.QuadPart = -2000000LL;        // 相对 200ms（重开信令重试，回连要快）

    for (i = 0; i < 12; i++) {      // 最多约 6 秒
        st = BthL2capOpen(Addr, AVDTP_SIGNAL_PSM, Chan);
        if (NT_SUCCESS(st)) {
            if (g_LogFn)
                g_LogFn("reopen: signaling OK after %u try(s), handle=0x%X mtu=%u",
                        i, (ULONG)(ULONG_PTR)Chan->Handle, Chan->Mtu);
            return STATUS_SUCCESS;
        }
        if (g_LogFn) g_LogFn("reopen: try %u failed 0x%08X", i, st);
        KeDelayExecutionThread(KernelMode, FALSE, &d);
        RtlZeroMemory(Chan, sizeof(*Chan));
    }
    return st;
}

