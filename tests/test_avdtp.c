// ============================================================================
//  test_avdtp.c — AVDTP 能力解析的纯逻辑单测（用户态，无内核）
//
//  重点覆盖历史上真实出过 bug 的三处：
//    1) Media Codec 首字节的高/低半字节读反（media type 在高 4 位）
//    2) Vendor ID / Codec ID 宽度（4 字节 + 2 字节），以及 BE/LE 两种排布
//    3) 单包信令没有 2 字节长度字段（这里体现在能力元素的 LOSC 两种编码形式）
// ============================================================================
#include "Avdtp.h"
#include <stdio.h>
#include <string.h>

static int g_Fail = 0;
static int g_Pass = 0;

#define CHECK(cond, fmt, ...)                                            \
    do {                                                                 \
        if (cond) { g_Pass++; }                                          \
        else {                                                           \
            g_Fail++;                                                    \
            printf("  [FAIL] %s:%d  " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
        }                                                                \
    } while (0)

// 内核日志回调（Avdtp.c 通过 g_LogFn 打日志）
static VOID TestLog(_In_ PCSTR fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    printf("      log: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}
PPROBE_LOG g_LogFn = TestLog;

// BthL2cap 的桩：解析测试不碰真实 L2CAP
NTSTATUS BthL2capAttach(PDEVICE_OBJECT p) { (void)p; return STATUS_UNSUCCESSFUL; }
NTSTATUS BthParseAddress(PCSTR s, BTH_ADDR *o) { (void)s; (void)o; return STATUS_UNSUCCESSFUL; }
NTSTATUS BthL2capOpen(BTH_ADDR a, USHORT psm, BTH_L2CAP_CHAN *c)
{ (void)a; (void)psm; (void)c; return STATUS_UNSUCCESSFUL; }
NTSTATUS BthL2capGetLocalAddress(BTH_ADDR *o) { (void)o; return STATUS_UNSUCCESSFUL; }
NTSTATUS BthL2capSend(BTH_L2CAP_CHAN *c, const UCHAR *b, ULONG l)
{ (void)c; (void)b; (void)l; return STATUS_UNSUCCESSFUL; }
NTSTATUS BthL2capRecv(BTH_L2CAP_CHAN *c, UCHAR *b, ULONG cap, ULONG t, ULONG *got)
{ (void)c; (void)b; (void)cap; (void)t; (void)got; return STATUS_UNSUCCESSFUL; }
VOID BthL2capClose(BTH_L2CAP_CHAN *c) { (void)c; }

// ---------------------------------------------------------------------------
//  用例
// ---------------------------------------------------------------------------
static void Test_SonyVendor_BigEndian(void)
{
    printf("\n[1] 标准形式：cat=MEDIA_CODEC，Sony VendorID 大端 00 00 01 2D\n");

    UCHAR body[] = {
        0x07, 0x09,                     // header: LOSC=0, cat=0x07(Media Codec) / len=9
        0x0A,                           // mediaType(高4位)=0(Audio), codecType(低4位)=0x0A
        0x00, 0x00, 0x01, 0x2D,         // Vendor ID (BE) = 0x0000012D (Sony)
        0x00, 0xAA,                     // Codec ID = 0x00AA (LDAC)
        0xFF, 0x02                      // codec specific: rates / modes 位图
    };

    AVDTP_SEP sep;
    memset(&sep, 0xAA, sizeof(sep));
    sep.Seid = 3;

    AvdtpParseSep(&sep, body, sizeof(body));

    CHECK(sep.HasCodec == TRUE,                       "应识别出 Media Codec 能力");
    CHECK(sep.MediaType == 0,                         "media type 应为 0(Audio)，实际 %u", sep.MediaType);
    CHECK(sep.CodecType == 0x0A,                      "codec type 应为 0x0A，实际 0x%X", sep.CodecType);
    CHECK(sep.VendorId == 0x0000012D,                 "VendorId 应为 0x12D，实际 0x%X", sep.VendorId);
    CHECK(sep.CodecId == 0x00AA,                      "CodecId 应为 0x00AA，实际 0x%04X", sep.CodecId);
    CHECK(sep.CapLen == sizeof(body),                 "CapLen 应为 %d，实际 %u", (int)sizeof(body), sep.CapLen);
    CHECK(memcmp(sep.Caps, body, sizeof(body)) == 0,  "Caps 应原样保留（SetConfiguration 要回放）");
    CHECK(sep.CodecSpecOff == 9,                      "CodecSpecOff 应为 9，实际 %u", sep.CodecSpecOff);
    CHECK(sep.CodecSpecOff < sep.CapLen && sep.Caps[sep.CodecSpecOff] == 0xFF,
          "偏移应指向 rates 位图 0xFF，实际 0x%02X",
          sep.CodecSpecOff < sep.CapLen ? sep.Caps[sep.CodecSpecOff] : 0);
}

static void Test_SonyVendor_LittleEndian(void)
{
    printf("\n[2] 兼容形式：VendorID 小端排布 2D 01 00 00\n");

    UCHAR body[] = {
        0x07, 0x09,
        0x0A,
        0x2D, 0x01, 0x00, 0x00,         // LE 排布
        0x00, 0xAA,
        0x03, 0x04
    };

    AVDTP_SEP sep;
    memset(&sep, 0, sizeof(sep));

    AvdtpParseSep(&sep, body, sizeof(body));

    CHECK(sep.HasCodec == TRUE,        "应识别出 codec");
    CHECK(sep.VendorId == 0x0000012D,  "小端排布也应还原成 0x12D，实际 0x%X", sep.VendorId);
    CHECK(sep.CodecId == 0x00AA,       "CodecId 应为 0x00AA，实际 0x%04X", sep.CodecId);
}

static void Test_LoscCompressed(void)
{
    printf("\n[3] LOSC 压缩形式：长度写在头字节高 2 位（无长度字节）\n");

    // (1<<6)|0x01 = 0x41 → LOSC=1 表示载荷 1 字节，类别 = Media Transport
    UCHAR body[] = { 0x41, 0x00 };

    AVDTP_SEP sep;
    memset(&sep, 0, sizeof(sep));
    sep.Seid = 5;

    AvdtpParseSep(&sep, body, sizeof(body));

    CHECK(sep.HasCodec == FALSE, "Media Transport 类别不是 codec，HasCodec 应为 FALSE，实际 %d", sep.HasCodec);
    CHECK(sep.CapLen == 2,       "CapLen 应为 2，实际 %u", sep.CapLen);
}

static void Test_MultiElement(void)
{
    printf("\n[4] 多元素：先 Reporting 再 Media Codec（偏移要跨过前一个元素）\n");

    UCHAR body[] = {
        0x02, 0x02, 0x00, 0x00,                    // cat=0x02 Reporting, len=2
        0x07, 0x09,                                // cat=0x07 Media Codec, len=9
        0x0A,
        0x00, 0x00, 0x01, 0x2D,
        0x00, 0xAA,
        0xFF, 0x02
    };

    AVDTP_SEP sep;
    memset(&sep, 0, sizeof(sep));

    AvdtpParseSep(&sep, body, sizeof(body));

    CHECK(sep.HasCodec == TRUE,       "应跳过 Reporting 找到 codec");
    CHECK(sep.VendorId == 0x0000012D, "VendorId 应为 0x12D，实际 0x%X", sep.VendorId);
    CHECK(sep.CodecSpecOff == 13,     "CodecSpecOff 应为 13（4+2+7），实际 %u", sep.CodecSpecOff);
    CHECK(sep.CodecSpecOff < sep.CapLen && sep.Caps[sep.CodecSpecOff] == 0xFF,
          "偏移应指向 0xFF，实际 0x%02X",
          sep.CodecSpecOff < sep.CapLen ? sep.Caps[sep.CodecSpecOff] : 0);
}

static void Test_Truncated(void)
{
    printf("\n[5] 畸形输入：声明的长度超过实际交付长度（不应越界读）\n");

    UCHAR body[] = { 0x07, 0x20, 0x0A, 0x00 };   // 声明 32 字节，实际只有 2 字节载荷

    AVDTP_SEP sep;
    memset(&sep, 0, sizeof(sep));

    AvdtpParseSep(&sep, body, sizeof(body));

    CHECK(sep.HasCodec == FALSE, "长度越界时应放弃解析，HasCodec 应为 FALSE");
    CHECK(sep.CapLen == sizeof(body), "CapLen 应为 %d，实际 %u", (int)sizeof(body), sep.CapLen);
}

static void Test_CapLenClamp(void)
{
    printf("\n[6] 边界：能力字节超过 AVDTP_MAX_CAPS(128) 时 CapLen 必须截断\n");

    UCHAR body[AVDTP_MAX_CAPS + 64];
    memset(body, 0x00, sizeof(body));
    body[0] = 0x02;   // Reporting
    body[1] = 0x00;

    AVDTP_SEP sep;
    memset(&sep, 0, sizeof(sep));

    AvdtpParseSep(&sep, body, sizeof(body));

    CHECK(sep.CapLen == AVDTP_MAX_CAPS, "CapLen 应截断到 %d，实际 %u", AVDTP_MAX_CAPS, sep.CapLen);
}

static void Test_NoCodecAtAll(void)
{
    printf("\n[7] 没有 Media Codec 元素\n");

    UCHAR body[] = { 0x02, 0x02, 0x00, 0x00 };

    AVDTP_SEP sep;
    memset(&sep, 0, sizeof(sep));

    AvdtpParseSep(&sep, body, sizeof(body));

    CHECK(sep.HasCodec == FALSE,       "HasCodec 应为 FALSE");
    CHECK(sep.VendorId == 0,           "VendorId 应为 0，实际 0x%X", sep.VendorId);
    CHECK(sep.CodecSpecOff == 0,       "CodecSpecOff 应为 0，实际 %u", sep.CodecSpecOff);
}

// ===========================================================================
//  媒体包（RTP 头 + LDAC 帧）
// ===========================================================================
//  A2DP 耳机对这几个字段极其敏感，错一个就表现为「连上了但没声音」：
//    * M 位：每个 talk spurt 首包必须为 1，否则部分 Sink 丢弃首帧
//    * 时间戳：必须按采样数累加，累加成字节数会让抖动缓冲立刻崩
//    * 序列号失败时不能推进：空洞会被当成丢包
// ===========================================================================
static void Test_MediaFirstPacket(void)
{
    AVDTP_MEDIA_STATE st;
    UCHAR payload[4] = { 0xAA, 0x35, 0x18, 0xA0 };
    UCHAR out[64];
    ULONG n;

    printf("\n[8] 首个媒体包：V=2 / PT=96 / M=1 / seq=0 / ts=0\n");

    AvdtpMediaReset(&st, 0x11223344);
    n = AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 256, out, sizeof(out));

    CHECK(n == 16, "总长应为 12+4=16，实际 %u", n);
    CHECK(out[0] == 0x80,                "byte0 应为 0x80(V=2)，实际 0x%02X", out[0]);
    CHECK((out[1] & 0x7F) == 96,         "PT 应为 96，实际 %u", out[1] & 0x7F);
    CHECK((out[1] & 0x80) != 0,          "首包 M 位必须为 1，否则耳机丢第一帧");
    CHECK(out[2] == 0x00 && out[3] == 0x00, "seq 应为 0，实际 %02X%02X", out[2], out[3]);
    CHECK(out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 0, "ts 应为 0");
    CHECK(out[8] == 0x11 && out[9] == 0x22 && out[10] == 0x33 && out[11] == 0x44,
          "SSRC 应大端 11223344，实际 %02X%02X%02X%02X",
          out[8], out[9], out[10], out[11]);
    CHECK(memcmp(out + 12, payload, sizeof(payload)) == 0, "载荷应原样拷贝");
}

static void Test_MediaSequenceAndTimestamp(void)
{
    AVDTP_MEDIA_STATE st;
    UCHAR payload[660];
    UCHAR out[680];
    ULONG n;

    printf("\n[9] 连续包：M 清掉、seq+1、timestamp += 采样数\n");

    memset(payload, 0xAA, sizeof(payload));
    AvdtpMediaReset(&st, 0xDEADBEEF);

    // 第一包：2 帧 = 256 采样
    n = AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 256, out, sizeof(out));
    CHECK(n == 672, "672 字节（12+660），实际 %u", n);
    CHECK(st.Seq == 1, "组装完 seq 应递增到 1，实际 %u", st.Seq);
    CHECK(st.Timestamp == 256, "ts 应为 256，实际 %u", st.Timestamp);

    // 第二包：M 必须清掉
    n = AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 256, out, sizeof(out));
    CHECK(n == 672, "第二包长度应为 672，实际 %u", n);
    CHECK((out[1] & 0x80) == 0, "非首包 M 必须为 0");
    CHECK(out[2] == 0x00 && out[3] == 0x01, "seq 应为 1（大端 0001），实际 %02X%02X", out[2], out[3]);
    CHECK(out[4] == 0x00 && out[5] == 0x00 && out[6] == 0x01 && out[7] == 0x00,
          "ts 应为 256（大端 00000100），实际 %02X%02X%02X%02X",
          out[4], out[5], out[6], out[7]);
    CHECK(st.Timestamp == 512, "第二包后 ts 应为 512，实际 %u", st.Timestamp);
}

static void Test_MediaSeqWrap(void)
{
    AVDTP_MEDIA_STATE st;
    UCHAR payload[2] = { 0xAA, 0x35 };
    UCHAR out[32];

    printf("\n[10] 序列号 16 位回绕\n");

    AvdtpMediaReset(&st, 1);
    st.Seq = 0xFFFF;
    AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 128, out, sizeof(out));

    CHECK(out[2] == 0xFF && out[3] == 0xFF, "本包应为 0xFFFF，实际 %02X%02X", out[2], out[3]);
    CHECK(st.Seq == 0x0000, "回绕后应为 0，实际 0x%04X", st.Seq);
}

static void Test_MediaTimestampWrap(void)
{
    AVDTP_MEDIA_STATE st;
    UCHAR payload[2] = { 0xAA, 0x35 };
    UCHAR out[32];

    printf("\n[11] 时间戳 32 位回绕（48kHz 下约 24.8 小时）\n");

    AvdtpMediaReset(&st, 1);
    st.Timestamp = 0xFFFFFF00UL;

    // 注意语义：RTP 包里放的是**该包第一个采样点的位置**，也就是自增之前的值。
    // 所以回绕这一刻，包内 ts 仍是 FFFFFF00，真正的 0 出现在下一个包。
    AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 256, out, sizeof(out));

    CHECK(out[4] == 0xFF && out[5] == 0xFF && out[6] == 0xFF && out[7] == 0x00,
          "回绕当包的 ts 应为 FFFFFF00，实际 %02X%02X%02X%02X",
          out[4], out[5], out[6], out[7]);
    CHECK(st.Timestamp == 0, "0xFFFFFF00+256 应回绕到 0，实际 0x%08X", st.Timestamp);

    // 下一包：时间戳从 0 重新开始
    AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 256, out, sizeof(out));
    CHECK(out[4] == 0 && out[5] == 0 && out[6] == 0 && out[7] == 0,
          "回绕后的下一包 ts 应为 0，实际 %02X%02X%02X%02X", out[4], out[5], out[6], out[7]);
    CHECK(st.Timestamp == 256, "再下一包 ts 应为 256，实际 %u", st.Timestamp);
}

static void Test_MediaCapacityTooSmall(void)
{
    AVDTP_MEDIA_STATE st;
    UCHAR payload[4] = { 1, 2, 3, 4 };
    UCHAR out[16];
    ULONG n;

    printf("\n[12] 缓冲区不足：返回 0 且不消耗序列号\n");

    AvdtpMediaReset(&st, 7);
    n = AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 256, out, 15);

    CHECK(n == 0,                      "容量 15 < 需要 16，应返回 0，实际 %u", n);
    CHECK(st.Seq == 0,                 "失败不能推进 seq，实际 %u", st.Seq);
    CHECK(st.Timestamp == 0,           "失败不能推进 ts，实际 %u", st.Timestamp);
    CHECK(st.MarkerPending == TRUE,    "失败后 marker 应仍然挂起");
}

static void Test_MediaBadArgs(void)
{
    AVDTP_MEDIA_STATE st;
    UCHAR payload[4] = { 1, 2, 3, 4 };
    UCHAR out[32];

    printf("\n[13] 非法参数一律返回 0\n");

    AvdtpMediaReset(&st, 7);

    CHECK(AvdtpMediaBuildPacket(&st, NULL,  4, 256, out, sizeof(out)) == 0, "载荷 NULL 应返回 0");
    CHECK(AvdtpMediaBuildPacket(&st, payload, 0, 256, out, sizeof(out)) == 0, "载荷长度 0 应返回 0");
    CHECK(AvdtpMediaBuildPacket(&st, payload, 4, 256, NULL, 32) == 0,        "输出 NULL 应返回 0");
    CHECK(st.Seq == 0, "全部失败后 seq 仍应为 0，实际 %u", st.Seq);
}

static void Test_MediaTalkSpurt(void)
{
    AVDTP_MEDIA_STATE st;
    UCHAR payload[2] = { 0xAA, 0x35 };
    UCHAR out[32];

    printf("\n[14] talk spurt：SUSPEND 恢复后重新置 M\n");

    AvdtpMediaReset(&st, 1);

    AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 128, out, sizeof(out));
    CHECK((out[1] & 0x80) != 0, "首包 M=1");

    AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 128, out, sizeof(out));
    CHECK((out[1] & 0x80) == 0, "第二包 M=0");

    AvdtpMediaBeginTalkSpurt(&st);
    AvdtpMediaBuildPacket(&st, payload, sizeof(payload), 128, out, sizeof(out));
    CHECK((out[1] & 0x80) != 0, "重新开始后 M 必须回到 1");
    CHECK(st.Seq == 3,          "seq 仍应连续累加到 3，实际 %u", st.Seq);
}

static void Test_MediaMtuBudget(void)
{
    printf("\n[15] MTU 预算：2-DH5 的 679 字节装得下 2 帧\n");

    ULONG maxPayload = AVDTP_MEDIA_MAX_PAYLOAD(679);

    CHECK(maxPayload == 667, "679 的载荷上限应为 667，实际 %u", maxPayload);
    // 实测编码器每包 2 帧 = 660 字节
    CHECK(660 + AVDTP_RTP_HDR_LEN <= 679,
          "660+12=%u 应 <= 679",
          660 + AVDTP_RTP_HDR_LEN);
    CHECK(AVDTP_MEDIA_MAX_PAYLOAD(12) == 0,  "MTU 等于头长时载荷上限应为 0");
    CHECK(AVDTP_MEDIA_MAX_PAYLOAD(8) == 0,   "MTU 小于头长时不能下溢成巨大值");
}

int main(void)
{
    printf("============================================\n");
    printf(" AVDTP 能力解析 + 媒体封包 单元测试\n");
    printf("============================================\n");

    Test_SonyVendor_BigEndian();
    Test_SonyVendor_LittleEndian();
    Test_LoscCompressed();
    Test_MultiElement();
    Test_Truncated();
    Test_CapLenClamp();
    Test_NoCodecAtAll();
    Test_MediaFirstPacket();
    Test_MediaSequenceAndTimestamp();
    Test_MediaSeqWrap();
    Test_MediaTimestampWrap();
    Test_MediaCapacityTooSmall();
    Test_MediaBadArgs();
    Test_MediaTalkSpurt();
    Test_MediaMtuBudget();

    printf("\n============================================\n");
    printf(" 结果: %d 通过, %d 失败\n", g_Pass, g_Fail);
    printf("============================================\n");
    return g_Fail == 0 ? 0 : 1;
}
