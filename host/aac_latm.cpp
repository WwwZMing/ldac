// ============================================================================
//  aac_latm.cpp — AAC-LC 编码（Media Foundation MFT）+ LATM 封帧
// ============================================================================
//  为什么不用 fdk-aac：许可证（Fraunhofer FDK AAC License）不允许随便分发
//  二进制；Windows 自带 AAC MFT 免许可、免打包，缺点是要自己拼 LATM。
//
//  ---- A2DP AAC 的负载格式（ISO/IEC 14496-3 LATM，muxConfigPresent=1）----
//  每帧一个 AudioMuxElement（按字节对齐，所以可以首尾相连）：
//
//    useSameStreamMux            1 bit   首帧 0（带 SMC），之后 1
//    [StreamMuxConfig]           ——仅首帧——
//      audioMuxVersion           1 bit   = 0
//      allStreamsSameTimeFraming 1 bit   = 1
//      numSubFrames              6 bit   = 0   （1 帧 - 1）
//      numProgram                4 bit   = 0   （1 个 - 1）
//      numLayer                  3 bit   = 0   （1 层 - 1）
//      AudioSpecificConfig:      AOT(5)=2 LC / SF索引(4) / 声道(4) /
//                                frameLengthFlag(1)=0 / dependsOnCoreCoder(1)=0 /
//                                extensionFlag(1)=0
//      frameLengthType           3 bit   = 0   （每 255 字节一个 0xFF 长度码）
//      latmBufferFullness        8 bit   = 0xFF（VBR/未知）
//      otherDataPresent          1 bit   = 0
//      crcCheckPresent           1 bit   = 0
//    PayloadLengthInfo           8*n bit       （255 切开，末字节余数）
//    PayloadMux                  AU 原始字节
//    fill                        补齐到字节边界
//
//  以上逐字段对照 AOSP external/aac 的 libMpegTPEnc/src/tpenc_latm.cpp 核过
//  （CreateStreamMuxConfig / WriteAuPayloadLengthInfo / AdvanceAudioMuxElement），
//  不是凭记忆写的。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <mmreg.h>
#include <stdio.h>
#include <string.h>

#include "aac_latm.h"

#ifndef WAVE_FORMAT_MPEG_HEAAC
#define WAVE_FORMAT_MPEG_HEAAC 0x1610
#endif

//  HEAACWAVEINFO 的后半截（WAVEFORMATEX 之后的部分），MF 要求放在
//  MF_MT_USER_DATA 里；wPayloadType=0 才是"裸 raw_data_block"，没有 ADTS 头。
#pragma pack(push, 1)
typedef struct {
    WAVEFORMATEX wf;
    WORD  wPayloadType;
    WORD  wAudioProfileLevelIndication;
    WORD  wStructType;
    DWORD dwReserved1;
    DWORD dwReserved2;
} HeaacWaveInfo;
#pragma pack(pop)

struct AacLatm {
    IMFTransform* mft;
    int      rate;
    int      ch;
    int      bitrate;
    LONGLONG ts;                 // 下一个输入样本的时间戳（100ns）
    int      configSent;         // 已经发过 StreamMuxConfig
    long long framesOut;
};

// ------------------------------------------------------------- 位写入器 ---
struct BitW {
    unsigned char* p;
    int cap;
    int bits;
};

static void BwPut(BitW* w, unsigned v, int n)
{
    int i;
    for (i = n - 1; i >= 0; i--) {
        int idx = w->bits >> 3;
        if (idx < w->cap && ((v >> i) & 1u)) {
            w->p[idx] |= (unsigned char)(0x80u >> (w->bits & 7));
        }
        w->bits++;
    }
}

static void BwAlign(BitW* w)
{
    while (w->bits & 7) BwPut(w, 0, 1);
}

static int AacSfIndex(int rate)
{
    switch (rate) {
    case 8000:  return 11;
    case 11025: return 10;
    case 12000: return 9;
    case 16000: return 8;
    case 22050: return 7;
    case 24000: return 6;
    case 32000: return 5;
    case 44100: return 4;
    case 48000: return 3;
    case 64000: return 2;
    case 88200: return 1;
    case 96000: return 0;
    default:    return -1;
    }
}

//  0 = 每帧带 SMC（默认，跟 Android 一致）；1 = 只在首帧带（对照实验）。
static int g_SmcOnce = -1;

//  把一帧 AU 封成 AudioMuxElement 追加到 out/reused 位上。
//  返回新的位数（调用方负责按 8 取整），失败返回 -1。
static int AppendMuxElement(AacLatm* c, BitW* w,
                            const unsigned char* au, int auLen)
{
    //  ⚠ 每帧都带 StreamMuxConfig（useSameStreamMux=0），不是只在首帧带。
    //  依据：Android A2DP 的 AAC 编码器（stack/a2dp/a2dp_aac_encoder.cc）明确把
    //  AACENC_HEADER_PERIOD 设成 1 —— fdk-aac 的 tpenc_latm.cpp 里 header period=1
    //  的效果就是 latmFrameCounter 每帧归零，于是**每帧都插一份 SMC**。
    //  我们的第一版只在首帧插、之后靠 useSameStreamMux=1 复用；这台 UGREEN
    //  会接受协商（SetConfig/Open/Start 全 Accept），但一个音符都不出。
    //  每帧多 5 字节，代价可以忽略。
    if (auLen <= 0) return -1;

    if (g_SmcOnce < 0) {
        const char* e = getenv("LDAC_LATM_SMC_ONCE");
        g_SmcOnce = (e != NULL && e[0] == '1') ? 1 : 0;
    }

    if (!g_SmcOnce || !c->configSent) {
        int sf    = AacSfIndex(c->rate);
        int ascBits = 5 + 4 + 4 + 3;     // AOT + sfIndex + chCfg + GASpecificConfig
        int ascLen, pad;

        if (sf < 0) return -1;

        //  ---- StreamMuxConfig（audioMuxVersion = 1，和 iPhone/Android 完全一致）----
        //  依据：StackOverflow 35915317 里用 ISO 14496-3 参考实现逐字段解出来的
        //  真实 iPhone 码流；也和我们从 AOSP external/aac 的 tpenc_latm.cpp
        //  CreateStreamMuxConfig() 读到的分支一一对上。
        //  ⚠ audioMuxVersion=1 不是"可选的高级玩法"：设为 0 时字段宽度完全不同
        //  （没有 audioMuxVersionA，没有 taraBufferFullness，ASC 也不带 ascLen），
        //  对端从第 3 位起就错位 —— 实测 UGREEN BT505 会照收不误但灯变色、不出声。
        BwPut(w, 0, 1);                          // useSameStreamMux = 0（带配置）
        BwPut(w, 1, 1);                          // audioMuxVersion = 1
        BwPut(w, 0, 1);                          // audioMuxVersionA = 0
        BwPut(w, 0xFF, 10);                      // taraBufferFullness = 255：先 2 位"值占几字节-1"=0，再 8 位值
        BwPut(w, 1, 1);                          // allStreamsSameTimeFraming = 1
        BwPut(w, 0, 6);                          // numSubFrames - 1 = 0
        BwPut(w, 0, 4);                          // numProgram - 1 = 0
        BwPut(w, 0, 3);                          // numLayer - 1 = 0

        //  ---- ascLen：长度字段自己占 10 位，ASC 16 位，合起来要补齐到字节边界 ----
        //  fdk 的算法是"先写一遍量出长度再回头写 ascLen"，等价于：
        //      ascLen = ascBits + 补齐到 (10+ascLen) 是 8 的倍数
        //  16 位 ASC → 10+22=32 → ascLen = 22（6 位填充位算在 ascLen 里）。
        //  iPhone 实测也是 ascLen = 22、fillBits = 6。
        pad = (8 - ((10 + ascBits) & 7)) & 7;
        ascLen = ascBits + pad;
        BwPut(w, (unsigned)ascLen, 10);          // 2 位"值占几字节-1" + 8 位值
        BwPut(w, 2, 5);                          // audioObjectType = 2 (AAC LC)
        BwPut(w, (unsigned)sf, 4);
        BwPut(w, (unsigned)c->ch, 4);
        BwPut(w, 0, 1);                          // frameLengthFlag
        BwPut(w, 0, 1);                          // dependsOnCoreCoder
        BwPut(w, 0, 1);                          // extensionFlag
        if (pad) BwPut(w, 0, pad);               // fillBits

        BwPut(w, 0, 3);                          // frameLengthType = 0
        BwPut(w, 0xC0, 8);                       // latmBufferFullness = 192（iPhone 实测值）
        BwPut(w, 0, 1);                          // otherDataPresent
        BwPut(w, 0, 1);                          // crcCheckPresent
        c->configSent = 1;
    } else {
        BwPut(w, 1, 1);                  // useSameStreamMux = 1
    }

    //  PayloadLengthInfo（frameLengthType 0：255 字节一段）
    {
        int rest = auLen;
        while (rest >= 255) {
            BwPut(w, 0xFF, 8);
            rest -= 255;
        }
        BwPut(w, (unsigned)rest, 8);
    }

    //  PayloadMux：AU 原始字节
    {
        int i;
        for (i = 0; i < auLen; i++) BwPut(w, au[i], 8);
    }

    return w->bits;
}

// --------------------------------------------------------------- 输出 ---
static void Drain(AacLatm* c, BitW* w, MFT_OUTPUT_STREAM_INFO* osi,
                  int* err)
{
    for (;;) {
        IMFSample*        smp = NULL;
        IMFMediaBuffer*   buf = NULL;
        MFT_OUTPUT_DATA_BUFFER odb;
        DWORD             status = 0;
        HRESULT           hr;
        int               provides = (osi->dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

        if (!provides) {
            DWORD need = osi->cbSize ? osi->cbSize : 16384;
            hr = MFCreateSample(&smp);
            if (FAILED(hr)) { *err = 1; return; }
            hr = MFCreateMemoryBuffer(need, &buf);
            if (FAILED(hr)) { smp->Release(); *err = 2; return; }
            smp->AddBuffer(buf);
            buf->Release();
        }

        memset(&odb, 0, sizeof(odb));
        odb.dwStreamID = 0;
        odb.pSample = smp;

        hr = c->mft->ProcessOutput(0, 1, &odb, &status);

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (smp) smp->Release();
            return;
        }
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            //  编码器换了输出类型（正常不会发生在固定参数下）；重新协商一次
            IMFMediaType* nt = NULL;
            if (SUCCEEDED(c->mft->GetOutputAvailableType(0, 0, &nt)) && nt) {
                c->mft->SetOutputType(0, nt, 0);
                nt->Release();
            }
            if (smp) smp->Release();
            continue;
        }
        if (FAILED(hr)) {
            if (smp) smp->Release();
            *err = 3;
            return;
        }

        if (smp) {
            IMFMediaBuffer* cb = NULL;
            BYTE* data = NULL;
            DWORD len = 0;

            if (SUCCEEDED(smp->ConvertToContiguousBuffer(&cb)) && cb) {
                if (SUCCEEDED(cb->Lock(&data, NULL, &len)) && data && len > 0) {
                    if (AppendMuxElement(c, w, data, (int)len) > 0) c->framesOut++;
                    cb->Unlock();
                }
                cb->Release();
            }
            smp->Release();
        }
        //  一次 ProcessOutput 只出一个 AU；继续拉直到 NEED_MORE_INPUT
    }
}

// ---------------------------------------------------------------- API ---
AacLatm* AacLatmOpen(int rate, int channels, int bitrate, char* err, int errLen)
{
    AacLatm*  c = NULL;
    HRESULT   hr;
    IMFActivate** acts = NULL;
    UINT32    nActs = 0;
    MFT_REGISTER_TYPE_INFO inInfo, outInfo;
    HeaacWaveInfo hw;
    IMFMediaType* otype = NULL;
    IMFMediaType* itype = NULL;
    static int mfStarted = 0;

    if (err && errLen > 0) err[0] = 0;

    if (!mfStarted) {
        hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(hr)) { if (err) sprintf_s(err, errLen, "MFStartup 0x%08lX", (unsigned long)hr); return NULL; }
        mfStarted = 1;
    }

    inInfo.guidMajorType  = MFMediaType_Audio;
    inInfo.guidSubtype    = MFAudioFormat_PCM;
    outInfo.guidMajorType = MFMediaType_Audio;
    outInfo.guidSubtype   = MFAudioFormat_AAC;

    hr = MFTEnumEx(MFT_CATEGORY_AUDIO_ENCODER,
                   MFT_ENUM_FLAG_ALL | MFT_ENUM_FLAG_SORTANDFILTER,
                   &inInfo, &outInfo, &acts, &nActs);
    if (FAILED(hr) || nActs == 0) {
        if (err) sprintf_s(err, errLen, "no AAC encoder MFT (0x%08lX, n=%u)", (unsigned long)hr, nActs);
        if (acts) CoTaskMemFree(acts);
        return NULL;
    }

    c = (AacLatm*)calloc(1, sizeof(*c));
    if (!c) { if (err) sprintf_s(err, errLen, "oom"); goto fail; }
    c->rate = rate;
    c->ch = channels;
    c->bitrate = bitrate;

    hr = acts[0]->ActivateObject(IID_IMFTransform, (void**)&c->mft);
    if (FAILED(hr) || !c->mft) {
        if (err) sprintf_s(err, errLen, "ActivateObject 0x%08lX", (unsigned long)hr);
        goto fail;
    }

    //  ---- 输出类型：AAC，裸 raw_data_block（payload type 0）----
    memset(&hw, 0, sizeof(hw));
    hw.wf.wFormatTag      = WAVE_FORMAT_MPEG_HEAAC;
    hw.wf.nChannels       = (WORD)channels;
    hw.wf.nSamplesPerSec  = (DWORD)rate;
    hw.wf.nAvgBytesPerSec = (DWORD)(bitrate / 8);
    hw.wf.nBlockAlign     = 1;
    hw.wf.wBitsPerSample  = 0;
    hw.wf.cbSize          = (WORD)(sizeof(HeaacWaveInfo) - sizeof(WAVEFORMATEX));
    hw.wPayloadType       = 0;       // 0 = 裸 AU（不是 ADTS）
    hw.wAudioProfileLevelIndication = 0x29;   // AAC LC
    hw.wStructType        = 0;

    hr = MFCreateMediaType(&otype);
    if (FAILED(hr)) { if (err) sprintf_s(err, errLen, "MFCreateMediaType 0x%08lX", (unsigned long)hr); goto fail; }
    otype->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    otype->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    otype->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    otype->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    otype->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    otype->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(bitrate / 8));
    otype->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    otype->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    otype->SetBlob(MF_MT_USER_DATA, (const UINT8*)&hw.wPayloadType,
                   (UINT32)(sizeof(HeaacWaveInfo) - sizeof(WAVEFORMATEX)));

    hr = c->mft->SetOutputType(0, otype, 0);
    if (FAILED(hr)) { if (err) sprintf_s(err, errLen, "SetOutputType 0x%08lX", (unsigned long)hr); goto fail; }

    //  ---- 输入类型：PCM 16 bit 交错 ----
    hr = MFCreateMediaType(&itype);
    if (FAILED(hr)) goto fail;
    itype->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    itype->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    itype->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
    itype->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)rate);
    itype->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    itype->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)(2 * channels));
    itype->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)(rate * 2 * channels));
    itype->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    hr = c->mft->SetInputType(0, itype, 0);
    if (FAILED(hr)) { if (err) sprintf_s(err, errLen, "SetInputType 0x%08lX", (unsigned long)hr); goto fail; }

    c->mft->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    c->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    c->mft->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    if (otype) otype->Release();
    if (itype) itype->Release();
    for (UINT32 k = 0; k < nActs; k++) acts[k]->Release();
    CoTaskMemFree(acts);
    return c;

fail:
    if (otype) otype->Release();
    if (itype) itype->Release();
    if (acts) {
        for (UINT32 k = 0; k < nActs; k++) acts[k]->Release();
        CoTaskMemFree(acts);
    }
    AacLatmClose(c);
    return NULL;
}

int AacLatmEncode(AacLatm* c, const short* pcm, int samples,
                  unsigned char* out, int outCap, int* frames)
{
    IMFMediaBuffer* ibuf = NULL;
    IMFSample*      ismp = NULL;
    MFT_OUTPUT_STREAM_INFO osi;
    BitW            w;
    int             bytes;
    long long       framesBefore;

    if (frames) *frames = 0;
    if (!c || !out || outCap <= 0) return -1;

    memset(out, 0, (size_t)outCap);
    w.p = out;
    w.cap = outCap;
    w.bits = 0;

    if (samples > 0) {
        DWORD byteLen = (DWORD)(samples * c->ch * 2);
        BYTE* p = NULL;

        if (FAILED(MFCreateMemoryBuffer(byteLen, &ibuf))) return -2;
        if (FAILED(ibuf->Lock(&p, NULL, NULL))) { ibuf->Release(); return -2; }
        memcpy(p, pcm, byteLen);
        ibuf->Unlock();
        ibuf->SetCurrentLength(byteLen);

        if (FAILED(MFCreateSample(&ismp))) { ibuf->Release(); return -2; }
        ismp->AddBuffer(ibuf);
        ibuf->Release();

        ismp->SetSampleTime(c->ts);
        ismp->SetSampleDuration((LONGLONG)samples * 10000000LL / c->rate);
        c->ts += (LONGLONG)samples * 10000000LL / c->rate;

        {
            HRESULT hr = c->mft->ProcessInput(0, ismp, 0);
            ismp->Release();
            if (FAILED(hr)) {
                if (hr != MF_E_NOTACCEPTING) return -3;
            }
        }
    }

    if (FAILED(c->mft->GetOutputStreamInfo(0, &osi))) return -4;
    framesBefore = c->framesOut;
    {
        int err = 0;
        Drain(c, &w, &osi, &err);
        if (err) return -5;
    }

    BwAlign(&w);
    bytes = w.bits >> 3;
    if (bytes <= 0) return 0;

    //  一次喂 1024 采样通常出 1 个 AU；开头补缓冲时可能一次出 2 个，
    //  所以帧数按实际情况报，RTP 时间戳才不会漂。
    if (frames) *frames = (int)(c->framesOut - framesBefore);
    return bytes;
}

void AacLatmClose(AacLatm* c)
{
    if (!c) return;
    if (c->mft) c->mft->Release();
    free(c);
}
