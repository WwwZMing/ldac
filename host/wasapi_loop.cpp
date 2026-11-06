// ============================================================================
//  wasapi_loop.cpp — WASAPI loopback 采集实现（C++/COM，独占 file）
// ============================================================================
//  参考 windows-driver-samples / Microsoft 文档的 loopback 标准用法：
//    默认渲染端点 + AUDCLNT_STREAMFLAGS_LOOPBACK，共享模式。
//  混音格式一般是 48kHz/2ch/float32（WAVE_FORMAT_EXTENSIBLE + IEEE_FLOAT 子格式）。
//  没声音时引擎会带 SILENT 标志交付，此时缓冲内容无意义，必须清零。
// ============================================================================

#include <stdio.h>
#include <string.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include "wasapi_loop.h"

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00aa00389b71}
// 手写这份 GUID，避免为它去拉 ksmedia.h
static const GUID kGuidIeeeFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

struct WasapiCap {
    IAudioClient*        Client;
    IAudioCaptureClient* Capture;
    WAVEFORMATEX*        Fmt;
};

static int HrFailed(HRESULT hr, const char* what)
{
    if (FAILED(hr)) {
        fprintf(stderr, "wasapi: %s failed 0x%08lX\n", what, (unsigned long)hr);
        return 1;
    }
    return 0;
}

//  按名字子串在启用的渲染端点里挑一个。找不到返回 NULL。
static IMMDevice* PickDeviceByName(IMMDeviceEnumerator* enumr, const char* needle)
{
    IMMDeviceCollection* coll = NULL;
    IMMDevice*           pick = NULL;
    UINT32               n = 0, i;

    if (!needle || !*needle) return NULL;
    if (FAILED(enumr->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || !coll)
        return NULL;

    coll->GetCount(&n);
    for (i = 0; i < n && !pick; ++i) {
        IMMDevice*     dev = NULL;
        IPropertyStore* store = NULL;
        PROPVARIANT     pv;
        BOOL            hit = FALSE;

        if (FAILED(coll->Item(i, &dev)) || !dev) continue;

        PropVariantInit(&pv);
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &store)) && store) {
            //  {a45c254e-df1c-4efd-8020-67d146a850e0},14 = PKEY_Device_FriendlyName
            const PROPERTYKEY kName = {
                { 0xa45c254e, 0xdf1c, 0x4efd,
                  { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
            if (SUCCEEDED(store->GetValue(kName, &pv)) && pv.vt == VT_LPWSTR) {
                char utf8[512];
                utf8[0] = 0;
                WideCharToMultiByte(CP_UTF8, 0, pv.pwszVal, -1, utf8, sizeof(utf8) - 1,
                                    NULL, NULL);
                if (strstr(utf8, needle)) hit = TRUE;
            }
            PropVariantClear(&pv);
            store->Release();
        }

        if (hit) pick = dev; else dev->Release();
    }
    coll->Release();
    return pick;
}

int WasapiStartMatch(const char* Match, WasapiCap** Out, unsigned* Rate,
                     unsigned* Channels, unsigned* Bits)
{
    WasapiCap*           c;
    IMMDeviceEnumerator* enumerator = NULL;
    IMMDevice*           device = NULL;
    HRESULT              hr;

    *Out = NULL;

    hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (HrFailed(hr, "CoInitializeEx")) return 1;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (HrFailed(hr, "CoCreateInstance(MMDeviceEnumerator)")) return 1;

    if (Match && *Match) {
        //  按名字挑，不用默认端点：SSH 是 session 0，默认端点不是用户听的那个
        device = PickDeviceByName(enumerator, Match);
        enumerator->Release();
        if (!device) {
            fprintf(stderr, "wasapi: no active render endpoint matches '%s'\n", Match);
            return 1;
        }
        fprintf(stderr, "wasapi: matched endpoint by name '%s'\n", Match);
    } else {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        enumerator->Release();
        if (HrFailed(hr, "GetDefaultAudioEndpoint")) return 1;
    }

    c = (WasapiCap*)calloc(1, sizeof(*c));
    if (!c) return 1;

    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&c->Client);
    device->Release();
    if (HrFailed(hr, "IMMDevice::Activate")) goto fail;

    hr = c->Client->GetMixFormat(&c->Fmt);
    if (HrFailed(hr, "GetMixFormat")) goto fail;

    // 只接 2ch；位深支持 32float（共享模式常态）与 16PCM，其余明确拒绝
    if (c->Fmt->nChannels != 2) {
        fprintf(stderr, "wasapi: mix format has %d channels, only 2 supported\n",
                c->Fmt->nChannels);
        goto fail;
    }
    if (c->Fmt->wBitsPerSample != 32 && c->Fmt->wBitsPerSample != 16) {
        fprintf(stderr, "wasapi: mix format %d-bit, only 16/32 supported\n",
                c->Fmt->wBitsPerSample);
        goto fail;
    }
    if (c->Fmt->wBitsPerSample == 32) {
        // 确认 32bit 是 float 而不是 int32
        BOOLEAN isFloat = (c->Fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
        if (!isFloat && c->Fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE && c->Fmt->cbSize >= 22) {
            const WAVEFORMATEXTENSIBLE* ex = (const WAVEFORMATEXTENSIBLE*)c->Fmt;
            isFloat = memcmp(&ex->SubFormat, &kGuidIeeeFloat, sizeof(GUID)) == 0;
        }
        if (!isFloat) {
            fprintf(stderr, "wasapi: 32-bit mix format is not float, unsupported\n");
            goto fail;
        }
    }

    // 200ms 缓冲，loopback 共享模式；轮询节奏由调用方决定
    hr = c->Client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK,
                               2000000, 0, c->Fmt, NULL);
    if (HrFailed(hr, "IAudioClient::Initialize(LOOPBACK)")) goto fail;

    hr = c->Client->GetService(__uuidof(IAudioCaptureClient), (void**)&c->Capture);
    if (HrFailed(hr, "GetService(IAudioCaptureClient)")) goto fail;

    hr = c->Client->Start();
    if (HrFailed(hr, "IAudioClient::Start")) goto fail;

    *Rate     = c->Fmt->nSamplesPerSec;
    *Channels = c->Fmt->nChannels;
    *Bits     = c->Fmt->wBitsPerSample;
    *Out      = c;
    return 0;

fail:
    WasapiStop(c);
    return 1;
}

int WasapiStart(WasapiCap** Out, unsigned* Rate, unsigned* Channels, unsigned* Bits)
{
    return WasapiStartMatch(NULL, Out, Rate, Channels, Bits);
}

int WasapiRead(WasapiCap* Cap, unsigned char* Buf, int Bytes)
{
    int total = 0;

    if (!Cap) return 0;

    while (total < Bytes) {
        UINT32 packet = 0;
        if (FAILED(Cap->Capture->GetNextPacketSize(&packet)) || packet == 0) {
            if (total == 0) Sleep(5);       // 没数据，稍等再来（调用方循环）
            break;
        }

        BYTE*    data = NULL;
        UINT32   frames = 0;
        DWORD    flags = 0;
        UINT32   bytes;
        int      want;

        if (FAILED(Cap->Capture->GetBuffer(&data, &frames, &flags, NULL, NULL)))
            break;

        bytes = frames * Cap->Fmt->nBlockAlign;
        want  = Bytes - total;
        if ((int)bytes > want) {
            // 一包超过剩余空间（不太可能，200ms 缓冲 vs 调用方大缓冲）：
            // 只能整包取，先放一部分，剩下的丢弃（宁丢不炸）
            bytes = (UINT32)want;
        }

        if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
            memset(Buf + total, 0, bytes);
        } else if (data) {
            memcpy(Buf + total, data, bytes);
        }
        total += (int)bytes;

        Cap->Capture->ReleaseBuffer(frames);
        if ((int)bytes < (int)(frames * Cap->Fmt->nBlockAlign)) {
            // 丢弃没装下的尾部（理论到不了这里）
            break;
        }
    }
    return total;
}

void WasapiStop(WasapiCap* Cap)
{
    if (!Cap) return;
    if (Cap->Client) Cap->Client->Stop();
    if (Cap->Capture) Cap->Capture->Release();
    if (Cap->Client) Cap->Client->Release();
    if (Cap->Fmt) CoTaskMemFree(Cap->Fmt);
    free(Cap);
}
