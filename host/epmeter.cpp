// ============================================================================
//  epmeter.cpp — 端点音量/电平仪表（判断"声音小"丢在哪一级）
//
//  为什么需要它：音量问题不能靠耳朵对号。要分清三件事：
//    (1) 端点主音量（0..100）以及该端点是否真有硬件音量
//        hw=0 => 音量条只能由 Windows 在音频引擎里做软件衰减，
//                送进驱动的采样已经变小了；
//    (2) 同一段落 loopback 采到的峰值/有效值 dBFS；
//    (3) 内置 0 dBFS 单音自测 => 不依赖外部播放器，结论可复现。
//
//  用法：
//    epmeter --list                          列出所有渲染端点 + 音量 + hw
//    epmeter --match LDAC --vol 100 --tone   放 0dBFS 997Hz 并测 loopback
//    epmeter --match LDAC --sec 5            只测 loopback 5 秒（自己放音乐）
//
//  端点选择用名字子串匹配，不走"默认端点"：SSH 会话(0)里默认端点不是
//  用户在控制台用的那个，按名字抓才稳。
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>

// 友好名属性键 {a45c254e-df1c-4efd-8020-67d146a850e0}, pid 14
// 自己写死，免得为 INITGUID/DEFINE_PROPERTYKEY 引一堆链接冲突
static const PROPERTYKEY kPkeyFriendlyName = {
    { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14
};

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ---------------------------------------------------------------- 小工具
static char* WideToUtf8(const wchar_t* w)
{
    static char buf[1024];
    buf[0] = 0;
    if (!w) return buf;
    WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf) - 1, NULL, NULL);
    return buf;
}

static const char* StateName(DWORD s)
{
    switch (s) {
    case DEVICE_STATE_ACTIVE:    return "active";
    case DEVICE_STATE_DISABLED:  return "disabled";
    case DEVICE_STATE_NOTPRESENT:return "notpresent";
    case DEVICE_STATE_UNPLUGGED: return "unplugged";
    default:                     return "?";
    }
}

struct EndpointInfo {
    int    volPct;      // -1 表示取不到
    int    muted;
    int    hwSupport;   // 0 = 没有硬件音量（软件音量）
    char   name[256];
};

static int QueryVolume(IMMDevice* dev, EndpointInfo* out)
{
    IAudioEndpointVolume* vol = NULL;
    HRESULT hr;
    out->volPct = -1;
    out->muted = -1;
    out->hwSupport = -1;

    hr = dev->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, NULL, (void**)&vol);
    if (FAILED(hr) || !vol) return -1;

    float scalar = 0.0f;
    if (SUCCEEDED(vol->GetMasterVolumeLevelScalar(&scalar)))
        out->volPct = (int)(scalar * 100.0f + 0.5f);

    BOOL mute = FALSE;
    if (SUCCEEDED(vol->GetMute(&mute)))
        out->muted = mute ? 1 : 0;

    DWORD hw = 0;
    if (SUCCEEDED(vol->QueryHardwareSupport(&hw)))
        out->hwSupport = (int)(hw & ENDPOINT_HARDWARE_SUPPORT_VOLUME) ? 1 : 0;

    vol->Release();
    return 0;
}

static void FillName(IMMDevice* dev, char* dst, size_t cap)
{
    IPropertyStore* store = NULL;
    PROPVARIANT     pv;
    dst[0] = 0;
    PropVariantInit(&pv);
    if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store))) {
        if (SUCCEEDED(store->GetValue(kPkeyFriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
            strncpy_s(dst, cap, WideToUtf8(pv.pwszVal), _TRUNCATE);
        }
        PropVariantClear(&pv);
        store->Release();
    }
}

static BOOL ContainsNoCase(const char* hay, const char* needle)
{
    if (!needle || !*needle) return TRUE;
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn > hn) return FALSE;
    for (size_t i = 0; i + nn <= hn; ++i) {
        if (_strnicmp(hay + i, needle, nn) == 0) return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------- 电平统计
struct Level {
    double peak;      // 归一化 0..1
    double sumsq;
    unsigned long long n;   // 采样点数（已展开多声道）
    unsigned long long silentPackets;
    unsigned long long frames;
};

static void LevelAdd(Level* lv, const BYTE* data, UINT32 frames, UINT32 ch, WORD bits,
                     bool isFloat, DWORD flags)
{
    if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        lv->silentPackets++;
        lv->frames += frames;
        lv->n += (unsigned long long)frames * ch;
        return;
    }
    if (!data) return;

    const double full = (bits == 32) ? 1.0 : 32768.0;
    for (UINT32 i = 0; i < frames; ++i) {
        for (UINT32 c = 0; c < ch; ++c) {
            double v;
            if (isFloat) v = ((const float*)data)[(size_t)i * ch + c];
            else         v = ((const short*)data)[(size_t)i * ch + c] / 32768.0;
            v = fabs(v);
            if (v > lv->peak) lv->peak = v;
            lv->sumsq += v * v;
            lv->n++;
        }
    }
    lv->frames += frames;
    (void)full;
}

static double ToDb(double v)
{
    if (v <= 1e-12) return -200.0;
    return 20.0 * log10(v);
}

// ---------------------------------------------------------------- 主流程
struct Args {
    int    list;
    int    tone;         // 用内置单音自测
    int    vol;          // >=0 表示要设置
    double freq;
    double sec;
    const char* match;
};

static int RunOnce(IMMDevice* device, const Args* a)
{
    HRESULT hr;
    IAudioClient*          cap = NULL;
    IAudioCaptureClient*   capSvc = NULL;
    IAudioClient*          ren = NULL;
    IAudioRenderClient*    renSvc = NULL;
    WAVEFORMATEX*          fmt = NULL;
    int                    rc = 1;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&cap);
    if (FAILED(hr)) { printf("ERR: Activate(loopback) 0x%08lX\n", (unsigned long)hr); return 1; }

    hr = cap->GetMixFormat(&fmt);
    if (FAILED(hr)) { printf("ERR: GetMixFormat 0x%08lX\n", (unsigned long)hr); goto done; }

    bool isFloat = (fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (!isFloat && fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && fmt->cbSize >= 22) {
        static const GUID kFloat =
            { 0x00000003, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } };
        isFloat = memcmp(&((WAVEFORMATEXTENSIBLE*)fmt)->SubFormat, &kFloat, sizeof(GUID)) == 0;
    }

    printf("mix: %lu Hz  %u ch  %u bit  %s\n",
           (unsigned long)fmt->nSamplesPerSec, fmt->nChannels, fmt->wBitsPerSample,
           isFloat ? "float" : "pcm");

    // ---- loopback 采集
    hr = cap->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                         2000000, 0, fmt, NULL);
    if (FAILED(hr)) { printf("ERR: cap Initialize 0x%08lX\n", (unsigned long)hr); goto done; }
    hr = cap->GetService(__uuidof(IAudioCaptureClient), (void**)&capSvc);
    if (FAILED(hr)) { printf("ERR: GetService(capture) 0x%08lX\n", (unsigned long)hr); goto done; }

    // ---- 单音渲染（可选）
    if (a->tone) {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&ren);
        if (FAILED(hr)) { printf("ERR: Activate(render) 0x%08lX\n", (unsigned long)hr); goto done; }
        hr = ren->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 1000000, 0, fmt, NULL);
        if (FAILED(hr)) { printf("ERR: ren Initialize 0x%08lX\n", (unsigned long)hr); goto done; }
        hr = ren->GetService(__uuidof(IAudioRenderClient), (void**)&renSvc);
        if (FAILED(hr)) { printf("ERR: GetService(render) 0x%08lX\n", (unsigned long)hr); goto done; }
    }

    hr = cap->Start();
    if (FAILED(hr)) { printf("ERR: cap Start 0x%08lX\n", (unsigned long)hr); goto done; }

    if (a->tone) {
        hr = ren->Start();
        if (FAILED(hr)) { printf("ERR: ren Start 0x%08lX\n", (unsigned long)hr); goto done; }
    }

    {
        Level lv;
        memset(&lv, 0, sizeof(lv));

        UINT32 renFrames = 0;
        if (a->tone) ren->GetBufferSize(&renFrames);

        double phase = 0.0;
        double step  = 2.0 * M_PI * a->freq / (double)fmt->nSamplesPerSec;
        ULONGLONG t0 = GetTickCount64();
        ULONGLONG toneMs = (ULONGLONG)(a->sec * 1000.0);
        bool playing = a->tone != 0;

        while (GetTickCount64() - t0 < toneMs) {
            // 1) 喂单音
            if (playing) {
                UINT32 pad = 0;
                if (SUCCEEDED(ren->GetCurrentPadding(&pad)) && pad < renFrames) {
                    UINT32 want = renFrames - pad;
                    if (want > 480) want = 480;     // 每次最多 10ms@48k，留够余量
                    BYTE* p = NULL;
                    if (SUCCEEDED(renSvc->GetBuffer(want, &p)) && p) {
                        for (UINT32 i = 0; i < want; ++i) {
                            double s = 0.999 * sin(phase);
                            phase += step;
                            if (phase > 2.0 * M_PI) phase -= 2.0 * M_PI;
                            for (UINT32 c = 0; c < fmt->nChannels; ++c) {
                                if (isFloat) ((float*)p)[(size_t)i * fmt->nChannels + c] = (float)s;
                                else         ((short*)p)[(size_t)i * fmt->nChannels + c] = (short)(s * 32767.0);
                            }
                        }
                        renSvc->ReleaseBuffer(want, 0);
                    }
                }
            }

            // 2) 收 loopback
            for (;;) {
                UINT32 packet = 0;
                if (FAILED(capSvc->GetNextPacketSize(&packet)) || packet == 0) break;
                BYTE* data = NULL;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(capSvc->GetBuffer(&data, &frames, &flags, NULL, NULL))) break;
                LevelAdd(&lv, data, frames, fmt->nChannels, fmt->wBitsPerSample, isFloat, flags);
                capSvc->ReleaseBuffer(frames);
            }
            Sleep(3);
        }

        if (playing) ren->Stop();

        // 收尾：把管线里剩下的取完
        for (int k = 0; k < 40; ++k) {
            UINT32 packet = 0;
            if (FAILED(capSvc->GetNextPacketSize(&packet)) || packet == 0) { Sleep(5); continue; }
            BYTE* data = NULL; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(capSvc->GetBuffer(&data, &frames, &flags, NULL, NULL))) break;
            LevelAdd(&lv, data, frames, fmt->nChannels, fmt->wBitsPerSample, isFloat, flags);
            capSvc->ReleaseBuffer(frames);
        }
        cap->Stop();

        double rms = (lv.n > 0) ? sqrt(lv.sumsq / (double)lv.n) : 0.0;
        printf("captured: %.2f s-equivalent frames=%llu  silentPkts=%llu\n",
               lv.frames / (double)fmt->nSamplesPerSec, lv.frames, lv.silentPackets);
        printf("peak: %.6f  %+7.2f dBFS\n", lv.peak, ToDb(lv.peak));
        printf("rms : %.6f  %+7.2f dBFS\n", rms,    ToDb(rms));
        rc = 0;
    }

done:
    if (ren)        ren->Stop();
    if (cap)        cap->Stop();
    if (renSvc)     renSvc->Release();
    if (capSvc)     capSvc->Release();
    if (ren)        ren->Release();
    if (cap)        cap->Release();
    if (fmt)        CoTaskMemFree(fmt);
    return rc;
}

int main(int argc, char** argv)
{
    Args a;
    memset(&a, 0, sizeof(a));
    a.vol = -1; a.freq = 997.0; a.sec = 3.0;

    for (int i = 1; i < argc; ++i) {
        if (!_stricmp(argv[i], "--list"))      a.list = 1;
        else if (!_stricmp(argv[i], "--tone")) a.tone = 1;
        else if (!_stricmp(argv[i], "--match") && i + 1 < argc) a.match = argv[++i];
        else if (!_stricmp(argv[i], "--vol")   && i + 1 < argc) a.vol = atoi(argv[++i]);
        else if (!_stricmp(argv[i], "--sec")   && i + 1 < argc) a.sec = atof(argv[++i]);
        else if (!_stricmp(argv[i], "--freq")  && i + 1 < argc) a.freq = atof(argv[++i]);
        else { printf("usage: epmeter [--list] [--match substr] [--vol 0..100] [--tone] [--sec N] [--freq Hz]\n"); return 2; }
    }

    SetConsoleOutputCP(CP_UTF8);

    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) { printf("ERR: CoInitializeEx 0x%08lX\n", (unsigned long)hr); return 1; }

    IMMDeviceEnumerator* enumr = NULL;
    IMMDeviceCollection* coll  = NULL;
    int rc = 1;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumr);
    if (FAILED(hr)) { printf("ERR: CoCreateInstance 0x%08lX\n", (unsigned long)hr); goto out; }

    hr = enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED |
                                   DEVICE_STATE_NOTPRESENT | DEVICE_STATE_UNPLUGGED, &coll);
    if (FAILED(hr)) { printf("ERR: EnumAudioEndpoints 0x%08lX\n", (unsigned long)hr); goto out; }

    {
        UINT32 n = 0;
        coll->GetCount(&n);
        printf("render endpoints: %lu\n", (unsigned long)n);

        IMMDevice* target = NULL;
        for (UINT32 i = 0; i < n; ++i) {
            IMMDevice* dev = NULL;
            if (FAILED(coll->Item(i, &dev)) || !dev) continue;
            EndpointInfo info;
            memset(&info, 0, sizeof(info));
            FillName(dev, info.name, sizeof(info.name));
            QueryVolume(dev, &info);
            DWORD st = 0;
            dev->GetState(&st);

            printf("  [%lu] %-46s state=%-10s vol=%3d%% mute=%d hw=%d\n",
                   (unsigned long)i, info.name, StateName(st), info.volPct,
                   info.muted, info.hwSupport);

            if (!target && info.name[0] && ContainsNoCase(info.name, a.match)) target = dev;
            if (target != dev) dev->Release();
        }

        if (!a.list) {
            if (!target) {
                printf("ERR: no endpoint matched '%s'\n", a.match ? a.match : "");
                rc = 3;
            } else {
                EndpointInfo info;
                memset(&info, 0, sizeof(info));
                FillName(target, info.name, sizeof(info.name));
                QueryVolume(target, &info);
                printf("\nselected: %s  (vol=%d%% mute=%d hw=%d)\n",
                       info.name, info.volPct, info.muted, info.hwSupport);

                if (a.vol >= 0) {
                    IAudioEndpointVolume* v = NULL;
                    if (SUCCEEDED(target->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL,
                                                   NULL, (void**)&v)) && v) {
                        float s = (float)a.vol / 100.0f;
                        HRESULT h1 = v->SetMasterVolumeLevelScalar(s, NULL);
                        HRESULT h2 = v->SetMute(FALSE, NULL);
                        float back = -1.0f;
                        v->GetMasterVolumeLevelScalar(&back);
                        printf("set vol %d%% -> hr=0x%08lX unmute=0x%08lX now=%.0f%%\n",
                               a.vol, (unsigned long)h1, (unsigned long)h2, back * 100.0f);
                        v->Release();
                    }
                }

                printf("\n--- measure (%.1f s%s) ---\n", a.sec, a.tone ? ", built-in 0dBFS tone" : "");
                rc = RunOnce(target, &a);
            }
        } else {
            rc = 0;
        }
    }

out:
    if (coll)  coll->Release();
    if (enumr) enumr->Release();
    return rc;
}
