// ============================================================================
//  MiniportTopology.cpp - 最小拓扑 miniport
// ============================================================================
//  作用：让音频端点在「声音」面板里成立。PortCls 要求 render 设备有一个
//  Topology 子设备，并通过 PcRegisterPhysicalConnection 与 Wave 子设备连起来。
//
//  这里给 2 个桥接引脚（in from wave / out to speaker），0 个节点 —— 纯直通。
//  桥接引脚的三条规矩（对照 sysvad/EndpointsCommon/speakertoptable.h）：
//    1) KSPIN_COMMUNICATION_NONE（不是 BRIDGE/SINK/SOURCE）
//    2) 数据范围是 ANALOG + SPECIFIER_NONE 的 KSDATARANGE，不是 PCM
//    3) 面向设备那个引脚 Category 必须是 KSNODETYPE_SPEAKER
//       —— AudioEndpointBuilder 靠它判断「这个桥接引脚代表一个扬声器端点」
// ============================================================================
#include "LdacKs.h"
#include <ksmedia.h>     // KSPROPSETID_Jack / KSJACK_DESCRIPTION

//  本地 GUID 常量：不依赖 ks.lib 导出符号，取地址也安全
//  （STATIC_XXX 是裸值列表，必须自己补花括号才成为一个 GUID 对象）
static const GUID g_GuidCatAudio    = { STATIC_KSCATEGORY_AUDIO };
static const GUID g_GuidCatTopology = { STATIC_KSCATEGORY_TOPOLOGY };
static const GUID g_GuidNodeSpeaker = { STATIC_KSNODETYPE_SPEAKER };

static KSDATARANGE g_TopoBridgeRange = {
    sizeof(KSDATARANGE),
    0,
    0,
    0,
    STATIC_KSDATAFORMAT_TYPE_AUDIO,
    STATIC_KSDATAFORMAT_SUBTYPE_ANALOG,
    STATIC_KSDATAFORMAT_SPECIFIER_NONE
};

static PKSDATARANGE g_TopoBridgeRangePtrs[] = { &g_TopoBridgeRange };

static PCPIN_DESCRIPTOR g_TopoPins[] = {
    // pin 0: KSPIN_TOPO_WAVEOUT_SOURCE —— 从 Wave 子设备接过来
    {
        0, 0, 0,
        NULL,
        {
            0, NULL,                              // Interfaces
            0, NULL,                              // Mediums
            SIZEOF_ARRAY(g_TopoBridgeRangePtrs),  // DataRangesCount
            g_TopoBridgeRangePtrs,                // DataRanges
            KSPIN_DATAFLOW_IN,                    // DataFlow
            KSPIN_COMMUNICATION_NONE,             // Communication
            (GUID *)&g_GuidCatAudio,              // Category
            NULL,                                 // Name
            0                                     // Reserved
        }
    },
    // pin 1: KSPIN_TOPO_LINEOUT_DEST —— 物理扬声器端点
    {
        0, 0, 0,
        NULL,
        {
            0, NULL,
            0, NULL,
            SIZEOF_ARRAY(g_TopoBridgeRangePtrs),
            g_TopoBridgeRangePtrs,
            KSPIN_DATAFLOW_OUT,
            KSPIN_COMMUNICATION_NONE,
            (GUID *)&g_GuidNodeSpeaker,
            NULL,
            0
        }
    }
};

static PCCONNECTION_DESCRIPTOR g_TopoConnections[] = {
    { PCFILTER_NODE, 0, PCFILTER_NODE, 1 }
};

static const GUID g_TopoCategories[] = {
    { STATIC_KSCATEGORY_AUDIO },
    { STATIC_KSCATEGORY_TOPOLOGY }
};

// ---------------------------------------------------------------------------
//  插孔在位状态（KSPROPERTY_JACK_DESCRIPTION.IsConnected）
// ---------------------------------------------------------------------------
//  Windows 表达"设备还在、但链路不在了"的标准方式是 jack presence，
//  不是注销子设备。注销子设备会撞 PortCls 的 filter 创建路径（0x3B，
//  详见 docs/lifecycle-audit.md）；而回答一个属性查询不会跟任何人抢资源。
//
//  为什么挂在 pin 1：我们的拓扑只有两个桥接引脚，
//    pin 0 = 从 Wave 接过来（KSPIN_TOPO_WAVEOUT_SOURCE）
//    pin 1 = 对外扬声器（KSNODETYPE_SPEAKER，AudioEndpointBuilder 认的就是它）
//  jack 描述属于"对外的那个插孔"，所以只有 pin 1 有。
static LONG g_LastJackQuery = -1;   // 只在状态变化时打一条日志，避免刷屏

//  KSPROPERTY_JACK_DESCRIPTION 的标准应答形态：
//    KSMULTIPLE_ITEM{ Size, Count=1 } + KSJACK_DESCRIPTION[Count]
static NTSTATUS
PropertyHandler_JackDescription(_In_ PPCPROPERTY_REQUEST Request)
{
    ULONG cbNeeded = sizeof(KSMULTIPLE_ITEM) + sizeof(KSJACK_DESCRIPTION);
    ULONG pinId;

    //  只有 pin 1（扬声器桥）有插孔
    if (Request->Instance == NULL || Request->InstanceSize < sizeof(ULONG)) {
        return STATUS_INVALID_PARAMETER;
    }
    pinId = *(PULONG)Request->Instance;
    if (pinId != 1) return STATUS_INVALID_DEVICE_REQUEST;

    if (Request->Verb & KSPROPERTY_TYPE_BASICSUPPORT) {
        PKSPROPERTY_DESCRIPTION desc;

        if (Request->ValueSize == 0) {
            Request->ValueSize = sizeof(KSPROPERTY_DESCRIPTION);
            return STATUS_BUFFER_OVERFLOW;
        }
        if (Request->ValueSize < sizeof(KSPROPERTY_DESCRIPTION)) {
            return STATUS_BUFFER_TOO_SMALL;
        }
        desc = (PKSPROPERTY_DESCRIPTION)Request->Value;
        desc->AccessFlags     = KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT;
        desc->DescriptionSize = sizeof(KSPROPERTY_DESCRIPTION);
        desc->PropTypeSet.Set   = GUID_NULL;
        desc->PropTypeSet.Id    = 0;
        desc->PropTypeSet.Flags = 0;
        desc->MembersListCount  = 0;
        desc->Reserved          = 0;
        return STATUS_SUCCESS;
    }

    if (Request->ValueSize == 0) {
        Request->ValueSize = cbNeeded;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (Request->ValueSize < cbNeeded) return STATUS_BUFFER_TOO_SMALL;

    if (Request->Verb & KSPROPERTY_TYPE_GET) {
        PKSMULTIPLE_ITEM    multi = (PKSMULTIPLE_ITEM)Request->Value;
        PKSJACK_DESCRIPTION jack  = (PKSJACK_DESCRIPTION)(multi + 1);

        multi->Size  = cbNeeded;
        multi->Count = 1;

        RtlZeroMemory(jack, sizeof(*jack));
        jack->ChannelMapping  = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        jack->Color           = 0;
        jack->ConnectionType  = eConnTypeUnknown;
        jack->GeoLocation     = eGeoLocBottom;
        jack->GenLocation     = eGenLocPrimaryBox;
        jack->PortConnection  = ePortConnJack;
        jack->IsConnected     = LdacKsIsLinkPresent();
        if ((LONG)jack->IsConnected != g_LastJackQuery) {
            g_LastJackQuery = (LONG)jack->IsConnected;
            ProbeLog("jack: system queried -> IsConnected=%lu", (ULONG)jack->IsConnected);
        }
        return STATUS_SUCCESS;
    }

    return STATUS_INVALID_DEVICE_REQUEST;
}

static NTSTATUS
PropertyHandler_TopoFilter(_In_ PPCPROPERTY_REQUEST Request)
{
    if (IsEqualGUIDAligned(*Request->PropertyItem->Set, KSPROPSETID_Jack) &&
        Request->PropertyItem->Id == KSPROPERTY_JACK_DESCRIPTION) {
        return PropertyHandler_JackDescription(Request);
    }
    return STATUS_INVALID_DEVICE_REQUEST;
}

static PCPROPERTY_ITEM g_TopoFilterProperties[] = {
    {
        &KSPROPSETID_Jack,
        KSPROPERTY_JACK_DESCRIPTION,
        KSPROPERTY_TYPE_GET | KSPROPERTY_TYPE_BASICSUPPORT,
        PropertyHandler_TopoFilter
    }
};

DEFINE_PCAUTOMATION_TABLE_PROP(g_TopoAutomation, g_TopoFilterProperties);
static PCFILTER_DESCRIPTOR g_TopoFilterDescriptor = {
    0,                                   // Version
    &g_TopoAutomation,                   // AutomationTable
    sizeof(PCPIN_DESCRIPTOR),            // PinSize
    SIZEOF_ARRAY(g_TopoPins),            // PinCount
    g_TopoPins,                          // Pins
    sizeof(PCNODE_DESCRIPTOR),           // NodeSize
    0, NULL,                             // 没有节点
    SIZEOF_ARRAY(g_TopoConnections),     // ConnectionCount
    g_TopoConnections,                   // Connections
    SIZEOF_ARRAY(g_TopoCategories),      // CategoryCount
    (GUID *)g_TopoCategories              // Categories
};

class CMiniportTopology : public IMiniportTopology, public CUnknown
{
private:
    PPORTTOPOLOGY m_Port;

public:
    DECLARE_STD_UNKNOWN_NODEL();
    IMP_IMiniportTopology;

    CMiniportTopology(PUNKNOWN OuterUnknown)
        : CUnknown(OuterUnknown), m_Port(NULL) {}

    ~CMiniportTopology() {}
};

STDMETHODIMP
CMiniportTopology::NonDelegatingQueryInterface(_In_ REFIID RefIid, _COM_Outptr_ PVOID *Object)
{
    if (IsEqualGUIDAligned(RefIid, IID_IUnknown) ||
        IsEqualGUIDAligned(RefIid, IID_IMiniportTopology)) {
        *Object = PVOID(PMINIPORTTOPOLOGY(this));
        ((PUNKNOWN)*Object)->AddRef();
        return STATUS_SUCCESS;
    }
    return CUnknown::NonDelegatingQueryInterface(RefIid, Object);
}

STDMETHODIMP
CMiniportTopology::GetDescription(_Out_ PPCFILTER_DESCRIPTOR *Description)
{
    *Description = &g_TopoFilterDescriptor;
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportTopology::DataRangeIntersection(
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
    UNREFERENCED_PARAMETER(MatchingDataRange);

    *ResultantFormatLength = sizeof(KSDATAFORMAT_WAVEFORMATEX);

    if (ResultantFormat == NULL) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    if (OutputBufferLength < sizeof(KSDATAFORMAT_WAVEFORMATEX)) {
        return STATUS_BUFFER_TOO_SMALL;
    }

    PKSDATAFORMAT_WAVEFORMATEX fmt = (PKSDATAFORMAT_WAVEFORMATEX)ResultantFormat;
    RtlZeroMemory(fmt, sizeof(*fmt));
    fmt->DataFormat.FormatSize  = sizeof(KSDATAFORMAT_WAVEFORMATEX);
    fmt->DataFormat.SampleSize  = 4;
    fmt->DataFormat.MajorFormat = KSDATAFORMAT_TYPE_AUDIO;
    fmt->DataFormat.SubFormat   = KSDATAFORMAT_SUBTYPE_PCM;
    fmt->DataFormat.Specifier   = KSDATAFORMAT_SPECIFIER_WAVEFORMATEX;
    fmt->WaveFormatEx.wFormatTag      = WAVE_FORMAT_PCM;
    fmt->WaveFormatEx.nChannels       = 2;
    fmt->WaveFormatEx.nSamplesPerSec  = 48000;
    fmt->WaveFormatEx.wBitsPerSample  = 16;
    fmt->WaveFormatEx.nBlockAlign     = 4;
    fmt->WaveFormatEx.nAvgBytesPerSec = 48000 * 4;
    return STATUS_SUCCESS;
}

STDMETHODIMP
CMiniportTopology::Init(
    _In_ PUNKNOWN       UnknownAdapter,
    _In_ PRESOURCELIST  ResourceList,
    _In_ PPORTTOPOLOGY  Port)
{
    UNREFERENCED_PARAMETER(UnknownAdapter);
    UNREFERENCED_PARAMETER(ResourceList);

    m_Port = Port;
    ProbeLog("topo: Init()");
    return STATUS_SUCCESS;
}

NTSTATUS
NewMiniportTopology(_Out_ PUNKNOWN *Out)
{
    CMiniportTopology *p = new (NonPagedPoolNx, LDAC_POOL_TAG) CMiniportTopology(NULL);
    if (p == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    p->AddRef();
    *Out = p;
    return STATUS_SUCCESS;
}
