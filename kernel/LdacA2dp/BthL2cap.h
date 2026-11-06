// ============================================================================
//  BthL2cap.h — 通过 bthport 的 BRB 接口做 L2CAP 客户端
// ============================================================================
#pragma once

#include <ntddk.h>
#include <bthdef.h>
#include <bthddi.h>
#include <bthioctl.h>

// 日志回调（实现放在 ProbeDriver.c 里，内核没有 CRT）
//  注意：BthL2cap.c / Avdtp.c 按 C 编译，Adapter.cpp 按 C++ 编译。不加
//  extern "C" 的话 C++ 侧会去找 ?g_LogFn@@3P6AXPEBDZZEA 这种修饰名 → LNK2001。
#ifdef __cplusplus
extern "C" {
#endif

typedef VOID (*PPROBE_LOG)(_In_ PCSTR fmt, ...);
extern PPROBE_LOG g_LogFn;

#define BTHL2CAP_TAG  '2LCB'

typedef struct _BTH_L2CAP_CHAN {
    BTH_ADDR             BtAddress;
    L2CAP_CHANNEL_HANDLE Handle;
    USHORT               Mtu;
    BOOLEAN              Open;
} BTH_L2CAP_CHAN, *PBTH_L2CAP_CHAN;

// 在“我们挂着的那个 BTHENUM 子设备栈”里，对 PDO 发 QI 取 BRB 接口。
// 只有这种上下文才合法（从外部打开设备接口再 QI 会绿屏，已实测）。
NTSTATUS
BthL2capAttach(_In_ PDEVICE_OBJECT BthPdo);

// 解析 "33:DB:8A:C7:E6:7A" → BTH_ADDR
NTSTATUS
BthParseAddress(_In_ PCSTR Str, _Out_ PBTH_ADDR Out);

NTSTATUS
BthL2capOpen(_In_ BTH_ADDR Addr, _In_ USHORT Psm, _Out_ PBTH_L2CAP_CHAN Chan);

//  同上，但轮询上限按墙上时间给（PollMs）。AVRCP 这类"可有可无"的通道
//  要用短预算，不能让它把常驻线程拖住。
NTSTATUS
BthL2capOpenEx(_In_ BTH_ADDR Addr, _In_ USHORT Psm, _Out_ PBTH_L2CAP_CHAN Chan,
               _In_ ULONG PollMs);

// ---- 服务端：接住设备主动拨入的 AVDTP 信令通道 --------------------------
//  设备（JieLi 等）连接时会主动拨 PSM 25；不接住它的握手，拨出也会被晾着。
//  RefDevice 用于栈回调期间的对象引用（传 FDO）。
//  RemoteAddr 传特定远端地址：通配 PSM 25 可能被 inbox btha2dp 占着，
//  「特定远端+PSM」注册可与其它驱动的通配注册共存（MS 文档明确）。
NTSTATUS
BthL2capRegisterServer(_In_ PDEVICE_OBJECT RefDevice, _In_ BTH_ADDR RemoteAddr);

VOID
BthL2capUnregisterServer(VOID);

//  —— 常驻信令通道：驱动自己拨号并持有（设备"已连接"提示靠它维持）——
NTSTATUS
BthL2capHoldSignaling(_In_ BTH_ADDR Addr);

//  发送器启动时接管常驻通道（拿到即清空，避免双开）
BOOLEAN
BthL2capTakeHeld(_Out_ PBTH_L2CAP_CHAN Out);

BOOLEAN
BthL2capIsHolding(VOID);

//  用完归还给 keeper（Discover 之后仍要保持通道）
VOID
BthL2capGiveBackHeld(_In_ PBTH_L2CAP_CHAN Chan);

//  关掉设备拨入的那条通道。它带 ReferenceObject（控制设备），
//  卸载时若只注销服务端、不关它，引用会 residual 在控制设备上 -> 映像不卸。
VOID
BthL2capCloseInbound(VOID);

//  某个通道句柄是不是「对端拨进来」的那条（inbound）。拨入通道意味着对端
//  自己会当 AVDTP INT，我们必须切到 ACP 被动模式，不能自己再发请求。
BOOLEAN
BthL2capIsInbound(_In_ L2CAP_CHANNEL_HANDLE Handle);

// 拿信令通道：优先用设备拨入的（L2CAP 握手已完成），等最多 8s，最后才拨出
NTSTATUS
BthL2capAcquireSignaling(_In_ BTH_ADDR Addr, _Out_ PBTH_L2CAP_CHAN Chan);

//  取 A2DP 传输通道：优先用设备拨入的第二条 PSM 25 通道，等不到才主动拨。
NTSTATUS
BthL2capAcquireMedia(_In_ BTH_ADDR Addr, _Out_ PBTH_L2CAP_CHAN Chan);

//  丢掉当前通道、重新拨一条干净的出来。
//  用在「耳机拨入只是探一下，探完就把通道丢了」之后 —— 旧 handle 已经不可用，
//  继续用只会拿到 STATUS_DEVICE_NOT_CONNECTED。
NTSTATUS
BthL2capReopenSignaling(_In_ BTH_ADDR Addr, _Out_ PBTH_L2CAP_CHAN Chan);

//  对端拆链次数（indication 回调里累加）。用来判断手里这条通道是不是已经死了。
extern volatile LONG g_L2capDrops;

//  打开通道用的 CF_LINK_* 组合。默认 CF_LINK_NOTHING，可由注册表
//  Parameters\ChannelFlags（DWORD）覆盖 —— 换值不用重编驱动。
extern ULONG g_L2capChannelFlags;

//  非 0 = 拒绝耳机拨进来的 PSM 25 连接（注册表 Parameters\RejectInbound）。
//  用来验证「对端拨入后留下的半个会话是不是它后来沉默的原因」。
extern ULONG g_L2capRejectInbound;

// 自检：取本机蓝牙地址。这个 BRB 必然有返回值，
// 用来判断「BRB 提交路径」本身是否正常（返回全 0 就说明 bthport 没处理我们的 IRP）。
NTSTATUS
BthL2capGetLocalAddress(_Out_ PBTH_ADDR Out);

//  L2CAP 层 Echo 探针：通了就说明数据能到对端、对端也能回，
//  之后 recv 读不到东西就只能是上层 AVDTP 语义问题，不是传输问题。
NTSTATUS
BthL2capPing(_In_ BTH_ADDR Addr, _Out_ PULONG RespLen);

NTSTATUS
BthL2capSend(_In_ PBTH_L2CAP_CHAN Chan, _In_reads_(Len) const UCHAR* Buf, _In_ ULONG Len);

NTSTATUS
BthL2capRecv(_In_ PBTH_L2CAP_CHAN Chan, _Out_writes_(Cap) UCHAR* Buf,
             _In_ ULONG Cap, _In_ ULONG TimeoutMs, _Out_ PULONG Got);

VOID
BthL2capClose(_In_ PBTH_L2CAP_CHAN Chan);

#ifdef __cplusplus
}
#endif
