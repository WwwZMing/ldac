// ============================================================================
//  wasapi_loop.h — WASAPI loopback 采集（默认播放设备）的 C 接口
// ============================================================================
//  实现（wasapi_loop.cpp，C++/COM）与调用方（ldac_send.c，C）之间的边界。
//  采集的是系统混音格式（通常是 48kHz/2ch/float32），把格式回报给调用方，
//  float→S16 的转换放在调用方做（它本来就要管 S16 缓冲）。
// ============================================================================
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WasapiCap WasapiCap;

//  打开默认播放设备的 loopback 采集。
//  成功返回 0；*Rate/*Channels/*Bits 回报混音格式（Bits = 16 PCM 或 32 float）。
int WasapiStart(WasapiCap** Out, unsigned* Rate, unsigned* Channels, unsigned* Bits);

//  同上，但按**端点名字子串**选设备（不依赖"默认端点"）。
//  为什么需要：SSH 登录是 session 0，那里的默认渲染端点不是用户在控制台
//  用的那个，于是采集到的是静音 —— 之前"发送端必须跑在 session 1"就是这么来的。
//  Match 传 NULL 等价于 WasapiStart。
int WasapiStartMatch(const char* Match, WasapiCap** Out, unsigned* Rate,
                     unsigned* Channels, unsigned* Bits);

//  拉走已捕获的音频（原始混音格式字节）。返回拷走的字节数（可能为 0，最多阻塞 ~20ms）。
int WasapiRead(WasapiCap* Cap, unsigned char* Buf, int Bytes);

void WasapiStop(WasapiCap* Cap);

#ifdef __cplusplus
}
#endif
