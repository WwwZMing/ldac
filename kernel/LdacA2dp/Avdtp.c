// ============================================================================
//  Avdtp.c — AVDTP 信令收发 + SEP 能力解析 + 建流状态机
// ============================================================================
//  两处旧实现跟规范不符，这里都改掉了：
//   1) 单包信令没有 2 字节长度字段（旧代码 buf[2..3] 塞长度，对端会把载荷解成垃圾）；
//   2) 信号 ID 编号有两套说法（Discover=0x00 还是 0x01），改为运行时校准。
//  另外旧代码把 Media Codec 首字节的高/低半字节读反了（media type 在高 4 位）。
// ============================================================================
#include "Avdtp.h"

static UCHAR   g_Tl = 0;              // Transaction Label，4 bit 循环
static LONG    g_SigOff = 0;          // 信号 ID 编号偏移：0 或 1
static BOOLEAN g_SigKnown = FALSE;

static UCHAR
SigId(_In_ UCHAR Base)
{
    return (UCHAR)(Base + g_SigOff);
}

// ------------------------------------------------- ACP：被动服务对端 ---
//  实测：JieLi 方案耳机拨入 PSM 25 后会自己当 INT 发 Discover，我们只能当 ACP。
//  之前两版都错在「一边等自己的响应、一边应答对端」——同一通道上并发提交两个
//  BRB，是高 IRQL 下系统直接复位的高度嫌疑。这里改成纯串行：
//      收一条命令 → 回一条响应 → 再收下一条
//  自己不发请求，就不存在并发。
//  实测（2026-11-03）：耳机拨进来发一条 Discover，我们按规范回 4 字节
//  （`02 01 04 08` = SEID 1 / audio），然后就安静了；有时紧接着把通道丢掉
//  （drops+1），有时留着不放。
//  这里等 12s 不是随便写的：这台对端**回包能慢到 9 秒**，用 2.5s 判"它不说话"
//  会把它的下一条命令（比如 GetCapabilities）直接判死，然后我们自己去驱动，
//  等于把它的话打断。宁可多等。
#define AVDTP_ACP_TIMEOUT_MS 12000
//  对端拨入后的**第一枪**只等这么短：实测 UGREEN-BT505 这类 sink 会主动拨 PSM 25，
//  但拨完一句话不说，等 source 先发 Discover（它只是先建通道，不是 INT）。
//  真 INT 的耳机（JieLi 系）几百毫秒内就会发 Discover，不会误判。
#define AVDTP_ACP_FIRST_TIMEOUT_MS 300   // 回连要快（原 800）

//  实现在文件后半段，ACP 循环要用（把耳机发来的原始字节记下来，
//  否则「它到底还说不说话」只能靠猜）。
static VOID
DumpPkt(_In_reads_(Len) const UCHAR* P, _In_ ULONG Len);

static VOID
AvdtpParseConfigured(_In_reads_(Len) const UCHAR* Cap, _In_ ULONG Len,
                     _Inout_ PAVDTP_ACP_RESULT Res)
{
    ULONG i = 0;

    while (i + 1 < Len) {
        UCHAR  b0   = Cap[i];
        UCHAR  cat  = (UCHAR)(b0 & 0x3F);
        UCHAR  losc = (UCHAR)((b0 >> 6) & 0x03);
        ULONG  hdr  = (losc != 0) ? 1 : 2;
        ULONG  cln  = (losc != 0) ? losc : Cap[i + 1];

        if (i + hdr + cln > Len) break;

        if (cat == AVDTP_CAT_MEDIA_CODEC && cln >= 10) {
            const UCHAR* v = Cap + i + hdr;
            if (v[0] == 0x00 && v[1] == 0xFF &&
                v[2] == 0x2D && v[3] == 0x01 && v[4] == 0x00 && v[5] == 0x00 &&
                v[6] == 0xAA && v[7] == 0x00) {
                Res->IsLdac  = TRUE;
                Res->RateBit = v[8];
                Res->ModeBit = v[9];
            }
        }
        i += hdr + cln;
    }
}

NTSTATUS
AvdtpServeAcp(_In_ PBTH_L2CAP_CHAN Chan, _Out_ PAVDTP_ACP_RESULT Res)
{
    UCHAR    pkt[768];
    UCHAR    out[96];
    ULONG    got;
    ULONG    guard = 0;
    LONG     drops0;
    NTSTATUS st;

    RtlZeroMemory(Res, sizeof(*Res));
    drops0 = g_L2capDrops;

    while (guard++ < 64) {
        //  对端拆链时 recv 会先卡满一个超时才报错，白等。indication 回调里
        //  有断开计数，涨了就说明这条通道没了，立刻收工。
        if (g_L2capDrops != drops0) {
            if (g_LogFn)
                g_LogFn("ACP: peer tore the channel down after %u msg(s)", guard - 1);
            return STATUS_CONNECTION_ABORTED;
        }
        got = 0;
        st = BthL2capRecv(Chan, pkt, sizeof(pkt),
                          (guard == 1) ? AVDTP_ACP_FIRST_TIMEOUT_MS : AVDTP_ACP_TIMEOUT_MS,
                          &got);
        if (!NT_SUCCESS(st)) {
            if (g_LogFn) g_LogFn("ACP: recv end 0x%08X (handled %u msgs)", st, guard - 1);
            return st;
        }
        if (got < 2) continue;
        DumpPkt(pkt, got);

        {
            UCHAR hdr = pkt[0];
            UCHAR pt  = (UCHAR)((hdr >> 2) & 0x03);
            UCHAR mt  = (UCHAR)(hdr & 0x03);
            UCHAR tl  = (UCHAR)(hdr >> 4);
            UCHAR raw = (UCHAR)(pkt[1] & 0x3F);
            //  对端用规范编号，我们内部常量 = Base + g_SigOff，查表要归一化；
            //  回包时原样回 raw（之前没归一化，把 Discover 当成 GetCapabilities 回错过）
            UCHAR sig = (UCHAR)((raw - g_SigOff) & 0x3F);
            ULONG o   = 0;

            //  只处理「单包命令」；响应/分片跳过
            if (pt != AVDTP_PKT_SINGLE || mt != AVDTP_MT_CMD) continue;

            out[o++] = (UCHAR)((tl << 4) | (AVDTP_PKT_SINGLE << 2) | AVDTP_MT_ACCEPT);
            out[o++] = raw;

            switch (sig) {
            case AVDTP_MSG_DISCOVER:
                //  SEP info = 2 字节：
                //    b0 = SEID<<2 | InUse<<1 | (低 2 位保留)
                //    b1 = MediaType<<4 | TSEP<<3
                //  我们是 SOURCE：MediaType=0(audio)、TSEP=0 -> b1 = 0x00。
                //  （0x08 是 SINK。之前把耳机自己的 3 个 sink SEP 抄了过来，
                //    等于告诉对端"我也是 sink"，它发现没有 source 就直接拆链。）
                out[o++] = (UCHAR)(1 << 2);                     // SEID 1
                out[o++] = 0x00;                                // audio | SOURCE
                Res->Discovered = TRUE;
                break;

            case AVDTP_MSG_GET_CAPABILITIES:
            case AVDTP_MSG_GET_ALL_CAPS:
                //  能力列表：Media Transport(01 00) + Media Codec(07 0A) + LDAC 参数。
                //  注意 `01 00` 不是"SEID + 0x00"，它就是 Media Transport 元素。
                out[o++] = AVDTP_CAT_MEDIA_TRANSPORT; out[o++] = 0;
                out[o++] = AVDTP_CAT_MEDIA_CODEC;     out[o++] = 10;
                out[o++] = 0x00; out[o++] = 0xFF;
                out[o++] = 0x2D; out[o++] = 0x01; out[o++] = 0x00; out[o++] = 0x00;
                out[o++] = 0xAA; out[o++] = 0x00;
                out[o++] = (UCHAR)(LDAC_RATE_44100 | LDAC_RATE_48000);
                out[o++] = (UCHAR)(LDAC_CH_MONO | LDAC_CH_DUAL | LDAC_CH_STEREO);
                break;

            case AVDTP_MSG_SET_CONFIGURATION:
                //  命令载荷：ACP SEID、INT SEID，然后才是能力元素。
                //  从 pkt+2 起解析会把两个 SEID 当成 capability，永远找不到 codec。
                if (got > 4) {
                    Res->IntSeid = (UCHAR)((pkt[3] >> 2) & 0x3F);
                    AvdtpParseConfigured(pkt + 4, got - 4, Res);
                }
                Res->Configured = TRUE;
                break;

            case AVDTP_MSG_OPEN:
                Res->Opened = TRUE;
                break;

            case AVDTP_MSG_START:
                Res->Started = TRUE;
                break;

            case AVDTP_MSG_CLOSE:
            case AVDTP_MSG_SUSPEND:
                break;

            default:
                out[0] = (UCHAR)((tl << 4) | (AVDTP_PKT_SINGLE << 2) | AVDTP_MT_REJECT);
                break;
            }

            if (g_LogFn) {
                g_LogFn("ACP: sig=0x%02X tl=%u reply=%u disc=%u cfg=%u open=%u start=%u ldac=%u rate=0x%02X mode=0x%02X",
                        sig, tl, o, Res->Discovered, Res->Configured,
                        Res->Opened, Res->Started, Res->IsLdac, Res->RateBit, Res->ModeBit);
            }

            st = BthL2capSend(Chan, out, o);
            if (!NT_SUCCESS(st)) {
                if (g_LogFn) g_LogFn("ACP: reply send failed 0x%08X", st);
                return st;
            }

            if (Res->Started) return STATUS_SUCCESS;

            //  对端是 sink 时，Discover 之后它不会替 source 做 SetConfiguration；
            //  它只是在"探源"。答完就把控制权交回上层，由 source 重开通道驱动。
            if (Res->Discovered && !Res->Configured) {
                if (g_LogFn)
                    g_LogFn("ACP: discovery answered; peer is a sink, handing back to source");
                return STATUS_SUCCESS;
            }
        }
    }
    return STATUS_IO_TIMEOUT;
}

// ---------------------------------------------------------------------------
//  原始包 hex dump —— 信令对不上时，唯一能一眼看出「对端到底回了什么」的
//  证据。上一轮 discover 失败就是因为内部日志被丢弃（g_LogFn=NULL），
//  只剩一个孤零零的 -1，什么也推不出来。别猜，看字节。
// ---------------------------------------------------------------------------
static VOID
DumpPkt(_In_reads_(Len) const UCHAR* P, _In_ ULONG Len)
{
    static const char kHex[] = "0123456789ABCDEF";
    CHAR   hex[3 * 16 + 1];
    ULONG  i, o = 0;
    ULONG  n = (Len < 16) ? Len : 16;

    if (g_LogFn == NULL || Len == 0) return;
    for (i = 0; i < n; i++) {
        hex[o++] = kHex[P[i] >> 4];
        hex[o++] = kHex[P[i] & 0x0F];
        hex[o++] = ' ';
    }
    if (o > 0 && hex[o - 1] == ' ') o--;
    hex[o] = '\0';
    g_LogFn("AVDTP recv: len=%u dump=%s%s", Len, hex, (Len > 16) ? " .." : "");
}

//  【2026-11-03 实测，别调小】这台耳机（UGREEN-BT505）的 AVDTP 事务延迟
//  固定在 **9 秒**左右 —— 用 3000ms/8000ms 等的时候，日志呈现的是"应答永远
//  比当前事务晚一拍"：
//      send tl=0 -> 8s 超时，然后 tl=0 的应答在 9s 才到（被我们当迟到包丢了）
//      send tl=1 -> 它的应答要 ~18s
//  按 TL 校验就会把这些应答全部当成"不是这条事务的"丢掉，看起来像对端死活不回。
//  它其实回，只是慢得离谱。所以超时给到 20s 并且**只发一次**：
//  重发只会让对端的应答队列更乱（实测 8s 版本第二次请求的应答直接落在超时之后）。
//  AVDTP 规范给客户端的推荐超时本来就有 30s，对端慢是允许的。
//  单次事务的读超时。UGREEN-BT505 实测每条应答稳定晚 ~5s，所以给 12s，
//  并且**每笔事务自己等够**，不靠重试去捡漏（捡漏会让 SEID 和 caps 错位一拍）。
#define AVDTP_XACT_TIMEOUT_MS 12000

//  同 TL 重发：每 2s 催一次，最多 6 轮（总计 12s）。
//  这台 sink 不催不回，但一催就用**同一条事务**的 TL 回，所以重发不换 TL。
//  每一轮"催 + 等"给 250ms 预算，最多催 6 轮（单条命令最坏 1.5 秒）。
//  实测这台的应答在毫秒级，250ms 足够；原来写的 800ms 其实一天都没生效过
//  （见下面 poll 循环里的说明）。
#define AVDTP_NUDGE_WAIT_MS 250
#define AVDTP_NUDGES        6

static ULONG g_XactTimeoutMs = AVDTP_XACT_TIMEOUT_MS;

//  单条 AVDTP 事务 ---
NTSTATUS
AvdtpXact(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR Sig,
          _In_reads_opt_(Len) const UCHAR* Payload, _In_ USHORT Len,
          _Out_ PAVDTP_REPLY Reply)
{
    UCHAR        buf[320];
    UCHAR        pkt[1600];
    UCHAR        tl;
    ULONG        got = 0;
    ULONG        used = 0;
    UCHAR        respSig = 0;
    UCHAR        msgType = 0;
    BOOLEAN      gotHead = FALSE;
    BOOLEAN      done = FALSE;
    ULONG        nudge;
    NTSTATUS     st;

    Reply->MsgType = 0;
    Reply->Sig     = 0;
    Reply->Len     = 0;

    if (Len > sizeof(buf) - 2) return STATUS_BUFFER_TOO_SMALL;

    tl = (UCHAR)(g_Tl & 0x0F);
    g_Tl = (UCHAR)((g_Tl + 1) & 0x0F);

    // 单包：只有头 + 信号 ID + 载荷，没有长度字段
    buf[0] = (UCHAR)((tl << 4) | (AVDTP_PKT_SINGLE << 2) | AVDTP_MT_CMD);
    buf[1] = Sig;
    if (Len) RtlCopyMemory(buf + 2, Payload, Len);

    if (g_LogFn) g_LogFn("AVDTP send: sig=0x%02X tl=%u len=%u", Sig, tl, (ULONG)Len);

    st = BthL2capSend(Chan, buf, 2u + Len);
    if (!NT_SUCCESS(st)) return st;

    //  ------------------------------------------------------------------
    //  重发（nudge）循环：**TL 不变**，同一个事务反复催。
    //  这台 sink 的应答稳定晚 ~5s，而且不催就不回；而"换新 TL 重开事务"会让
    //  应答整体错位一拍（2026-11-05 实测：SEID 和 capabilities 对不上，LDAC SEP
    //  被整个跳过）。同 TL 重发时迟到的应答仍然对得上号，不会错位。
    //  ------------------------------------------------------------------
    for (nudge = 0; nudge < AVDTP_NUDGES && !done; nudge++) {
        LONGLONG t0;
        LONGLONG budget;

        if (nudge != 0) {
            if (g_LogFn)
                g_LogFn("AVDTP nudge %u/%u: resend sig=0x%02X tl=%u",
                        nudge, AVDTP_NUDGES - 1, Sig, tl);
            (VOID)BthL2capSend(Chan, buf, 2u + Len);
        }

        t0     = (LONGLONG)KeQueryInterruptTime();          // 100ns
        budget = (LONGLONG)AVDTP_NUDGE_WAIT_MS * 10000LL;

        while (!done) {
            LARGE_INTEGER poll;

            poll.QuadPart = -10LL * 10000LL;                // 相对 10ms

            st = BthL2capRecv(Chan, pkt, sizeof(pkt), 50, &got);
            if (!NT_SUCCESS(st)) {
                //  其他状态码 = 通道本身出问题，直接收工。
                if (st != STATUS_IO_TIMEOUT) {
                    if (g_LogFn)
                        g_LogFn("AVDTP recv fail: 0x%08X (sig=0x%02X tl=%u)", st, Sig, tl);
                    break;
                }
                //  ⚠ 这个 BT 栈的 ACL_TRANSFER IN 在"没有数据"时**立刻**返回
                //  STATUS_IO_TIMEOUT，根本不 honor Timeout 字段（2026-11-09 实测：
                //  连续 5 次 recv 全在 5 毫秒内返回）。所以原来那句
                //  "while (!done && guard++ < 16)" 实际是在 5 毫秒里轮询 5 次就判死
                //  —— 对端应答只要晚到 10ms 就必然被丢掉，表现就是"宿主机上第一次
                //  Discover 全灭、紧接着第二次却通"。现在按**墙上时间**给预算，
                //  两次之间真的睡一下（不能按次数算：内核相对延时会被 tick 放大）。
                if ((LONGLONG)KeQueryInterruptTime() - t0 >= budget) break;
                KeDelayExecutionThread(KernelMode, FALSE, &poll);
                continue;
            }
            if (got < 1) return STATUS_DATA_ERROR;
            DumpPkt(pkt, got);

        UCHAR hdr = pkt[0];
        UCHAR pt  = (UCHAR)((hdr >> 2) & 0x03);
        UCHAR mt  = (UCHAR)(hdr & 0x03);
        UCHAR rtl = (UCHAR)(hdr >> 4);

        if (pt == AVDTP_PKT_SINGLE) {
            if (got < 2) return STATUS_DATA_ERROR;

            //  对端发来的命令不在这里回应——并发提交 BRB 风险太高。
            //  走 ACP 的场合由 AvdtpServeAcp 串行处理；这里只丢弃继续等自己的响应。
            if (mt == AVDTP_MT_CMD) {
                if (g_LogFn) g_LogFn("AVDTP: peer CMD sig=0x%02X dropped (xact mode)", pkt[1] & 0x3F);
                continue;
            }

            //  TL 不匹配有两种情况：
            //   a) 真的是更早事务的迟到应答 —— 对端（UGREEN-BT505 实测）会慢
            //      5s 以上才回，重试期间上一轮的应答刚好到达；
            //   b) 对端就爱用旧 TL 回（不规范实现）。
            //  两种情况下**信号 ID 对得上**就说明它答的是我们这类请求，Discover
            //  这类幂等请求直接收下即可；只有信号也不同才是真的无关报文。
            //  实测：3 次重试全被当成 stale 丢掉，最后报"对端不回"，其实每轮都答了。
            //  **严格按 TL 匹配**。有了"同 TL 重发"之后，对端迟到的应答会带着
            //  当前 TL 回来，所以 TL 不同的必定是上一条事务的残余应答 —— 收下就会
            //  让 SEID 与 capabilities 整体错位（实测会跳过带 LDAC 的 SEID 3）。
            if (rtl != tl) {
                if (g_LogFn)
                    g_LogFn("AVDTP: stale response sig=0x%02X tl=%u (waiting %u) dropped",
                            (ULONG)(pkt[1] & 0x3F), rtl, tl);
                continue;
            }
            respSig = pkt[1];
            msgType = mt;
            used = got - 2;
            if (used > sizeof(Reply->Body)) used = sizeof(Reply->Body);
            RtlCopyMemory(Reply->Body, pkt + 2, used);
            gotHead = TRUE;
            done = TRUE;
        } else if (pt == AVDTP_PKT_START) {
            if (got < 3) return STATUS_DATA_ERROR;
            if (mt == AVDTP_MT_CMD) {
                continue;                                 // 同上：xact 模式下不并发应答
            }
            if (rtl != tl) continue;
            respSig = pkt[2];                         // [1] 是 NOSP(高4位)
            msgType = mt;
            used = got - 3;
            if (used > sizeof(Reply->Body)) used = sizeof(Reply->Body);
            RtlCopyMemory(Reply->Body, pkt + 3, used);
            gotHead = TRUE;
        } else {                                      // CONTINUE / END
            if (!gotHead) continue;                   // 分片中间，先不管
            ULONG add = got - 1;
            if (used + add > sizeof(Reply->Body)) add = sizeof(Reply->Body) - used;
            RtlCopyMemory(Reply->Body + used, pkt + 1, add);
            used += add;
            if (pt == AVDTP_PKT_END) done = TRUE;
            }
        }
    }

    if (!gotHead) return STATUS_IO_TIMEOUT;

    Reply->MsgType = msgType;
    Reply->Sig     = respSig;
    Reply->Len     = used;

    if (msgType == AVDTP_MT_ACCEPT) {
        return STATUS_SUCCESS;
    }

    if (g_LogFn) {
        g_LogFn("AVDTP sig=0x%02X rejected: mt=0x%02X err=0x%02X len=%u",
                Sig, msgType, used ? Reply->Body[0] : 0, used);
    }
    return (msgType == AVDTP_MT_GENERAL) ? STATUS_NOT_SUPPORTED : STATUS_INVALID_DEVICE_REQUEST;
}

// ------------------------------------------------------------ 信号 ID 校准 ---
//  AVDTP 1.3 Table 8.19：Discover = 0x01（0x00 是 RESERVED）。
//  旧实现「两个编号都试一遍」，退到偏移 -1 时发出去的正是 **0x00 这个保留值**。
//  那很危险：对端收到不认识的信号通常不会断 L2CAP，只是静默丢弃、顺手可能把
//  这次会话标成异常 —— 表现是"之后我发什么都不理我"，而我们却会误判成传输问题。
//  所以这里只按规范值走一次，把「等对端就绪」交给 AvdtpDiscover 的时间退避重试。
LONG
AvdtpCalibrate(_In_ PBTH_L2CAP_CHAN Chan)
{
    AVDTP_REPLY r;
    NTSTATUS    st;

    g_SigOff = 0;
    g_Tl     = 0;

    st = AvdtpXact(Chan, AVDTP_MSG_DISCOVER, NULL, 0, &r);
    if (NT_SUCCESS(st) && r.Len >= 2) {
        g_SigKnown = TRUE;
        if (g_LogFn)
            g_LogFn("AVDTP signal base = 0x01 (discover ok, %u SEP(s))", r.Len / 2);
        return 0;
    }
    if (g_LogFn) g_LogFn("AVDTP calibrate: discover(0x01) failed 0x%08X", st);
    return -1;
}

// ------------------------------------------------------------------ 能力 ---
static UCHAR
CodecName(_In_ UCHAR MediaType, _In_ UCHAR CodecType)
{
    if (MediaType != 0x00) return '?';
    switch (CodecType) {
    case 0x00: return 'S';      // SBC
    case 0x01: return 'M';      // MPEG-1/2
    case 0x02: return 'A';      // AAC
    case 0x03: return 'T';      // ATRAC
    case 0x0F: return 'V';      // Vendor（低 4 位）
    case 0xFF: return 'V';
    default:   return '?';
    }
}

//  在给定缓冲里找 Sony vendor id（两种字节序都试，因为 A2DP 的 Vendor ID 字节序
//  各家实现有差异，硬编码一种会漏）。找到返回索引，否则 -1。
static LONG
FindPattern(_In_reads_(Len) const UCHAR* B, _In_ ULONG Len,
            _In_reads_(N) const UCHAR* Pat, _In_ ULONG N)
{
    ULONG i;
    if (Len < N) return -1;
    for (i = 0; i + N <= Len; i++) {
        if (RtlEqualMemory(B + i, Pat, N)) return (LONG)i;
    }
    return -1;
}

VOID
AvdtpParseSep(_Inout_ PAVDTP_SEP Sep, _In_reads_(Len) const UCHAR* Body, _In_ ULONG Len)
{
    static const UCHAR sonyBe[4] = { 0x00, 0x00, 0x01, 0x2D };
    static const UCHAR sonyLe[4] = { 0x2D, 0x01, 0x00, 0x00 };
    ULONG i = 0;

    Sep->HasCodec    = FALSE;
    Sep->CapLen      = (Len > AVDTP_MAX_CAPS) ? AVDTP_MAX_CAPS : Len;
    Sep->CodecSpecOff = 0;
    RtlCopyMemory(Sep->Caps, Body, Sep->CapLen);

    if (g_LogFn) g_LogFn("  caps[%u]: %u bytes", Sep->Seid, Len);

    while (i + 1 < Len) {
        UCHAR        b0   = Body[i];
        UCHAR        cat  = (UCHAR)(b0 & 0x3F);
        UCHAR        losc = (UCHAR)((b0 >> 6) & 0x03);
        ULONG        hdr  = 2;
        ULONG        cln;
        const UCHAR* v;

        if (losc != 0) {                 // LOSC 非 0：长度就是 LOSC 本身，没有长度字节
            cln = losc;
            hdr = 1;
        } else {
            cln = Body[i + 1];
        }
        if (i + hdr + cln > Len) break;
        v = Body + i + hdr;

        if (cat == AVDTP_CAT_MEDIA_CODEC && cln >= 6) {
            LONG vi = FindPattern(v, cln, sonyBe, 4);
            ULONG after = 4;

            Sep->HasCodec  = TRUE;
            //  Media Codec 能力元素里，Media Type 和 Codec Type 各占一个字节：
            //    00 ff 2d 01 00 00 aa 00 3c 07
            //    │  │  └ vendor ┘ └codec┘ └spec┘
            //    │  └ codec type 0xFF = non-A2DP (vendor codec)
            //    └ media type 0x00 = audio
            //  老写法把它们当同一字节的高低半字节，会把 LDAC 误认成 SBC（codec 0x00）。
            Sep->MediaType = v[0];
            Sep->CodecType = v[1];
            Sep->VendorId  = 0;
            Sep->CodecId   = 0;

            //  Vendor ID / Codec ID 在线上都是小端（0x2d 0x01 0x00 0x00 = 0x0000012D）
            if (vi < 0) {
                vi = FindPattern(v, cln, sonyLe, 4);
                if (vi >= 0) {
                    Sep->VendorId = ((ULONG)v[vi]) | ((ULONG)v[vi + 1] << 8) |
                                    ((ULONG)v[vi + 2] << 16) | ((ULONG)v[vi + 3] << 24);
                }
            } else {
                Sep->VendorId = ((ULONG)v[vi] << 24) | ((ULONG)v[vi + 1] << 16) |
                                ((ULONG)v[vi + 2] << 8) | v[vi + 3];
            }

            if (g_LogFn) {
                g_LogFn("  codec: mediaType=%u codecType=0x%X first=0x%02X",
                        Sep->MediaType, Sep->CodecType, v[0]);
            }

            if (vi >= 0) {
                ULONG p = (ULONG)vi + after;              // 跳过 Vendor ID
                if (p + 2 <= cln) {
                    Sep->CodecId = (USHORT)(v[p] | (v[p + 1] << 8));   // 小端：aa 00 = 0x00AA
                    p += 2;
                }
                Sep->CodecSpecOff = (ULONG)(i + hdr) + p;  // 相对 Caps 的偏移

                if (p + 2 <= cln) {
                    Sep->Rates = v[p];
                    Sep->Modes = v[p + 1];
                }
                if (g_LogFn) {
                    g_LogFn("  vendor=0x%08X codecId=0x%04X specOff=%u rates=0x%02X modes=0x%02X",
                            Sep->VendorId, Sep->CodecId, Sep->CodecSpecOff,
                            Sep->Rates, Sep->Modes);
                }
            }
        } else if (cat == AVDTP_CAT_MEDIA_TRANSPORT) {
            if (g_LogFn) g_LogFn("  MediaTransport");
        } else if (cat == AVDTP_CAT_REPORTING) {
            if (g_LogFn) g_LogFn("  Reporting");
        } else if (cat == AVDTP_CAT_CONTENT_PROTECTION) {
            if (g_LogFn) g_LogFn("  ContentProtection");
        } else {
            if (g_LogFn) g_LogFn("  cat=0x%02X len=%u", cat, cln);
        }

        i += hdr + cln;
    }
}

// --------------------------------------------------------------- DISCOVER ---
//  带时间退避的重试，次数和间隔都刻意保守（总时长 < 15s）。
//
//  为什么必须重试：耳机刚建好 ACL 的头几百毫秒到几秒里，它的 AVDTP 常常还没就绪
//  （它自己在忙 AVRCP / 角色切换 / 内部状态机）。这时候收到的 Discover 会被
//  **静默丢弃** —— 不回报错、也不断链，就是不理。btha2dp 那边是先做 SDP、再连
//  AVRCP，才轮到 A2DP 信令，天生"晚"，所以同一个 00 01 它发必答；
//  我们建好通道立刻发，就一次都问不出来。这条差异是 2026-11-03 定性的。
//  实测 UGREEN-BT505：每条 AVDTP 应答稳定晚 ~5s 才到（不是丢包）。
//  所以 Discover 只发一次、等够 12s —— 重试只会让应答错位一拍，
//  把 SEID 和 capabilities 对错（2026-11-05 实测：靠"下一笔捡漏"会漏掉 LDAC SEP）。
#define AVDTP_DISCOVER_TRIES 1
#define AVDTP_DISCOVER_TRY_TIMEOUT_MS 12000

NTSTATUS
AvdtpDiscover(_In_ PBTH_L2CAP_CHAN Chan,
              _Out_writes_(Max) PAVDTP_SEP Seps, _In_ ULONG Max, _Out_ PULONG Count)
{
    AVDTP_REPLY   r;
    ULONG         n = 0;
    ULONG         attempt;
    LARGE_INTEGER gap;
    NTSTATUS      st = STATUS_IO_TIMEOUT;
    ULONG         savedTimeout;

    *Count = 0;
    RtlZeroMemory(&r, sizeof(r));

    //  常量已按规范对齐；这里顺手把可能残留的偏移清掉（Discover 只能是 0x01）。
    if (!g_SigKnown) {
        g_SigOff = 0;
        g_Tl     = 0;
    }

    gap.QuadPart = -1500000LL;              // 相对 1.5s

    //  每轮只等 5s：3 轮 + 2 次 1.5s 间隔 = 18s，仍在原来 20s 的预算内。
    savedTimeout = g_XactTimeoutMs;
    g_XactTimeoutMs = AVDTP_DISCOVER_TRY_TIMEOUT_MS;

    for (attempt = 0; attempt < AVDTP_DISCOVER_TRIES; attempt++) {
        st = AvdtpXact(Chan, SigId(AVDTP_MSG_DISCOVER), NULL, 0, &r);
        if (NT_SUCCESS(st) && r.Len >= 2) {
            g_SigKnown = TRUE;
            if (g_LogFn)
                g_LogFn("AVDTP discover ok (attempt %u, %u SEP(s))", attempt, r.Len / 2);
            break;
        }
        if (g_LogFn) {
            g_LogFn("AVDTP discover attempt %u/%u failed 0x%08X%s",
                    attempt + 1, AVDTP_DISCOVER_TRIES, st,
                    (attempt + 1 < AVDTP_DISCOVER_TRIES) ? " -> retry" : "");
        }
        if (attempt + 1 < AVDTP_DISCOVER_TRIES) {
            KeDelayExecutionThread(KernelMode, FALSE, &gap);
        }
    }

    g_XactTimeoutMs = savedTimeout;

    if (!NT_SUCCESS(st) || r.Len < 2) {
        return NT_SUCCESS(st) ? STATUS_NOT_SUPPORTED : st;
    }

    for (ULONG i = 0; i + 1 < r.Len && n < Max; i += 2) {
        RtlZeroMemory(&Seps[n], sizeof(AVDTP_SEP));
        Seps[n].Seid  = (UCHAR)(r.Body[i] >> 2);
        Seps[n].InUse = (BOOLEAN)((r.Body[i] >> 1) & 0x01);
        //  第二字节 bit3 才是 TSEP（1=SNK）；之前 &0x03 取，0x08 永远算成 0，
        //  日志里才会一直是"全是 SRC"。
        Seps[n].Tsep  = (UCHAR)((r.Body[i + 1] >> 3) & 0x01);
        if (g_LogFn) {
            g_LogFn("SEP seid=%u %s inUse=%u",
                    Seps[n].Seid,
                    Seps[n].Tsep == AVDTP_TSEP_SNK ? "SNK" : "SRC",
                    Seps[n].InUse);
        }
        n++;
    }

    *Count = n;
    return STATUS_SUCCESS;
}

// ---------------------------------------------------- GET_CAPABILITIES ---
//  上一笔 GetCapabilities 的结果。这台 sink 会**重复回上一条请求的应答**
//  （nudge 重发引起），于是 SEID 2 常被塞进 SEID 1 的 SBC 能力，整体错位，
//  真正带 LDAC 的 SEID 3 永远问不到。判据：内容与上一笔完全相同 = 这是重复包。
static UCHAR g_LastCaps[AVDTP_MAX_CAPS];
static ULONG g_LastCapsLen;

NTSTATUS
AvdtpGetCapabilities(_In_ PBTH_L2CAP_CHAN Chan, _Inout_ PAVDTP_SEP Sep)
{
    AVDTP_REPLY r;
    UCHAR       p[1];
    NTSTATUS    st;
    ULONG       tries;

    p[0] = (UCHAR)(Sep->Seid << 2);

    for (tries = 0; tries < 4; tries++) {
        st = AvdtpXact(Chan, SigId(AVDTP_MSG_GET_CAPABILITIES), p, 1, &r);
        if (!NT_SUCCESS(st)) return st;

        if (g_LastCapsLen != 0 && g_LastCapsLen == r.Len &&
            RtlEqualMemory(g_LastCaps, r.Body, r.Len)) {
            if (g_LogFn)
                g_LogFn("  caps duplicate of previous SEP (%u bytes) -> retry", r.Len);
            continue;               // 重复包：不是这个 SEID 的能力，再问一次
        }
        break;                      // 内容变了，就是本 SEID 的真实能力
    }
    if (!NT_SUCCESS(st)) return st;

    if (r.Len <= sizeof(g_LastCaps)) {
        RtlCopyMemory(g_LastCaps, r.Body, r.Len);
        g_LastCapsLen = r.Len;
    }

    //  GetCapabilities Response 的载荷就是能力元素列表本身，**没有** ACP SEID。
    //  之前把开头的 `01 00`（Media Transport，cat=0x01 len=0）当成
    //  "SEID 1 + 0x00" 跳过：Media Transport 丢了、Caps 从 0x07 开始，
    //  后面 SetConfiguration 回放的能力元素少一项，对端可能直接拒绝。
    AvdtpParseSep(Sep, r.Body, r.Len);
    return STATUS_SUCCESS;
}

// ------------------------------------------------------ SET_CONFIGURATION ---
NTSTATUS
AvdtpSetConfiguration(_In_ PBTH_L2CAP_CHAN Chan, _In_ const PAVDTP_SEP Sep,
                      _In_ UCHAR IntSeid, _In_ UCHAR RateBit, _In_ UCHAR ModeBit)
{
    AVDTP_REPLY  r;
    UCHAR        p[AVDTP_MAX_CAPS + 4];
    ULONG        o = 0;
    ULONG        i = 0;
    NTSTATUS     st;

    //  告诉对端：ACP SEID（耳机那个）配给我们的 INT SEID（本机源端点）
    p[o++] = (UCHAR)(Sep->Seid << 2);
    p[o++] = (UCHAR)((IntSeid & 0x3F) << 2);

    //  回放对端自己上报的能力元素，只留 Media Transport + Media Codec，
    //  并把 codec specific 的两个字节改成「单比特」的实际配置。
    while (i + 1 < Sep->CapLen && o + 8 < sizeof(p)) {
        UCHAR  b0   = Sep->Caps[i];
        UCHAR  cat  = (UCHAR)(b0 & 0x3F);
        UCHAR  losc = (UCHAR)((b0 >> 6) & 0x03);
        ULONG  hdr  = (losc != 0) ? 1 : 2;
        ULONG  cln  = (losc != 0) ? losc : Sep->Caps[i + 1];

        if (i + hdr + cln > Sep->CapLen) break;
        if (o + hdr + cln > sizeof(p)) break;

        if (cat == AVDTP_CAT_MEDIA_TRANSPORT || cat == AVDTP_CAT_MEDIA_CODEC) {
            RtlCopyMemory(p + o, Sep->Caps + i, hdr + cln);

            if (cat == AVDTP_CAT_MEDIA_CODEC && Sep->Rates != 0) {
                ULONG rel = Sep->CodecSpecOff - (i + hdr);      // codec spec 在元素内的偏移
                if (Sep->CodecSpecOff >= i + hdr && rel + 2 <= cln) {
                    p[o + hdr + rel]     = RateBit;
                    p[o + hdr + rel + 1] = ModeBit;
                }
            }
            o += hdr + cln;
        }
        i += hdr + cln;
    }

    if (g_LogFn) {
        g_LogFn("SET_CONFIGURATION acp=%u int=%u capBytes=%u (rate=0x%02X mode=0x%02X)",
                Sep->Seid, IntSeid, o, RateBit, ModeBit);
    }

    st = AvdtpXact(Chan, SigId(AVDTP_MSG_SET_CONFIGURATION), p, (USHORT)o, &r);
    return st;
}

// ------------------------------------------------- SBC SET_CONFIGURATION ---
//  对照实验用：把 ACP 的 SBC SEP 配成本机定的参数（48k / joint stereo /
//  16 blocks / 8 subbands / loudness / bitpool 2..38）。SBC 的 codec specific
//  4 个字节必须逐字段填死，不能像 LDAC 那样回放对端 caps（那是"支持哪些"的位图）。
NTSTATUS
AvdtpSetSbcConfiguration(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR AcpSeid, _In_ UCHAR IntSeid,
                         _In_ UCHAR FreqNibble, _In_ UCHAR ModeNibble,
                         _In_ UCHAR MinBp, _In_ UCHAR MaxBp)
{
    AVDTP_REPLY r;
    UCHAR       p[12];
    ULONG       o = 0;

    p[o++] = (UCHAR)(AcpSeid << 2);
    p[o++] = (UCHAR)((IntSeid & 0x3F) << 2);

    p[o++] = AVDTP_CAT_MEDIA_TRANSPORT; p[o++] = 0;

    p[o++] = AVDTP_CAT_MEDIA_CODEC;     p[o++] = 6;
    p[o++] = 0x00;                                  // Media Type: audio
    p[o++] = 0x00;                                  // Codec Type: SBC
    p[o++] = (UCHAR)(((FreqNibble & 0x0F) << 4) | (ModeNibble & 0x0F));
    p[o++] = (UCHAR)((3 << 4) | (1 << 2) | 1);      // 16 blocks, 8 subbands, Loudness
    p[o++] = MinBp;
    p[o++] = MaxBp;

    if (g_LogFn)
        g_LogFn("SET_CONFIGURATION(SBC) acp=%u int=%u freq=%u mode=%u bp=%u..%u",
                AcpSeid, IntSeid, FreqNibble, ModeNibble, MinBp, MaxBp);

    return AvdtpXact(Chan, SigId(AVDTP_MSG_SET_CONFIGURATION), p, (USHORT)o, &r);
}

// ------------------------------------------------- AAC SET_CONFIGURATION ---
//  字段来源（权威）：third_party 之外的 AOSP 头 a2dp_aac_constants.h。
//  注意第 3/4 字节不是「一个 16 位大端掩码」，而是**按位分摊**：
//    Octet1 bit0..7 = 44100/32000/24000/22050/16000/12000/11025/8000
//    Octet2 bit7..4 = 48000/64000/88200/96000，bit3..2 = 声道，bit1..0 = RFA
//  实测这台 sink：80 01 8C 82 00 00 = MPEG-2 LC / 44.1k+48k / mono+stereo /
//  VBR 支持 / 128 kbps(0x020000)。配置时每项只留一个比特。
NTSTATUS
AvdtpSetAacConfiguration(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR AcpSeid, _In_ UCHAR IntSeid,
                         _In_ UCHAR ObjType, _In_ UCHAR SfLow, _In_ UCHAR SfHigh,
                         _In_ UCHAR ChMode, _In_ BOOLEAN Vbr, _In_ ULONG BitRate)
{
    AVDTP_REPLY r;
    UCHAR       p[16];
    ULONG       o = 0;

    p[o++] = (UCHAR)(AcpSeid << 2);
    p[o++] = (UCHAR)((IntSeid & 0x3F) << 2);

    p[o++] = AVDTP_CAT_MEDIA_TRANSPORT; p[o++] = 0;

    p[o++] = AVDTP_CAT_MEDIA_CODEC;     p[o++] = 8;      // AAC 的 LOSC 固定 8
    p[o++] = 0x00;                                       // Media Type: audio
    p[o++] = 0x02;                                       // Codec Type: AAC
    p[o++] = ObjType;                                    // MPEG-2/4 LC
    p[o++] = SfLow;                                      // 8k..44.1k 选一
    p[o++] = (UCHAR)((SfHigh & 0xF0) | (ChMode & 0x0C)); // 48k..96k | 声道
    p[o++] = (UCHAR)((Vbr ? 0x80 : 0x00) |
                     ((BitRate >> 16) & 0x7F));
    p[o++] = (UCHAR)((BitRate >> 8) & 0xFF);
    p[o++] = (UCHAR)(BitRate & 0xFF);

    if (g_LogFn) {
        g_LogFn("SET_CONFIGURATION(AAC) acp=%u int=%u obj=0x%02X sf=%02X/%02X ch=0x%02X vbr=%u br=%u",
                AcpSeid, IntSeid, ObjType, SfLow, SfHigh, ChMode,
                (ULONG)Vbr, BitRate);
    }

    return AvdtpXact(Chan, SigId(AVDTP_MSG_SET_CONFIGURATION), p, (USHORT)o, &r);
}

// ------------------------------------------------------------------ OPEN ---
NTSTATUS
AvdtpOpen(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR Seid)
{
    AVDTP_REPLY r;
    UCHAR       p[1];

    p[0] = (UCHAR)(Seid << 2);
    return AvdtpXact(Chan, SigId(AVDTP_MSG_OPEN), p, 1, &r);
}

// ----------------------------------------------------------------- START ---
NTSTATUS
AvdtpStart(_In_ PBTH_L2CAP_CHAN Chan, _In_ UCHAR Seid)
{
    AVDTP_REPLY r;
    UCHAR       p[1];

    p[0] = (UCHAR)(Seid << 2);
    return AvdtpXact(Chan, SigId(AVDTP_MSG_START), p, 1, &r);
}

// ===========================================================================
//  媒体传输（PSM 33）：RTP 头 + LDAC 帧
// ===========================================================================
static VOID
PutBe16(_Out_writes_(2) UCHAR* p, _In_ USHORT v)
{
    p[0] = (UCHAR)(v >> 8);
    p[1] = (UCHAR)(v & 0xFF);
}

static VOID
PutBe32(_Out_writes_(4) UCHAR* p, _In_ ULONG v)
{
    p[0] = (UCHAR)(v >> 24);
    p[1] = (UCHAR)(v >> 16);
    p[2] = (UCHAR)(v >> 8);
    p[3] = (UCHAR)(v);
}

VOID
AvdtpMediaReset(_Out_ PAVDTP_MEDIA_STATE St, _In_ ULONG Ssrc)
{
    St->Seq           = 0;
    St->Timestamp     = 0;
    St->Ssrc          = Ssrc;
    St->PayloadType   = AVDTP_RTP_PT_LDAC;
    //  新流的第一包必须带 marker：耳机把「M=1 且序列号不连续」当成新的
    //  talk spurt 起点。漏置的话部分 Sink 会把第一帧当不完整帧丢掉。
    St->MarkerPending = TRUE;
}

VOID
AvdtpMediaBeginTalkSpurt(_Inout_ PAVDTP_MEDIA_STATE St)
{
    St->MarkerPending = TRUE;
}

ULONG
AvdtpMediaBuildPacket(_Inout_ PAVDTP_MEDIA_STATE St,
                      _In_reads_(PayloadLen) const UCHAR* Payload,
                      _In_ ULONG PayloadLen,
                      _In_ ULONG SamplesInPayload,
                      _Out_writes_(OutCap) UCHAR* Out,
                      _In_ ULONG OutCap)
{
    ULONG need;

    if (St == NULL || Out == NULL || Payload == NULL || PayloadLen == 0) {
        return 0;
    }

    need = AVDTP_RTP_HDR_LEN + PayloadLen;
    if (OutCap < need) {
        return 0;
    }

    Out[0] = 0x80;                          // V=2, P=0, X=0, CC=0
    Out[1] = (UCHAR)((St->PayloadType & 0x7F) |
                     (St->MarkerPending ? 0x80 : 0x00));
    PutBe16(&Out[2], St->Seq);
    PutBe32(&Out[4], St->Timestamp);
    PutBe32(&Out[8], St->Ssrc);

    RtlCopyMemory(Out + AVDTP_RTP_HDR_LEN, Payload, PayloadLen);

    //  只在成功时推进状态。容量不足/参数非法时提前返回，序列号不会凭空跳一格
    //  ——RTP 序列号空洞会让对端认为是丢包并触发重传或抖动缓冲重置。
    St->Seq++;
    St->Timestamp    += SamplesInPayload;
    St->MarkerPending = FALSE;

    return need;
}
