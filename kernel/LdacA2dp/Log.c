// ============================================================================
//  Log.c — 内核日志：DbgPrint + 追加写 \SystemRoot\Temp\ldac-probe.log
//  内核没有 CRT：ntstrsafe 的 *Printf* 会解析到 UCRT 的 __stdio_common_vsprintf
//  导致 LNK2019，所以这里自带一个最小格式化器（%s %c %d %u %x %X，支持宽度/补零）。
// ============================================================================
#include "Log.h"
#include <stdarg.h>     // 内核里没人帮你带 va_list

//  日志落 C:\Users\Public：之前写 \SystemRoot\Temp 属于 Windows\Temp，
//  ACL 只给管理员，非提权读不了（实测 Test-Path 直接被拒）。
//  Public 目录创建的普通文件默认对 Users 可读，省掉每轮迭代都要提权看日志。
#define LOG_PATH L"\\??\\C:\\Users\\Public\\ldac-probe.log"

static SIZE_T
KStrLen(_In_opt_ PCSTR s)
{
    SIZE_T n = 0;
    if (s == NULL) return 0;
    while (s[n]) n++;
    return n;
}

static VOID
KPutNum(_Inout_ PCHAR* p, _In_ PCHAR end, _In_ ULONG_PTR v, _In_ ULONG base,
        _In_ BOOLEAN upper, _In_ ULONG width, _In_ BOOLEAN zeroPad)
{
    CHAR  digits[32];
    ULONG n = 0;
    PCSTR alpha = upper ? "0123456789ABCDEF" : "0123456789abcdef";

    do {
        digits[n++] = alpha[v % base];
        v /= base;
    } while (v != 0 && n < sizeof(digits));

    while (zeroPad && width > n && *p < end) { *(*p)++ = '0'; width--; }
    while (n > 0 && *p < end) { *(*p)++ = digits[--n]; }
}

static VOID
KFormat(_Out_writes_(cap) PCHAR buf, _In_ SIZE_T cap, _In_ PCSTR fmt, _In_ va_list ap)
{
    PCHAR p   = buf;
    PCHAR end = buf + cap - 1;

    if (cap == 0) return;

    while (*fmt && p < end) {
        if (*fmt != '%') { *p++ = *fmt++; continue; }
        fmt++;

        BOOLEAN zeroPad = FALSE;
        BOOLEAN big64   = FALSE;
        ULONG   width   = 0;

        if (*fmt == '0') { zeroPad = TRUE; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') { width = width * 10 + (ULONG)(*fmt - '0'); fmt++; }

        //  长度修饰符必须真正记账，不能跳过了事。
        //  之前在这里直接 fmt++ 把 l/z/I 扔掉，结果 %p 落进 default 只打印
        //  一个 'p' 而且**不消费参数**，后面每个 %X 都把指针当状态码读 ——
        //  "attach -> 0x845C9080" 打的其实是 PDO 地址，不是 NTSTATUS。
        //  x64 上 long/long long/size_t/指针全是 8 字节。
        for (;;) {
            if (*fmt == 'l') { big64 = TRUE; fmt++; if (*fmt == 'l') fmt++; }
            else if (*fmt == 'z' || *fmt == 'I' || *fmt == 'j' || *fmt == 't') { big64 = TRUE; fmt++; }
            else if (*fmt == 'h') { fmt++; }
            else break;
        }

        switch (*fmt) {
        case 's': {
            PCSTR s = va_arg(ap, PCSTR);
            if (s == NULL) s = "(null)";
            while (*s && p < end) *p++ = *s++;
            break;
        }
        case 'c':
            if (p < end) *p++ = (CHAR)va_arg(ap, int);
            break;
        case 'p': {
            //  指针按 16 位十六进制打，内核指针高位是 0xFFFF，截断成 8 位
            //  会和合法的 NTSTATUS 撞脸（两者都是 0x8xxxxxxx）
            ULONG_PTR v = (ULONG_PTR)va_arg(ap, PVOID);
            KPutNum(&p, end, v, 16, TRUE, 16, TRUE);
            break;
        }
        case 'd': {
            LONGLONG v = big64 ? va_arg(ap, LONGLONG) : (LONGLONG)va_arg(ap, LONG);
            if (v < 0 && p < end) *p++ = '-';
            KPutNum(&p, end, (ULONG_PTR)(v < 0 ? -v : v), 10, FALSE, width, zeroPad);
            break;
        }
        case 'u': {
            ULONG_PTR v = big64 ? (ULONG_PTR)va_arg(ap, ULONGLONG) : (ULONG_PTR)va_arg(ap, ULONG);
            KPutNum(&p, end, v, 10, FALSE, width, zeroPad);
            break;
        }
        case 'x': {
            ULONG_PTR v = big64 ? (ULONG_PTR)va_arg(ap, ULONGLONG) : (ULONG_PTR)va_arg(ap, ULONG);
            KPutNum(&p, end, v, 16, FALSE, width, zeroPad);
            break;
        }
        case 'X': {
            ULONG_PTR v = big64 ? (ULONG_PTR)va_arg(ap, ULONGLONG) : (ULONG_PTR)va_arg(ap, ULONG);
            KPutNum(&p, end, v, 16, TRUE, width, zeroPad);
            break;
        }
        default:
            if (p < end) *p++ = *fmt;
            break;
        }
        fmt++;
    }

    *p = ANSI_NULL;
}

VOID
ProbeMark(_In_ PCSTR tag)
{
    static const WCHAR kPrefix[] = L"\\??\\C:\\Users\\Public\\ldac-mark-";
    WCHAR             path[160];
    WCHAR*            p = path;
    const WCHAR*      q = kPrefix;
    PCSTR             s = tag;
    UNICODE_STRING    us;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   iosb;
    HANDLE            h = NULL;

    //  同 ProbeLog：DPC / 高 IRQL 下不能建文件。
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        DbgPrint("[ldac-mark!] %s\n", tag);
        return;
    }

    while (*q && p < path + 130) *p++ = *q++;
    while (*s && p < path + 140) *p++ = (WCHAR)(UCHAR)*s++;
    *p++ = L'.'; *p++ = L't'; *p++ = L'x'; *p++ = L't';
    *p = L'\0';

    RtlInitUnicodeString(&us, path);
    InitializeObjectAttributes(&oa, &us, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    //  FILE_OVERWRITE_IF so a repeat run does not fail on an existing marker.
    //  The body text is a bonus: even if it is lost, the file name survives.
    if (NT_SUCCESS(ZwCreateFile(&h,
                                FILE_WRITE_DATA | SYNCHRONIZE,
                                &oa, &iosb, NULL,
                                FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_OVERWRITE_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                                NULL, 0))) {
        ZwWriteFile(h, NULL, NULL, NULL, &iosb, (PVOID)tag, (ULONG)KStrLen(tag), NULL, NULL);
        ZwClose(h);
    }
    DbgPrint("[ldac-mark] %s\n", tag);
}

//  时间基准：DriverEntry 后第一次写日志的时刻。
//  每行前缀 "[秒.毫秒] "，用来量"从设备连上到出声音"每一段的耗时 ——
//  之前只有事件顺序没有时间，延迟只能靠猜。
static volatile LONGLONG g_LogT0 = 0;

VOID
ProbeLog(_In_ PCSTR fmt, ...)
{
    CHAR      body[512];
    SIZE_T    n;
    PCHAR     p    = body;
    PCHAR     bend = body + sizeof(body) - 1;
    ULONGLONG dtms;
    va_list   ap;

    if (g_LogT0 == 0) {
        InterlockedCompareExchange64(&g_LogT0, (LONGLONG)KeQueryInterruptTime(), 0);
    }
    dtms = (ULONGLONG)((LONGLONG)KeQueryInterruptTime() - g_LogT0) / 10000ULL;

    *p++ = '[';
    KPutNum(&p, bend, (ULONG_PTR)(dtms / 1000ULL), 10, FALSE, 0, FALSE);
    *p++ = '.';
    KPutNum(&p, bend, (ULONG_PTR)(dtms % 1000ULL), 10, FALSE, 3, TRUE);
    *p++ = ']';
    *p++ = ' ';

    va_start(ap, fmt);
    KFormat(p, (SIZE_T)(bend - p), fmt, ap);
    va_end(ap);

    //  !!! 这是本驱动最危险的一处调用点 !!!
    //
    //  IMiniportWaveRTStream::GetPosition 允许被 PortCls 在 DISPATCH_LEVEL
    //  调用，SetState 也可能在提升后的 IRQL 上进来。而下面用到的
    //  ZwCreateFile / ZwWriteFile / ZwClose 全部要求 PASSIVE_LEVEL ——
    //  在 DISPATCH 上走文件系统会直接 bugcheck（本机已实测到 0x7E 与 0x1C6）。
    //
    //  DbgPrint 本身在任何 IRQL 都安全，所以高 IRQL 时只落调试输出。
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) {
        DbgPrint("[ldac!] %s\n", body);
        return;
    }

    DbgPrint("[ldac] %s\n", body);

    UNICODE_STRING    path;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK   iosb;
    HANDLE            h = NULL;

    RtlInitUnicodeString(&path, LOG_PATH);
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    //  FILE_WRITE_THROUGH is not optional here. Without it ZwWriteFile only
    //  fills the file system cache, and a bugcheck loses every line written in
    //  the last few hundred milliseconds - which is exactly the window we care
    //  about. That is why the 17:49 crash produced an empty log even though
    //  ProbeLog runs twice before KsAudioCreate.
    if (NT_SUCCESS(ZwCreateFile(&h,
                                FILE_APPEND_DATA | SYNCHRONIZE,
                                &oa, &iosb, NULL,
                                FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                FILE_OPEN_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH,
                                NULL, 0))) {
        //  \r\n 直接补在 body 尾部。原来是 body[600] + line[640] 两份拷贝，
        //  单个 ProbeLog 吃掉 1.2 KB 内核栈，这里压到 512 字节。
        n = KStrLen(body);
        if (n > sizeof(body) - 3) n = sizeof(body) - 3;
        body[n++] = '\r';
        body[n++] = '\n';
        body[n]   = ANSI_NULL;
        ZwWriteFile(h, NULL, NULL, NULL, &iosb, body, (ULONG)n, NULL, NULL);
        //  ZwFlushBuffersFile is not declared by ntddk.h (and C4013 makes it an
        //  undeclared-returns-int call), so we rely on FILE_WRITE_THROUGH alone.
        //  With FILE_SYNCHRONOUS_IO_NONALERT that makes the write go straight
        //  through to the volume instead of sitting in the cache.
        ZwClose(h);
    }
}
