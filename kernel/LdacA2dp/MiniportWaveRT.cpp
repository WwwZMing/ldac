// ============================================================================
//  MiniportWaveRT.cpp - WaveRT render miniport
// ============================================================================
//  只描述 filter/pin/node/connection，工厂由 PortCls 创建（不自己建 KS 工厂）。
//
//  render 端点必须有 **两个** 引脚（这条踩过坑）：
//    pin 0  流引脚：音频引擎把 PCM 写进来（DATAFLOW_IN / SINK）
//    pin 1  桥接引脚：和设备侧拓扑相连（DATAFLOW_OUT / NONE，ANALOG 范围）
//  只有流引脚时，AudioEndpointBuilder 拿不到「设备侧」桥接引脚，
//  GetMixFormat 返回 AUDCLNT_E_UNSUPPORTED_FORMAT，播放直接失败，
//  而且整个过程根本不会调用到 miniport。
// ============================================================================
#include "LdacKs.h"

// ---------------------------------------------------------------------------
//  支持格式：PCM 16bit / IEEE float 32bit，2 声道，44.1k ~ 96k
//
//  Windows 共享模式的混音格式通常是 32-bit float；只声明 16bit PCM
//  会让 WASAPI 初始化走窄路径甚至失败，所以两种都列。
// ---------------------------------------------------------------------------
static KSDATARANGE_AUDIO g_WaveRanges[] = {
    {
        { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
          STATIC_KSDATAFORMAT_TYPE_AUDIO,
          STATIC_KSDATAFORMAT_SUBTYPE_PCM,
          STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX },
        2,          // MaximumChannels
        16,         // MinimumBitsPerSample
        16,         // MaximumBitsPerSample
        44100,      // MinimumSampleFrequency
        96000       // MaximumSampleFrequency
    },
    {
        { sizeof(KSDATARANGE_AUDIO), 0, 0, 0,
          STATIC_KSDATAFORMAT_TYPE_AUDIO,
          STATIC_KSDATAFORMAT_SUBTYPE_IEEE_FLOAT,
          STATIC_KSDATAFORMAT_SPECIFIER_WAVEFORMATEX },
        2, 32, 32, 44100, 96000
    }
};

static PKSDATARANGE g_WaveRangePtrs[] = {
    (PKSDATARANGE)&g_WaveRanges[0],
    (PKSDATARANGE)&g_WaveRanges[1]
};

//  桥接引脚用模拟范围（同 sysvad 的 SpeakerPinDataRangesBridge）
static KSDATARANGE g_WaveBridgeRange = {
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    STATIC_KSDATAFORMAT_TYPE_AUDIO,
    STATIC_KSDATAFORMAT_SUBTYPE_ANALOG,
    STATIC_KSDATAFORMAT_SPECIFIER_NONE
};

static PKSDATARANGE g_WaveBridgeRangePtrs[] = { &g_WaveBridgeRange };

static const GUID g_GuidCatAudio = { STATIC_KSCATEGORY_AUDIO };

static PCPIN_DESCRIPTOR g_WavePins[] = {
    // pin 0: KSPIN_WAVE_RENDER_SINK
    {
        1, 1, 0,                                  // MaxGlobal / MaxFilter / MinFilter
        NULL,                                     // AutomationTable
        {
            0, NULL,                              // Interfaces
            0, NULL,                              // Mediums
            SIZEOF_ARRAY(g_WaveRangePtrs),        // DataRangesCount
            g_WaveRangePtrs,                      // DataRanges
            KSPIN_DATAFLOW_IN,                    // DataFlow
            KSPIN_COMMUNICATION_SINK,             // Communication
            (GUID *)&g_GuidCatAudio,              // Category
            NULL,                                 // Name
            0                                     // Reserved
        }
    },
    // pin 1: KSPIN_WAVE_RENDER_SOURCE（桥接）
    {
        0, 0, 0,
        NULL,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_WaveBridgeRangePtrs),
            g_WaveBridgeRangePtrs,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            (GUID *)&g_GuidCatAudio,
            NULL,
            0
        }
    }
};

// ---------------------------------------------------------------------------
//  KSPROPERTY_PIN_PROPOSEDATAFORMAT 必须实现
//
//  音频引擎通过它把「设备格式」下发。没有这个 handler，引擎 SetProperty 失败，
//  端点永远没有可用格式，GetMixFormat 返回 AUDCLNT_E_UNSUPPORTED_FORMAT，
//  而且 miniport 的 DataRangeIntersection / NewStream 一次都不会被调用。
// ---------------------------------------------------------------------------
static NTSTATUS
WaveFilterPropertyHandler(_In_ PPCPROPERTY_REQUEST PropertyRequest);

static volatile LONG g_PdCount = 0;   // 日志节流

static PCPROPERTY_ITEM g_WaveFilterProperties[] = {
    {
        &KSPROPSETID_General,
        KSPROPERTY_GENERAL_COMPONENTID,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        WaveFilterPropertyHandler
    },
    {
        &KSPROPSETID_Pin,
        KSPROPERTY_PIN_PROPOSEDATAFORMAT,
        KSPROPERTY_TYPE_SET | KSPROPERTY_TYPE_BASICSUPPORT,
        WaveFilterPropertyHandler
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(AutomationWaveFilter, g_WaveFilterProperties);

//  DAC 节点：pin0 -> node0(引脚1) -> node0(引脚0) -> pin1
static PCNODE_DESCRIPTOR g_WaveNodes[] = {
    {
        0,                          // Flags
        NULL,                       // AutomationTable
        &KSNODETYPE_DAC,            // Type
        NULL                        // Name
    }
};

static PCCONNECTION_DESCRIPTOR g_WaveConnections[] = {
    { PCFILTER_NODE, 0, 0, 1 },     // pin0 -> node0 输入
    { 0, 0, PCFILTER_NODE, 1 }      // node0 输出 -> pin1
};

static PCFILTER_DESCRIPTOR g_WaveFilterDescriptor = {
    0,                                   // Version
    &AutomationWaveFilter,               // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),            // PinSize
    SIZEOF_ARRAY(g_WavePins),            // PinCount
    g_WavePins,                          // Pins
    sizeof(PCNODE_DESCRIPTOR),           // NodeSize
    SIZEOF_ARRAY(g_WaveNodes),           // NodeCount
    g_WaveNodes,                         // Nodes
    SIZEOF_ARRAY(g_WaveConnections),     // ConnectionCount
    g_WaveConnections,                   // Connections
    0,                                   // CategoryCount
    NULL                                 // Categories - PortCls 默认
};

static NTSTATUS
WaveFilterPropertyHandler(_In_ PPCPROPERTY_REQUEST PropertyRequest)
{
    if (PropertyRequest == NULL || PropertyRequest->PropertyItem == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    const GUID* set = PropertyRequest->PropertyItem->Set;

    //  BASICSUPPORT 统一回 GET|SET
    if ((PropertyRequest->Verb & KSPROPERTY_TYPE_BASICSUPPORT) != 0) {
        if (PropertyRequest->Value == NULL ||
            PropertyRequest->ValueSize < sizeof(ULONG)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        *(PULONG)PropertyRequest->Value = 0x1 | 0x2;   // SUPPORT_GET | SUPPORT_SET
        PropertyRequest->ValueSize = sizeof(ULONG);
        return STATUS_SUCCESS;
    }

    //  KSPROPERTY_GENERAL_COMPONENTID
    if (IsEqualGUIDAligned(*set, KSPROPSETID_General) &&
        PropertyRequest->PropertyItem->Id == KSPROPERTY_GENERAL_COMPONENTID) {

        if ((PropertyRequest->Verb & KSPROPERTY_TYPE_GET) == 0) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        if (PropertyRequest->Value == NULL) {
            return STATUS_INVALID_PARAMETER;
        }
        if (PropertyRequest->ValueSize < sizeof(KSCOMPONENTID)) {
            return STATUS_BUFFER_TOO_SMALL;
        }

        KSCOMPONENTID* cid = (KSCOMPONENTID*)PropertyRequest->Value;
        RtlZeroMemory(cid, sizeof(*cid));
        cid->Version  = 1;
        cid->Revision = 0;

        PropertyRequest->ValueSize = sizeof(KSCOMPONENTID);
        return STATUS_SUCCESS;
    }

    //  KSPROPERTY_PIN_PROPOSEDATAFORMAT
    if (IsEqualGUIDAligned(*set, KSPROPSETID_Pin) &&
        PropertyRequest->PropertyItem->Id == KSPROPERTY_PIN_PROPOSEDATAFORMAT) {

        if ((PropertyRequest->Verb & KSPROPERTY_TYPE_SET) == 0) {
            return STATUS_INVALID_DEVICE_REQUEST;
        }
        if (PropertyRequest->Value == NULL ||
            PropertyRequest->ValueSize < sizeof(KSDATAFORMAT)) {
            return STATUS_INVALID_PARAMETER;
        }

        PKSDATAFORMAT fmt = (PKSDATAFORMAT)PropertyRequest->Value;
        LONG n = InterlockedIncrement(&g_PdCount);
        if (n <= 8) {
            ProbeLog("ProposedDataFormat: size=%lu flags=%08X",
                  fmt->FormatSize, (ULONG)fmt->Flags);
        }

        PropertyRequest->ValueSize = sizeof(KSDATAFORMAT);
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

// ===========================================================================
//  CMiniportWaveRTStream
// ===========================================================================
class CMiniportWaveRTStream : public IMiniportWaveRTStream, public CUnknown
{
private:
    PPORTWAVERTSTREAM  m_PortStream;
    KSPIN_LOCK         m_Lock;
    PMDL               m_BufferMdl;
    PBYTE              m_Buffer;
    ULONG              m_BufferSize;
    KSSTATE            m_State;
    ULONGLONG          m_RunStart;      // 100ns
    ULONG              m_BytesPerSec;

    ULONG              m_Channels;
    ULONG              m_SampleRate;
    ULONG              m_BitsPerSample;
    BOOLEAN            m_IsFloat;
    BOOLEAN            m_MonitorOn;

public:
    DECLARE_STD_UNKNOWN_NODEL();
    IMP_IMiniportWaveRTStream;

    NTSTATUS ApplyFormat(_In_ PKSDATAFORMAT DataFormat);

    CMiniportWaveRTStream(PUNKNOWN OuterUnknown, PPORTWAVERTSTREAM PortStream)
        : CUnknown(OuterUnknown),
          m_PortStream(PortStream),
          m_BufferMdl(NULL),
          m_Buffer(NULL),
          m_BufferSize(0),
          m_State(KSSTATE_STOP),
          m_RunStart(0),
          m_BytesPerSec(48000 * 4),
          m_Channels(2),
          m_SampleRate(48000),
          m_BitsPerSample(16),
          m_IsFloat(FALSE),
          m_MonitorOn(FALSE)
    {
        KeInitializeSpinLock(&m_Lock);
    }

    ~CMiniportWaveRTStream()
    {
        //  兜底：正常路径上 FreeAudioBuffer 已经停过观察线程。
        //  只在 PASSIVE 停 —— 停线程要 KeWaitForSingleObject，DPC 里等会死锁。
        if (m_MonitorOn && KeGetCurrentIrql() == PASSIVE_LEVEL) {
            LdacPcmStop();
            m_MonitorOn = FALSE;
        }
    }

    NTSTATUS PowerNotify(ULONG State) { UNREFERENCED_PARAMETER(State); return STATUS_SUCCESS; }
};

STDMETHODIMP
CMiniportWaveRTStream::NonDelegatingQueryInterface(_In_ REFIID RefIid, _COM_Outptr_ PVOID *Object)
{
    if (IsEqualGUIDAligned(RefIid, IID_IUnknown) ||
        IsEqualGUIDAligned(RefIid, IID_IMiniportWaveRTStream)) {
        *Object = PVOID(PMINIPORTWAVERTSTREAM(this));
        ((PUNKNOWN)*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return CUnknown::NonDelegatingQueryInterface(RefIid, Object);
}

STDMETHODIMP
CMiniportWaveRTStream::SetFormat(_In_ PKSDATAFORMAT DataFormat)
{
    return ApplyFormat(DataFormat);
}

NTSTATUS
CMiniportWaveRTStream::ApplyFormat(_In_ PKSDATAFORMAT DataFormat)
{
    if (DataFormat == NULL) return STATUS_INVALID_PARAMETER;

    if (!IsEqualGUIDAligned(DataFormat->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {
        ProbeLog("stream: ApplyFormat rejected specifier");
        return STATUS_UNSUCCESSFUL;
    }

    PWAVEFORMATEX wf = (PWAVEFORMATEX)((PBYTE)DataFormat + sizeof(KSDATAFORMAT));
    if (wf->nChannels == 0 || wf->nSamplesPerSec == 0 || wf->nBlockAlign == 0) {
        return STATUS_INVALID_PARAMETER;
    }

    //  共享模式混音格式常是 WAVE_FORMAT_EXTENSIBLE(0xFFFE)，
    //  真正的采样类型在 SubFormat GUID 里，wFormatTag 本身看不出是否 float。
    BOOLEAN isFloat = FALSE;

    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        wf->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        PWAVEFORMATEXTENSIBLE wfx = (PWAVEFORMATEXTENSIBLE)wf;
        isFloat = IsEqualGUIDAligned(wfx->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)
                      ? TRUE : FALSE;
    } else if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        isFloat = TRUE;
    }

    m_Channels      = wf->nChannels;
    m_SampleRate    = wf->nSamplesPerSec;
    m_BitsPerSample = wf->wBitsPerSample;
    m_IsFloat       = isFloat;
    m_BytesPerSec   = wf->nSamplesPerSec * wf->nBlockAlign;

    ProbeLog("stream: SetFormat %uch %luhz %ubit tag=%04X float=%u blockAlign=%u",
          wf->nChannels, wf->nSamplesPerSec, wf->wBitsPerSample,
          wf->wFormatTag, (ULONG)isFloat, wf->nBlockAlign);
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportWaveRTStream::SetState(_In_ KSSTATE State)
{
    KIRQL     irql;
    ULONGLONG runStart = 0;

    KeAcquireSpinLock(&m_Lock, &irql);
    m_State = State;
    if (State == KSSTATE_RUN) {
        m_RunStart = KeQueryInterruptTime();
        runStart   = m_RunStart;
    }
    KeReleaseSpinLock(&m_Lock, irql);

    ProbeLog("stream: SetState -> %d", State);
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportWaveRTStream::GetPosition(_Out_ PKSAUDIO_POSITION Position)
{
    KIRQL irql;
    ULONG pos = 0;

    if (Position == NULL) {
        return STATUS_INVALID_PARAMETER;
    }

    //  这里可能在 DISPATCH_LEVEL 被调用（PortCls 的 DPC 取播放位置）。
    //  所以只做纯算术 + 无锁计数，绝不写日志、绝不在锁外碰 m_Buffer
    //  （FreeAudioBuffer 可能已经把 MDL 解映射了）。
    KeAcquireSpinLock(&m_Lock, &irql);
    if (m_State == KSSTATE_RUN && m_BufferSize != 0) {
        ULONGLONG elapsed = KeQueryInterruptTime() - m_RunStart;   // 100ns
        ULONGLONG bytes   = elapsed * m_BytesPerSec / 10000000ULL;
        pos = (ULONG)(bytes % m_BufferSize);
    }
    KeReleaseSpinLock(&m_Lock, irql);

    Position->PlayOffset  = pos;
    Position->WriteOffset = pos;
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportWaveRTStream::AllocateAudioBuffer(
    _In_  ULONG                RequestedSize,
    _Out_ PMDL                *AudioBufferMdl,
    _Out_ ULONG               *ActualSize,
    _Out_ ULONG               *OffsetFromFirstPage,
    _Out_ MEMORY_CACHING_TYPE *CacheType)
{
    PHYSICAL_ADDRESS low, high;

    low.QuadPart  = 0;
    high.QuadPart = MAXLONGLONG;

    if (m_BufferMdl != NULL) {
        return STATUS_UNSUCCESSFUL;     // 只分配一次
    }

    //  引擎正常情况下要几十 KB。设上限，防止 (size + PAGE_SIZE - 1) 溢出回绕
    //  成一个小数字，然后 MmAllocatePagesForMdlEx 给出一个远小于预期的缓冲区。
    if (RequestedSize == 0 || RequestedSize > (16u * 1024u * 1024u)) {
        ProbeLog("AllocateAudioBuffer: rejected RequestedSize=%lu", RequestedSize);
        return STATUS_INVALID_PARAMETER;
    }

    //  引擎默认只要 20480 字节 —— 48k/2ch/32bit 下只有 53 毫秒。
    //  环形缓冲这么小，任何"定时去抓"的人都来不及：抓之前引擎已经绕回来覆盖了。
    //  所以这里主动放大到至少 250ms，观察线程 50ms 轮询才留得下余量。
    //  WaveRT 允许 miniport 返回比请求更大的 *ActualSize（必须是整页）。
    ULONG size;
    {
        ULONG want = m_BytesPerSec / 4;             // 250ms
        if (want < RequestedSize) want = RequestedSize;
        if (want > (1u * 1024u * 1024u)) want = 1u * 1024u * 1024u;

        size = (want + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }

    //  MM_ALLOCATE_FULLY_REQUIRED：要么给满，要么失败。
    //  不带这个 flag 时 MDL 可能比 size 短，接下来 RtlZeroMemory(m_Buffer, size)
    //  就写到映射范围之外去了 —— 踩坏相邻内核 VA，症状正是随机硬挂/无 dump。
    m_BufferMdl = MmAllocatePagesForMdlEx(low, high, low, size, MmCached,
                                          MM_ALLOCATE_FULLY_REQUIRED);
    if (m_BufferMdl == NULL) {
        ProbeLog("AllocateAudioBuffer: MmAllocatePagesForMdlEx(%lu) failed", size);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //  再核一次实际到手多少字节，别信 flag 一定听话
    {
        ULONG got = MmGetMdlByteCount(m_BufferMdl);
        if (got < size) {
            ProbeLog("AllocateAudioBuffer: short MDL got=%lu want=%lu", got, size);
            MmFreePagesFromMdl(m_BufferMdl);
            IoFreeMdl(m_BufferMdl);
            m_BufferMdl = NULL;
            return STATUS_INSUFFICIENT_RESOURCES;
        }
    }

    m_Buffer = (PBYTE)MmGetSystemAddressForMdlSafe(m_BufferMdl,
                                                   NormalPagePriority | MdlMappingNoExecute);
    if (m_Buffer == NULL) {
        //  MmAllocatePagesForMdlEx 建出来的 MDL 只能用 IoFreeMdl 释放
        MmFreePagesFromMdl(m_BufferMdl);
        IoFreeMdl(m_BufferMdl);
        m_BufferMdl = NULL;
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    RtlZeroMemory(m_Buffer, size);
    m_BufferSize = size;

    *AudioBufferMdl      = m_BufferMdl;
    *ActualSize          = size;
    *OffsetFromFirstPage = 0;
    *CacheType           = MmCached;

    //  记下映射地址：万一观察线程读到 unmapped，日志里能直接对上故障地址
    ProbeLog("AllocateAudioBuffer: req=%lu actual=%lu mdl=%p va=%p",
          RequestedSize, size, m_BufferMdl, m_Buffer);

    //  起 PCM 观察线程：它按 m_BytesPerSec 推算引擎写指针，读最近一小段算电平。
    //  失败不影响播放，只是看不到电平。
    {
        NTSTATUS mst = LdacPcmStart(m_Buffer, m_BufferSize, m_BytesPerSec,
                                  m_Channels, m_BitsPerSample, m_IsFloat);
        m_MonitorOn = NT_SUCCESS(mst) ? TRUE : FALSE;
        ProbeLog("pcm monitor start -> 0x%08X (on=%u)", mst, (ULONG)m_MonitorOn);
    }

    return STATUS_SUCCESS;
}

STDMETHODIMP_(VOID)
CMiniportWaveRTStream::FreeAudioBuffer(_In_opt_ PMDL AudioBufferMdl, _In_ ULONG BufferSize)
{
    //  这一行是关键的判据：如果崩溃前日志里出现过它，
    //  就说明缓冲区是被这里解映射的，而不是观察线程自己越界。
    ProbeLog("FreeAudioBuffer: enter  mdl=%p va=%p size=%lu  bufSizeArg=%lu monitor=%u",
          AudioBufferMdl, m_Buffer, m_BufferSize, BufferSize, (ULONG)m_MonitorOn);

    UNREFERENCED_PARAMETER(BufferSize);

    //  必须先停观察线程，否则它还在读 m_Buffer 这个内核映射，
    //  下面 MmUnmapLockedPages / IoFreeMdl 之后就是 use-after-free。
    if (m_MonitorOn) {
        BOOLEAN stopped = LdacPcmStop();
        m_MonitorOn = FALSE;
        if (!stopped) {
            //  线程没停干净。缓冲区绝对不能释放 —— 泄漏一个几十 KB 的
            //  MDL 换一次不蓝屏，这笔账很划算。设备下次枚举会重新分配。
            ProbeLog("!!! FreeAudioBuffer: monitor still running, leaking buffer (%lu bytes)",
                  m_BufferSize);
            m_BufferMdl  = NULL;
            m_Buffer     = NULL;
            m_BufferSize = 0;
            return;
        }
    }

    if (AudioBufferMdl != NULL) {
        if (m_Buffer != NULL) {
            MmUnmapLockedPages(m_Buffer, AudioBufferMdl);
        }
        MmFreePagesFromMdl(AudioBufferMdl);
        IoFreeMdl(AudioBufferMdl);      // 不是 ExFreePool
    }
    m_BufferMdl  = NULL;
    m_Buffer     = NULL;
    m_BufferSize = 0;
    ProbeLog("FreeAudioBuffer: done (mapping released)");
}

STDMETHODIMP_(VOID)
CMiniportWaveRTStream::GetHWLatency(_Out_ KSRTAUDIO_HWLATENCY *hwLatency)
{
    hwLatency->FifoSize     = 0;
    hwLatency->ChipsetDelay = 0;
    hwLatency->CodecDelay   = 0;
}

STDMETHODIMP
CMiniportWaveRTStream::GetPositionRegister(_Out_ KSRTAUDIO_HWREGISTER *Register)
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_IMPLEMENTED;      // 走 GetPosition 轮询路径
}

STDMETHODIMP
CMiniportWaveRTStream::GetClockRegister(_Out_ KSRTAUDIO_HWREGISTER *Register)
{
    UNREFERENCED_PARAMETER(Register);
    return STATUS_NOT_IMPLEMENTED;
}

// ===========================================================================
//  CMiniportWaveRT
// ===========================================================================
class CMiniportWaveRT : public IMiniportWaveRT, public CUnknown
{
private:
    PPORTWAVERT m_Port;

public:
    DECLARE_STD_UNKNOWN_NODEL();
    IMP_IMiniportWaveRT;

    CMiniportWaveRT(PUNKNOWN OuterUnknown)
        : CUnknown(OuterUnknown), m_Port(NULL) {}

    ~CMiniportWaveRT() {}
};

STDMETHODIMP
CMiniportWaveRT::NonDelegatingQueryInterface(_In_ REFIID RefIid, _COM_Outptr_ PVOID *Object)
{
    if (IsEqualGUIDAligned(RefIid, IID_IUnknown) ||
        IsEqualGUIDAligned(RefIid, IID_IMiniportWaveRT)) {
        *Object = PVOID(PMINIPORTWAVERT(this));
        ((PUNKNOWN)*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return CUnknown::NonDelegatingQueryInterface(RefIid, Object);
}

STDMETHODIMP
CMiniportWaveRT::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &g_WaveFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportWaveRT::DataRangeIntersection(
    _In_  ULONG        PinId,
    _In_  PKSDATARANGE DataRange,
    _In_  PKSDATARANGE MatchingDataRange,
    _In_  ULONG        OutputBufferLength,
    _Out_writes_bytes_to_opt_(OutputBufferLength, *ResultantFormatLength)
          PVOID        ResultantFormat,
    _Out_ PULONG       ResultantFormatLength)
{
    UNREFERENCED_PARAMETER(PinId);
    UNREFERENCED_PARAMETER(DataRange);

    ULONG needed = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    *ResultantFormatLength = needed;

    //  PortCls 先传 OutputBufferLength=0 问需要多大，这时必须回
    //  STATUS_BUFFER_TOO_SMALL；回 INVALID_PARAMETER 会让上层认为
    //  这个 pin 没有可用格式，端点就打不开了。
    if (ResultantFormat == NULL || OutputBufferLength < needed) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    //  默认 48k/16bit/2ch，但对端指定了就尽量照办，
    //  否则共享模式会因为「回了一个不一样的格式」而失败。
    WORD  ch      = 2;
    DWORD rate    = 48000;
    WORD  bits    = 16;
    BOOLEAN isFloat = FALSE;

    if (MatchingDataRange != NULL &&
        MatchingDataRange->FormatSize >= sizeof(KSDATAFORMAT_WAVEFORMATEX) &&
        IsEqualGUIDAligned(MatchingDataRange->Specifier, KSDATAFORMAT_SPECIFIER_WAVEFORMATEX)) {

        if (IsEqualGUIDAligned(MatchingDataRange->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            isFloat = TRUE;
            bits    = 32;
        }

        PWAVEFORMATEX req = (PWAVEFORMATEX)((PBYTE)MatchingDataRange + sizeof(KSDATAFORMAT));
        if (req->nChannels >= 1 && req->nChannels <= 2) {
            ch = req->nChannels;
        }
        if (req->nSamplesPerSec >= 44100 && req->nSamplesPerSec <= 96000) {
            rate = req->nSamplesPerSec;
        }
        if (!isFloat && req->wBitsPerSample == 16) {
            bits = 16;
        }
    }

    PKSDATAFORMAT_WAVEFORMATEX fmt = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
    RtlZeroMemory(fmt, needed);

    fmt->DataFormat.FormatSize  = needed;
    fmt->DataFormat.Flags       = 0;
    fmt->DataFormat.SampleSize  = (WORD)(ch * bits / 8);
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat   = isFloat ? KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
                                          : KSDATAFORMAT_SUBTYPE_PCM;
    fmt->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;

    fmt->WaveFormatEx.wFormatTag      = isFloat ? WAVE_FORMAT_IEEE_FLOAT : WAVE_FORMAT_PCM;
    fmt->WaveFormatEx.nChannels       = ch;
    fmt->WaveFormatEx.nSamplesPerSec  = rate;
    fmt->WaveFormatEx.wBitsPerSample  = bits;
    fmt->WaveFormatEx.nBlockAlign     = (WORD)(ch * bits / 8);
    fmt->WaveFormatEx.nAvgBytesPerSec = rate * fmt->WaveFormatEx.nBlockAlign;
    fmt->WaveFormatEx.cbSize          = 0;

    ProbeLog("DataRangeIntersection: %uch %luhz %ubit float=%u", ch, rate, bits, (ULONG)isFloat);
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportWaveRT::Init(
    _In_ PUNKNOWN      UnknownAdapter,
    _In_ PRESOURCELIST ResourceList,
    _In_ PPORTWAVERT   Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);

    m_Port = Port;
    ProbeLog("wave: Init()");
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportWaveRT::NewStream(
    _Out_ PMINIPORTWAVERTSTREAM *Stream,
    _In_  PPORTWAVERTSTREAM      PortStream,
    _In_  ULONG                  Pin,
    _In_  BOOLEAN                Capture,
    _In_  PKSDATAFORMAT          DataFormat)
{
    ProbeLog("wave: NewStream pin=%lu capture=%u", Pin, (ULONG)Capture);

    if (Pin != 0 || Capture) {
        return STATUS_INVALID_PARAMETER;    // 只有 render pin 0
    }

    CMiniportWaveRTStream *s =
        new (NonPagedPoolNx, LDAC_POOL_TAG) CMiniportWaveRTStream(NULL, PortStream);
    if (s == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //  设备格式从这里进来（PortCls 不调 SetFormat）。不记下来的话，
    //  AllocateAudioBuffer 起观察线程时用的是构造函数默认值。
    if (DataFormat != NULL) {
        NTSTATUS fst = s->ApplyFormat(DataFormat);
        ProbeLog("wave: NewStream ApplyFormat -> 0x%08X", fst);
    }

    s->AddRef();
    *Stream = s;
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportWaveRT::GetDeviceDescription(_Out_ PDEVICE_DESCRIPTION DeviceDescription)
{
    RtlZeroMemory(DeviceDescription, sizeof(*DeviceDescription));
    DeviceDescription->Master             = TRUE;
    DeviceDescription->ScatterGather      = FALSE;
    DeviceDescription->Dma32BitAddresses  = TRUE;
    DeviceDescription->InterfaceType      = PCIBus;
    DeviceDescription->MaximumLength      = 0xFFFFFFFF;
    return STATUS_SUCCESS;
}

NTSTATUS
NewMiniportWaveRT(_Out_ PUNKNOWN *Out)
{
    CMiniportWaveRT *p = new (NonPagedPoolNx, LDAC_POOL_TAG) CMiniportWaveRT(NULL);
    if (p == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    p->AddRef();
    *Out = p;
    return STATUS_SUCCESS;
}
