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
#include "Raii.h"
#include <aux_klib.h>

PPROBE_LOG g_LogFn = NULL;

//  逐包日志开关（注册表 Parameters\VerboseLog=1 打开）。
//  默认关的理由：流媒体时每个 BRB 两条日志 ≈ 374 次内核文件写/秒，
//  而这个驱动是从 System 进程的内核线程里写 —— 代码里 KsAdapter 那段
//  注释已经记录过这个模式把整机拖死。日志只留状态变化。
BOOLEAN g_VerboseLog = FALSE;

//  KsAdapter 里缓存的 BTHENUM A2DP PDO（AddDevice 时写入）。
//  设备移除后必须由 Detach 清掉 —— 否则 IOCTL 路径会拿着悬空指针发 IRP。
extern "C" PDEVICE_OBJECT g_LdacBthPdo;


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

//  在飞的 BRB 数量。SubmitBrb 进出成对加减；卸载/关通道前应当等它归零，
//  否则就是 0xCE（驱动带着未完成操作被卸载）—— 实测过一次。
static volatile LONG g_PendingBrbs = 0;
static volatile LONG g_Shutdown = 0;      // 1 = 正在卸载：新 BRB 一律拒绝

VOID
BthL2capBeginShutdown(VOID)
{
    InterlockedExchange(&g_Shutdown, 1);
}

//  接口函数指针是否还指向"活着的模块"。
//  设备被移除后 bthport 提供的那些指针会变成野指针，keeper 却还在按 300ms
//  的节奏调它们 —— 内核 CFG（间接调用保护）会当场 0x139 (subcode 0xA)。
//  RtlPcToFileHeader 对已卸载模块的地址返回 NULL，正好用来做这个探针。
static BOOLEAN
IfPointersAlive(VOID)
{
    ULONG                    size = 0;
    ULONG                    count;
    PAUX_MODULE_EXTENDED_INFO mods;
    ULONG                    i;
    ULONG_PTR                target;
    BOOLEAN                  alive = FALSE;

    if (g_If.BthAllocateBrb == NULL) return FALSE;

    //  RtlPcToFileHeader 内核不导出，用 aux_klib 的模块表判断指针还活不活。
    if (!NT_SUCCESS(AuxKlibInitialize())) return TRUE;   // 查不了就别误杀

    if (!NT_SUCCESS(AuxKlibQueryModuleInformation(&size, sizeof(AUX_MODULE_EXTENDED_INFO), NULL))
        || size == 0) {
        return TRUE;
    }

    PoolGuard modsMem(ExAllocatePoolWithTag(NonPagedPool, size, 'modL'), 'modL');
    if (modsMem.Get() == NULL) return TRUE;
    mods = static_cast<PAUX_MODULE_EXTENDED_INFO>(modsMem.Get());

    count = size / sizeof(AUX_MODULE_EXTENDED_INFO);
    if (NT_SUCCESS(AuxKlibQueryModuleInformation(&size, sizeof(AUX_MODULE_EXTENDED_INFO), mods))) {
        target = (ULONG_PTR)g_If.BthAllocateBrb;
        for (i = 0; i < count; i++) {
            ULONG_PTR base = (ULONG_PTR)mods[i].BasicInfo.ImageBase;
            ULONG_PTR end  = base + mods[i].ImageSize;
            if (target >= base && target < end) { alive = TRUE; break; }
        }
    } else {
        alive = TRUE;       // 查不到就放行，不要因为诊断失败把功能锁死
    }

    return alive;   // mods 由 guard 释放
}

//  对外：接口现在还能用吗（keeper 在跑 setup 前先问一句）
BOOLEAN
BthL2capIsAttached(VOID)
{
    if (g_Shutdown != 0) return FALSE;
    if (!g_IfReady) return FALSE;
    return IfPointersAlive();
}

//  设备被移除时调用：把 PDO 和接口一起作废。
//  这是 0xC9 的正面解法 —— verifier 实测报过
//  "Invalid Device object passed to IoCallDriver"：
//  设备节点删掉后 g_Pdo 成了悬空指针，keeper 还在拿它发 IRP，
//  往已释放的设备对象里写 = 内核链表被写花 = 后面随机 0x139_3。
VOID
BthL2capDetach(VOID)
{
    InterlockedExchange(&g_Shutdown, 1);
    InterlockedExchangePointer((PVOID volatile*)&g_Pdo, NULL);
    InterlockedExchangePointer((PVOID volatile*)&g_RefDo, NULL);
    //  缓存的那个 BTHENUM PDO 也必须清掉：设备节点删掉后它是悬空指针，
    //  而 LdacSetup（IOCTL 路径）过去会拿它再发一次 QI —— 正是 0xC9/0x35 的来源。
    InterlockedExchangePointer((PVOID volatile*)&g_LdacBthPdo, NULL);
    g_IfReady = FALSE;
    RtlZeroMemory(&g_If, sizeof(g_If));
    if (g_LogFn) g_LogFn("BthL2capDetach: interface and PDO released");
}

//  作废接口：模块没了，指针再也别用
static VOID
DetachIfDead(VOID)
{
    if (g_IfReady && !IfPointersAlive()) {
        if (g_LogFn) g_LogFn("interface is dead (module unloaded), detaching");
        g_IfReady = FALSE;
        g_Pdo = NULL;
        RtlZeroMemory(&g_If, sizeof(g_If));
    }
}

BOOLEAN
BthL2capIsShuttingDown(VOID)
{
    return g_Shutdown != 0;
}

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
//  完成上下文和 BRB 一样**不能放栈上**。以前这里用栈上 KEVENT，而且是无界等待：
//  设备正在被移除时那一下 QI 可能永远不完成，线程就永久挂住 —— 整机卡死、
//  不蓝屏（2026-11-08 实测）。现在放非页池，等待有界，超时就泄漏 IRP/ctx。
typedef struct _QI_CTX {
    KEVENT Done;
} QI_CTX, *PQI_CTX;

#define QI_CTX_TAG 'xCIQ'

static NTSTATUS
QiComplete(_In_ PDEVICE_OBJECT DeviceObject, _In_ PIRP Irp, _In_ PVOID Context)
{
    PQI_CTX c = (PQI_CTX)Context;
    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Irp);
    KeSetEvent(&c->Done, IO_NO_INCREMENT, FALSE);
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
    PQI_CTX            ctx;
    LARGE_INTEGER      wait;

    //  同一个 PDO 才复用接口。A2DP 设备会被反复枚举（重连/重配/disable-enable），
    //  换了 PDO 还继续用旧接口 = 拿着已删除设备的函数指针发 BRB。
    if (g_IfReady && g_Pdo == BthPdo) return STATUS_SUCCESS;
    if (BthPdo == NULL) return STATUS_INVALID_PARAMETER;

    g_IfReady = FALSE;       // PDO 变了，旧接口作废

    RtlZeroMemory(&g_If, sizeof(g_If));
    g_If.Interface.Size    = sizeof(BTH_PROFILE_DRIVER_INTERFACE);
    g_If.Interface.Version = BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI;

    //  RAII：ctx / IRP 都由守卫持有，任何 return 都不会漏释放。
    PoolGuard ctxMem(ExAllocatePoolWithTag(NonPagedPool, sizeof(QI_CTX), QI_CTX_TAG), QI_CTX_TAG);
    if (ctxMem.Get() == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    ctx = static_cast<PQI_CTX>(ctxMem.Get());
    RtlZeroMemory(ctx, sizeof(*ctx));
    KeInitializeEvent(&ctx->Done, NotificationEvent, FALSE);

    //  和 SubmitBrb 完全同一个坑（0x35 NO_MORE_IRP_STACK_LOCATIONS）：
    //  这个 QI IRP 会被 Bthenum 继续往下转发（BthEnumForwardQueryInterface），
    //  按 PDO->StackSize 精确分配时最后那一层正好被用光。实测证据：
    //  扬声器开机、设备刚连上的瞬间立刻 0x35，栈顶就是
    //  BthEnum!WaitForLowerDriverToCompleteIrp+0x97  <- BthEnumForwardQueryInterface。
    //  多给两层，并夹到合理区间（PDO 正在移除时 StackSize 可能是 0）。
    {
        int stackSize = (int)BthPdo->StackSize + 2;
        if (stackSize < 4) stackSize = 4;
        if (stackSize > 120) stackSize = 120;
        irp = IoAllocateIrp((CCHAR)stackSize, FALSE);
    }
    IrpGuard irpMem(irp);
    if (irp == NULL) return STATUS_INSUFFICIENT_RESOURCES;   // ctxMem 自动释放

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_PNP;
    sp->MinorFunction = IRP_MN_QUERY_INTERFACE;
    sp->Parameters.QueryInterface.InterfaceType = (PGUID)&g_ProfileIfGuid;
    sp->Parameters.QueryInterface.Size          = sizeof(BTH_PROFILE_DRIVER_INTERFACE);
    sp->Parameters.QueryInterface.Version       = BTHDDI_PROFILE_DRIVER_INTERFACE_VERSION_FOR_QI;
    sp->Parameters.QueryInterface.Interface     = (PINTERFACE)&g_If;
    sp->Parameters.QueryInterface.InterfaceSpecificData = NULL;

    irp->IoStatus.Status = STATUS_NOT_SUPPORTED;
    IoSetCompletionRoutine(irp, QiComplete, ctx, TRUE, TRUE, TRUE);

    st = IoCallDriver(BthPdo, irp);
    if (st == STATUS_PENDING) {
        //  有界等待：PASSIVE 上 10 秒足够一次 QI 往返。等不到就取消，再给 2 秒；
        //  仍然等不到说明 bthport 还攥着这块内存 —— 泄漏 IRP/ctx，绝不 free。
        wait.QuadPart = -100000000LL;               // 10s
        st = KeWaitForSingleObject(&ctx->Done, Executive, KernelMode, FALSE, &wait);
        if (st == STATUS_TIMEOUT) {
            IoCancelIrp(irp);
            wait.QuadPart = -20000000LL;            // 2s
            if (KeWaitForSingleObject(&ctx->Done, Executive, KernelMode, FALSE,
                                      &wait) == STATUS_TIMEOUT) {
                if (g_LogFn) {
                    g_LogFn("attach: QI cancel got no completion -> LEAK irp=%p ctx=%p",
                            irp, ctx);
                }
                //  ⚠ 这里**故意**交出所有权：协议栈可能还会写这块内存，
                //     释放就是 use-after-free。守卫 Dismiss 掉，泄漏一次。
                (VOID)irpMem.Detach();
                (VOID)ctxMem.Detach();
                return STATUS_IO_TIMEOUT;
            }
        }
        st = irp->IoStatus.Status;
    }
    IoSetCancelRoutine(irp, NULL);
    IoSetCompletionRoutine(irp, NULL, NULL, FALSE, FALSE, FALSE);
    //  irpMem / ctxMem 出作用域自动释放

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

    //  ⚠ 这一行是必须的：BthL2capDetach() 会把 g_Shutdown 置 1（设备被移除时），
    //  而重新挂载（新设备实例 / 重新启用）时必须把它清掉，否则驱动会永久
    //  停在"正在卸载"状态 —— 所有 BRB 都返回 STATUS_DEVICE_NOT_READY
    //  (0xC00000A3)，建链永远失败，发送端一起来就死。
    //  实测症状：配对成功、设备显示已连接，但 self-test FAILED 0xC00000A3。
    InterlockedExchange(&g_Shutdown, 0);
    if (g_LogFn) {
        g_LogFn("BthL2capAttach OK: version=0x%04X size=%u",
                g_If.Interface.Version, g_If.Interface.Size);
    }
    return STATUS_SUCCESS;
}

// --------------------------------------------------------- 提交 BRB 同步等 ---
//  完成上下文：**不能放在栈上**。
//  以前 KEVENT 是 SubmitBrb 的局部变量，完成例程拿着它的地址；只要出现
//  "取消没兜住、IRP 在我们返回之后才完成"，完成例程就会去写已经失效的栈槽
//  —— 这正是 0x7E(访问越界)、0x139(等待链表被写花)、0xCE(bthport 带着未完成
//  操作被卸载) 的共同来源。现在它跟着 IRP 一起走，谁完成谁释放。
typedef struct _BRB_CTX {
    KEVENT Done;
    PIRP   Irp;
    PBRB   Brb;
    //  完成例程被调用的次数。>1 就说明 bthport 对同一个 IRP **完成了两次** ——
    //  如果那时 ctx 已经释放，第二次 KeSetEvent 写的就是已归还内存里的
    //  等待块链表，正好对应我们在 KiProcessThreadWaitList 看到的 0x139_3。
    volatile LONG Completions;
} BRB_CTX, *PBRB_CTX;

#define BRB_CTX_TAG 'xCRB'

static NTSTATUS
BrbComplete(_In_ PDEVICE_OBJECT DevObj, _In_ PIRP Irp, _In_ PVOID Ctx)
{
    PBRB_CTX c = (PBRB_CTX)Ctx;
    UNREFERENCED_PARAMETER(DevObj);
    UNREFERENCED_PARAMETER(Irp);

    //  STATUS_MORE_PROCESSING_REQUIRED：I/O 管理器**不会**替我们完成/回收 IRP，
    //  所有权仍然在我们手里。于是"释放 IRP"和"完成例程还在跑"不可能重叠。
    {
        LONG n = InterlockedIncrement(&c->Completions);
        if (n > 1 && g_LogFn) {
            g_LogFn("!!! BRB completed TWICE (n=%ld) irp=%p brb=%p type=0x%04X",
                    n, Irp, c->Brb, c->Brb ? (ULONG)c->Brb->BrbHeader.Type : 0);
        }
    }
    KeSetEvent(&c->Done, IO_NO_INCREMENT, FALSE);
    return STATUS_MORE_PROCESSING_REQUIRED;
}

//  Leak = TRUE：提交完**不释放** IRP 和上下文（实验用）。
//
//  假说（2026-11-08 完整内核 dump 指向）：bthport 对同一个 IRP 可能完成两次。
//  第二次完成时我们的 BRB_CTX 已经释放，完成例程会对一块已归还的 KEVENT
//  做 KeSetEvent —— KEVENT 内部挂着等待块链表，写进去正好变成
//  "定时器/等待链表被写花"（0x139_3，受害者随机）。
//  如果不释放，第二次完成只会对一块还活着的内存重复 set 事件，无害。
//
//  目前只用在高频的发包路径上（BthL2capSend），其它 BRB 仍然正常释放。
static NTSTATUS
SubmitBrb(_In_ PBRB Brb, _In_ ULONG TimeoutMs, _In_ BOOLEAN Leak)
{
    NTSTATUS           st;
    NTSTATUS           iocall;
    PIRP               irp;
    PIO_STACK_LOCATION sp;
    PBRB_CTX           ctx;
    LARGE_INTEGER      timeout;
    int                stackSize;

    if (g_Pdo == NULL) return STATUS_INVALID_DEVICE_STATE;

    //  卸载中：keeper / AVRCP 线程可能还在并发提交。直接拒绝，
    //  让它们自然退场，而不是继续往正在消失的栈里塞 IRP。
    if (g_Shutdown != 0) return STATUS_DEVICE_NOT_READY;

    //  0x35（NO_MORE_IRP_STACK_LOCATIONS）的老根：
    //  这里原来按 g_Pdo->StackSize 精确分配。bthport 收到
    //  IOCTL_INTERNAL_BTH_SUBMIT_BRB 后会把 IRP 再往下转发一层，
    //  栈位置刚好用完 -> 0x35。多给两层；PDO 正在移除时 StackSize 可能
    //  是 0/无效，至少给 4，避免写越界。
    //  模块被卸载后 g_If 里的指针就是野的，CFG 会直接 0x139 —— 先自检再调。
    DetachIfDead();
    if (!g_IfReady) return STATUS_DEVICE_NOT_READY;

    stackSize = (int)g_Pdo->StackSize + 2;
    if (stackSize < 4) stackSize = 4;
    if (stackSize > 120) stackSize = 120;

    //  RAII：ctx 和 IRP 的生命周期绑在作用域上。只有「协议栈可能还攥着」
    //  的两种情况才 Detach（超时取消无果、Leak 实验），并在那里写明原因。
    PoolGuard ctxMem(ExAllocatePoolWithTag(NonPagedPool, sizeof(BRB_CTX), BRB_CTX_TAG), BRB_CTX_TAG);
    if (ctxMem.Get() == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    ctx = static_cast<PBRB_CTX>(ctxMem.Get());
    RtlZeroMemory(ctx, sizeof(*ctx));
    KeInitializeEvent(&ctx->Done, NotificationEvent, FALSE);
    ctx->Brb = Brb;

    IrpGuard irpMem(IoAllocateIrp((CCHAR)stackSize, FALSE));
    if (irpMem.Get() == NULL) return STATUS_INSUFFICIENT_RESOURCES;  // ctxMem 自动释放
    irp = irpMem.Get();
    ctx->Irp = irp;
    InterlockedIncrement(&g_PendingBrbs);

    if (g_VerboseLog && g_LogFn) {
        g_LogFn("brb enter: type=0x%04X stack=%d pdo=%p irp=%p ctx=%p",
                Brb ? Brb->BrbHeader.Type : 0, stackSize, g_Pdo, irp, ctx);
    }

    sp = IoGetNextIrpStackLocation(irp);
    sp->MajorFunction = IRP_MJ_INTERNAL_DEVICE_CONTROL;
    sp->Parameters.Others.Argument1 = Brb;      // 偏移 0
    sp->Parameters.DeviceIoControl.IoControlCode = IOCTL_INTERNAL_BTH_SUBMIT_BRB;  // 偏移 8

    irp->IoStatus.Status = STATUS_PENDING;
    IoSetCompletionRoutine(irp, BrbComplete, ctx, TRUE, TRUE, TRUE);

    iocall = IoCallDriver(g_Pdo, irp);
    st = iocall;
    if (st == STATUS_PENDING) {
        // 相对时间：负数，单位 100ns
        timeout.QuadPart = -((LONGLONG)TimeoutMs * 10000);
        st = KeWaitForSingleObject(&ctx->Done, Executive, KernelMode, FALSE,
                                   (TimeoutMs == 0) ? NULL : &timeout);
        if (st == STATUS_TIMEOUT) {
            //  取消之后必须等完成例程跑过，才能碰 IRP；但**不能无限等**。
            //  实测（2026-11-08）：这个栈上取消后完成例程可能永远不来，
            //  无限等会把 keeper 线程永久挂住 -> 整机卡死（不蓝屏）。
            //
            //  所以：有界等待 2 秒。等到了才释放；等不到就**泄漏 IRP 和 ctx**，
            //  并且**不递减 g_PendingBrbs** —— 让 BthL2capWaitIdle/卸载知道
            //  "还有东西在协议栈手里"，宁可卸载失败也不要 use-after-free。
            LARGE_INTEGER ct;
            NTSTATUS      w2;

            IoCancelIrp(irp);
            ct.QuadPart = -20000000LL;          // 相对 2s
            w2 = KeWaitForSingleObject(&ctx->Done, Executive, KernelMode, FALSE, &ct);
            if (w2 == STATUS_TIMEOUT) {
                if (g_LogFn) {
                    g_LogFn("brb: cancel no completion in 2s -> LEAK irp=%p ctx=%p (type=0x%04X)",
                            irp, ctx, Brb ? Brb->BrbHeader.Type : 0);
                }
                //  irp / ctx 一律不释放；g_PendingBrbs 也不递减。
                //  守卫必须显式交出所有权，否则析构会 free 掉协议栈还在写的内存。
                (VOID)irpMem.Detach();
                (VOID)ctxMem.Detach();
                return STATUS_IO_TIMEOUT;
            }
            //  ⚠ 这里必须换成 0xC00000B5，不能留 STATUS_TIMEOUT。
            //  STATUS_TIMEOUT 是 0x00000102 —— **正数**，NT_SUCCESS() 对它返回
            //  TRUE。留原值会让调用方把"我们等超时了"当成"BRB 成功了"，
            //  实测就是这么误报出 `ping OK respLen=0` 的（其实是彻底超时）。
            st = STATUS_IO_TIMEOUT;
        } else {
            st = irp->IoStatus.Status;
        }
    }
    if (g_VerboseLog && g_LogFn) {
        //  实测：BRB_L2CA_OPEN_CHANNEL 的提交 IRP 会**立刻**以 SUCCESS 完成，
        //  而真正的结果（ChannelHandle / OutResults）稍后才写进 BRB。
        //  这两者必须分开看，否则就是"假成功"。
        g_LogFn("brb submit: iocall=0x%08X irp=0x%08X brb=0x%08X bt=0x%02X",
                iocall, irp->IoStatus.Status,
                Brb ? Brb->BrbHeader.Status : 0,
                Brb ? Brb->BrbHeader.BtStatus : 0);
    }
    //  走到这里时完成例程一定已经跑过（MORE_PROCESSING_REQUIRED 保证 IRP
    //  没有被 I/O 管理器回收）。先摘掉取消例程和完成例程，再释放。
    if (Leak) {
        static volatile LONG leaked = 0;
        LONG n = InterlockedIncrement(&leaked);
        if (g_LogFn && (n % 2000) == 0) {
            g_LogFn("brb: LEAK experiment -- %ld irp/ctx kept alive (send path)", n);
        }
        //  ⚠ 刻意不释放 irp / ctx，也不递减 g_PendingBrbs：
        //     第二次完成时会话仍在，不会写到已归还的内存。
        (VOID)irpMem.Detach();
        (VOID)ctxMem.Detach();
        return st;
    }

    IoSetCancelRoutine(irp, NULL);
    IoSetCompletionRoutine(irp, NULL, NULL, FALSE, FALSE, FALSE);
    //  irpMem / ctxMem 出作用域自动释放（IoFreeIrp + ExFreePoolWithTag）
    InterlockedDecrement(&g_PendingBrbs);

    // 关键：bthport 把真实结果放在 BRB 头里，IRP 状态可能仍是 SUCCESS
    if (NT_SUCCESS(st) && Brb != NULL && !NT_SUCCESS(Brb->BrbHeader.Status)) {
        st = Brb->BrbHeader.Status;
    }
    return st;
}

//  卸载/拆通道前的栅栏：等到没有 BRB 在飞，再让调用方去拆。
//  这就是 0xCE（DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS）
//  的正面解法 —— bthport 手里还攥着我们的 IRP 时，映像绝不能消失。
NTSTATUS
BthL2capWaitIdle(_In_ ULONG TimeoutMs)
{
    LARGE_INTEGER iv;
    LONGLONG t0 = (LONGLONG)KeQueryInterruptTime();
    LONGLONG budget = (LONGLONG)TimeoutMs * 10000LL;

    iv.QuadPart = -10000LL;                 // 相对 1ms

    for (;;) {
        LONG pending = InterlockedCompareExchange(&g_PendingBrbs, 0, 0);
        if (pending == 0) return STATUS_SUCCESS;

        if ((LONGLONG)KeQueryInterruptTime() - t0 >= budget) {
            if (g_LogFn) {
                g_LogFn("wait idle: TIMEOUT with %ld BRB(s) still in flight", pending);
            }
            return STATUS_TIMEOUT;
        }
        //  让出 CPU 等完成例程跑：PairedPending 只会在完成路径上递减
        KeDelayExecutionThread(KernelMode, FALSE, &iv);
    }
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

// ------------------------------------------------------------------ RAII --
//  BRB 的所有权写进类型里：Adopt 之后由析构器负责释放。
//  这个项目至今的三次蓝屏（0x7E / 0x139 / 0xCE）全部来自"某个出口忘了释放，
//  或者提前释放"。靠注释提醒不可靠，靠析构器可靠。
namespace {

class BrbGuard final {
public:
    BrbGuard() = default;
    BrbGuard(const BrbGuard&) = delete;
    BrbGuard& operator=(const BrbGuard&) = delete;
    ~BrbGuard() { Release(); }

    //  接管刚分配出来的 BRB，返回同一个指针，方便写成一行
    PBRB Adopt(_In_opt_ PBRB Brb) noexcept
    {
        Release();
        Owned_ = Brb;
        return Owned_;
    }

    //  放弃所有权：协议栈可能还会往这块内存里写，宁可泄漏也不释放
    PBRB Detach() noexcept
    {
        PBRB p = Owned_;
        Owned_ = NULL;
        return p;
    }

private:
    void Release() noexcept
    {
        if (Owned_ != NULL) {
            if (g_If.BthFreeBrb != NULL) g_If.BthFreeBrb(Owned_);
            Owned_ = NULL;
        }
    }

    PBRB Owned_ = NULL;
};

} // namespace

// ------------------------------------------------------------- 自检 BRB ---
//  L2CAP 层 Echo。这是唯一能把「链路/L2CAP 层通不通」和「AVDTP 语义对不对」
//  切开的探针：ping 通了就证明数据真的能到对端、对端也真的回了，
//  那之后 recv 读不到东西就只能是上层（耳机不认我们的 AVDTP 请求）。
NTSTATUS
BthL2capPing(_In_ BTH_ADDR Addr, _Out_ PULONG RespLen)
{
    NTSTATUS st;
    PBRB brb;
    BrbGuard guard;

    *RespLen = 0;
    if (!g_IfReady) return STATUS_INVALID_DEVICE_STATE;

    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_L2CA_PING, BTHL2CAP_TAG));
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    brb->BrbL2caPing.BtAddress         = Addr;
    brb->BrbL2caPing.PingRequestLength = 4;
    brb->BrbL2caPing.PingRequestData[0] = 'L';
    brb->BrbL2caPing.PingRequestData[1] = 'D';
    brb->BrbL2caPing.PingRequestData[2] = 'A';
    brb->BrbL2caPing.PingRequestData[3] = 'C';

    st = SubmitBrb(brb, 6000, FALSE);
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
    return st;
}

NTSTATUS
BthL2capGetLocalAddress(_Out_ PBTH_ADDR Out)
{
    NTSTATUS st;
    PBRB brb;
    BrbGuard guard;

    *Out = 0;
    if (!g_IfReady) return STATUS_INVALID_DEVICE_STATE;

    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_HCI_GET_LOCAL_BD_ADDR, BTHL2CAP_TAG));
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    st = SubmitBrb(brb, 5000, FALSE);
    if (NT_SUCCESS(st)) {
        *Out = brb->BrbGetLocalBdAddress.BtAddress;
    } else if (g_LogFn) {
        g_LogFn("local addr FAIL: status=0x%08X btStatus=0x%02X",
                brb->BrbGetLocalBdAddress.Hdr.Status,
                brb->BrbGetLocalBdAddress.Hdr.BtStatus);
    }
    return st;
}

// ------------------------------------------------------------- 打开通道 ---
NTSTATUS
BthL2capOpenEx(_In_ BTH_ADDR Addr, _In_ USHORT Psm, _Out_ PBTH_L2CAP_CHAN Chan,
              _In_ ULONG PollMs)
{
    NTSTATUS st;
    PBRB brb;
    BrbGuard guard;
    USHORT   mtu;

    if (!g_IfReady) return STATUS_INVALID_DEVICE_STATE;
    if (Chan == NULL) return STATUS_INVALID_PARAMETER;

    RtlZeroMemory(Chan, sizeof(*Chan));
    Chan->BtAddress = Addr;

    //  signaling 通道跟 BthA2dp / sink 自己声明的一样用 679；
    //  媒体通道维持 1500（RTP 包需要更大）。
    mtu = (Psm == AVDTP_SIGNAL_PSM) ? L2CAP_MTU_SIGNALING : L2CAP_MTU_DEFAULT;

    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL, BTHL2CAP_TAG));
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

    st = SubmitBrb(brb, OPEN_TIMEOUT_MS, FALSE);

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
            //  协议栈可能还会往这块内存里写 ChannelHandle —— 故意放弃所有权，
            //  让守卫别释放它。这里是全文件唯一"该泄漏"的地方。
            guard.Detach();
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
    PBRB brb;
    BrbGuard guard;
    PUCHAR   tmp;

    if (!g_IfReady || Chan == NULL || !Chan->Open) return STATUS_INVALID_DEVICE_STATE;
    if (Buf == NULL || Len == 0) return STATUS_INVALID_PARAMETER;

    PoolGuard acl(AclAlloc(Len), BTHL2CAP_TAG);
    tmp = static_cast<PUCHAR>(acl.Get());
    if (tmp == NULL) return STATUS_INSUFFICIENT_RESOURCES;
    RtlCopyMemory(tmp, Buf, Len);

    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, BTHL2CAP_TAG));
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;   // acl 自动释放

    brb->BrbL2caAclTransfer.BtAddress     = Chan->BtAddress;
    brb->BrbL2caAclTransfer.ChannelHandle = Chan->Handle;
    brb->BrbL2caAclTransfer.TransferFlags = ACL_TRANSFER_DIRECTION_OUT;
    brb->BrbL2caAclTransfer.BufferSize    = Len;
    brb->BrbL2caAclTransfer.Buffer        = tmp;
    brb->BrbL2caAclTransfer.BufferMDL     = NULL;
    brb->BrbL2caAclTransfer.Timeout       = 0;

    //  实验（2026-11-08）：发包路径不释放 IRP / BRB_CTX。
    //  见 SubmitBrb 的注释：怀疑 bthport 二次完成会写已释放的 KEVENT。
    st = SubmitBrb(brb, 5000, FALSE);
    if (!NT_SUCCESS(st) && g_LogFn) {
        g_LogFn("send FAIL: status=0x%08X btStatus=0x%02X handle=0x%X len=%u",
                brb->BrbL2caAclTransfer.Hdr.Status, brb->BrbL2caAclTransfer.Hdr.BtStatus,
                (ULONG)(ULONG_PTR)Chan->Handle, Len);
    }
    return st;   // acl 出作用域释放
}

// --------------------------------------------------------------- 接收 ---
NTSTATUS
BthL2capRecv(_In_ PBTH_L2CAP_CHAN Chan, _Out_writes_(Cap) UCHAR* Buf,
             _In_ ULONG Cap, _In_ ULONG TimeoutMs, _Out_ PULONG Got)
{
    NTSTATUS st;
    PBRB brb;
    BrbGuard guard;
    PUCHAR   tmp;
    ULONG    got = 0;

    *Got = 0;
    if (!g_IfReady || Chan == NULL || !Chan->Open) return STATUS_INVALID_DEVICE_STATE;
    if (Buf == NULL || Cap == 0) return STATUS_INVALID_PARAMETER;

    PoolGuard acl(AclAlloc(Cap), BTHL2CAP_TAG);
    tmp = static_cast<PUCHAR>(acl.Get());
    if (tmp == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_L2CA_ACL_TRANSFER, BTHL2CAP_TAG));
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;   // acl 自动释放

    brb->BrbL2caAclTransfer.BtAddress     = Chan->BtAddress;
    brb->BrbL2caAclTransfer.ChannelHandle = Chan->Handle;
    brb->BrbL2caAclTransfer.TransferFlags =
        ACL_TRANSFER_DIRECTION_IN | ACL_SHORT_TRANSFER_OK | ACL_TRANSFER_TIMEOUT;
    brb->BrbL2caAclTransfer.BufferSize    = Cap;
    brb->BrbL2caAclTransfer.Buffer        = tmp;
    brb->BrbL2caAclTransfer.BufferMDL     = NULL;
    brb->BrbL2caAclTransfer.Timeout       = (LONGLONG)TimeoutMs;

    if (g_VerboseLog && g_LogFn) {
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

    st = SubmitBrb(brb, TimeoutMs + 2000, FALSE);
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
    return st;   // acl 出作用域释放
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
static BOOLEAN             g_SrvEventsInit = FALSE;   // 事件对象只许初始化一次
//  实测 UGREEN-BT505 会**自己拨两条 PSM 25**：第一条当 AVDTP 信令，第二条就是
//  A2DP 的传输通道（抓 cold2.pcap：四条 Connection Request 方向全是 IN）。
//  BthA2dp 也是直接用这条拨入通道发 RTP，而不是自己开 client 通道 ——
//  我们原来自己 open 的那条，sink 接了但根本不往里收数据（数据进黑洞、静音）。
static BTH_L2CAP_CHAN      g_Inbound2;           // 设备拨入的第 2 条（传输通道）
static KEVENT              g_Inbound2Event;
static BOOLEAN             g_SrvInit = FALSE;

struct ACCEPT_WORK {
    PIO_WORKITEM         Item;      // 注册时预分配，终生复用（不在 DISPATCH 上分配/释放）
    BTH_ADDR             Addr;
    L2CAP_CHANNEL_HANDLE Handle;
    LONG                 Queued;    // 1 = 工作项正在排队/执行
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
    PBRB brb;
    BrbGuard guard;

    if (!g_IfReady || Chan == NULL || !Chan->Open) return;

    // 若关的是拨入通道，同步服务端状态
    if (g_SrvInit && g_Inbound.Open && Chan->Handle == g_Inbound.Handle) {
        g_Inbound.Open   = FALSE;
        g_Inbound.Handle = NULL;
    }

    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_L2CA_CLOSE_CHANNEL, BTHL2CAP_TAG));
    if (brb != NULL) {
        brb->BrbL2caCloseChannel.BtAddress     = Chan->BtAddress;
        brb->BrbL2caCloseChannel.ChannelHandle = Chan->Handle;
        st = SubmitBrb(brb, 5000, FALSE);
        UNREFERENCED_PARAMETER(st);
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
    PBRB brb;
    BrbGuard guard;
    NTSTATUS     st;
    PIO_WORKITEM item = g_Accept.Item;

    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(DeviceObject);

    //  工作项是终生复用的：这里只放开"排队中"标志，绝不释放它。
    //  （以前在这里 IoFreeWorkItem，而指示回调又会新分配一个，两边对同一个
    //    工作项的所有权没有约定 —— 释放到正在排队的对象上就写花了队列链表。）
    InterlockedExchange(&g_Accept.Queued, 0);

    if (!g_IfReady || item == NULL) {
        return;
    }

    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_L2CA_OPEN_CHANNEL_RESPONSE, BTHL2CAP_TAG));
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

        st = SubmitBrb(brb, 5000, FALSE);
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
    }
    //  不释放工作项：它是终生复用的资源（见 indication 里那段说明）。
}

//  Driver.c 里的会话通道（断开指示里要按 handle 清掉）
//  这三个定义在 Driver.c（按 C 编译）。C++ 里裸写 extern 会被改名成
//  ?g_SigChan@@3U... 这种修饰名，链接直接找不到 —— 必须套 extern "C"。
extern "C" {
extern BTH_L2CAP_CHAN g_SigChan;
extern BTH_L2CAP_CHAN g_MediaChan;
extern LONG           g_State;        // 0 = IDLE（keeper 靠它决定要不要重建会话）
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
        if (g_WorkDo != NULL && g_Accept.Item != NULL) {
            g_Accept.Addr   = Params->BtAddress;
            g_Accept.Handle = Params->ConnectionHandle;

            //  ⚠ 这个回调在 DISPATCH_LEVEL 跑：
            //   - IoAllocateWorkItem / IoFreeWorkItem 都要求 PASSIVE，
            //     在这里调会破坏系统工作队列的内部链表 —— 实测 0x139_3
            //     (LIST_ENTRY corruption)。所以工作项改为注册时预分配、
            //     终生复用（IoQueueWorkItem 允许 <= DISPATCH）。
            //   - 用原子"排队中"标志保证同一时刻只有一个在工作。
            if (InterlockedCompareExchange(&g_Accept.Queued, 1, 0) == 0) {
                IoQueueWorkItem(g_Accept.Item, LdacAcceptWorker,
                                DelayedWorkQueue, NULL);
            } else if (g_LogFn) {
                g_LogFn("indication: connect IGNORED (worker busy)");
            }
        } else if (g_LogFn) {
            g_LogFn("indication: connect IGNORED (no work item / no device)");
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

BOOLEAN
BthL2capIsServerRegistered(VOID)
{
    return g_SrvInit ? TRUE : FALSE;
}

NTSTATUS
BthL2capRegisterServer(_In_ PDEVICE_OBJECT RefDevice, _In_ BTH_ADDR RemoteAddr)
{
    PBRB brb;
    BrbGuard guard;
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

    //  工作项在这里（PASSIVE_LEVEL）预先分配，之后终生复用。
    //  绝不能放到 indication 回调里分配 —— 那是在 DISPATCH 上，
    //  IoAllocateWorkItem 会破坏系统工作队列链表（0x139_3 的根）。
    if (g_Accept.Item == NULL) {
        g_Accept.Item = IoAllocateWorkItem(RefDevice);
        if (g_LogFn) g_LogFn("accept work item preallocated = 0x%p", g_Accept.Item);
    }
    RtlZeroMemory(&g_Inbound, sizeof(g_Inbound));
    RtlZeroMemory(&g_Inbound2, sizeof(g_Inbound2));
    //  ⚠ 事件对象**只初始化一次**。
    //  KeInitializeEvent 会 InitializeListHead(&Event->Header.WaitListHead)，
    //  而 KeWaitForSingleObject 的等待块此刻可能正挂在这条链表上（用户态
    //  supervisor 每秒都在重试 START，BthL2capAcquireSignaling 会在
    //  g_InboundEvent 上等 6 秒）。表头被重置后，等该线程的定时器到期、
    //  KiProcessThreadWaitList 去摘链表项时就是 LIST_ENTRY 双重删除 =
    //  0x139_3 —— 受害栈永远是 KiProcessExpiredTimerList -> KiProcessThreadWaitList，
    //  和实测 dump 完全一致。重新注册只允许清状态，绝不许碰链表。
    if (!g_SrvEventsInit) {
        KeInitializeEvent(&g_InboundEvent,  NotificationEvent, FALSE);
        KeInitializeEvent(&g_Inbound2Event, NotificationEvent, FALSE);
        g_SrvEventsInit = TRUE;
    } else {
        KeResetEvent(&g_InboundEvent);
        KeResetEvent(&g_Inbound2Event);
    }
    g_SrvInit = TRUE;

    // 「特定远端 + PSM」注册：通配 PSM 25 可能被 inbox btha2dp 占着，
    // 特定配对可与其它驱动的通配注册共存（MS 文档明确允许）
    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_L2CA_REGISTER_SERVER, BTHL2CAP_TAG));
    if (brb == NULL) return STATUS_INSUFFICIENT_RESOURCES;

    brb->BrbL2caRegisterServer.BtAddress   = RemoteAddr;
    brb->BrbL2caRegisterServer.PSM         = AVDTP_SIGNAL_PSM;
    brb->BrbL2caRegisterServer.IndicationFlags = 0;
    brb->BrbL2caRegisterServer.IndicationCallback = LdacL2capIndication;
    brb->BrbL2caRegisterServer.IndicationCallbackContext = NULL;
    brb->BrbL2caRegisterServer.ReferenceObject = RefDevice;

    st = SubmitBrb(brb, 5000, FALSE);

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
            BrbGuard psmGuard;
            PBRB     psmBrb;
            NTSTATUS stPsm;

            psmBrb = psmGuard.Adopt(g_If.BthAllocateBrb(BRB_REGISTER_PSM, BTHL2CAP_TAG));
            if (psmBrb == NULL) {
                st = STATUS_INSUFFICIENT_RESOURCES;
            } else {
                psmBrb->BrbPsm.Psm = AVDTP_SIGNAL_PSM;
                stPsm = SubmitBrb(psmBrb, 5000, FALSE);
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
    return st;
}

VOID
BthL2capUnregisterServer(VOID){
    PBRB brb;
    BrbGuard guard;
    NTSTATUS st;

    if (!g_SrvInit || !g_IfReady) {
        if (g_LogFn) g_LogFn("unregister: skipped (srvInit=%u ifReady=%u)",
                             (ULONG)g_SrvInit, (ULONG)g_IfReady);
        return;
    }

    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_L2CA_UNREGISTER_SERVER, BTHL2CAP_TAG));
    if (brb != NULL) {
        //  ⚠ 必须用**注册时的那个远端地址**，不能填 0。
        //  注册是「特定远端 + PSM」，注销时 bthport 要按 (地址, PSM) 找那条记录；
        //  填 0 找不到 -> 注销失败（而且它不一定报错），服务端注册连同它持有的
        //  ReferenceObject 引用一起留着 -> 控制设备删了也卸不掉驱动映像。
        //  这正是 2026-11-03 热重载"闸门"卡住的直接原因。
        brb->BrbL2caUnregisterServer.BtAddress    = g_SrvAddr;
        brb->BrbL2caUnregisterServer.ServerHandle = g_SrvHandle;
        brb->BrbL2caUnregisterServer.Psm          = AVDTP_SIGNAL_PSM;
        st = SubmitBrb(brb, 5000, FALSE);
        if (g_LogFn)
            g_LogFn("unregister server: st=0x%08X brb=0x%08X addr=0x%X%08X",
                    st, brb->BrbL2caUnregisterServer.Hdr.Status,
                    (ULONG)(g_SrvAddr >> 32), (ULONG)(g_SrvAddr & 0xFFFFFFFF));
    }

    //  对应 RegisterServer 里那次 BRB_REGISTER_PSM
    brb = guard.Adopt(g_If.BthAllocateBrb(BRB_UNREGISTER_PSM, BTHL2CAP_TAG));
    if (brb != NULL) {
        brb->BrbPsm.Psm = AVDTP_SIGNAL_PSM;
        st = SubmitBrb(brb, 5000, FALSE);
        if (g_LogFn) g_LogFn("unregister PSM %u: st=0x%08X", AVDTP_SIGNAL_PSM, st);
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
    NTSTATUS st;

    // 1) 设备已经拨进来了（驱动加载早于/伴随设备连接时常见）
    if (g_Inbound.Open && g_Inbound.BtAddress == Addr) {
        *Chan = g_Inbound;
        if (g_LogFn)
            g_LogFn("signaling: using inbound channel (mtu=%u)", g_Inbound.Mtu);
        return STATUS_SUCCESS;
    }

    // 2) 不再"等设备拨进来"。
    //    2026-11-09 实测：这一段固定等 8 秒（日志 [16.869] -> [23.876]），
    //    而设备实际从来不在我们 START 之前拨入 —— 它的拨入只发生在配对/重连，
    //    而且拨进来也只是探一下就把通道丢掉。会话永远由 source 主动拨出，
    //    所以这里直接进入回退路径，省掉这 8 秒。

    // 3) 回退：主动拨出
    st = BthL2capOpen(Addr, AVDTP_SIGNAL_PSM, Chan);
    return st;
}

//  取传输通道：**优先用设备自己拨进来的第二条 PSM 25**（抓包实证：sink 主动发起），
//  等不到才退回自己拨。自己拨的那条 sink 虽然回 Success，但不会往里收 RTP。
NTSTATUS
BthL2capAcquireMedia(_In_ BTH_ADDR Addr, _Out_ PBTH_L2CAP_CHAN Chan)
{
    NTSTATUS st;

    RtlZeroMemory(Chan, sizeof(*Chan));

    if (g_Inbound2.Open && g_Inbound2.BtAddress == Addr) {
        *Chan = g_Inbound2;
        if (g_LogFn)
            g_LogFn("transport: using sink-dialed channel (mtu=%u)", g_Inbound2.Mtu);
        return STATUS_SUCCESS;
    }

    //  同上：不再等 6 秒。设备拨入的传输通道只有在它自己主动发起时才出现，
    //  我们的会话永远是 source 自己拨，直接走回退。
    if (g_LogFn) g_LogFn("transport: opening outbound channel");

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


