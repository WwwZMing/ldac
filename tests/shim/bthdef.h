// 用户态 shim：bthdef.h 只需要几个蓝牙基础类型
#pragma once

#include <windows.h>

typedef ULONGLONG BTH_ADDR;
typedef BTH_ADDR *PBTH_ADDR;
typedef void     *L2CAP_CHANNEL_HANDLE;
