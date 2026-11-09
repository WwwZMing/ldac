// ============================================================================
//  aac_latm.h — A2DP AAC 的编码 + LATM 封帧（用户态）
// ============================================================================
//  A2DP 的 AAC 负载不是 ADTS，而是 LATM 的 AudioMuxElement（每帧一个）。
//  编码用 Windows 自带 Media Foundation 的 AAC MFT（raw payload），
//  LATM 封装在这里自己做 —— 见 aac_latm.cpp 顶部的位布局说明。
// ============================================================================
#ifndef LDAC_AAC_LATM_H
#define LDAC_AAC_LATM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AacLatm AacLatm;

//  打开 AAC-LC 编码器。rate: 44100/48000；channels: 1/2；bitrate: bps。
//  失败返回 NULL，原因写进 err。
AacLatm* AacLatmOpen(int rate, int channels, int bitrate, char* err, int errLen);

//  每帧固定 1024 采样（AAC-LC）。
#define AAC_LATM_FRAME_SAMPLES 1024

//  编码一帧：pcm 是交错 S16，samples = 每声道采样数。
//  输出已经是封好的 LATM AudioMuxElement（可直接当 A2DP 媒体负载）。
//  返回字节数，*frames = 里面包含几个 AudioMuxElement；0 = 还没攒够。
int AacLatmEncode(AacLatm* c, const short* pcm, int samples,
                  unsigned char* out, int outCap, int* frames);

void AacLatmClose(AacLatm* c);

#ifdef __cplusplus
}
#endif
#endif
