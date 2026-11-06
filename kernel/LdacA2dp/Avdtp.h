// ============================================================================
//  Avdtp.h — AVDTP 信令（PSM 25）与 SEP 能力解析
// ============================================================================
#pragma once

#include <ntddk.h>
#include "BthL2cap.h"
#include "ldac_io.h"          // LDAC_RATE_* / LDAC_CH_*（codec 能力位，内核/用户态共享）

// --------------------------------------------------------------- 报文头 ---
//  AVDTP 信令单元 = [1B 头][1B 信号 ID][载荷...]，**没有长度字段**。
//  载荷长度由 L2CAP 交付长度决定。
//    头 byte0:  bit7-4 Transaction Label | bit3-2 Packet Type | bit1-0 Message Type
//    分片时：Start 包多 1 字节 NOSP(高4位)，信号 ID 顺延到第 3 字节；
//            Continue / End 包只有 1 字节头，没有信号 ID。
#define AVDTP_PKT_SINGLE   0x00
#define AVDTP_PKT_START    0x01
#define AVDTP_PKT_CONTINUE 0x02
#define AVDTP_PKT_END      0x03

#define AVDTP_MT_CMD        0x00
#define AVDTP_MT_ACCEPT     0x02
#define AVDTP_MT_REJECT     0x03
#define AVDTP_MT_GENERAL    0x01

// ------------------------------------------------------------- 信号 ID ---
//  只认规范值。网上有资料按 0x00 起算整表偏移 -1（AOSP 旧注释/部分博客），
//  早先的实现曾「两种都试」——退到偏移 -1 时发出的是 **0x00（RESERVED）**，
//  等于拿保留信号去戳对端，会让它静默把会话标成异常（不断链、只是不再理人）。
//  2026-11-03 已删掉那条探测路径，只发 0x01。
//  AVDTP 信号标识 —— 这些就是规范值（AVDTP 1.3 Table 8.19），别再偏移：
//    Discover = 0x01, GetCapabilities = 0x02, SetConfiguration = 0x03,
//    Open = 0x06, Start = 0x07 ...
//  之前整套写成 0x00 起（Discover=0x00），靠 g_SigOff 校准去补 —— 结果接收侧
//  没归一化，耳机发来的 Discover(0x01) 被当成 GetCapabilities，回了 17 字节
//  能力列表，耳机不认就沉默了。常量对齐规范，偏移机制只留作最后兜底。
#define AVDTP_MSG_DISCOVER         0x01
#define AVDTP_MSG_GET_CAPABILITIES 0x02
#define AVDTP_MSG_SET_CONFIGURATION 0x03
#define AVDTP_MSG_GET_CONFIGURATION 0x04
#define AVDTP_MSG_RECONFIGURE      0x05
#define AVDTP_MSG_OPEN             0x06
#define AVDTP_MSG_START            0x07
#define AVDTP_MSG_CLOSE            0x08
#define AVDTP_MSG_SUSPEND          0x09
#define AVDTP_MSG_ABORT            0x0A
#define AVDTP_MSG_GET_ALL_CAPS     0x0C

// TSEP：0=SRC（音源），1=SNK（耳机端）
#define AVDTP_TSEP_SRC 0
#define AVDTP_TSEP_SNK 1

// Service Category（能力元素里的类别号）
#define AVDTP_CAT_MEDIA_TRANSPORT   0x01
#define AVDTP_CAT_REPORTING         0x02
#define AVDTP_CAT_RECOVERY          0x03
#define AVDTP_CAT_CONTENT_PROTECTION 0x04
#define AVDTP_CAT_HEADER_COMPRESSION 0x05
#define AVDTP_CAT_MULTIPLEXING      0x06
#define AVDTP_CAT_MEDIA_CODEC       0x07
#define AVDTP_CAT_DELAY_REPORTING   0x08

// Sony 的蓝牙公司 ID（LDAC 的 vendor id）与 LDAC codec id
#define AVDTP_SONY_VENDOR_ID   0x0000012D
#define AVDTP_SONY_CODEC_LDAC  0x00AA

// LDAC 采样率/声道模式位定义已移至 ldac_io.h（内核/用户态共享的唯一权威）。

#define AVDTP_MAX_SEPS      8
#define AVDTP_MAX_CAPS      128
// ------------------------------------------------------------------ SEP ---
typedef struct _AVDTP_SEP {
    UCHAR   Seid;
    UCHAR   Tsep;                    // AVDTP_TSEP_SRC / _SNK
    BOOLEAN InUse;

    BOOLEAN HasCodec;
    UCHAR   MediaType;               // 0 = audio
    UCHAR   CodecType;               // 低 4 位
    ULONG   VendorId;
    USHORT  CodecId;

    // codec specific 参数在原始能力字节里的偏移（相对 Caps）
    ULONG   CodecSpecOff;
    UCHAR   Rates;                   // 支持的采样率位图
    UCHAR   Modes;                   // 支持的声道模式位图

    ULONG   CapLen;                  // 原始能力字节（SetConfiguration 时要回放）
    UCHAR   Caps[AVDTP_MAX_CAPS];
} AVDTP_SEP, *PAVDTP_SEP;

// --------------------------------------------------------------- 接口 ---
//  一次事务的结果
typedef struct _AVDTP_REPLY {
    UCHAR  MsgType;                  // AVDTP_MT_*
    UCHAR  Sig;
    ULONG  Len;
    UCHAR  Body[512];
} AVDTP_REPLY, *PAVDTP_REPLY;

//  ACP（被动）模式协商结果。耳机（JieLi）拨入后自己当 INT，我们只能应答。
typedef struct _AVDTP_ACP_RESULT {
    BOOLEAN Discovered;
    BOOLEAN Configured;
    BOOLEAN Opened;
    BOOLEAN Started;
    BOOLEAN IsLdac;
    UCHAR   RateBit;
    UCHAR   ModeBit;
    UCHAR   IntSeid;                 // 对端分配的本地 SEID
} AVDTP_ACP_RESULT, *PAVDTP_ACP_RESULT;

//  纯 ACP 被动服务：收一条命令 → 回一条响应 → 再收。全程串行，
//  绝不并发提交 BRB（并发正是之前系统直接复位的高度嫌疑）。
NTSTATUS
AvdtpServeAcp(_In_ PBTH_L2CAP_CHAN Chan, _Out_ PAVDTP_ACP_RESULT Res);

//  最底层事务：发一条命令，等它的响应（校验 Transaction Label 与 Message Type）
NTSTATUS
AvdtpXact(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR Sig,
          _In_reads_opt_(Len) const UCHAR* Payload, _In_ USHORT Len,
          _Out_ PAVDTP_REPLY Reply);

//  校准信号 ID 编号（0x00 起还是 0x01 起）。返回实际偏移，失败返回 -1。
LONG
AvdtpCalibrate(_In_ PBTH_L2CAP_CHAN Chan);

//  DISCOVER：拿到对端全部 SEP
NTSTATUS
AvdtpDiscover(_In_ PBTH_L2CAP_CHAN Chan,
              _Out_writes_(Max) PAVDTP_SEP Seps, _In_ ULONG Max, _Out_ PULONG Count);

//  GET_CAPABILITIES：填 Seps[i] 的 codec 字段与原始能力字节
NTSTATUS
AvdtpGetCapabilities(_In_ PBTH_L2CAP_CHAN Chan, _Inout_ PAVDTP_SEP Sep);

//  SET_CONFIGURATION：把 Sep 的能力字节回放给对端，只把 codec specific 改成单比特
NTSTATUS
AvdtpSetConfiguration(_In_ PBTH_L2CAP_CHAN Chan, _In_ const PAVDTP_SEP Sep,
                      _In_ UCHAR IntSeid, _In_ UCHAR RateBit, _In_ UCHAR ModeBit);

//  SET_CONFIGURATION（SBC）：对照实验用，参数写死 48k / joint / 16 blocks / 8 subbands。
NTSTATUS
AvdtpSetSbcConfiguration(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR AcpSeid, _In_ UCHAR IntSeid,
                         _In_ UCHAR FreqNibble, _In_ UCHAR ModeNibble,
                         _In_ UCHAR MinBp, _In_ UCHAR MaxBp);

NTSTATUS
AvdtpOpen(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR Seid);

NTSTATUS
AvdtpStart(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR Seid);

//  能力解析/打印（Seid 仅用于日志）
VOID
AvdtpParseSep(_Inout_ PAVDTP_SEP Sep, _In_reads_(Len) const UCHAR* Body, _In_ ULONG Len);

// ===========================================================================
//  媒体传输（第二个 L2CAP 通道，PSM 33）
// ===========================================================================
//  信令走 PSM 25，媒体走 PSM 33，是两个独立通道。
//
//  AVDTP 媒体包 = 12 字节 RTP 头 + N 个**完整**的 LDAC transport frame：
//
//     0                   1                   2                   3
//     0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |V=2|P|X|  CC   |M|     PT      |       sequence number         |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                           timestamp                           |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                             SSRC                              |
//    +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
//    |                  media payload (LDAC frames)                  |
//
//  * byte0 恒为 0x80（V=2 / P=X=0 / CC=0）
//  * PT（载荷类型）= 96，A2DP 约定的动态类型
//  * sequence number 每包 +1，16 位自然回绕
//  * timestamp 单位是 **PCM 采样数**，每包 + (128 × 帧数)；32 位自然回绕
//  * M（marker）只在每个 talk spurt 的第一个包置 1 —— 耳机靠它判断
//    「新的一段音频开始了」，START / 重新开始播放后漏置会导致首帧被丢
//
//  和信令不同，媒体包**没有长度字段**：载荷长度就是 L2CAP 交付长度减 12。
//  所以一个包里的载荷必须正好是若干个完整帧，不能把半帧塞进去。
//  传输通道 PSM：规范不固定，取决于 ACP。UGREEN-BT505 实测对 33 完全不应答，
//  改用与信令相同的 25（L2CAP 允许同一 PSM 上多条通道）。
#define AVDTP_MEDIA_PSM       25
#define AVDTP_SIGNAL_PSM      25
#define AVDTP_RTP_HDR_LEN     12
#define AVDTP_RTP_PT_LDAC     96

//  一帧 LDAC 的采样数（每声道），和采样率无关
#define AVDTP_LDAC_LSU        128

typedef struct _AVDTP_MEDIA_STATE {
    USHORT  Seq;                // 下一个包用的序列号
    ULONG   Timestamp;          // 下一个包用的 RTP 时间戳（采样数）
    ULONG   Ssrc;               // 流标识，同一个流内不变
    UCHAR   PayloadType;        // 默认 AVDTP_RTP_PT_LDAC
    BOOLEAN MarkerPending;      // TRUE = 下一个包置 M 位
} AVDTP_MEDIA_STATE, *PAVDTP_MEDIA_STATE;

//  开一条新流时调一次：Seq=0、Timestamp=0、M 置位（首个包必须带 marker）
VOID
AvdtpMediaReset(_Out_ PAVDTP_MEDIA_STATE St, _In_ ULONG Ssrc);

//  标记「下一包是新 talk spurt 的起点」：START / 从 SUSPEND 恢复时调
VOID
AvdtpMediaBeginTalkSpurt(_Inout_ PAVDTP_MEDIA_STATE St);

//  组一个媒体包。SamplesInPayload = 128 × 本次载荷里的帧数。
//  返回写入的总字节数（12 + PayloadLen）；参数不合法或容量不够返回 0，
//  且**不会**改动 St（失败不能消耗序列号/时间戳，否则对端会看到空洞）。
ULONG
AvdtpMediaBuildPacket(_Inout_ PAVDTP_MEDIA_STATE St,
                      _In_reads_(PayloadLen) const UCHAR* Payload,
                      _In_ ULONG PayloadLen,
                      _In_ ULONG SamplesInPayload,
                      _Out_writes_(OutCap) UCHAR* Out,
                      _In_ ULONG OutCap);

//  给定 L2CAP MTU，一个媒体包最多能装多少载荷字节
#define AVDTP_MEDIA_MAX_PAYLOAD(Mtu) \
    (((Mtu) > AVDTP_RTP_HDR_LEN) ? ((ULONG)(Mtu) - AVDTP_RTP_HDR_LEN) : 0UL)
