/**
 * LVGL memory allocator backed by PSRAM.
 *
 * By routing LVGL's internal allocations to PSRAM (8 MB, mostly free) instead
 * of internal DRAM (~320 KB total, of which ~40 KB is reserved by the RGB
 * bounce-buffer DMA), the WiFi driver can claim the ~100 KB of internal DRAM
 * it needs during esp_wifi_init().
 *
 * Included by lv_conf.h via LV_MEM_CUSTOM_INCLUDE.
 * The three functions below satisfy the LV_MEM_CUSTOM_ALLOC / _FREE / _REALLOC
 * contract (same signatures as malloc / free / realloc).
 */

#pragma once
#include <stddef.h>
#include <esp_heap_caps.h>

static inline void *lv_mem_psram_alloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
}

static inline void *lv_mem_psram_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM);
}
