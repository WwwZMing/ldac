// ============================================================================
//  ldac_send.c — 用户态 LDAC 发送端 v2
// ============================================================================
//  用户态优先架构的另一半：采集/编码/封帧/时钟全在这里，崩了只是进程退出。
//
//  两种模式：
//    ldac-send.exe <file.wav> [...]        WAV 文件模式（验证链路用）
//    ldac-send.exe --loopback [...]        WASAPI loopback：把系统正在播的
//                                          声音实时推给耳机（产品形态）
//  公共参数：[--addr AA:BB:CC:DD:EE:FF] [--eqmid hq|sq|mq]
//
//  流程：连接 \\.\LdacCtl →（loopback：先拿系统混音率作协商偏好）START
//        → libldac 编码（每次喂 128 采样/声道，输出恒为完整帧边界）
//        → RTP 聚包（一包装 N 个完整帧，≤ 内核 MediaMtu）→ SEND。
// ============================================================================

#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "../common/ldac_io.h"
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")
#include "ldacBT.h"
#include "sbc_encoder.h"        // AOSP bluedroid SBC 编码器（--codec sbc 对照实验用）
#include "aac_latm.h"           // AAC-LC（MF 编码器）+ LATM 封帧（--codec aac）
#include "wasapi_loop.h"

// ------------------------------------------------------------- 全局/工具 ----
static volatile LONG g_stop = 0;

static BOOL WINAPI CtrlHandler(DWORD type)
{
    (void)type;
    InterlockedExchange(&g_stop, 1);
    return TRUE;
}

static void Die(const char* msg, unsigned long code)
{
    fprintf(stderr, "FATAL: %s (0x%08lX)\n", msg, code);
    exit(1);
}

//  --log：同时写一份到文件。进程被面板/计划任务拉起时 stderr 是管道，
//  stdio 缓冲会让日志"看起来不动"，落盘才可靠（分析脚本也直接读它）。
static FILE* g_Log = NULL;

static void LogNote(const char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fflush(stderr);

    if (g_Log != NULL) {
        va_start(ap, fmt);
        vfprintf(g_Log, fmt, ap);
        va_end(ap);
        fflush(g_Log);
    }
}

static int HexByte(const char* s)
{
    int v;
    if (*s >= '0' && *s <= '9') v = *s - '0';
    else if (*s >= 'a' && *s <= 'f') v = *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F') v = *s - 'A' + 10;
    else return -1;
    v <<= 4;
    s++;
    if (*s >= '0' && *s <= '9') v |= *s - '0';
    else if (*s >= 'a' && *s <= 'f') v |= *s - 'a' + 10;
    else if (*s >= 'A' && *s <= 'F') v |= *s - 'A' + 10;
    else return -1;
    return v;
}

// ------------------------------------------------------------------ WAV ----
typedef struct {
    unsigned formatTag;
    unsigned rate;
    unsigned channels;
    unsigned bits;
    unsigned char* pcm;
    long long bytes;
} WavFile;

static void LoadWav(const char* path, WavFile* w)
{
    FILE* f = fopen(path, "rb");
    unsigned char hdr[12];
    char id[4];
    unsigned size;
    int haveFmt = 0;

    if (!f) Die("cannot open wav file", 0);
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4))
        Die("not a RIFF/WAVE file", 0);

    memset(w, 0, sizeof(*w));
    while (fread(id, 1, 4, f) == 4 && fread(&size, 4, 1, f) == 1) {
        if (!memcmp(id, "fmt ", 4)) {
            unsigned char fmt[40];
            unsigned take = size < 40 ? size : 40;
            unsigned char pad = (unsigned char)(size & 1);
            if (fread(fmt, 1, take, f) != take) Die("truncated fmt", 0);
            if (pad) fgetc(f);
            w->formatTag  = fmt[0] | (fmt[1] << 8);
            w->channels   = fmt[2] | (fmt[3] << 8);
            w->rate       = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((unsigned)fmt[7] << 24);
            w->bits       = fmt[14] | (fmt[15] << 8);
            haveFmt = 1;
        } else if (!memcmp(id, "data", 4)) {
            long long avail;
            long pos = ftell(f);
            fseek(f, 0, SEEK_END);
            avail = ftell(f) - (long long)pos;
            w->bytes = ((long long)size < avail) ? (long long)size : avail;
            fseek(f, pos, SEEK_SET);
            w->pcm = (unsigned char*)calloc((size_t)w->bytes + 8, 1);
            if (!w->pcm) Die("out of memory for pcm", 0);
            if (fread(w->pcm, 1, (size_t)w->bytes, f) != (size_t)w->bytes)
                Die("truncated data", 0);
        } else {
            unsigned char pad = (unsigned char)(size & 1);
            fseek(f, (long)size, SEEK_CUR);
            if (pad) fgetc(f);
        }
        if (w->pcm && haveFmt) break;
    }
    fclose(f);

    if (!haveFmt || !w->pcm || w->bytes == 0) Die("wav missing fmt/data", 0);
    if (w->formatTag != 1) Die("wav is not PCM", 0);
    if (w->channels != 2) Die("wav must be stereo (2ch)", 0);
    if (w->bits != 16) Die("wav must be 16-bit", 0);
}

// ------------------------------------------- 用户态 AVDTP 模式（重构）----
//  面板自己跑完 AVDTP 建链后，把"媒体槽位号"传进来；发送端直接用内核的
//  L2CAP 原语发包，不再经过 IOCTL_LDAC_START / 内核的 g_MediaChan。
//  默认 0 = 走原来的内核路径，行为完全不变。
static unsigned long g_MediaSlot = 0;
static unsigned long g_MediaMtuOverride = 0;

static BOOL SendPacket(HANDLE dev, const unsigned char* data, int len)
{
    DWORD br;

    if (g_MediaSlot == 0)
        return DeviceIoControl(dev, IOCTL_LDAC_SEND, (LPVOID)data, (DWORD)len,
                               NULL, 0, &br, NULL);

    if (len <= 0 || len > LDAC_SEND_MAX) return FALSE;
    {
        unsigned char buf[LDAC_SEND_MAX + 8];
        unsigned long l = (unsigned long)len;
        memcpy(buf, &g_MediaSlot, 4);
        memcpy(buf + 4, &l, 4);
        memcpy(buf + 8, data, (size_t)len);
        return DeviceIoControl(dev, IOCTL_LDAC_L2CAP_SEND, buf, (DWORD)(8 + len),
                               NULL, 0, &br, NULL);
    }
}

// ------------------------------------------------------- RTP 聚包发送器 ----
typedef struct {
    HANDLE Dev;
    unsigned long MediaMtu;
    unsigned char Pkt[LDAC_SEND_MAX];
    int Len;                    // = 12 + 已装载荷
    int Frames;
    int LastPeak;               // 最近一包 PCM 的峰值（诊断音量用）
    unsigned short Seq;
    unsigned int Ts;
    unsigned int Ssrc;
    int Marker;
    long long PacketsSent;
    long long BytesSent;
    //  —— 发送节拍器 ——
    //  读 4096B 会把 4~5 个包一次发出去（实测包间隔 3.5~27ms 抖动），sink 的
    //  抖动缓冲被吃穿就是"一卡一卡"。这里按样本时间轴均匀发包：第 k 个包
    //  应在 t0 + samplesSent/freq 时刻发出，早到就等，迟到就补（不回退）。
    long long   T0;             // QueryPerformanceCounter 起点
    long long   Qpf;
    long long   SamplesSent;    // 已发包累计样本数
    int         Freq;           // 协商采样率（节拍换算用）
    int         NoMediaHdr;     // 1 = 不带那 1 字节"帧数"头（AAC 就是这种）
    int         Pacing;         // 1 = 开启节拍
    long        Fails;          // 连续 SEND 失败次数（链路抖动）
} RtpSender;

//  A2DP 媒体负载头：RTP 头之后先跟 1 个字节，低 4 位 = 本包的 codec 帧数
//  （SBC / LDAC / Opus 都是这个规则，MPEG-2/4 AAC 用的是 AU 头，另一套）。
//  少了这个字节，sink 会把第一帧的第一个字节当成帧数，解码全废 —— 实测
//  两边都是"数据在流、灯也变了、就是没声"。
#define RTP_HDR 12
#define MEDIA_HDR 1

//  一个 codec 帧等于多少采样/声道：LDAC/SBC 128，AAC-LC 1024。
//  RTP 时间戳、发送节拍、队列积累量都必须乘以它，写死 128 会让 AAC 的时间轴
//  只有 1/8 速（和当初 LDAC 多帧包踩的坑一模一样）。
static int g_SamplesPerFrame = 128;

//  ---- 诊断模式：直接把"别人生成的 LATM"当媒体负载发出去 ------------------
//  用途：把"我的封帧"和"封帧以外的一切"分开。发的是 ffmpeg latmenc 生成的
//  AudioMuxElement；如果这样也无声，问题就不在封帧上。
static unsigned char* g_LatmBuf = NULL;
static int            g_LatmLen = 0;
static int            g_LatmPos = 0;

static int LoadLatmFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    long n;
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    g_LatmBuf = (unsigned char*)malloc((size_t)n);
    if (!g_LatmBuf) { fclose(f); return -1; }
    g_LatmLen = (int)fread(g_LatmBuf, 1, (size_t)n, f);
    fclose(f);
    g_LatmPos = 0;
    return g_LatmLen;
}

//  取下一个 AudioMuxElement（只支持 LOAS 封装：11 位同步 0x2B7 + 13 位长度）。
//  Style（诊断组合）：
//    0 = 只发元素（A2DP 规范写法）
//    1 = 保留 LOAS 三字节头（0x2B7 同步 + 13 位长度）
//   +2 = 前面再加 1 字节"帧数"头（SBC/LDAC 的传统写法）
static int g_LatmStyle = 0;

static int LatmNext(unsigned char* out, int cap)
{
    int len, keep, total = 0;
    if (g_LatmPos + 3 > g_LatmLen) return 0;
    if ((g_LatmBuf[g_LatmPos] != 0x56) || ((g_LatmBuf[g_LatmPos + 1] & 0xE0) != 0xE0)) return -1;
    len  = ((g_LatmBuf[g_LatmPos + 1] & 0x1F) << 8) | g_LatmBuf[g_LatmPos + 2];
    keep = (g_LatmStyle & 1) ? 3 : 0;
    if (g_LatmPos + 3 + len > g_LatmLen) return 0;
    if (len + keep + 1 > cap) return -1;
    if (g_LatmStyle & 2) out[total++] = 0x01;              // 1 帧
    memcpy(out + total, g_LatmBuf + g_LatmPos + (3 - keep), (size_t)(keep + len));
    total += keep + len;
    g_LatmPos += 3 + len;
    return total;
}

// ---------------------------------------------------------- 采集环 ------
//  参考实现（A2DP-Windows-Bridge pipeline）的结构：
//     capture thread → SPSC 字节环 → encode 线程 → 包环 → 发送
//  环默认 200ms（这里 256KB ≈ 1.3s @48k/16bit/2ch），把 WASAPI 抓取的抖动
//  全部吸收，编码线程永远不会"没数据可编"。
#define CAP_RING_BYTES (256 * 1024)
static unsigned char  g_CapRing[CAP_RING_BYTES];
static volatile LONG  g_CapW = 0, g_CapR = 0;      // 绝对字节计数
static volatile LONG  g_CapStop = 0;
static WasapiCap*     g_CapObj = NULL;

static volatile LONG  g_CapSilence = 0;   // 采集断流期间补进去的静音字节（诊断）
static int            g_CapFreq = 48000;  // 采集流采样率（补静音换算用）
static int            g_CapAlign = 4;     // 采集流 block align

//  目标水位：字节环里始终留 ~200ms 音频，吸收抓取抖动
#define CAP_TARGET_LAG_MS 200

static DWORD WINAPI CapThread(LPVOID p)
{
    static unsigned char buf[8192];
    LARGE_INTEGER qpf, t0, now;
    long long written = 0, lag;
    (void)p;

    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    lag = (long long)CAP_TARGET_LAG_MS * g_CapFreq * g_CapAlign / 1000;

    while (!g_CapStop && !g_stop)
    {
        int got = WasapiRead(g_CapObj, buf, sizeof(buf));
        LONG w, r;
        long long target;

        //  水位控制：按墙钟算出"现在累计应该有多少字节"，实际写进去的少了就补。
        //  只认总量，不认单次读取结果 —— 否则同一段时间会被真实数据和静音算两遍。
        QueryPerformanceCounter(&now);
        target = (now.QuadPart - t0.QuadPart) * (long long)g_CapFreq * g_CapAlign
               / qpf.QuadPart + lag;

        if (got <= 0)
        {
            //  采集端没数据：播放器暂停 / 引擎空闲 / 端点重建。
            //  接收端缓冲只有几百毫秒，干等就是"滋滋 -> 静音"；更糟的是发送端
            //  取不到包会整条链路停顿，恢复时再暴冲追帧。补静音就完全没有这些。
            long long shortfall = target - written;
            if (shortfall < g_CapAlign) { Sleep(1); continue; }
            got = (int)(shortfall > (long long)sizeof(buf) ? (long long)sizeof(buf) : shortfall);
            got -= got % g_CapAlign;
            if (got <= 0) { Sleep(1); continue; }
            memset(buf, 0, (size_t)got);
            InterlockedExchangeAdd(&g_CapSilence, got);
        }
        written += got;
        w = g_CapW;
        r = g_CapR;
        if ((LONG)(w - r) + got > CAP_RING_BYTES) {
            g_CapR = w - (CAP_RING_BYTES - got);   // 溢出：丢最旧
            r = g_CapR;
        }
        {
            int off = (int)(w % CAP_RING_BYTES);
            int first = CAP_RING_BYTES - off;
            if (first > got) first = got;
            memcpy(g_CapRing + off, buf, (size_t)first);
            if (got > first) memcpy(g_CapRing, buf + first, (size_t)(got - first));
        }
        g_CapW = w + got;
    }
    return 0;
}

// ---------------------------------------------------------- 发包队列 ----
//  采集线程（生产者）把编码好的整包塞进队列，发包线程（消费者）按样本
//  时间轴匀速取出并下发。队列里预存 120ms 音频，吸收 WASAPI 抓取抖动。
#define QCAP 256
typedef struct {
    int   Len;
    int   Frames;
    unsigned char Data[800];
} QPkt;

static QPkt         g_Q[QCAP];
static volatile LONG g_QH = 0, g_QT = 0;      // [H, T) 区间
static volatile LONG g_ProdDone = 0;
static volatile LONG g_QStop = 0;
static volatile LONG g_SendErr = 0;
//  真正交到驱动手里的包数（RtpFlush 里的 PacketsSent 只是"入队"数，两者差很大）
static volatile LONG g_SentOk = 0;

static int QueuedCount(void) { return (int)((g_QT - g_QH + QCAP) % QCAP); }

static int QueuedSamples(void)
{
    int n = QueuedCount(), i, s = 0;
    for (i = 0; i < n; i++) s += g_Q[(g_QH + i) % QCAP].Frames * g_SamplesPerFrame;
    return s;
}

static void QueuePut(const RtpSender* r)
{
    while (((g_QT + 1) % QCAP) == g_QH && !g_QStop) Sleep(1);   // 队列满则等
    if (g_QStop) return;
    g_Q[g_QT].Len = r->Len;
    g_Q[g_QT].Frames = r->Frames;
    memcpy(g_Q[g_QT].Data, r->Pkt, (size_t)r->Len);
    g_QT = (g_QT + 1) % QCAP;
}

//  —— 发送线程 ——
//  每次 DeviceIoControl(IOCTL_LDAC_SEND) 都要等 BRB 完成才返回（内核同步写），
//  控制器队列一满就阻塞十几毫秒。放在编码循环里 = 直接变成包间隔抖动。
//  这里改成：编码侧只管入队，发送侧按样本时间轴匀速取包、阻塞也只堵在自己身上。
static DWORD WINAPI SenderThread(LPVOID p)
{
    RtpSender* r = (RtpSender*)p;
    long long  samples = 0;

    while (!g_QStop)
    {
        LARGE_INTEGER now;
        long long due;
        QPkt* pk;

        if (g_QH == g_QT) { Sleep(1); continue; }

        due = r->T0 + (samples * r->Qpf) / (r->Freq > 0 ? r->Freq : 48000);
        QueryPerformanceCounter(&now);
        if (now.QuadPart < due) {
            long long waitUs = ((due - now.QuadPart) * 1000000) / r->Qpf;
            if (waitUs > 1500) Sleep(1);
            else Sleep(0);
            continue;
        }

        //  积压超过 ~190ms：链路喂不动，丢最旧保低延迟
        if (QueuedCount() > 40) g_QH = (g_QH + 1) % QCAP;

        pk = &g_Q[g_QH];
        {
            DWORD br;
            if (!SendPacket(r->Dev, pk->Data, (int)pk->Len)) {
                //  链路短暂不在 STREAMING（对端休眠 / keeper 正在重建）时 SEND 会返回
                //  INVALID_DEVICE_STATE。以前这里直接 ExitProcess，结果是"链路一抖，
                //  发送端就没了，声音再也不回来"。现在只丢包、等链路回来，
                //  恢复时把节拍基准重置，避免暴冲追帧。
                DWORD err = GetLastError();
                r->Fails++;
                if ((r->Fails % 100) == 1) {
                    LogNote("SEND failed err=%lu (consecutive=%ld), waiting for link\n",
                            err, (long)r->Fails);
                }
                if (r->Fails > 2000) {
                    LogNote("SEND failed %ld times in a row, giving up\n", (long)r->Fails);
                    ExitProcess(3);
                }
                InterlockedExchange(&g_SendErr, (LONG)err);
                Sleep(2);
                continue;
            }
            InterlockedIncrement(&g_SentOk);
            if (r->Fails > 0) {
                LARGE_INTEGER now2;
                QueryPerformanceCounter(&now2);
                r->T0 = now2.QuadPart - (samples * r->Qpf) / (r->Freq > 0 ? r->Freq : 48000);
                LogNote("link back after %ld failed sends, pacing resynced\n", (long)r->Fails);
                r->Fails = 0;
            }
        }
        samples += (long long)pk->Frames * g_SamplesPerFrame;
        g_QH = (g_QH + 1) % QCAP;
    }
    return 0;
}

static void RtpFlush(RtpSender* r)
{
    if (r->Frames == 0) return;

    //  ⚠ 只有 SBC/LDAC 这类 codec 在 RTP 之后跟一个"本包帧数"字节。
    //  MPEG-2/4 AAC 没有这个字节：RTP 之后直接就是 LATM AudioMuxElement。
    //  证据：iPhone 的 A2DP AAC 抓包（SO 35915317）RTP 头之后紧接
    //  47 fc 00 00 b0 90 80 03，逐字段解出 useSameStreamMux=0 /
    //  audioMuxVersion=1 / ascLen=22 —— 前面没有帧数字节。
    //  多写这一个字节，对端会把 0x01 当成 LATM 的第一个字节，从第 4 位起全错位，
    //  表现就是"协商全 Accept、灯变色、一个音符都不出"。
    if (!r->NoMediaHdr) {
        r->Pkt[RTP_HDR] = (unsigned char)(r->Frames & 0x0F);   /* 媒体负载头 */
    }

    r->Pkt[0] = 0x80;                                        /* V=2 P=0 X=0 CC=0 */
    r->Pkt[1] = (unsigned char)(96 | (r->Marker ? 0x80 : 0));
    r->Pkt[2] = (unsigned char)(r->Seq >> 8);
    r->Pkt[3] = (unsigned char)(r->Seq & 0xFF);
    r->Pkt[4] = (unsigned char)(r->Ts >> 24);
    r->Pkt[5] = (unsigned char)((r->Ts >> 16) & 0xFF);
    r->Pkt[6] = (unsigned char)((r->Ts >> 8) & 0xFF);
    r->Pkt[7] = (unsigned char)(r->Ts & 0xFF);
    r->Pkt[8]  = (unsigned char)(r->Ssrc >> 24);
    r->Pkt[9]  = (unsigned char)((r->Ssrc >> 16) & 0xFF);
    r->Pkt[10] = (unsigned char)((r->Ssrc >> 8) & 0xFF);
    r->Pkt[11] = (unsigned char)(r->Ssrc & 0xFF);
    //  ⚠ 这里**绝对不能**再写 r->Pkt[RTP_HDR]：上面那个 if 已经处理过了。
    //  原来这里留着一行无条件写入，结果每加完保护又把它盖回去 ——
    //  表现就是"元素第一个字节被替换成帧数 0x01"，抓包逐字节对比才看得见
    //  （RTP 负载 01 FC 00 00 ... vs 我编的 47 FC 00 00 ...）。

    if ((r->PacketsSent % 100) == 0) {
        LogNote("[pkt %lld] peak=%d frames=%d len=%d fill=%ld\n",
                (long long)r->PacketsSent, r->LastPeak, r->Frames, r->Len,
                (long)g_CapSilence);
    }
    QueuePut(r);            // 交给发送线程，绝不在编码循环里阻塞

    r->Seq++;
    r->Ts += (unsigned)(g_SamplesPerFrame * r->Frames);
    r->Marker = 0;
    r->PacketsSent++;
    r->BytesSent += r->Len;
    r->Len = RTP_HDR + (r->NoMediaHdr ? 0 : MEDIA_HDR);
    r->Frames = 0;
}

//  frames = 这段数据里包含的 **LDAC 帧数**（不是调用次数）。
//  libldac 一次 ldacBT_encode 可能吐出整包（多帧），RTP 时间戳必须按真实
//  帧数推进，否则每包只前进 128 样本而实际装了 768 样本 —— 设备的时间轴
//  只有 1/6 速，抖动缓冲直接失效，表现为"数据在流但完全没声"。
static void RtpAppend(RtpSender* r, const unsigned char* frame, int frameSz, int frames)
{
    if (r->Len + frameSz > (int)r->MediaMtu) RtpFlush(r);
    memcpy(r->Pkt + r->Len, frame, (size_t)frameSz);
    r->Len += frameSz;
    r->Frames += frames;
}

// ------------------------------------------------------------- 主流程 ------
int main(int argc, char** argv)
{
    const char* wavPath = NULL;
    int loopback = 0;
    int unloadOnly = 0;
    unsigned char addr[6];
    int haveAddr = 0;
    int eqmid = LDACBT_EQMID_HQ;
    const char* matchName = NULL;   // --match: 按端点名字子串选采集源
    const char* logPath = NULL;     // --log: 输出写文件并脱离控制台（计划任务用）
    int probeRate = 0;              // --probe-rate: 只报混音率后退出
    double gain = 1.0;              // --gain：编码前数字增益（限幅）
    int useSbc = 0;                 // --codec sbc：走 SBC 通路（对照实验）
    int useAac = 0;                 // --codec aac：走 AAC-LC + LATM 通路
    int forceRate = 0;              // --rate <hz>：强制协商采样率（诊断用）
    const char* latmPath = NULL;    // --latm <file>：直接发现成的 LATM 元素（诊断用）
    int latmMode = 0;
    AacLatm* aac = NULL;
    static SBC_ENC_PARAMS sbc;
    static unsigned char  sbcFrame[1024];
    int i;

    HANDLE_LDAC_BT hLdac = NULL;
    HANDLE hDev = INVALID_HANDLE_VALUE;
    WasapiCap* cap = NULL;
    LDAC_STATUS st;
    DWORD bytesRet;

    int freq = 0, cm = 0;
    unsigned mixRate = 0;
    unsigned char enc[8192];
    unsigned char s16[8192];
    unsigned char acc[1 << 16];         /* PCM 累积缓冲（S16 立体声） */
    int accLen = 0;
    unsigned capBits = 16;              /* loopback 混音格式位深 */
    RtpSender snd;
    long long framesOut = 0;
    int rc;
    LARGE_INTEGER qpf, t0, now;
    WavFile wav;
    int wavMode;

    memset(&wav, 0, sizeof(wav));
    memset(&snd, 0, sizeof(snd));

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--addr") && i + 1 < argc) {
            const char* s = argv[++i];
            int k, v;
            for (k = 0; k < 6; k++) {
                v = HexByte(s);
                if (v < 0) Die("bad --addr (expect AA:BB:CC:DD:EE:FF)", 0);
                addr[k] = (unsigned char)v;
                s += 2;
                if (k < 5 && (*s == ':' || *s == '-')) s++;
            }
            haveAddr = 1;
        } else if (!strcmp(argv[i], "--eqmid") && i + 1 < argc) {
            const char* s = argv[++i];
            if (!_stricmp(s, "hq")) eqmid = LDACBT_EQMID_HQ;
            else if (!_stricmp(s, "sq")) eqmid = LDACBT_EQMID_SQ;
            else if (!_stricmp(s, "mq")) eqmid = LDACBT_EQMID_MQ;
            else Die("--eqmid must be hq|sq|mq", 0);
        } else if (!strcmp(argv[i], "--gain") && i + 1 < argc) {
            gain = atof(argv[++i]);
            if (gain <= 0.0) gain = 1.0;
            if (gain > 32.0) gain = 32.0;
        } else if (!strcmp(argv[i], "--codec") && i + 1 < argc) {
            const char* c = argv[++i];
            if (!_stricmp(c, "sbc")) useSbc = 1;
            else if (!_stricmp(c, "ldac")) useSbc = 0;
            else if (!_stricmp(c, "aac")) useAac = 1;
            else Die("--codec must be ldac|aac|sbc", 0);
        } else if (!strcmp(argv[i], "--rate") && i + 1 < argc) {
            forceRate = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--latm") && i + 1 < argc) {
            latmPath = argv[++i];
        } else if (!strcmp(argv[i], "--latm-style") && i + 1 < argc) {
            g_LatmStyle = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--log") && i + 1 < argc) {
            logPath = argv[++i];
        } else if (!strcmp(argv[i], "--probe-rate")) {
            //  只报采集端点的混音率就退出：面板要用它决定 AVDTP 协商哪个采样率。
            //  这个模式**不碰驱动**（驱动可能还没装/没加载）。
            probeRate = 1;
        } else if (!strcmp(argv[i], "--media-slot") && i + 1 < argc) {
            //  用户态 AVDTP：面板已建链，按槽位发包；不再走内核 START。
            g_MediaSlot = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--media-mtu") && i + 1 < argc) {
            g_MediaMtuOverride = strtoul(argv[++i], NULL, 0);
        } else if (!strcmp(argv[i], "--match") && i + 1 < argc) {
            matchName = argv[++i];
        } else if (!strcmp(argv[i], "--loopback")) {
            loopback = 1;
        } else if (!strcmp(argv[i], "--unload")) {
            unloadOnly = 1;
        } else if (!wavPath) {
            wavPath = argv[i];
        } else {
            Die("too many arguments", 0);
        }
    }
    wavMode = (wavPath != NULL);
    latmMode = (latmPath != NULL);
    if (!unloadOnly && !probeRate && !wavMode && !loopback && !latmMode) {
        fprintf(stderr,
            "usage: ldac-send.exe <file.wav> [opts]   WAV mode\n"
            "       ldac-send.exe --loopback [opts]   stream system audio\n"
            "       ldac-send.exe --probe-rate        print the loopback mix rate only\n"
            "       ldac-send.exe --unload            tear down + delete the control\n"
            "                                         device so the driver image can\n"
            "                                         actually unload (dev only)\n"
            "  opts: [--addr AA:BB:CC:DD:EE:FF] [--eqmid hq|sq|mq] [--gain N]\n"
            "        [--codec ldac|sbc] [--match <endpoint name substring>]\n"
            "  --match: pick the loopback source by endpoint name instead of the\n"
            "           default render device (SSH runs in session 0, whose default\n"
            "           endpoint is not the one the user hears)\n");
        return 1;
    }
    if (!unloadOnly && wavMode && loopback) Die("--loopback and wav file are mutually exclusive", 0);

    //  单实例：服务、面板、手工调试都可能同时拉起一个。两个流同时往一个 sink
    //  发包 = 序号/时间戳互相踩，实测就是"滋滋"。用一个全局互斥挡住，
    //  谁先起谁干活，后来的直接退出（服务会看到退出码，日志里有记录）。
    {
        HANDLE hOne = CreateMutexA(NULL, FALSE, "Global\\LdacSendSingle");
        if (hOne != NULL && GetLastError() == ERROR_ALREADY_EXISTS) {
            fprintf(stderr, "another ldac-send is already running; exiting\n");
            return 3;
        }
        // 句柄故意不关：进程退出时由系统回收，互斥随之释放
    }

    //  --log：把 stdout/stderr 落到文件，然后**释放控制台**。
    //  计划任务用 cmd /c 跑会带一个黑窗口，那个窗口一被关掉进程就死
    //  （实测现象："黑窗口不显示就没声音"）。FreeConsole 之后没有窗口可关，
    //  进程只受任务/进程管理影响。
    //  关于"黑窗口"：那是计划任务用 cmd /c 在交互会话里跑出来的控制台，
    //  关掉它等于杀掉进程（实测现象："黑窗口不显示就没声音"）。
    //  正解不是让 exe 自己重绑 stdout（试过 _dup2/freopen：stdout 能写、
    //  stderr 全丢，而 [pkt] 日志正好走 stderr），而是让任务跑在 **SYSTEM/session 0**
    //  —— 那里根本不创建可见控制台，配合 cmd 重定向照样有日志。
    if (logPath != NULL) {
        g_Log = fopen(logPath, "w");
        if (g_Log == NULL) {
            fprintf(stderr, "cannot open log '%s'\n", logPath);
        }
        //  不做 setvbuf：MSVC 下传 NULL 缓冲 + size 0 会触发非法参数处理器，
        //  进程直接 0xC0000409 终止。LogNote 每行 fflush 已经够可靠。
    }

    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    //  --probe-rate：只探测混音率，不打开驱动。输出一行 "RATE <hz>"。
    if (probeRate) {
        unsigned pch, pbits;
        if (WasapiStartMatch(matchName, &cap, &mixRate, &pch, &pbits) != 0)
            Die("WasapiStartMatch", 0);
        printf("RATE %u\n", mixRate);
        WasapiStop(cap);
        return 0;
    }

    // ---- 打开内核通道 -----------------------------------------------------
    hDev = CreateFileA("\\\\.\\LdacCtl", GENERIC_READ | GENERIC_WRITE, 0,
                       NULL, OPEN_EXISTING, 0, NULL);
    if (hDev == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "cannot open \\\\.\\LdacCtl — driver loaded? (GetLastError=%lu)\n",
                GetLastError());
        return 1;
    }

    if (unloadOnly) {
        if (!DeviceIoControl(hDev, IOCTL_LDAC_UNLOAD, NULL, 0, NULL, 0, &bytesRet, NULL)) {
            fprintf(stderr, "UNLOAD failed (GetLastError=%lu)\n", GetLastError());
            CloseHandle(hDev);
            return 1;
        }
        CloseHandle(hDev);
        printf("unload armed: control device is being deleted; image unloads once the\n"
               "              A2DP node is removed too\n");
        return 0;
    }

    if (!DeviceIoControl(hDev, IOCTL_LDAC_STATUS, NULL, 0,
                         &st, sizeof(st), &bytesRet, NULL))
        Die("IOCTL_LDAC_STATUS", GetLastError());
    printf("driver: state=%ld lastErr=0x%08lX mediaMtu=%lu\n",
           st.State, (unsigned long)st.LastError, st.MediaMtu);

    // ---- 采集源（loopback 先行：混音率要当协商偏好传下去）------------------
    if (loopback) {
        unsigned ch, bits;
        if (WasapiStartMatch(matchName, &cap, &mixRate, &ch, &bits) != 0)
            Die("WasapiStartMatch", 0);
        //  采集线程：持续把系统声音灌进字节环，编码/发送按自己的节拍取
        g_CapObj = cap;
        g_CapStop = 0;
        g_CapFreq = (int)mixRate;
        g_CapAlign = (int)(2 * (bits / 8));
        capBits = bits;
        CreateThread(NULL, 0, CapThread, NULL, 0, NULL);
        printf("wasapi loopback: mix = %u Hz / %u ch / %u-bit %s\n",
               mixRate, ch, bits, bits == 32 ? "float" : "pcm");
        switch (mixRate) {
        case 96000: i = LDAC_RATE_96000; break;
        case 88200: i = LDAC_RATE_88200; break;
        case 48000: i = LDAC_RATE_48000; break;
        case 44100: i = LDAC_RATE_44100; break;
        default:    i = 0; break;        /* 混音率不在 LDAC 集内 → 不带偏好 */
        }
        if (i == 0) {
            fprintf(stderr,
                "wasapi: mix rate %u Hz is outside LDAC's set (44.1/48/88.2/96k),\n"
                "        negotiating highest supported — audio pitch will be wrong.\n"
                "        (set a matching default format for the sink in mmsys.cpl)\n",
                mixRate);
        }
    }

    // ---- 建流 -------------------------------------------------------------
    if (g_MediaSlot != 0) {
        //  用户态 AVDTP 模式：链路是面板建的，内核状态还是 IDLE，别去 START。
        if (g_MediaMtuOverride) st.MediaMtu = g_MediaMtuOverride;
        printf("user-mode AVDTP: media slot=%lu mtu=%lu (kernel START skipped)\n",
               g_MediaSlot, st.MediaMtu);
    } else if (st.State != LDAC_STATE_STREAMING) {
        LDAC_START_REQ req;
        memset(&req, 0, sizeof(req));
        if (haveAddr) memcpy(req.Addr, addr, 6);
        if (loopback && !haveAddr) {
            /* 地址走注册表/默认；RateBit 由调用方偏好填充 */
        }
        //  --rate 强制协商采样率（诊断用）：某些 sink 的 AAC 解码器只认 44.1k，
        //  而它能力集里两档都声明了，光看协商结果是看不出来的。
        if (forceRate == 44100)      req.RateBit = LDAC_RATE_44100;
        else if (forceRate == 48000) req.RateBit = LDAC_RATE_48000;
        else if (forceRate == 88200) req.RateBit = LDAC_RATE_88200;
        else if (forceRate == 96000) req.RateBit = LDAC_RATE_96000;
        else if (loopback) {
            switch (mixRate) {
            case 96000: req.RateBit = LDAC_RATE_96000; break;
            case 88200: req.RateBit = LDAC_RATE_88200; break;
            case 48000: req.RateBit = LDAC_RATE_48000; break;
            case 44100: req.RateBit = LDAC_RATE_44100; break;
            }
        }
        printf("starting AVDTP session (kernel blocks for a few seconds)...\n");
        if (!DeviceIoControl(hDev, IOCTL_LDAC_START,
                             &req, sizeof(req), NULL, 0, &bytesRet, NULL)) {
            DeviceIoControl(hDev, IOCTL_LDAC_STATUS, NULL, 0,
                            &st, sizeof(st), &bytesRet, NULL);
            fprintf(stderr, "START failed: GetLastError=%lu driverLastErr=0x%08lX\n",
                    GetLastError(), (unsigned long)st.LastError);
            fprintf(stderr, "check log: C:\\Users\\Public\\ldac-probe.log\n");
            if (cap) g_CapStop = 1; Sleep(60); WasapiStop(cap);
            return 1;
        }
        if (!DeviceIoControl(hDev, IOCTL_LDAC_STATUS, NULL, 0,
                             &st, sizeof(st), &bytesRet, NULL))
            Die("IOCTL_LDAC_STATUS after start", GetLastError());
        printf("streaming: rateBit=0x%02lX modeBit=0x%02lX seid=%lu mediaMtu=%lu\n",
               st.RateBit, st.ModeBit, st.SinkSeid, st.MediaMtu);
    }

    // ---- 协商结果 → 编码器参数 -------------------------------------------
    switch (st.RateBit) {
    case LDAC_RATE_96000: freq = 96000; break;
    case LDAC_RATE_48000: freq = 48000; break;
    case LDAC_RATE_44100: freq = 44100; break;
    case LDAC_RATE_88200: freq = 88200; break;
    default: Die("unknown negotiated rate bit", 0);
    }
    switch (st.ModeBit) {
    case LDAC_CH_STEREO: cm = LDACBT_CHANNEL_MODE_STEREO; break;
    case LDAC_CH_DUAL:   cm = LDACBT_CHANNEL_MODE_DUAL_CHANNEL; break;
    case LDAC_CH_MONO:   cm = LDACBT_CHANNEL_MODE_MONO; break;
    default: Die("unknown negotiated mode bit", 0);
    }

    if (latmMode) {
        int n = LoadLatmFile(latmPath);
        if (n <= 0) Die("--latm: cannot read file", 0);
        printf("latm mode: %d bytes from %s (will send pre-made AudioMuxElements)\n", n, latmPath);
    }
    if (wavMode) {
        LoadWav(wavPath, &wav);
        if ((int)wav.rate != freq) {
            fprintf(stderr,
                "FATAL: wav is %u Hz but sink negotiated %d Hz (resample not supported;\n"
                "       feed a %d Hz wav)\n", wav.rate, freq, freq);
            DeviceIoControl(hDev, IOCTL_LDAC_STOP, NULL, 0, NULL, 0, &bytesRet, NULL);
            return 1;
        }
    } else if (!latmMode && (int)mixRate != freq) {
        fprintf(stderr,
            "FATAL: system mix rate %u Hz but sink negotiated %d Hz.\n"
            "       The sink doesn't support the mix rate. Set the sink's default\n"
            "       format to 16bit/%u Hz in mmsys.cpl, or accept mismatch is fatal here.\n",
            mixRate, freq, mixRate);
        DeviceIoControl(hDev, IOCTL_LDAC_STOP, NULL, 0, NULL, 0, &bytesRet, NULL);
        g_CapStop = 1; Sleep(60); WasapiStop(cap);
        return 1;
    }

    // ---- 编码器 -----------------------------------------------------------
    g_SamplesPerFrame = useAac ? AAC_LATM_FRAME_SAMPLES : 128;
    if (latmMode) {
        //  不建编码器：负载直接来自文件
    } else if (useAac) {
        char errbuf[256];
        aac = AacLatmOpen(freq, (cm == LDAC_CH_MONO) ? 1 : 2, 128000,
                          errbuf, (int)sizeof(errbuf));
        if (!aac) Die(errbuf, 0);
        printf("encoder: AAC-LC freq=%d ch=%d bitrate=128000 LATM mtu=%lu\n",
               freq, (cm == LDAC_CH_MONO) ? 1 : 2, st.MediaMtu);
    } else if (useSbc) {
        //  对照实验：SBC 48kHz / 16 blocks / 8 subbands / joint stereo / loudness。
        //  u16BitRate 单位是 **kbps**（AOSP 实现如此），267 正好落在设备声明的
        //  max bitpool = 38。SBC 帧长 89 字节。
        memset(&sbc, 0, sizeof(sbc));
        sbc.s16SamplingFreq     = (freq == 44100) ? SBC_sf44100 : SBC_sf48000;
        sbc.s16ChannelMode      = SBC_JOINT_STEREO;
        sbc.s16NumOfChannels    = 2;
        sbc.s16NumOfSubBands    = 8;
        sbc.s16NumOfBlocks      = 16;
        sbc.s16AllocationMethod = SBC_LOUDNESS;
        sbc.u16BitRate          = 267;
        sbc.u8NumPacketToEncode = 1;
        sbc.pu8Packet           = sbcFrame;
        SBC_Encoder_Init(&sbc);
        printf("encoder: SBC freq=%d bitpool=%d mtu=%lu\n", freq, (int)sbc.s16BitPool, st.MediaMtu);
    } else {
    hLdac = ldacBT_get_handle();
    if (!hLdac) Die("ldacBT_get_handle", 0);
    if (ldacBT_init_handle_encode(hLdac, (int)st.MediaMtu, eqmid, cm,
                                  LDACBT_SMPL_FMT_S16, freq) != 0)
        Die("ldacBT_init_handle_encode", (unsigned long)ldacBT_get_error_code(hLdac));
    printf("encoder: eqmid=%d freq=%d cm=%d mtu=%lu  (Ctrl+C to stop)\n",
           eqmid, freq, cm, st.MediaMtu);
    }

    snd.Dev = hDev;
    snd.MediaMtu = st.MediaMtu;
    snd.NoMediaHdr = (latmMode ? ((g_LatmStyle & 2) ? 0 : 1) : useAac);
    snd.Len = RTP_HDR + (useAac ? 0 : MEDIA_HDR);
    snd.Marker = 1;
    //  RFC 3550：序号和时间戳必须随机初值。抓包对照微软栈是从随机值开始的
    //  （58563 / 6047293），我们从 0 开始 —— 有些 sink 会因此判流无效。
    snd.Ssrc = (unsigned)GetTickCount() | 1;
    snd.Seq  = (unsigned short)(GetTickCount() * 2654435761u);
    snd.Ts   = (unsigned)(GetTickCount() * 2246822519u);
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    //  Windows 默认定时器精度 15.6ms，Sleep(1) 实际会睡到 15ms —— 节拍器
    //  一旦睡过头就是"一卡一卡"。抬到 1ms 精度。
    timeBeginPeriod(1);
    //  启动预缓冲：先攒 150ms 音频再开始发，吸收开头的抓取抖动
    if (!wavMode && !latmMode) {
        long long need = (long long)freq * 2 * (capBits / 8) * 150 / 1000;
        int waited = 0;
        while ((g_CapW - g_CapR) < need && waited < 3000 && !g_stop) {
            Sleep(10);
            waited += 10;
        }
        printf("prebuffer: %d ms waited, %d bytes queued\n",
               waited, (int)(g_CapW - g_CapR));
    }
    snd.Pacing = 1;
    snd.Qpf = qpf.QuadPart;
    snd.T0 = t0.QuadPart;
    snd.Freq = freq;
    CreateThread(NULL, 0, SenderThread, &snd, 0, NULL);   // 独立发送线程

    if (wavMode) {
        printf("streaming %.1f s of audio ...\n",
               (double)wav.bytes / (freq * 4.0));
    } else {
        printf("streaming system audio (Ctrl+C to stop)...\n");
    }

    // ---- 主循环：源 → S16 累积 → 512B/帧 编码 → RTP 聚包 → 发送 ----------
    for (;;) {
        unsigned char* src = NULL;
        long long availBytes = 0;
        int chunk = 4096;
        int got;
        long long target;

        if (g_stop) break;

        //  --latm：负载不是自己编的，而是从文件里逐个取 AudioMuxElement。
        if (latmMode) {
            unsigned char elem[2048];
            int n;
            while (QueuedCount() > 20 && !g_stop) Sleep(2);
            n = LatmNext(elem, (int)sizeof(elem));
            if (n < 0) Die("--latm: bad LOAS framing", 0);
            if (n == 0) break;                  // 文件放完
            RtpAppend(&snd, elem, n, 1);
            framesOut += 1;
            continue;
        }

        //  WAV 是"一次性读到底"的：不节流的话 5 秒音频在几十毫秒内全塞进队列，
        //  发送线程为了保低延迟会丢最旧的（队列 >40 就丢），结果只有一部分真的发出去
        //  （实测 sent=136 / queued=232），听感就是断断续续或干脆没声。
        //  这里按队列深度节流，模拟 loopback 的"实时供给"，行为才和实际产品一致。
        if (wavMode) {
            while (QueuedCount() > 20 && !g_stop) Sleep(2);
        }

        /* --- 取一段源数据 --- */
        if (wavMode) {
            static long long wavPos = 0;
            if (wavPos >= wav.bytes && accLen < (useAac ? 4096 : 512)) break;  /* 喂完且编码完 */
            if (wavPos < wav.bytes) {
                if (chunk > (int)(wav.bytes - wavPos)) chunk = (int)(wav.bytes - wavPos);
                src = wav.pcm + wavPos;
                availBytes = chunk;
                wavPos += chunk;
            }
            /* 实时节拍由 RtpFlush 里的样本时间轴节拍器负责（WAV/loopback 统一） */
        } else {
            {
                LONG w = g_CapW, rr = g_CapR;
                int avail2 = (int)(w - rr);
                if (avail2 <= 0) { Sleep(1); continue; }
                if (avail2 > (int)sizeof(s16)) avail2 = (int)sizeof(s16);
                {
                    int off = (int)(rr % CAP_RING_BYTES);
                    int first = CAP_RING_BYTES - off;
                    if (first > avail2) first = avail2;
                    memcpy(s16, g_CapRing + off, (size_t)first);
                    if (avail2 > first)
                        memcpy(s16 + first, g_CapRing, (size_t)(avail2 - first));
                }
                g_CapR = rr + avail2;
                got = avail2;
            }
            src = s16;
            availBytes = got;
        }

        /* --- 推入累积缓冲（loopback 是 float32 时先转 S16）--- */
        {
            const unsigned char* p = src;
            long long n = availBytes;
            unsigned char tmp[8192];

            if (!wavMode && capBits == 32) {
                /* float32 → S16（混音格式常态） */
                const float* f = (const float*)src;
                short* o = (short*)tmp;
                long long ns = n / 4;
                long long k;
                for (k = 0; k < ns; k++) {
                    float v = f[k];
                    if (v > 1.0f) v = 1.0f;
                    if (v < -1.0f) v = -1.0f;
                    o[k] = (short)(v * 32767.0f);
                }
                p = tmp;
                n = ns * 2;
            }

            if (accLen + n > (long long)sizeof(acc)) {
                /* 累积缓冲要满：先编码掉一部分（512 的倍数） */
                int dropTo = (int)(sizeof(acc) - n);
                dropTo -= dropTo % (useAac ? 4096 : 512);
                fprintf(stderr, "warn: pcm backlog, encoding %d bytes early\n", accLen - dropTo);
                /* 简单起见：直接丢最老的（loopback 实时流可丢；WAV 不会走到这） */
                if (dropTo > 0) {
                    memmove(acc, acc + (accLen - dropTo), (size_t)dropTo);
                    accLen = dropTo;
                } else {
                    accLen = 0;
                }
            }
            memcpy(acc + accLen, p, (size_t)n);

            {   /* 记录瞬时峰值，确认送进编码器的电平 */
                const short* s = (const short*)(acc + accLen);
                long long ns = n / 2, k;
                int pk = 0;
                for (k = 0; k < ns; k++) {
                    int a = s[k] < 0 ? -s[k] : s[k];
                    if (a > pk) pk = a;
                }
                snd.LastPeak = pk;
            }

            //  数字增益：端点音量拉满仍然偏小时，从源头把电平顶起来（带限幅）
            if (gain != 1.0) {
                short* s = (short*)(acc + accLen);
                long long ns = n / 2, k;
                for (k = 0; k < ns; k++) {
                    double v = s[k] * gain;
                    if (v > 32767.0)  v = 32767.0;
                    if (v < -32768.0) v = -32768.0;
                    s[k] = (short)v;
                }
            }

            accLen += (int)n;
        }

        /* --- 逐帧编码：LDAC/SBC 每帧 512B（128 采样），AAC 每帧 4096B（1024 采样） --- */
        while (accLen >= (useAac ? 4096 : 512)) {
            int inLen = useAac ? 4096 : 512, streamSz = 0, frameNum = 0;

            if (useAac) {
                //  喂 1024 采样/声道 → 一个 AAC AU → 封成 LATM AudioMuxElement。
                //  编码器内部会缓冲，返回值可能包含 0 个或多个元素，帧数按实际来。
                int got = AacLatmEncode(aac, (const short*)acc, AAC_LATM_FRAME_SAMPLES,
                                        enc, (int)sizeof(enc), &frameNum);
                if (got < 0) Die("AacLatmEncode failed", (unsigned long)(-got));
                memmove(acc, acc + 4096, (size_t)(accLen - 4096));
                accLen -= 4096;
                if (got > 0) {
                    RtpAppend(&snd, enc, got, frameNum);
                    framesOut += frameNum;
                }
                if (g_stop) break;
                continue;
            }

            if (useSbc) {
                /* 一帧 SBC = 16 blocks × 8 subbands = 128 采样/声道 = 512 字节 */
                if (framesOut == 0) {
                    const short* d = (const short*)acc;
                    printf("pcm[0..7] = %d %d %d %d %d %d %d %d\n",
                           d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
                }
                memcpy(sbc.as16PcmBuffer, acc, 512);
                memmove(acc, acc + 512, (size_t)(accLen - 512));
                accLen -= 512;
                sbc.pu8Packet = sbcFrame;
                //  ⚠ 编码器内部有个 ps16NextPcmBuffer，SBC_Encoder() 结尾会把它
                //  往前挪 ch*blocks（为了一次编多帧）。我们是一次一帧，不重置的话
                //  从第二帧起它就跑到缓冲区外，编出来的是越界的旧数据（实测连续
                //  几帧字节完全相同）。
                sbc.ps16NextPcmBuffer = sbc.as16PcmBuffer;
                SBC_Encoder(&sbc);
                if (sbc.u16PacketLength > 0) {
                    RtpAppend(&snd, sbcFrame, (int)sbc.u16PacketLength, 1);
                    framesOut += 1;
                }
                if (g_stop) break;
                continue;
            }

            rc = ldacBT_encode(hLdac, acc, &inLen, enc, &streamSz, &frameNum);
            if (rc != 0) Die("ldacBT_encode", (unsigned long)ldacBT_get_error_code(hLdac));
            memmove(acc, acc + inLen, (size_t)(accLen - inLen));
            accLen -= inLen;

            if (streamSz > 0) {
                RtpAppend(&snd, enc, streamSz, frameNum);
                framesOut += frameNum;
            }
            if (g_stop) break;
        }
    }

    /* 冲刷编码器内部缓冲 */
    for (i = 0; i < 32 && !useAac; i++) {
        int inLen = 0, streamSz = 0, frameNum = 0;
        if (ldacBT_encode(hLdac, NULL, &inLen, enc, &streamSz, &frameNum) != 0
            || streamSz <= 0)
            break;
        RtpAppend(&snd, enc, streamSz, frameNum);
        framesOut += frameNum;
    }
    RtpFlush(&snd);

    //  ⚠ RtpFlush 只是**入队**，PacketsSent 数的也是入队数。发送线程按样本时间轴
    //  一包一包往外放，队列里最多积压 256 包（AAC 一包一帧 ≈ 5 秒）。
    //  原来这里直接 Sleep(80) 就把线程停了 —— 5 秒音频实际只出去一百多毫秒，
    //  听感就是"一小段/干脆没声音"，极易误判成编解码器坏了（2026-11-09 踩过）。
    //  测试模式必须等队列真的放空。
    if (wavMode || latmMode) {
        int waited = 0;
        while (g_QH != g_QT && waited < 20000) { Sleep(5); waited += 5; }
        printf("drained in %d ms\n", waited);
    }

    if (wavMode || latmMode) {
        double secs = (double)framesOut * (double)g_SamplesPerFrame / freq;
        printf("\n=== done ===\n");
        printf("frames=%lld queued=%lld sent=%ld bytes=%lld (%.1f kbps, %.1f s)\n",
               framesOut, snd.PacketsSent, (long)g_SentOk, snd.BytesSent,
               secs > 0 ? (double)snd.BytesSent * 8.0 / secs / 1000.0 : 0.0, secs);
    } else {
        printf("\n=== stopped ===\n");
        printf("frames=%lld queued=%lld sent=%ld bytes=%lld\n",
               framesOut, snd.PacketsSent, (long)g_SentOk, snd.BytesSent);
    }

    g_QStop = 1; Sleep(80);                 // 先停发送线程，再关设备
    ldacBT_close_handle(hLdac);
    ldacBT_free_handle(hLdac);
    DeviceIoControl(hDev, IOCTL_LDAC_STOP, NULL, 0, NULL, 0, &bytesRet, NULL);
    CloseHandle(hDev);
    g_CapStop = 1; Sleep(60); WasapiStop(cap);
    free(wav.pcm);
    return 0;
}
