/* MSVC 兼容层：强制包含（/FI）。
 *
 * libldac 是 AOSP 里按 clang/gcc 写的代码，用了若干 GCC 扩展。
 * 这里用宏把它们消掉，避免逐个改上游源码（保持 third_party 原样）。
 */
#ifndef LDAC_MSVC_COMPAT_H
#define LDAC_MSVC_COMPAT_H

#if defined(_MSC_VER)

/* __attribute__((unused)) / ((packed)) / ... 全部忽略 */
#ifndef __attribute__
#define __attribute__(x)
#endif

/* 一些 AOSP 代码会用 __restrict__ */
#ifndef __restrict__
#define __restrict__ __restrict
#endif

#endif /* _MSC_VER */

#endif /* LDAC_MSVC_COMPAT_H */
