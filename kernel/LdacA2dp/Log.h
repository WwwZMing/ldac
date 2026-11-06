// ============================================================================
//  Log.h — 内核日志（无 CRT 依赖）
// ============================================================================
#pragma once

#include <ntddk.h>

// KsAudio.c 按 C++ 编译（ks.h 的 GUID 声明需要 C++ 语义），Log.c 按 C 编译。
// 不加 extern "C" 的话 C++ 那边会去找 ?ProbeLog@@YAXPEBDZZ 这种修饰名 → LNK2019。
#ifdef __cplusplus
extern "C" {
#endif

VOID
ProbeLog(_In_ PCSTR fmt, ...);

//  Progress marker that survives a bugcheck even when file *data* does not.
//
//  Writing to probe.log proved unreliable: after a crash the file exists with
//  the right length but contains 0x00 bytes, because the file length metadata
//  reaches disk while the buffered data does not - FILE_WRITE_THROUGH or not.
//  Creating a *separate short file* per milestone gets around this: the name is
//  a directory entry, and NTFS journals that synchronously. So after a crash,
//  listing C:\Users\Public\ldac-mark-*.txt tells us exactly how far the driver
//  got, purely from which files exist.
VOID
ProbeMark(_In_ PCSTR tag);

#ifdef __cplusplus
}
#endif
