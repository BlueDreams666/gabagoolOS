#ifndef FAKE_PCM_H
#define FAKE_PCM_H
#include <stddef.h>
#include <stdbool.h>
enum pcm_dma_status { PCM_DMAST_ERR_DMA = -1, PCM_DMAST_OK = 0, PCM_DMAST_STARTED = 1 };
extern unsigned long pcm_sampr;
bool pcm_play_dma_complete_callback(enum pcm_dma_status status, const void **addr, size_t *size);
enum pcm_dma_status pcm_play_dma_status_callback(enum pcm_dma_status status);
#endif
