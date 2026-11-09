// ============================================================================
//  Adapter.cpp - PortCls 适配器入口 + PCM 观察线程
// ============================================================================
//  调用链（PortCls 标准套路）：
//    DriverEntry -> PcInitializeAdapterDriver
//    AddDevice   -> PcAddAdapterDevice
//    StartDevice -> NewMiniportXxx(&miniport)        我们的实现
//                   PcNewPort(&port, CLSID_PortXxx)  PortCls 的 port
//                   port->Init(DeviceObject, Irp, miniport, NULL, NULL)
//                   PcRegisterSubdevice(DeviceObject, L"Name", port)
//                   PcRegisterPhysicalConnection(...)
//
//  两个必须是坑：
//    1) PcRegisterSubdevice 第三参要 **PORT 对象(IPort*)**，不是 miniport 的
//       IUnknown —— 传错得到 STATUS_INVALID_PARAMETER(0xC000000D)。
//    2) PcNewPort 要 **CLSID_** 不是 **IID_**（CLSID_PortWaveRT =
//       {cc9be57a-...}，IID_IPortWaveRT = {339ff909-...}）。
// ============================================================================
#include "LdacKs.h"
#include "Raii.h"

//  BTHENUM 的 A2DP 子设备 PDO，AddDevice 里填，Driver.c 拿它取 BDIF 接口。
//  extern "C" 是必须的：Driver.c 是 C 编译的，不加就链不上（名字修饰不同）。
extern "C" PDEVICE_OBJECT g_LdacBthPdo = NULL;

// ---------------------------------------------------------------------------
//  适配器状态
//
//  【2026-11-02 修正】原来这里是一组全局变量 + g_Started 单实例守卫。
//  只要系统里出现第二个绑定到 VsToy.inf 的设备实例（devcon install 每跑一次
//  就多建一个 ROOT\MEDIA\NNNN），第二个实例就会在守卫处直接返回
//  STATUS_SUCCESS 却什么都没注册 —— 结果是一堆坏设备 + 端点变 PHANTOM。
//  现在全部搬进 PcAddAdapterDevice 分配的 **设备扩展**，每实例一份。
// ---------------------------------------------------------------------------

// ===========================================================================
//  PCM 观察线程
// ===========================================================================
//  为什么单独开线程：GetPosition 可能在 DISPATCH_LEVEL 被调用，
//  那里不能写文件、也不能长时间持锁。看电平这种事只能在 PASSIVE 慢慢来。
//
//  为什么必须能在 FreeAudioBuffer 前停干净：缓冲区是 MDL 映射的内核地址，
//  线程还在读的时候解映射 = use-after-free。
//
//  【2026-11-02】整个观察器现在由 LDAC_PCM_MONITOR 开关控制，默认 0 关闭。
//  原因：主机上 21:15 那次播放把整台机器硬挂了（无 bugcheck、无 dump、
//  日志只剩 38 个零字节）。在没排除它之前，先证明「端点 + PCM 通路」本身
//  没问题，再把观察器按开关单独打开。二分比猜快。
// ===========================================================================
#if LDAC_PCM_MONITOR
typedef struct _VS_PCM {

    PVOID       ThreadObject;      // PKTHREAD
    KEVENT      StopEvent;
    PVOID       Buffer;
    ULONG       Size;
    ULONG       BytesPerSec;
    ULONG       Channels;
    ULONG       Bits;
    BOOLEAN     IsFloat;
    BOOLEAN     Running;
    ULONGLONG   RunStart;          // 100ns，和 GetPosition 同一个锚点

    //  【只写不读的统计量】线程里绝不做 I/O，结果全攒在这里，
    //  由 LdacPcmStop 在普通线程上下文里一次性落盘。
    volatile LONG Ticks;           // 循环了几次（证明线程真的在跑）
    volatile LONG Samples;         // 一共读了多少个样本
    LONG          PeakL, PeakR;    // 历史峰值 0..1000
    LONG          LastL, LastR;    // 最近一次采样的电平

    //  ---- 录音：把引擎写进环形缓冲的 PCM 抄到一段线性内存，停止时落成 WAV ----
    //  这是虚拟声卡唯一"能听见"的办法：它没有物理输出，PCM 到了缓冲区就停住了。
    //  顺带一提，这段"从环形缓冲按帧抓 PCM"的代码就是 LDAC 主线要用的东西。
    PBYTE         CapBuf;          // NonPagedPool，线性录音缓冲
    ULONG         CapSize;         // 字节
    ULONG         CapWritten;      // 已写入字节
    ULONG         LastPos;         // 上次抓到的引擎写指针（字节偏移，帧对齐）
    BOOLEAN       CapOverflow;     // 缓冲满了，后面丢弃（不覆盖，保持时间连续）
} LDAC_PCM;

static LDAC_PCM g_Pcm;
static KSPIN_LOCK g_PcmLock;

#define LDAC_CAP_PATH  L"\\??\\C:\\Users\\Public\\vs-toy-capture.wav"

// ---------------------------------------------------------------------------
//  抓帧：把引擎新写进环形缓冲的数据抄进线性录音缓冲
//
//  这一步就是"从 KS 引脚取 PCM"，LDAC 主线要用的正是同一套逻辑，
//  只是那边取到之后交给用户态编码，这里取到之后落成 WAV。
// ---------------------------------------------------------------------------
static VOID
LdacCaptureNew(_Inout_ LDAC_PCM *pcm, _In_ ULONG head, _In_ ULONG frame)
{
    ULONG delta, first, room;

    if (pcm->CapBuf == NULL || pcm->CapOverflow || frame == 0) return;

    //  环形缓冲里"自上次以来新写进来的字节数"
    delta = (head + pcm->Size - pcm->LastPos) % pcm->Size;
    if (delta == 0) return;
    if (delta > pcm->Size) delta = pcm->Size;       // 防御：不可能，但别信

    room = pcm->CapSize - pcm->CapWritten;
    if (delta > room) {
        delta = room;
        pcm->CapOverflow = TRUE;                    // 满了就停，不覆盖，保持时间连续
    }

    delta -= delta % frame;                         // 只抄整帧
    if (delta == 0) return;

    //  环绕：先抄 [LastPos, Size)，不够再从 0 开始
    first = pcm->Size - pcm->LastPos;
    if (first > delta) first = delta;

    RtlCopyMemory(pcm->CapBuf + pcm->CapWritten,
                  (PBYTE)pcm->Buffer + pcm->LastPos, first);
    if (delta > first) {
        RtlCopyMemory(pcm->CapBuf + pcm->CapWritten + first,
                      (PBYTE)pcm->Buffer, delta - first);
    }

    pcm->CapWritten += delta;
    pcm->LastPos     = (pcm->LastPos + delta) % pcm->Size;
}

//  读一个样本（16bit 整数或 32bit float），归一化到 0..1000
//
//  注意：全程纯整数运算。内核里没有保存/恢复 FPU 上下文就不能用浮点指令，
//  否则会破坏用户线程的 x87/SSE 状态。float32 在这里只用来算电平，
//  直接按 IEEE754 位模式比较就够了 —— 正数的 bit pattern 是单调的。
static LONG
LdacSampleLevel(_In_ PBYTE p, _In_ BOOLEAN IsFloat, _In_ ULONG Bits)
{
    LONG v;

    if (IsFloat && Bits == 32) {
        ULONG bits = *(ULONG *)p;
        bits &= 0x7FFFFFFFUL;                   // 清符号位 == 取绝对值
        if (bits >= 0x3F800000UL) {             // >= 1.0f，已经削顶
            v = 1000;
        } else {
            v = (LONG)(((ULONGLONG)bits * 1000ULL) / 0x3F800000UL);
        }
    } else if (!IsFloat && Bits == 32) {
        //  32bit 整数 PCM（WAVE_FORMAT_EXTENSIBLE + PCM 子格式会出现）
        LONG s = *(LONG *)p;
        if (s < 0) s = -s;
        v = (LONG)(((LONGLONG)s * 1000LL) / 0x80000000LL);
    } else {
        SHORT s = *(SHORT *)p;
        v = s;
        if (v < 0) v = -v;
        v = (v * 1000) / 32768;
    }
    return v;
}

static VOID
LdacPcmThread(_In_ PVOID Context)
{
    LDAC_PCM        *pcm = (LDAC_PCM *)Context;
    LARGE_INTEGER  timeout;

    //  ========================================================================
    //  这个线程里【绝对不做任何 I/O】——不写日志、不建文件、不碰注册表。
    //
    //  实测证据（2026-11-02，VM 里复现）：
    //    带 ProbeLog 的版本只要播放一进入 KSSTATE_RUN，整个客户机就卡在内核里：
    //      - 桌面、任务栏时钟、文件拷贝全部正常（vmtoolsd 还活着）
    //      - 但是**任何新进程都创建不出来**（runProgramInGuest 起不来进程）
    //      - 日志里一条 vss 心跳都没有，最后一行停在 "SetState -> 3"
    //    把线程里的 I/O 全部去掉后问题消失。
    //
    //  原因不必再深究到寄存器级：从 PsCreateSystemThread 出来的线程跑在
    //  System 进程里，在那里做文件 I/O 会把文件系统 / 进程管理器共用的
    //  资源拖进一个不该有的等待关系里。规矩就一条 —— 内核线程只算数，
    //  落盘交给已经有正常进程上下文的线程（这里就是 FreeAudioBuffer）。
    //  ========================================================================
    for (;;) {
        LONG  peakL = 0, peakR = 0;

        //  50ms 一轮。必须明显快于环形缓冲的时长（我们把它放大到 250ms），
        //  否则引擎会在两次轮询之间绕回来把数据覆盖掉，录出来的音频是断续的。
        timeout.QuadPart = -500000LL;       // 相对 50ms
        if (KeWaitForSingleObject(&pcm->StopEvent, Executive, KernelMode,
                                  FALSE, &timeout) != STATUS_TIMEOUT) {
            break;                          // 收到停止信号
        }

        InterlockedIncrement(&pcm->Ticks);

        //  按时间推算引擎「刚写完」的位置，回看最近 20ms 算电平
        //
        //  ⚠ 这里原来直接算字节偏移：
        //        off = (head + Size - window + i) % Size
        //        read(pcm->Buffer + off)          // 左声道
        //        read(pcm->Buffer + off + Bits/8) // 右声道
        //    head 是任意字节位置，所以 off 可以落在 Size-1 附近，
        //    off + Bits/8 就跨过了缓冲区末尾。
        //    缓冲区是 MmAllocatePagesForMdlEx 出来的 5 页 MDL 映射，
        //    第 6 页并不存在 —— 2026-11-02 在 VM 里稳定复现 0x50，
        //    P1 正好等于「映射基址 + Size」，页对齐。
        //
        //  改法：按**帧**寻址。帧号对帧数取模，o = 帧号 * 帧长，
        //        o + 帧长 <= Size 恒成立，不需要任何边界判断。
        {
            ULONG frame  = (pcm->Bits / 8) * pcm->Channels;
            ULONG frames = frame ? (pcm->Size / frame) : 0;

            if (frames != 0) {
                ULONGLONG elapsed = KeQueryInterruptTime() - pcm->RunStart;
                ULONGLONG written = elapsed * pcm->BytesPerSec / 10000000ULL;
                ULONG     head    = (ULONG)(written % pcm->Size);
                ULONG     window  = pcm->BytesPerSec / 50;      // 20ms
                ULONG     want, startFrame, k;

                head -= head % frame;       // 帧对齐：抓帧和看电平共用同一个指针

                LdacCaptureNew(pcm, head, frame);

                if (window > pcm->Size) window = pcm->Size;
                want = window / frame;
                if (want == 0)  want = 1;
                if (want > frames) want = frames;

                startFrame = ((head + pcm->Size - want * frame) % pcm->Size) / frame;

                for (k = 0; k < want; k++) {
                    ULONG fi = (startFrame + k) % frames;       // 帧号，恒 < frames
                    ULONG o  = fi * frame;                      // o + frame <= Size
                    LONG  l  = LdacSampleLevel((PBYTE)pcm->Buffer + o,
                                             pcm->IsFloat, pcm->Bits);
                    LONG  r  = (pcm->Channels >= 2)
                                 ? LdacSampleLevel((PBYTE)pcm->Buffer + o + pcm->Bits / 8,
                                                 pcm->IsFloat, pcm->Bits)
                                 : l;
                    if (l > peakL) peakL = l;
                    if (r > peakR) peakR = r;
                }

                InterlockedExchangeAdd(&pcm->Samples, (LONG)want);
            }
        }

        pcm->LastL = peakL;
        pcm->LastR = peakR;
        if (peakL > pcm->PeakL) pcm->PeakL = peakL;
        if (peakR > pcm->PeakR) pcm->PeakR = peakR;

        //  这里原来有一条 ProbeLog（字符电平条），已经删掉 —— 见函数头的说明。
        //  想看电平请读 LdacPcmStop 打的 "pcm summary"。
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

extern "C" NTSTATUS
LdacPcmStart(_In_ PVOID Buffer, _In_ ULONG Size, _In_ ULONG BytesPerSec,
           _In_ ULONG Channels, _In_ ULONG BitsPerSample, _In_ BOOLEAN IsFloat)
{
    HANDLE   h = NULL;
    NTSTATUS st;

    //  PsCreateSystemThread / ObReferenceObjectByHandle / ZwClose 都要求
    //  PASSIVE_LEVEL。AllocateAudioBuffer 万一在 DISPATCH 上下来，
    //  这里必须直接放弃 —— 硬着头皮调下去就是 IRQL bugcheck。
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        ProbeMark("vs-pcmstart-bad-irql");
        return STATUS_INVALID_DEVICE_STATE;
    }

    if (g_Pcm.Running) {
        return STATUS_ALREADY_REGISTERED;
    }

    RtlZeroMemory(&g_Pcm, sizeof(g_Pcm));
    KeInitializeSpinLock(&g_PcmLock);
    KeInitializeEvent(&g_Pcm.StopEvent, NotificationEvent, FALSE);

    g_Pcm.Buffer      = Buffer;
    g_Pcm.Size        = Size;
    g_Pcm.BytesPerSec = BytesPerSec;
    g_Pcm.Channels    = Channels;
    g_Pcm.Bits        = BitsPerSample;
    g_Pcm.IsFloat     = IsFloat;
    g_Pcm.RunStart    = KeQueryInterruptTime();

    //  录音缓冲：约 8 秒。分配失败不影响电平功能，只是听不到。
    {
        ULONG cap = BytesPerSec * 8;
        if (cap > (8u * 1024u * 1024u)) cap = 8u * 1024u * 1024u;
        if (cap != 0) {
            g_Pcm.CapBuf = (PBYTE)ExAllocatePool2(POOL_FLAG_NON_PAGED, cap, LDAC_POOL_TAG);
            if (g_Pcm.CapBuf != NULL) {
                g_Pcm.CapSize = cap;
            } else {
                ProbeLog("pcm: capture buffer alloc(%lu) failed, levels only", cap);
            }
        }
    }

    st = PsCreateSystemThread(&h, THREAD_ALL_ACCESS, NULL, NULL, NULL,
                              LdacPcmThread, &g_Pcm);
    if (!NT_SUCCESS(st)) {
        ProbeLog("pcm: PsCreateSystemThread failed 0x%08X", st);
        return st;
    }

    //  线程句柄 -> 对象指针，句柄用完即关
    st = ObReferenceObjectByHandle(h, THREAD_ALL_ACCESS, NULL, KernelMode,
                                   &g_Pcm.ThreadObject, NULL);
    ZwClose(h);
    if (!NT_SUCCESS(st)) {
        g_Pcm.ThreadObject = NULL;
        return st;
    }

    g_Pcm.Running = TRUE;
    ProbeLog("pcm: monitor thread up  buf=%p size=%lu  %luHz %luch %lubit%s  bps=%lu",
          Buffer, Size,
          BytesPerSec / ((BitsPerSample / 8) * Channels), Channels, BitsPerSample,
          IsFloat ? " float" : "", BytesPerSec);
    return STATUS_SUCCESS;
}

// ---------------------------------------------------------------------------
//  float32 -> int16，纯整数运算
//
//  内核里没有保存/恢复 FPU 上下文就不能用浮点指令。IEEE754 的位模式
//  完全可以手工算：value = mant * 2^(exp-150)，乘 32767 后右移即可。
// ---------------------------------------------------------------------------
static SHORT
LdacFloatToS16(_In_ ULONG bits)
{
    ULONG sign = bits >> 31;
    ULONG exp  = (bits >> 23) & 0xFFUL;
    ULONG mant = bits & 0x7FFFFFUL;
    LONG  v;

    if (exp == 0)   return 0;                                   // 0 / denormal
    if (exp >= 150) return sign ? -32767 : 32767;               // >=1.0 或 inf/nan
    if (exp < 120)  return 0;                                   // < 2^-7，当 0

    mant |= 0x800000UL;                                         // 补隐含位
    v = (LONG)(((ULONGLONG)mant * 32767ULL) >> (150 - exp));
    return (SHORT)(sign ? -v : v);
}

#pragma pack(push, 1)
typedef struct _VS_WAV_HDR {
    CHAR  Riff[4];
    ULONG RiffSize;
    CHAR  Wave[4];
    CHAR  Fmt[4];
    ULONG FmtSize;
    WORD  Format;
    WORD  Channels;
    ULONG SampleRate;
    ULONG AvgBytes;
    WORD  BlockAlign;
    WORD  Bits;
    CHAR  Data[4];
    ULONG DataSize;
} LDAC_WAV_HDR;
#pragma pack(pop)

// ---------------------------------------------------------------------------
//  把录音缓冲写成 16bit PCM 的 WAV
//
//  统一降到 16bit：播放器对 32bit-int / float 的 WAV 兼容性很差，
//  而我们只是想"听见"，不是要保真。
// ---------------------------------------------------------------------------
static VOID
LdacWriteWavFile(_In_ LDAC_PCM *pcm)
{
    LDAC_WAV_HDR          hdr;
    UNICODE_STRING      path;
    OBJECT_ATTRIBUTES   oa;
    IO_STATUS_BLOCK     iosb;
    HANDLE              h = NULL;
    NTSTATUS            st;
    ULONG               frame, srcBytes, samples, outBytes, i, sampleRate;
    PBYTE               out, src;

    if (pcm->CapBuf == NULL) return;

    frame = (pcm->Bits / 8) * pcm->Channels;
    if (frame == 0) return;

    srcBytes = pcm->CapWritten - (pcm->CapWritten % frame);     // 只取整帧
    if (srcBytes == 0) {
        ProbeLog("pcm capture: nothing captured (ticks=%ld)", pcm->Ticks);
        return;
    }

    samples    = srcBytes / (pcm->Bits / 8);
    outBytes   = samples * 2;
    sampleRate = pcm->BytesPerSec / frame;
    src        = pcm->CapBuf;

    out = (PBYTE)ExAllocatePool2(POOL_FLAG_NON_PAGED, outBytes, LDAC_POOL_TAG);
    if (out == NULL) {
        ProbeLog("pcm capture: out of memory for %lu bytes", outBytes);
        return;
    }

    for (i = 0; i < samples; i++) {
        SHORT s;
        if (pcm->Bits == 16) {
            s = *(SHORT *)(src + i * 2);
        } else if (pcm->IsFloat) {
            s = LdacFloatToS16(*(ULONG *)(src + i * 4));
        } else {
            s = (SHORT)(*(LONG *)(src + i * 4) >> 16);          // 32bit int，取高 16 位
        }
        *(SHORT *)(out + i * 2) = s;
    }

    RtlZeroMemory(&hdr, sizeof(hdr));
    RtlCopyMemory((PVOID)hdr.Riff, "RIFF", 4);
    RtlCopyMemory((PVOID)hdr.Wave, "WAVE", 4);
    RtlCopyMemory((PVOID)hdr.Fmt,  "fmt ", 4);
    RtlCopyMemory((PVOID)hdr.Data, "data", 4);
    hdr.FmtSize    = 16;
    hdr.Format     = 1;                                          // WAVE_FORMAT_PCM
    hdr.Channels   = (WORD)pcm->Channels;
    hdr.SampleRate = sampleRate;
    hdr.Bits       = 16;
    hdr.BlockAlign = (WORD)(pcm->Channels * 2);
    hdr.AvgBytes   = sampleRate * hdr.BlockAlign;
    hdr.DataSize   = outBytes;
    hdr.RiffSize   = 36 + outBytes;

    RtlInitUnicodeString(&path, LDAC_CAP_PATH);
    InitializeObjectAttributes(&oa, &path,
                               OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    st = ZwCreateFile(&h, FILE_WRITE_DATA | SYNCHRONIZE, &oa, &iosb, NULL,
                      FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
                      FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH, NULL, 0);
    if (!NT_SUCCESS(st)) {
        ProbeLog("pcm capture: ZwCreateFile failed 0x%08X", st);
    } else {
        (VOID)ZwWriteFile(h, NULL, NULL, NULL, &iosb, &hdr, sizeof(hdr), NULL, NULL);
        (VOID)ZwWriteFile(h, NULL, NULL, NULL, &iosb, out, outBytes, NULL, NULL);
        ZwClose(h);
        ProbeLog("pcm capture: %lu samples (%lu bytes, %luch %luHz)%s -> vs-toy-capture.wav",
              samples, outBytes, pcm->Channels, sampleRate,
              pcm->CapOverflow ? " [TRUNCATED]" : "");
    }

    ExFreePoolWithTag(out, LDAC_POOL_TAG);
}

extern "C" BOOLEAN
LdacPcmStop(VOID)
{
    LARGE_INTEGER timeout;
    NTSTATUS      st;

    if (!g_Pcm.Running) {
        return TRUE;
    }

    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        //  停线程要 KeWaitForSingleObject，DISPATCH 上等会死锁。
        //  这里原来是 __debugbreak() —— 那是 int 3，没有内核调试器时
        //  等于 bugcheck 0x80000003。留个标记让线程自己退出，绝不主动崩。
        ProbeMark("vs-pcmstop-bad-irql");
        return FALSE;
    }

    KeSetEvent(&g_Pcm.StopEvent, IO_NO_INCREMENT, FALSE);

    if (g_Pcm.ThreadObject != NULL) {
        //  有界等待：线程里已经没有 I/O，正常情况下几个 tick 就退出了。
        //  等不到就放弃 —— 调用方会跳过 MmUnmapLockedPages，宁可泄漏。
        timeout.QuadPart = -100000000LL;        // 相对 10 秒
        st = KeWaitForSingleObject(g_Pcm.ThreadObject, Executive, KernelMode,
                                   FALSE, &timeout);
        if (st == STATUS_TIMEOUT) {
            ProbeLog("!!! pcm: thread did not exit in 10s (ticks=%ld); leaving buffer mapped",
                  g_Pcm.Ticks);
            return FALSE;                       // 不清 Running，也不解引用
        }
        ObDereferenceObject(g_Pcm.ThreadObject);
        g_Pcm.ThreadObject = NULL;
    }

    g_Pcm.Running = FALSE;
    g_Pcm.Buffer  = NULL;

    //  汇总。这里能安全写日志：调用链是
    //  FreeAudioBuffer <- PortCls <- 音频引擎的普通用户线程，有正常进程上下文。
    {
        LONG pl = g_Pcm.PeakL, pr = g_Pcm.PeakR;
        LONG ll = g_Pcm.LastL, lr = g_Pcm.LastR;
        CHAR bar[20];
        ULONG k, cells = (ULONG)((pl * 16) / 1000);

        if (cells > 16) cells = 16;
        for (k = 0; k < 16; k++) {
            bar[k] = (k < cells) ? '#' : '.';
        }
        bar[16] = '\0';

        ProbeLog("pcm summary: ticks=%ld samples=%ld peakL=%ld.%ld%% peakR=%ld.%ld%%",
              g_Pcm.Ticks, g_Pcm.Samples,
              pl / 10, pl % 10, pr / 10, pr % 10);
        ProbeLog("pcm |%s| last L=%ld.%ld%% R=%ld.%ld%%  %luHz %luch %lubit%s",
              bar, ll / 10, ll % 10, lr / 10, lr % 10,
              g_Pcm.BytesPerSec / ((g_Pcm.Bits / 8) * g_Pcm.Channels),
              g_Pcm.Channels, g_Pcm.Bits,
              g_Pcm.IsFloat ? " float" : "");
    }

    //  落盘成 WAV。线程已经退出，缓冲区不再被读，可以安全写文件。
    LdacWriteWavFile(&g_Pcm);

    if (g_Pcm.CapBuf != NULL) {
        ExFreePoolWithTag(g_Pcm.CapBuf, LDAC_POOL_TAG);
        g_Pcm.CapBuf  = NULL;
        g_Pcm.CapSize = 0;
    }

    return TRUE;
}

#else   // !LDAC_PCM_MONITOR -----------------------------------------------------

//  关闭态：两个桩。调用点（Miniport WaveRT）完全不用改，
//  日志里能明确看到"是被编译开关关掉的"，而不是"启动失败"。
extern "C" NTSTATUS
LdacPcmStart(_In_ PVOID Buffer, _In_ ULONG Size, _In_ ULONG BytesPerSec,
           _In_ ULONG Channels, _In_ ULONG BitsPerSample, _In_ BOOLEAN IsFloat)
{
    UNREFERENCED_PARAMETER(Buffer);
    UNREFERENCED_PARAMETER(BytesPerSec);
    UNREFERENCED_PARAMETER(Channels);
    UNREFERENCED_PARAMETER(BitsPerSample);
    UNREFERENCED_PARAMETER(IsFloat);

    ProbeLog("pcm: monitor DISABLED by build switch (buf=%lu)", Size);
    return STATUS_NOT_SUPPORTED;
}

extern "C" BOOLEAN
LdacPcmStop(VOID)
{
    return TRUE;
}

#endif  // LDAC_PCM_MONITOR

// ===========================================================================
// ===========================================================================
//  动态端点：按链路在位安装 / 移除（照 SysVAD 的做法，2026-11-09）
// ===========================================================================
//  0x3B 的根因不是"动态"本身，而是**复用 port**：卸载后再用同一个 port
//  调 Init，会崩在 portcls!CPortFilterWaveRT::Init —— port 与 miniport 的
//  绑定在 IUnregisterSubdevice 之后已经失效，Init 直接解引用它。
//
//  微软样例的处理（sysvad/common.cpp: InstallSubDevice / RemoveEndpointFilters）：
//    * 每次安装都**新建 miniport + port**，用
//      port->Init(DeviceObject, NULL, Miniport, NULL, NULL)（动态路径 Irp 就是 NULL）；
//    * 移除时 DisconnectPhysicalConnection + IUnregisterSubdevice + Release，
//      一个对象都不复用。
//  我们照抄这个形状：链路在位 -> 安装；链路断开 -> 移除。
//  端点因此会随耳机开关出现/消失，不再常驻。
static PLDAC_ADAPTER_EXT g_LinkExt     = NULL;
static PIO_WORKITEM      g_LinkWork    = NULL;
static volatile LONG     g_LinkPresent = 0;
static volatile LONG     g_LinkPending = -1;   // -1 无请求 / 0 移除 / 1 安装

extern "C" BOOLEAN
LdacKsIsLinkPresent(VOID)
{
    return (InterlockedCompareExchange(&g_LinkPresent, 0, 0) != 0) ? TRUE : FALSE;
}

static VOID
LdacKsRemoveEndpoint(_In_ PLDAC_ADAPTER_EXT Ext)
{
    if (Ext == NULL || Ext->EndpointInstalled == 0) return;

    // 1) 断开物理连接
    if (Ext->WavePort != NULL && Ext->TopoPort != NULL) {
        PUNREGISTERPHYSICALCONNECTION raw = NULL;
        if (NT_SUCCESS(Ext->WavePort->QueryInterface(IID_IUnregisterPhysicalConnection,
                                                     (PVOID *)&raw)) && raw != NULL) {
            ComRef<IUnregisterPhysicalConnection> upc(raw);
            upc->UnregisterPhysicalConnection(Ext->DeviceObject, Ext->WavePort, 1,
                                              Ext->TopoPort, 0);
        }
    }

    // 2) 注销两个子设备（顺序无所谓，但必须在 Release 之前）
    {
        PUNREGISTERSUBDEVICE raw = NULL;
        if (Ext->WavePort != NULL &&
            NT_SUCCESS(Ext->WavePort->QueryInterface(IID_IUnregisterSubdevice,
                                                     (PVOID *)&raw)) && raw != NULL) {
            ComRef<IUnregisterSubdevice> usd(raw);
            usd->UnregisterSubdevice(Ext->DeviceObject, Ext->WavePort);
        }
    }
    {
        PUNREGISTERSUBDEVICE raw = NULL;
        if (Ext->TopoPort != NULL &&
            NT_SUCCESS(Ext->TopoPort->QueryInterface(IID_IUnregisterSubdevice,
                                                     (PVOID *)&raw)) && raw != NULL) {
            ComRef<IUnregisterSubdevice> usd(raw);
            usd->UnregisterSubdevice(Ext->DeviceObject, Ext->TopoPort);
        }
    }

    // 3) 释放：注销过的 port 一律不复用，下次安装重新建
    if (Ext->TopoPort    != NULL) { Ext->TopoPort->Release();    Ext->TopoPort    = NULL; }
    if (Ext->WavePort    != NULL) { Ext->WavePort->Release();    Ext->WavePort    = NULL; }
    if (Ext->TopoUnknown != NULL) { Ext->TopoUnknown->Release(); Ext->TopoUnknown = NULL; }
    if (Ext->WaveUnknown != NULL) { Ext->WaveUnknown->Release(); Ext->WaveUnknown = NULL; }

    InterlockedExchange(&Ext->EndpointInstalled, 0);
    ProbeLog("link-down: endpoint removed");
}

static NTSTATUS
LdacKsInstallEndpoint(_In_ PLDAC_ADAPTER_EXT Ext)
{
    PUNKNOWN wave = NULL, topo = NULL;
    PPORT    wavePort = NULL, topoPort = NULL;
    NTSTATUS st;

    if (Ext == NULL || Ext->DeviceObject == NULL) return STATUS_INVALID_DEVICE_STATE;
    if (InterlockedCompareExchange(&Ext->EndpointInstalled, 1, 0) != 0) return STATUS_SUCCESS;

    st = NewMiniportWaveRT(&wave);
    if (!NT_SUCCESS(st)) { ProbeLog("install: NewMiniportWaveRT 0x%08X", st); goto fail; }
    st = NewMiniportTopology(&topo);
    if (!NT_SUCCESS(st)) { ProbeLog("install: NewMiniportTopology 0x%08X", st); goto fail; }
    st = PcNewPort(&wavePort, CLSID_PortWaveRT);
    if (!NT_SUCCESS(st)) { ProbeLog("install: PcNewPort(WaveRT) 0x%08X", st); goto fail; }
    st = PcNewPort(&topoPort, CLSID_PortTopology);
    if (!NT_SUCCESS(st)) { ProbeLog("install: PcNewPort(Topology) 0x%08X", st); goto fail; }

    //  动态路径没有 IRP：SysVAD 就是传 NULL（sysvad/A2dpHpDevice.cpp:3627）。
    st = topoPort->Init(Ext->DeviceObject, NULL, topo, NULL, NULL);
    if (!NT_SUCCESS(st)) { ProbeLog("install: topoPort->Init 0x%08X", st); goto fail; }
    st = wavePort->Init(Ext->DeviceObject, NULL, wave, NULL, NULL);
    if (!NT_SUCCESS(st)) { ProbeLog("install: wavePort->Init 0x%08X", st); goto fail; }

    st = PcRegisterSubdevice(Ext->DeviceObject, L"Topology", topoPort);
    if (!NT_SUCCESS(st)) { ProbeLog("install: register Topology 0x%08X", st); goto fail; }
    st = PcRegisterSubdevice(Ext->DeviceObject, L"Wave", wavePort);
    if (!NT_SUCCESS(st)) { ProbeLog("install: register Wave 0x%08X", st); goto fail; }
    st = PcRegisterPhysicalConnection(Ext->DeviceObject, wavePort, 1, topoPort, 0);
    if (!NT_SUCCESS(st)) { ProbeLog("install: PhysConn 0x%08X", st); goto fail; }

    Ext->WaveUnknown = wave;  wave = NULL;
    Ext->TopoUnknown = topo;  topo = NULL;
    Ext->WavePort    = wavePort; wavePort = NULL;
    Ext->TopoPort    = topoPort; topoPort = NULL;
    ProbeLog("link-up: endpoint installed (fresh port+miniport)");
    return STATUS_SUCCESS;

fail:
    if (topoPort != NULL) topoPort->Release();
    if (wavePort != NULL) wavePort->Release();
    if (topo     != NULL) topo->Release();
    if (wave     != NULL) wave->Release();
    InterlockedExchange(&Ext->EndpointInstalled, 0);
    return st;
}

//  端点操作**永远不在调用方栈上做**，统一丢给工作项：
//    * SetState 可能来自 DISPATCH（L2CAP 断开指示）；
//    * AddDevice/BthBoot 阶段（adapter 还没 StartDevice 完成）注册子设备会卡死整机
//      —— 2026-11-09 实测：开机黑屏，无 dump。
//  工作项里再检查 g_AdapterReady（StartDevice 结束时置 1），没就绪就什么都不做，
//  由 StartDevice 结束时补一次。
static volatile LONG g_AdapterReady = 0;

static VOID
LdacLinkWorkItem(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context);

static VOID
LdacKsQueueLinkUpdate(VOID)
{
    if (g_LinkWork == NULL) return;
    InterlockedExchange(&g_LinkPending, g_LinkPresent ? 1 : 0);
    IoQueueWorkItem(g_LinkWork, LdacLinkWorkItem, DelayedWorkQueue, NULL);
}

static VOID
LdacLinkWorkItem(_In_ PDEVICE_OBJECT DeviceObject, _In_opt_ PVOID Context)
{
    LONG want = InterlockedExchange(&g_LinkPending, -1);
    PLDAC_ADAPTER_EXT ext = g_LinkExt;

    UNREFERENCED_PARAMETER(DeviceObject);
    UNREFERENCED_PARAMETER(Context);

    if (ext == NULL || want < 0) return;
    if (InterlockedCompareExchange(&g_AdapterReady, 0, 0) == 0) {
        ProbeLog("link: adapter not started yet, defer to StartDevice");
        return;
    }
    if (want == 1)      (VOID)LdacKsInstallEndpoint(ext);
    else if (want == 0) LdacKsRemoveEndpoint(ext);
}

//  链路在位 = 会话真的能建起来（LdacSetup 进到 STREAMING）。
//  端点跟随这个信号：在位 -> 安装（每次新建 port/miniport，绝不复用）；
//  不在位 -> 拆除。耳机一关机，端点就从系统里消失；再开机重新装一套。
extern "C" VOID
LdacKsSetLinkPresent(_In_ BOOLEAN Present)
{
    BOOLEAN changed = (InterlockedExchange(&g_LinkPresent, Present ? 1 : 0) == (Present ? 1 : 0)) ? FALSE : TRUE;

    if (!changed) return;
    ProbeLog("link: %s  jack.IsConnected=%lu", Present ? "up" : "down",
             (ULONG)(Present ? 1 : 0));
    LdacKsQueueLinkUpdate();
}

//  设备节点到达/移除只做清理与建工作项，不直接决定端点（端点跟链路）。
extern "C" VOID
LdacKsSetDevicePresent(_In_ BOOLEAN Present)
{
    ProbeLog("device: %s", Present ? "arrived" : "removed");
    if (!Present) {
        InterlockedExchange(&g_LinkPresent, 0);
        LdacKsQueueLinkUpdate();
    }
}

extern "C" VOID
LdacKsOnDeviceRemove(VOID)
{
    PLDAC_ADAPTER_EXT ext = g_LinkExt;

    //  PnP remove 是 PASSIVE：可以在这里移除端点。
    InterlockedExchange(&g_AdapterReady, 0);
    if (ext != NULL) {
        LdacKsRemoveEndpoint(ext);
        InterlockedExchange(&ext->Started, 0);
    }
    g_LinkExt = NULL;

    //  工作项只有在没有排队请求时才回收；否则宁可泄漏一个，
    //  也不能 free 一个可能还在队列里的 IO_WORKITEM（历史踩过 0x139）。
    if (g_LinkWork != NULL) {
        if (InterlockedCompareExchange(&g_LinkPending, -1, -1) == -1) {
            IoFreeWorkItem(g_LinkWork);
            g_LinkWork = NULL;
        } else {
            ProbeLog("remove: work item still queued, leaked");
        }
    }
}


//  StartDevice
// ===========================================================================
static NTSTATUS
LdacKsStartDevice(
    _In_ PDEVICE_OBJECT DeviceObject,
    _In_ PIRP           Irp,
    _In_ PRESOURCELIST  ResourceList)
{
    PLDAC_ADAPTER_EXT ext = LDAC_EXT_FROM_DEVOBJ(DeviceObject);
    UNREFERENCED_PARAMETER(ResourceList);
    UNREFERENCED_PARAMETER(Irp);

    ProbeMark("vs-10-startdevice");
    ProbeLog("=== PortCls StartDevice ===");

    if (ext == NULL) {
        ProbeLog("!! StartDevice: 没有设备扩展（PcAddAdapterDevice 第五参没给大小？）");
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }
    if (InterlockedCompareExchange(&ext->Started, 1, 0) != 0) {
        ProbeLog("StartDevice: 本实例已启动过，忽略重复调用");
        return STATUS_SUCCESS;
    }
    ext->DeviceObject = DeviceObject;

    //  端点本身不在这里建：它跟着蓝牙链路走（LdacKsSetLinkPresent ->
    //  LdacKsInstallEndpoint）。StartDevice 只准备设备扩展和工作项。
    g_LinkExt = ext;
    if (g_LinkWork == NULL) {
        g_LinkWork = IoAllocateWorkItem(DeviceObject);
        ProbeLog("StartDevice: link work item = %p", g_LinkWork);
    }

    //  到这里 adapter 才算可用：之前注册子设备会卡死（开机黑屏，无 dump）。
    InterlockedExchange(&g_AdapterReady, 1);
    LdacKsQueueLinkUpdate();

    ProbeLog("=== PortCls StartDevice OK（端点等链路在位再装）===" );
    ProbeMark("vs-19-startdevice-ok");
    return STATUS_SUCCESS;
}

// ===========================================================================
//  AddDevice（PortCls 适配器）
// ===========================================================================
//  注意：DriverEntry 不在这里 —— 它在 Driver.c 里，因为入口还要创建控制设备
//  并且要跟 PortCls 抢 IRP_MJ_* 派发表。这里只提供给 PcInitializeAdapterDriver
//  用的 AddDevice 回调。
// ===========================================================================

//  这个 PDO 是不是 BTHENUM 子设备？
//
//  ⚠ 必须有这个判断。实测踩过：pnputil /install 会把包装到所有硬件 ID
//    匹配的设备上（我们的 INF 里还有一条 ROOT\LdacA2dp 用于隔离测试），
//    于是同一个驱动有两个实例，而 g_LdacBthPdo 是**单个全局**——
//    后执行的 AddDevice 把 ROOT 的 PDO 写了进去，之后所有 BRB/L2CAP
//    都对着一个 root 设备发，QI 直接 0xC00000BB（真机上就是这么翻的，
//    VM 里只有一个实例所以没暴露）。
static BOOLEAN
LdacIsBthEnumNode(_In_ PDEVICE_OBJECT Pdo)
{
    WCHAR           buf[64];
    ULONG           len = 0;
    UNICODE_STRING  a, b;
    NTSTATUS        st;

    RtlZeroMemory(buf, sizeof(buf));
    st = IoGetDeviceProperty(Pdo, DevicePropertyEnumeratorName,
                             sizeof(buf) - sizeof(WCHAR), buf, &len);
    if (!NT_SUCCESS(st)) {
        return FALSE;
    }
    buf[(sizeof(buf) / sizeof(WCHAR)) - 1] = UNICODE_NULL;

    RtlInitUnicodeString(&a, buf);
    RtlInitUnicodeString(&b, L"BTHENUM");
    return RtlEqualUnicodeString(&a, &b, TRUE);
}

extern "C"
NTSTATUS
LdacKsAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT PhysicalDeviceObject)
{
    NTSTATUS  st;
    BOOLEAN   isBth = LdacIsBthEnumNode(PhysicalDeviceObject);

    ProbeMark("ad-01-adddevice");
    ProbeLog("=== AddDevice (PortCls adapter) pdo=%p bthenum=%u ===",
             PhysicalDeviceObject, (ULONG)isBth);

    //  BTHENUM 的 A2DP 子设备才记下来。BRB/L2CAP 全靠对它发
    //  IRP_MN_QUERY_INTERFACE 拿 BTH_PROFILE_DRIVER_INTERFACE。
    if (isBth) {
        g_LdacBthPdo = PhysicalDeviceObject;
    }

    //  第 5 参必须 >= 0x1A8（PortCls 自己的扩展），私有数据接在后面。
    //  详细依据见 LdacKs.h 里 LDAC_PC_EXT_SIZE 的注释。
    st = PcAddAdapterDevice(DriverObject, PhysicalDeviceObject,
                            LdacKsStartDevice, LDAC_MAX_SUBDEV, LDAC_EXT_TOTAL);
    ProbeLog("PcAddAdapterDevice -> 0x%08X (ext total=%lu, ours=%lu @0x%X)",
             st, (ULONG)LDAC_EXT_TOTAL, (ULONG)sizeof(LDAC_ADAPTER_EXT),
             (ULONG)LDAC_PC_EXT_SIZE);
    if (!NT_SUCCESS(st)) {
        return st;
    }

    //  BRB 接口 + L2CAP 服务端要尽早挂上：耳机连上时会主动拨 PSM 25，
    //  必须赶在它拨之前把"电话铃"挂好，否则握手被别人（btha2dp）吞掉。
    //  只对蓝牙节点做 —— 对 root 实例做只会白报一条 QI 失败。
    if (isBth) {
        LdacBthBoot(PhysicalDeviceObject);
    }

    return STATUS_SUCCESS;
}
