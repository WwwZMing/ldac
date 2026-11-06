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
