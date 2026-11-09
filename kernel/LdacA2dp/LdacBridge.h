// ============================================================================
//  LdacBridge.h - Driver.c (C) 与 KsAdapter.cpp (C++) 之间的窄接口
// ============================================================================
//  为什么单独开一个头：
//    - Driver.c 按 **C** 编译（要和 Avdtp.c / BthL2cap.c 一致，它们都是 C）
//    - KsAdapter.cpp 按 **C++** 编译（PortCls 的 IMP_* 宏和 miniport 类要 C++）
//    C 编译器不认 `extern "C"`，所以桥接声明放这里（纯 C、无 portcls/stdunk），
//    由 LdacKs.h 在 `extern "C" { ... }` 里 include，符号名才对得上。
// ============================================================================
#pragma once

#include <ntddk.h>

#ifdef __cplusplus
extern "C" {
#endif

//  PortCls 的 AddDevice 回调（实现在 KsAdapter.cpp）。
//  DriverEntry 在 Driver.c 里，把它传给 PcInitializeAdapterDriver。
NTSTATUS
LdacKsAddDevice(
    _In_ PDRIVER_OBJECT DriverObject,
    _In_ PDEVICE_OBJECT Pdo);

//  BTHENUM 的 A2DP 子设备 PDO。KsAdapter 的 AddDevice 填，
//  Driver.c 的 BRB/L2CAP 拿它去 QUERY_INTERFACE 取 BDIF。
extern PDEVICE_OBJECT g_LdacBthPdo;

//  挂 BRB 接口 + 注册 L2CAP 服务端（实现在 Driver.c）。
VOID
LdacBthBoot(
    _In_ PDEVICE_OBJECT Pdo);

//  链路在位状态：由 Driver.c 在"设备连上/断开"时更新，
//  由 MiniportTopology 通过 KSPROPERTY_JACK_DESCRIPTION.IsConnected 汇报给系统。
//  （2026-11-09：不再用它驱动子设备注册/注销。）
VOID
LdacKsSetLinkPresent(_In_ BOOLEAN Present);

//  设备节点到达/移除：安装或拆除端点（jack 只管链路状态，不管端点）。
VOID
LdacKsSetDevicePresent(_In_ BOOLEAN Present);

//  PnP 移除时调用：把当前端点整个拆掉并清掉全局引用。
//  （只做清理；不在这里做任何注册。）
VOID
LdacKsOnDeviceRemove(VOID);

#ifdef __cplusplus
}
#endif