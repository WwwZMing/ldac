// ============================================================================
//  LdacKs.h - LDAC A2DP 的 PortCls 音频端点（KS 层）共享声明
// ============================================================================
//  这套结构是从玩具 `toy/VirtualSpeaker/` 原样搬过来的（2026-11-02 在
//  Win11 26300 上全程验证通过），解决的老病根是：
//
//    顶替 btha2dp.sys 之后，**没人再给耳机建音频端点** —— 设备管理器里
//    一片黄叹号，音频端点消失，于是"配对成功"永远等不到。
//
//  PortCls 端点就是补这个洞：只要是功能驱动，不管上层是不是蓝牙，
//  都能通过 PcRegisterSubdevice 建出 KSCATEGORY_AUDIO/RENDER 子设备，
//  AudioEndpointBuilder 就会照常生成端点。
//
//  数据流：音频引擎 -> Wave pin0(流) -> DAC node -> Wave pin1(桥接)
//          -> PhysConn -> Topology pin0 -> Topology pin1(扬声器)
//          -> 观察线程把 PCM 抓出来交给用户态编码
// ============================================================================
#pragma once

#include <ntddk.h>
#include <portcls.h>
#include <stdunk.h>
#include <ksmedia.h>

#include "Log.h"

// ---------------------------------------------------------------------------
//  ks.h 里的 STATIC_XXX 宏是「裸值列表」（没有外层花括号）：
//      #define STATIC_KSCATEGORY_AUDIO  0x6994ad04L, 0x93ef, ...
//  它只能配 DEFINE_GUID，或者塞进聚合初始化里扁平展开。
//  要拿到一个 GUID 常量对象，必须自己补花括号：{ STATIC_XXX }
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
//  COM 三件套：不要用 stdunk.h 的 DECLARE_STD_UNKNOWN()
//
//  那个宏把 QueryInterface/AddRef/Release 委托给 GetOuterUnknown()，
//  而简单 miniport 构造时 outer 传 NULL（不做聚合）→ 空指针调用 → 0x7E。
//  改成 non-delegating：直接落到 CUnknown.cpp 的实现上。
// ---------------------------------------------------------------------------
#define DECLARE_STD_UNKNOWN_NODEL()                                            \
    STDMETHODIMP NonDelegatingQueryInterface(_In_ REFIID riid,                 \
                                             _COM_Outptr_ void **ppv);         \
    STDMETHODIMP QueryInterface(_In_ REFIID riid, _COM_Outptr_ void **ppv)     \
    {                                                                          \
        return NonDelegatingQueryInterface(riid, ppv);                         \
    }                                                                          \
    STDMETHODIMP_(ULONG) AddRef()  { return NonDelegatingAddRef(); }           \
    STDMETHODIMP_(ULONG) Release() { return NonDelegatingRelease(); }

#define LDAC_POOL_TAG     'yTSV'      // 'VSTy'
#define LDAC_MAX_SUBDEV   2           // Wave + Topology

// ---------------------------------------------------------------------------
//  PCM 观察器编译开关（默认关）
//
//  它会在缓冲区上跑一个自己的系统线程读电平。这是整个玩具里唯一
//  异步碰音频缓冲区的代码，也是最可疑的崩溃源，所以做成开关：
//      默认 0 -> 只证明「端点 + PCM 通路」，不碰缓冲
//      置 1   -> 加上电平观察
//  由 build-toy.ps1 -Monitor 覆盖（msbuild /p:VsPcmMonitor=1）。
// ---------------------------------------------------------------------------
#ifndef LDAC_PCM_MONITOR
#define LDAC_PCM_MONITOR  0
#endif

// 适配器设备扩展
//
// ⚠ 这份结构必须是"每实例一份"，绝不能退回全局变量。
//   实测：devcon install 每跑一次就多一个 ROOT\MEDIA\NNNN 设备实例，
//   用全局 g_Started 守卫的话第二个实例直接 STATUS_SUCCESS 返回、
//   却一个 subdevice 都没注册 —— 设备管理器里一堆黄色叹号，
//   音频端点变成 CM_PROB_PHANTOM。
//   PcAddAdapterDevice 的最后一个参数就是扩展大小，PortCls 会替我们分配。
typedef struct _VS_ADAPTER_EXT {
    PDEVICE_OBJECT  DeviceObject;
    PUNKNOWN        WaveUnknown;
    PUNKNOWN        TopoUnknown;
    PPORT           WavePort;
    PPORT           TopoPort;
    volatile LONG   Started;
} LDAC_ADAPTER_EXT, *PLDAC_ADAPTER_EXT;

// ---------------------------------------------------------------------------
//  PcAddAdapterDevice 的 DeviceExtensionSize 有个隐藏下限
//
//  反汇编 portcls.sys（Win11 26300，PcAddAdapterDevice @ RVA 0x64B60）：
//
//      mov  ebx, [rbp+0x7f]      ; 第 5 参 DeviceExtensionSize
//      test ebx, ebx
//      jne  .chk
//      mov  ebx, 0x1a8           ; 传 0 -> 用默认 0x1A8
//  .chk:
//      cmp  ebx, 0x1a8
//      jb   STATUS_INVALID_PARAMETER (0xC000000D)
//
//  前 0x1A8 字节是 PortCls 自己的 ADAPTER_DEVICE_EXTENSION
//  （同一个函数里一路写到 [rdi+0x134]，别去抢）。
//  结论：想要私有扩展，必须传 0x1A8 + sizeof(自己)，数据放在 0x1A8 之后。
//  踩坑记录：直接传 sizeof(LDAC_ADAPTER_EXT)=48 得到的是 0xC000000D。
// ---------------------------------------------------------------------------
#define LDAC_PC_EXT_SIZE  0x1A8
#define LDAC_EXT_TOTAL            (LDAC_PC_EXT_SIZE + sizeof(LDAC_ADAPTER_EXT))
#define LDAC_EXT_FROM_DEVOBJ(d)   \
    ((PLDAC_ADAPTER_EXT)((PBYTE)((d)->DeviceExtension) + LDAC_PC_EXT_SIZE))

// 两个 miniport 的工厂
NTSTATUS NewMiniportWaveRT(_Out_ PUNKNOWN *Out);
NTSTATUS NewMiniportTopology(_Out_ PUNKNOWN *Out);

// ---------------------------------------------------------------------------
//  PortCls 侧对 Driver.c 暴露的接口
// ---------------------------------------------------------------------------
//  声明在 LdacBridge.h（纯 C 头），这里放在 extern "C" 里引入，
//  这样 C++ 编译出来的符号名不修饰，C 那边能直接链上。
extern "C" {
#include "LdacBridge.h"
}

// ---------------------------------------------------------------------------
//  PCM 观察器（定义在 Adapter.cpp）
//  WaveRT stream 分配好缓冲区后启动，释放缓冲区前必须 Stop。
//  线程只在 PASSIVE_LEVEL 跑，只读缓冲区 + 写日志，不碰任何 PortCls 对象。
// ---------------------------------------------------------------------------
extern "C" NTSTATUS  LdacPcmStart(_In_ PVOID Buffer, _In_ ULONG Size,
                                _In_ ULONG BytesPerSec, _In_ ULONG Channels,
                                _In_ ULONG BitsPerSample, _In_ BOOLEAN IsFloat);
//  返回 TRUE = 线程确定已退出，缓冲区可以安全解映射；
//  FALSE = 没能停干净，调用方**必须**放弃释放缓冲区（宁可泄漏也不能 UAF）。
extern "C" BOOLEAN   LdacPcmStop(VOID);
