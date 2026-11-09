// 用户态 shim：让内核源码可以在测试里编译
// 只补内核专有的那几个东西，类型全部复用 windows.h（UCHAR/USHORT/ULONG/BOOLEAN/NTSTATUS 都在 winnt.h）
#pragma once

#include <windows.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifndef RtlZeroMemory
#define RtlZeroMemory(d, l) memset((d), 0, (l))
#endif
#ifndef RtlCopyMemory
#define RtlCopyMemory(d, s, l) memcpy((d), (s), (l))
#endif
#ifndef NT_SUCCESS
#define NT_SUCCESS(s) (((NTSTATUS)(s)) >= 0)
#endif
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_UNSUCCESSFUL
#define STATUS_UNSUCCESSFUL ((NTSTATUS)0xC0000001L)
#endif
#ifndef STATUS_INVALID_PARAMETER
#define STATUS_INVALID_PARAMETER ((NTSTATUS)0xC000000DL)
#endif
#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#endif
#ifndef STATUS_DEVICE_NOT_READY
#define STATUS_DEVICE_NOT_READY ((NTSTATUS)0xC00000A3L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_DATA_ERROR
#define STATUS_DATA_ERROR ((NTSTATUS)0xC000003EL)
#endif
#ifndef STATUS_IO_TIMEOUT
#define STATUS_IO_TIMEOUT ((NTSTATUS)0xC00000B5L)
#endif
#ifndef STATUS_NOT_SUPPORTED
#define STATUS_NOT_SUPPORTED ((NTSTATUS)0xC00000BBL)
#endif
#ifndef STATUS_INVALID_BUFFER_SIZE
#define STATUS_INVALID_BUFFER_SIZE ((NTSTATUS)0xC0000206L)
#endif

// SAL 注解：用户态下 sal.h 不一定被引入，这里补成空宏（内核里它们也是空展开）
#ifndef _In_
#define _In_
#define _In_opt_
#define _Out_
#define _Out_opt_
#define _Inout_
#define _Inout_opt_
#define _In_reads_(n)
#define _In_reads_opt_(n)
#define _In_reads_bytes_(n)
#define _Out_writes_(n)
#define _Out_writes_opt_(n)
#define _Out_writes_bytes_(n)
#define _Out_writes_bytes_to_opt_(a, b)
#define _Outptr_
#define _COM_Outptr_
#endif

// 内核里的 PAGED_CODE / 断言在用户态是空的
#define PAGED_CODE()
#define NT_ASSERT(x) ((void)0)

// 内核独有类型：测试桩只要能用指针就够了
typedef struct _DEVICE_OBJECT      *PDEVICE_OBJECT;
typedef struct _DRIVER_OBJECT      *PDRIVER_OBJECT;
typedef struct _IRP                *PIRP;
typedef struct _UNICODE_STRING      UNICODE_STRING, *PUNICODE_STRING;
typedef void                       *PVOID;
