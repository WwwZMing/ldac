// ============================================================================
//  Avrcp.c — AVRCP 控制器（PSM 23 / AVCTP）：读设备绝对音量 + 推到最大
// ============================================================================
//  为什么需要它（实测依据见 docs/15-volume-root-cause.md）：
//    Windows 自带的 A2DP 端点带"硬件音量节点"（hw=1），音量走 AVRCP 绝对音量
//    写进耳机，PC 侧采样一个 bit 都不衰减（实测 vol 100%/50% 的 loopback
//    峰值都是 -0.11 dBFS）。我们没有音量节点也没有 AVRCP：采样只能被软件
//    衰减，耳机则一直停在它上一次的音量（实测 BT505 报回 9/127 ≈ -21 dB）。
//
//  ⚠ 前置条件：本机 Windows 的 AVRCP 节点（svc=Microsoft_Bluetooth_AvrcpTransport）
//    必须不在线，否则它会把我们拨出去的 PSM 23 连接**在本地吃掉**，
//    表现就是 BrbStatus 永远是 0xFF、8 秒超时、连 L2CAP 结论都收不到。
//    这和 PSM 25 上踩过的是同一类坑（本机 server 拦下拨出连接）。
//
//  帧格式（AVCTP 单包 + AV/C Panel 子单元）：
//    00 11 0E | ctype 48 opcode | operand...
//      AVCTP byte0 = TL<<4 | PT<<2 | CR  （PT=0 单包，CR=0 命令 / 1 响应）
//      AVCTP byte1..2 = profile 0x110E (AVRCP)
//      AV/C  ctype  ：0x00 CONTROL, 0x03 NOTIFY
//            响应  ：0x09 ACCEPTED, 0x0A REJECTED, 0x0C CHANGED,
//                    0x0D STABLE, 0x0F INTERIM
//            subunit：0x48 = PANEL(0x09)<<3 | id 0
//            opcode ：0x10 GetCapabilities, 0x31 RegisterNotification,
//                    0x50 SetAbsoluteVolume
//
//  对端（BT505）的回包不合套路：给我们的固定 14 字节
//    10 11 0e 01 48 00 00 19 58 10 00 00 01 03
//  CR 位是 0、ctype 也不在已知集合里，而且 SetAbsoluteVolume 完全不回。
//  所以这里不假设它守规范：一次连接里把几种探测都发一遍，把回包原样打出来，
//  再由"读回的 volume"这个结果来判定到底哪条路有效。
// ============================================================================
#include "Avrcp.h"
#include "Log.h"
#include <ntstrsafe.h>

#define AVRCP_PSM_L2CAP     0x0017

#define AVC_SUBUNIT_PANEL   0x48
#define AVC_CTYPE_CONTROL   0x00
#define AVC_CTYPE_NOTIFY    0x03
#define AVC_OP_GET_CAPS     0x10
#define AVC_OP_REG_NOTIFY   0x31
#define AVC_OP_SET_ABS_VOL  0x50
#define AVRCP_EVENT_VOLUME  0x0D
#define AVRCP_CAP_EVENTS    0x02

#define AVRCP_RESP_TIMEOUT  2000

static VOID
DumpHex(_In_ PCSTR tag, _In_reads_(Len) const UCHAR* Buf, _In_ ULONG Len)
{
    CHAR  line[200];
    ULONG i, pos = 0;

    for (i = 0; i < Len && pos + 4 < sizeof(line); i++) {
        static const CHAR hex[] = "0123456789abcdef";
        line[pos++] = hex[(Buf[i] >> 4) & 0xF];
        line[pos++] = hex[Buf[i] & 0xF];
        line[pos++] = ' ';
    }
    line[pos] = 0;
    ProbeLog("avrcp: %s [%lu] %s", tag, Len, line);
}

static VOID
Pause(_In_ LONGLONG Ms)
{
    //  系统 tick 默认 15.6ms，这里给的量级足够，不用精确
    LARGE_INTEGER d;
    d.QuadPart = -(Ms * 10000LL);
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

//  组一帧 AVCTP 单包 + 可选 AV/C，发出去。Reset=TRUE 时先切 CR 位为 1（响应）。
static NTSTATUS
SendFrame(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR Tl, _In_ UCHAR Ctype, _In_ UCHAR Opcode,
          _In_reads_opt_(OpLen) const UCHAR* Op, _In_ ULONG OpLen)
{
    UCHAR buf[32];
    ULONG len;

    if (OpLen > sizeof(buf) - 6) return STATUS_INVALID_PARAMETER;

    buf[0] = (UCHAR)((Tl & 0x0F) << 4);   // PT=0(单包), C/R=0(命令)
    buf[1] = 0x11;
    buf[2] = 0x0E;
    buf[3] = Ctype;
    buf[4] = AVC_SUBUNIT_PANEL;
    buf[5] = Opcode;
    if (OpLen && Op) RtlCopyMemory(buf + 6, Op, OpLen);
    len = 6 + OpLen;

    DumpHex("tx", buf, len);
    return BthL2capSend(Chan, buf, len);
}

//  发一帧并收一帧。TimeoutMs 内没数据就返回 STATUS_IO_TIMEOUT。
static NTSTATUS
Xfer(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR Tl, _In_ UCHAR Ctype, _In_ UCHAR Opcode,
     _In_reads_opt_(OpLen) const UCHAR* Op, _In_ ULONG OpLen,
     _Out_writes_(Cap) UCHAR* Resp, _In_ ULONG Cap, _Out_ PULONG pGot)
{
    NTSTATUS st;
    ULONG    got = 0;

    *pGot = 0;
    st = SendFrame(Chan, Tl, Ctype, Opcode, Op, OpLen);
    if (!NT_SUCCESS(st)) {
        ProbeLog("avrcp: send op=0x%02X -> 0x%08X", (ULONG)Opcode, st);
        return st;
    }

    Pause(30);                            // 让对端把回包送上来
    st = BthL2capRecv(Chan, Resp, Cap, AVRCP_RESP_TIMEOUT, &got);
    if (!NT_SUCCESS(st)) {
        ProbeLog("avrcp: recv op=0x%02X -> 0x%08X", (ULONG)Opcode, st);
        return st;
    }
    *pGot = got;
    if (got) DumpHex("rx", Resp, got);
    return STATUS_SUCCESS;
}

//  从各种可能的排布里抠出音量值：找 [00][event][value] 这个三元组
static BOOLEAN
ParseVolume(_In_reads_(Len) const UCHAR* R, _In_ ULONG Len, _In_ UCHAR Event,
            _Out_ PUCHAR pVol)
{
    ULONG i;

    for (i = 0; i + 2 < Len; i++) {
        if (R[i] == 0x00 && R[i + 1] == Event) {
            *pVol = (UCHAR)(R[i + 2] & 0x7F);
            return TRUE;
        }
    }
    return FALSE;
}

NTSTATUS
AvrcpSetVolume(_In_ BTH_ADDR Addr, _In_ UCHAR SetTo, _Out_opt_ PUCHAR pCurrent,
               _Out_opt_ PUCHAR pResponseCtype)
{
    BTH_L2CAP_CHAN chan;
    UCHAR          resp[64];
    ULONG          got = 0;
    UCHAR          cur = 0xFF;
    UCHAR          ct  = 0xFF;
    NTSTATUS       st;
    UCHAR          op[8];

    if (pCurrent) *pCurrent = 0xFF;
    if (pResponseCtype) *pResponseCtype = 0xFF;

    RtlZeroMemory(&chan, sizeof(chan));
    st = BthL2capOpenEx(Addr, AVRCP_PSM_L2CAP, &chan, 4000);
    if (!NT_SUCCESS(st)) {
        ProbeLog("avrcp: open PSM 23 -> 0x%08X", st);
        return st;
    }
    ProbeLog("avrcp: PSM 23 open, mtu=%lu", (ULONG)chan.Mtu);

    // ---- 0) 把通道里可能残留的东西先倒掉，免得把旧回包当成新回包
    for (;;) {
        ULONG junk = 0;
        if (!NT_SUCCESS(BthL2capRecv(&chan, resp, sizeof(resp), 50, &junk))) break;
        if (junk == 0) break;
        DumpHex("rx(stale)", resp, junk);
    }

    // ---- 1) GetCapabilities(EVENTS_SUPPORTED)：先走一个最基本的查询，
    //      看它到底认不认这套排布（任何合规 TG 都必须应答 0x10）
    op[0] = AVRCP_CAP_EVENTS;
    (void)Xfer(&chan, 0, AVC_CTYPE_CONTROL, AVC_OP_GET_CAPS, op, 1,
               resp, sizeof(resp), &got);

    // ---- 2) RegisterNotification(VOLUME_CHANGED)：合规设备在 INTERIM 里带当前音量
    op[0] = 0x00;
    op[1] = AVRCP_EVENT_VOLUME;
    op[2] = op[3] = op[4] = op[5] = 0x00;
    if (NT_SUCCESS(Xfer(&chan, 1, AVC_CTYPE_NOTIFY, AVC_OP_REG_NOTIFY, op, 6,
                        resp, sizeof(resp), &got)) && got) {
        if (ParseVolume(resp, got, AVRCP_EVENT_VOLUME, &cur)) {
            ProbeLog("avrcp: device volume = %lu/127 (%lu%%)",
                     (ULONG)cur, (ULONG)cur * 100 / 127);
        } else {
            ProbeLog("avrcp: notify reply has no volume tuple (ctype=0x%02X len=%lu)",
                     got > 3 ? (ULONG)resp[3] : 0UL, got);
        }
    }

    if (pCurrent) *pCurrent = cur;

    // ---- 3) SetAbsoluteVolume
    if (SetTo <= 0x7F) {
        op[0] = op[1] = op[2] = 0x00;
        op[3] = (UCHAR)(SetTo & 0x7F);
        if (NT_SUCCESS(Xfer(&chan, 2, AVC_CTYPE_CONTROL, AVC_OP_SET_ABS_VOL, op, 4,
                            resp, sizeof(resp), &got)) && got >= 4) {
            ct = resp[3];
            ProbeLog("avrcp: set abs vol %lu -> ctype 0x%02X%s",
                     (ULONG)(SetTo & 0x7F), (ULONG)ct,
                     (ct == 0x09) ? " (ACCEPTED)" : "");
        } else {
            ProbeLog("avrcp: set abs vol %lu: no reply", (ULONG)(SetTo & 0x7F));
        }

        // ---- 4) 再读一次，确认它到底有没有吃下去（操作数要重新填）
        Pause(80);
        op[0] = 0x00;
        op[1] = AVRCP_EVENT_VOLUME;
        op[2] = op[3] = op[4] = op[5] = 0x00;
        if (NT_SUCCESS(Xfer(&chan, 3, AVC_CTYPE_NOTIFY, AVC_OP_REG_NOTIFY, op, 6,
                            resp, sizeof(resp), &got)) && got) {
            UCHAR after = 0xFF;
            if (ParseVolume(resp, got, AVRCP_EVENT_VOLUME, &after))
                ProbeLog("avrcp: volume after set = %lu/127", (ULONG)after);
        }
    }

    if (pResponseCtype) *pResponseCtype = ct;

    BthL2capClose(&chan);
    return STATUS_SUCCESS;
}
