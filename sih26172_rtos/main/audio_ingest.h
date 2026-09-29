#pragma once

#include <stdint.h>

/* audio_ingest_task: I2S/DMA -> Q15.16 -> ring buffer, one hop per DMA interrupt.
 * Creates and starts the I2S channel itself (so the DMA ISR lands on core 0). */
void audio_ingest_task(void *arg);

typedef struct {
    uint32_t hops_total;        /* hops written since boot */
    uint32_t hops_interval;     /* hops written in the last stats interval */
    uint32_t dma_queue_overflows; /* DMA buffers dropped because ingest fell behind (since boot) */
    uint32_t read_errors;       /* i2s_channel_read errors other than "no data" (since boot) */
    uint32_t empty_wakes;       /* wakeups that found no buffer (normal: already drained in the previous loop) */
    uint32_t write_pos;         /* ring write position at snapshot time */
    float peak_dbfs;            /* peak |sample| in the interval */
    float rms_dbfs;             /* AC (DC-removed) RMS in the interval */
    float dc_offset;            /* mean sample value in the interval, full scale = 1.0 */
} ingest_stats_t;

/* Snapshot published by audio_ingest_task every stats period (thread safe). */
void audio_ingest_get_stats(ingest_stats_t *out);

/* Static RAM owned by the ingest module (not counting its task stack). */
uint32_t audio_ingest_static_bytes(void);
