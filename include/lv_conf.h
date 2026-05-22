/**
 * LVGL Configuration for CrowPanel 7.0" (800x480 RGB565)
 * LVGL v8.3.x
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

// Color settings
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// Memory - route ALL LVGL allocations to PSRAM so the WiFi driver (~100 KB)
// can claim internal DRAM without competing with LVGL widgets/objects.
#define LV_MEM_CUSTOM 1
#if LV_MEM_CUSTOM == 1
#define LV_MEM_CUSTOM_INCLUDE "lv_mem_psram.h"
#define LV_MEM_CUSTOM_ALLOC lv_mem_psram_alloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC lv_mem_psram_realloc
#endif

// HAL - use Arduino millis() for tick
#define LV_TICK_CUSTOM 1
#if LV_TICK_CUSTOM == 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())
#endif

// Display refresh period (ms)
#define LV_DISP_DEF_REFR_PERIOD 16

// Maximum number of individual dirty areas LVGL will track before giving up
// and invalidating the entire screen. With 14 bands * 3 objects + 4 meters,
// loading propagation data exceeds the default of 32 areas. When this overflows,
// it triggers a full-screen redraw, which causes a 1.2MB PSRAM-to-PSRAM memcpy
// that starves the RGB DMA bounce buffer and creates tearing artifacts.
#define LV_INV_BUF_SIZE 256

// Input device read period (ms)
#define LV_INDEV_DEF_READ_PERIOD 30

// DPI for size calculations
#define LV_DPI_DEF 130

// Logging
#define LV_USE_LOG 0

// GPU - none
#define LV_USE_GPU_SDL 0

// Fonts
#define LV_FONT_MONTSERRAT_10 1
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1

#define LV_FONT_DEFAULT &lv_font_montserrat_16

// Theme
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1

// Widgets
#define LV_USE_ARC 1
#define LV_USE_BAR 1
#define LV_USE_BTN 1
#define LV_USE_BTNMATRIX 1
#define LV_USE_CANVAS 0
#define LV_USE_CHECKBOX 1
#define LV_USE_DROPDOWN 1
#define LV_USE_IMG 1
#define LV_USE_LABEL 1
#define LV_USE_LINE 1
#define LV_USE_ROLLER 0
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TABLE 1
#define LV_USE_TEXTAREA 1

// Extra widgets
#define LV_USE_ANIMIMG 0
#define LV_USE_CALENDAR 0
#define LV_USE_CHART 0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN 0
#define LV_USE_KEYBOARD 0
#define LV_USE_LED 1
#define LV_USE_LIST 1
#define LV_USE_METER 1
#define LV_USE_MSGBOX 1
#define LV_USE_SPAN 0
#define LV_USE_SPINBOX 0
#define LV_USE_SPINNER 1
#define LV_USE_TABVIEW 1
#define LV_USE_TILEVIEW 0
#define LV_USE_WIN 0

// Layouts
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

#endif // LV_CONF_H
