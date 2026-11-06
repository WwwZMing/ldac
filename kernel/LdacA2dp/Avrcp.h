// ============================================================================
//  Avrcp.h — AVRCP 控制器（PSM 23）：读/写耳机绝对音量
// ============================================================================
#pragma once

#include "BthL2cap.h"

//  读一次设备的绝对音量，再把它设成 SetTo（0..127；传 0xFF 表示只读）。
//  成功时 *pCurrent = 设备当前音量（读不到则为 0xFF）。
NTSTATUS
AvrcpSetVolume(_In_ BTH_ADDR Addr, _In_ UCHAR SetTo, _Out_opt_ PUCHAR pCurrent,
               _Out_opt_ PUCHAR pResponseCtype);
