/*
 * ldac_enc_test.c -- libldac 编码器自检
 *
 * 目的：在没有蓝牙硬件的情况下验证 Sony LDAC 编码器能正确工作。
 * 判据（LDAC 规格，48kHz / 2ch）：
 *   - 每秒帧数 ≈ 48000 / 128 = 375
 *   - HQ 档码率 ≈ 990 kbps → 每秒约 123,750 字节
 *   - SQ ≈ 660 kbps、MQ ≈ 330 kbps
 * 如果帧数和码率落在这个范围，说明编码器正确初始化并按档位工作。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "ldacBT.h"

#define FS        48000
#define CHANNELS  2
#define SECONDS   1
#define MTU       679        /* 典型 A2DP LDAC L2CAP MTU */
#define CHUNK     4096       /* 每次喂给编码器的 PCM 字节数 */

int main(void)
{
    int i;
    int n_samples  = FS * SECONDS;
    int pcm_bytes  = n_samples * CHANNELS * (int)sizeof(short);
    short *pcm     = (short *)malloc((size_t)pcm_bytes);
    unsigned char out[8192];
    HANDLE_LDAC_BT h;
    int rc;
    int fed = 0, total_out = 0, frames = 0, calls = 0;
    int first_frame_sz = -1;
    unsigned char first_frame_head[8];
    int err;

    if (pcm == NULL) {
        printf("malloc failed\n");
        return 1;
    }

    /* 440 Hz 正弦，左右声道相同 */
    for (i = 0; i < n_samples; i++) {
        short v = (short)(9000.0 * sin(2.0 * 3.14159265358979 * 440.0 * (double)i / (double)FS));
        pcm[i * 2 + 0] = v;
        pcm[i * 2 + 1] = v;
    }

    h = ldacBT_get_handle();
    if (h == NULL) {
        printf("ldacBT_get_handle failed\n");
        return 1;
    }

    rc = ldacBT_init_handle_encode(h, MTU, LDACBT_EQMID_HQ, CHANNELS,
                                   LDACBT_SMPL_FMT_S16, FS);
    printf("init_handle_encode rc=%d (0=ok)\n", rc);
    if (rc != 0) {
        ldacBT_free_handle(h);
        free(pcm);
        return 1;
    }

    while (fed < pcm_bytes) {
        int pcm_used = pcm_bytes - fed;
        int stream_sz = 0;
        int frame_num = 0;

        if (pcm_used > CHUNK) {
            pcm_used = CHUNK;
        }

        rc = ldacBT_encode(h, (unsigned char *)pcm + fed, &pcm_used,
                           out, &stream_sz, &frame_num);
        if (rc != 0) {
            err = ldacBT_get_error_code(h);
            printf("encode rc=%d err=0x%08X (fed=%d)\n", rc, err, fed);
            break;
        }

        fed       += pcm_used;
        total_out += stream_sz;
        frames    += frame_num;
        calls++;

        if (first_frame_sz < 0 && stream_sz > 0) {
            first_frame_sz = stream_sz / (frame_num > 0 ? frame_num : 1);
            memcpy(first_frame_head, out, 8);
        }
    }

    /* 用 NULL 冲刷内部缓冲 */
    for (i = 0; i < 64; i++) {
        int pcm_used = 0, stream_sz = 0, frame_num = 0;
        rc = ldacBT_encode(h, NULL, &pcm_used, out, &stream_sz, &frame_num);
        if (rc != 0 || stream_sz <= 0) {
            break;
        }
        total_out += stream_sz;
        frames    += frame_num;
    }

    printf("\n=== result ===\n");
    printf("pcm fed      : %d bytes (%.3f s)\n", fed, (double)fed / (FS * CHANNELS * 2.0));
    printf("encoded      : %d bytes, %d frames, %d calls\n", total_out, frames, calls);
    printf("~bitrate     : %.1f kbps\n",
           (double)total_out * 8.0 / ((double)fed / (FS * CHANNELS * 2.0)) / 1000.0);
    printf("frame size   : ~%d bytes\n", first_frame_sz);
    printf("first header : %02X %02X %02X %02X %02X %02X %02X %02X\n",
           first_frame_head[0], first_frame_head[1], first_frame_head[2], first_frame_head[3],
           first_frame_head[4], first_frame_head[5], first_frame_head[6], first_frame_head[7]);

    ldacBT_close_handle(h);
    ldacBT_free_handle(h);
    free(pcm);

    printf("\nPASS criteria: frames ~375/s, bitrate ~990 kbps for HQ\n");
    return 0;
}
