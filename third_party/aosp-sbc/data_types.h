/* Windows shim: AOSP bluedroid 的 data_types.h 只提供基本定宽类型，
   这里用最小定义代替（原文件来自 system/bt/stack/include）。 */
#ifndef DATA_TYPES_H
#define DATA_TYPES_H

#include <stdint.h>

typedef uint8_t  UINT8;
typedef int8_t   INT8;
typedef uint16_t UINT16;
typedef int16_t  INT16;
typedef uint32_t UINT32;
typedef int32_t  INT32;
typedef uint64_t UINT64;
typedef int64_t  INT64;

/* AOSP 里 SBC_API 来自 bt_common.h（导出符号修饰），这里定义为空 */
#ifndef SBC_API
#define SBC_API
#endif

/* AOSP 的 trace 宏（bt_trace.h），这儿全部吞掉 */
#ifndef APPL_TRACE_EVENT
#define APPL_TRACE_EVENT(...) do {} while (0)
#endif
#ifndef APPL_TRACE_ERROR
#define APPL_TRACE_ERROR(...) do {} while (0)
#endif
#ifndef APPL_TRACE_WARNING
#define APPL_TRACE_WARNING(...) do {} while (0)
#endif
#ifndef APPL_TRACE_DEBUG
#define APPL_TRACE_DEBUG(...) do {} while (0)
#endif

#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif

#endif
