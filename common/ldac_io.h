// ============================================================================
//  ldac_io.h — 内核通道与用户态服务共享的 IOCTL 契约
// ============================================================================
//  同时被两边的编译器包含：
//    内核：ntddk.h 环境（DEFINE_GUID 交给 Driver.c 里的 initguid 实体化）
//    用户态：windows.h 环境（GUID 落成 static const）
//  只用两边都有的类型：ULONG/LONG/UCHAR。
// ============================================================================
#pragma once

#ifdef _KERNEL_MODE
#include <ntddk.h>
// 内核别碰 um\winioctl.h：CTL_CODE/DEVICE_TYPE 在 wdm.h（ntddk 链）里已有，
// 再拉用户态头会宏冲突 + DWORD 未定义，雪崩到 C1046 struct 嵌套超限。
#else
#include <windows.h>
#include <winioctl.h>
#endif

//  控制设备接口 GUID（用户态用 SetupDi 枚举，或直接开 \\.\LdacCtl 符号链接）
//  {8D1A9C42-6E7B-4E63-9A2F-3C5D1B4E7F10}
#ifdef _KERNEL_MODE
DEFINE_GUID(GUID_LDAC_CTL_INTERFACE,
    0x8d1a9c42, 0x6e7b, 0x4e63, 0x9a, 0x2f, 0x3c, 0x5d, 0x1b, 0x4e, 0x7f, 0x10);
#else
static const GUID GUID_LDAC_CTL_INTERFACE =
    { 0x8d1a9c42, 0x6e7b, 0x4e63, { 0x9a, 0x2f, 0x3c, 0x5d, 0x1b, 0x4e, 0x7f, 0x10 } };
#endif

// ------------------------------------------------------------------ IOCTL ---
//  START   ：内核去跟目标耳机跑完整 AVDTP 协商（阻塞数秒，用户态给足超时）。
//            输入 = LDAC_START_REQ（7 字节）：Addr 全零 = 用注册表/内置默认；
//            RateBit 非 0 = 采样率偏好（必须在对端能力集内才生效）。
//  STOP    ：关掉信令+媒体两条 L2CAP，回到 IDLE。
//  STATUS  ：输出 LDAC_STATUS。
//  SEND    ：输入一个完整的媒体包（12B RTP 头 + LDAC 载荷，用户态已封好帧），
//            内核只做一次 BRB ACL 转发。长度必须 <= MediaMtu。
//  UNLOAD  ：【开发期专用】让驱动映像能卸掉，见 Driver.c 的 LdacSelfDestructThread。
//            控制设备是驱动对象的最后一个引用，不删它 PnP 永远不调 DriverUnload，
//            于是"装了新包、设备也重枚举了，跑的还是旧映像"。
//            用法：先发这个 IOCTL，再 pnputil /remove-device，最后重启蓝牙无线电。
#define IOCTL_LDAC_START   CTL_CODE(FILE_DEVICE_UNKNOWN, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LDAC_STOP    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LDAC_STATUS  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LDAC_SEND    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LDAC_UNLOAD  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x804, METHOD_BUFFERED, FILE_ANY_ACCESS)

//  编解码器选择值（SET_CODEC 的输入、LDAC_STATUS.Codec、注册表 Parameters\Codec）。
//  用户态必须按同一个值选编码器，否则帧格式和协商结果对不上。
#define LDAC_CODEC_LDAC 0
#define LDAC_CODEC_SBC  1
#define LDAC_CODEC_AAC  2

//  SET_CODEC：输入一个 ULONG（0 = LDAC，1 = SBC，2 = AAC）。
//  只影响**下一次**会话建立（LdacSetup 读它选 SEP/配置），等价于把
//  Parameters\ForceSbc 变成运行时可改。要生效需要 STOP+START 重建会话。
#define IOCTL_LDAC_SET_CODEC CTL_CODE(FILE_DEVICE_UNKNOWN, 0x805, METHOD_BUFFERED, FILE_ANY_ACCESS)

// -------------------------------------------------- L2CAP 原语（重构用） ---
//  为了让 AVDTP 状态机能搬到用户态而新增的四个原子动作。
//  内核从此**不理解 AVDTP**，只负责：开一条 L2CAP 通道 / 发一段字节 /
//  收一段字节 / 关掉。协议逻辑、重试、退避都在用户态。
#define IOCTL_LDAC_L2CAP_OPEN  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x810, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LDAC_L2CAP_SEND  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x811, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LDAC_L2CAP_RECV  CTL_CODE(FILE_DEVICE_UNKNOWN, 0x812, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_LDAC_L2CAP_CLOSE CTL_CODE(FILE_DEVICE_UNKNOWN, 0x813, METHOD_BUFFERED, FILE_ANY_ACCESS)

#define LDAC_L2CAP_MAX_SLOTS 4
#define LDAC_L2CAP_RECV_MAX  1500

typedef struct _LDAC_L2CAP_OPEN_REQ {
    ULONG Psm;              // 25 = AVDTP 信令/传输
    UCHAR Addr[6];          // 全零 = 用注册表/内置默认目标
    UCHAR Reserved[2];
} LDAC_L2CAP_OPEN_REQ, *PLDAC_L2CAP_OPEN_REQ;

typedef struct _LDAC_L2CAP_OPEN_RSP {
    ULONG Handle;           // 0 = 失败；否则 1..LDAC_L2CAP_MAX_SLOTS
    ULONG Mtu;
    LONG  Status;           // NTSTATUS
} LDAC_L2CAP_OPEN_RSP, *PLDAC_L2CAP_OPEN_RSP;

typedef struct _LDAC_L2CAP_IO_REQ {
    ULONG Handle;
    ULONG Length;           // 紧随其后的载荷字节数
} LDAC_L2CAP_IO_REQ, *PLDAC_L2CAP_IO_REQ;

typedef struct _LDAC_L2CAP_RECV_REQ {
    ULONG Handle;
    ULONG TimeoutMs;
} LDAC_L2CAP_RECV_REQ, *PLDAC_L2CAP_RECV_REQ;

typedef struct _LDAC_L2CAP_HANDLE_REQ {
    ULONG Handle;
} LDAC_L2CAP_HANDLE_REQ, *PLDAC_L2CAP_HANDLE_REQ;


// ---------------------------------------------------- LDAC 能力位（协议） ---
//  与 Avdtp.h 的 codec specific 布局一致，但这里是内核/用户态共享的唯一权威定义
//  （Avdtp.h 不再重复定义）。
//  LDAC codec specific information（LDAC Spec of Bluetooth A2DP Rev.2.0.1，
//  权威定义见 third_party/libldac/inc/ldacBT.h 的同名常量）：
//    Octet 6 = 采样率 mask，bits 5-0
//    Octet 7 = 声道模式 mask，bits 2-0
//  实测耳机上报 3c 07 = (44.1k|48k|88.2k|96k) + (mono|dual|stereo)。
//  ⚠ 这两个字节是「位掩码」不是「枚举值」，别按 1/2/4/8 顺序猜。
#define LDAC_RATE_44100 0x20
#define LDAC_RATE_48000 0x10
#define LDAC_RATE_88200 0x08
#define LDAC_RATE_96000 0x04
#define LDAC_CH_MONO    0x04
#define LDAC_CH_DUAL    0x02
#define LDAC_CH_STEREO  0x01

//  START 的输入：系统混音率必须和协商率一致（v2 不做重采样），
//  所以用户态先拿到自己的混音率，再把偏好传给内核去协商。
typedef struct _LDAC_START_REQ {
    UCHAR Addr[6];       // 目标 BD_ADDR，全零 = 用注册表/内置默认
    UCHAR RateBit;       // 采样率偏好（LDAC_RATE_* 位），0 = 无偏好（选对端最高）
} LDAC_START_REQ, *PLDAC_START_REQ;

// ------------------------------------------------------------------ 状态机 ---
#define LDAC_STATE_IDLE      0
#define LDAC_STATE_STARTING  1
#define LDAC_STATE_STREAMING 2
#define LDAC_STATE_ERROR     3

#define LDAC_STATUS_VERSION 3   // v3: 加 Codec 字段

typedef struct _LDAC_STATUS {
    ULONG Version;       // LDAC_STATUS_VERSION
    LONG  State;         // LDAC_STATE_*
    LONG  LastError;     // 最近一次失败的 NTSTATUS（0 = 无）
    ULONG SigMtu;        // 信令通道 MTU
    ULONG MediaMtu;      // 媒体通道 MTU（SEND 的上限）
    ULONG RateBit;       // 已协商的采样率位（LDAC_RATE_*）
    ULONG ModeBit;       // 已协商的声道模式位（LDAC_CH_*）
    ULONG SinkSeid;      // 对端 LDAC SEP 的 SEID
    //  v2：内核累计发出的包数/字节数。用户态 supervisor 用它判断
    //  "链路是不是还活着"（STREAMING 但计数不动 = 实际已断）。
    ULONG Packets;
    ULONG Bytes;
    //  v3：本次会话协商的编解码器（LDAC_CODEC_*）。用户态据此选编码器，
    //  不信自己的记忆 —— 内核可能因为注册表默认值和请求不一致。
    ULONG Codec;
} LDAC_STATUS, *PLDAC_STATUS;

//  SEND 的合法长度边界：至少 RTP 头 + 1 字节载荷
#define LDAC_SEND_MIN 13
#define LDAC_SEND_MAX 1600
