/**
 * TouchController board pin/timing profiles.
 *
 * Default profile: CrowPanel 7" 800x480.
 * Optional profile: generic ESP32-S3 7" 1024x600 boards
 * (enabled with -DBOARD_PROFILE_1024X600 in platformio.ini).
 *
 * Note: RGB/touch pin mappings vary by vendor. The 1024x600 profile below
 * keeps the same pin map as the working CrowPanel profile unless overridden.
 */

#ifndef BOARD_PINOUT_H
#define BOARD_PINOUT_H

// ========== RGB Display Pins (16-bit: R5 G6 B5) ==========
#if defined(BOARD_PROFILE_1024X600)
// 1024x600 (Waveshare-like) mapping
// Blue channel (5 bits)
#define LCD_B0 14
#define LCD_B1 38
#define LCD_B2 18
#define LCD_B3 17
#define LCD_B4 10

// Green channel (6 bits)
#define LCD_G0 39
#define LCD_G1 0
#define LCD_G2 45
#define LCD_G3 48
#define LCD_G4 47
#define LCD_G5 21

// Red channel (5 bits)
#define LCD_R0 1
#define LCD_R1 2
#define LCD_R2 42
#define LCD_R3 41
#define LCD_R4 40

// Sync signals
#define LCD_HSYNC 46
#define LCD_VSYNC 3
#define LCD_DE 5
#define LCD_PCLK 7

// Backlight often routed via external expander on this profile.
#define LCD_BL -1
#else
// CrowPanel 7" (800x480) mapping
// Blue channel (5 bits)
#define LCD_B0 8
#define LCD_B1 3
#define LCD_B2 46
#define LCD_B3 9
#define LCD_B4 1

// Green channel (6 bits)
#define LCD_G0 5
#define LCD_G1 6
#define LCD_G2 7
#define LCD_G3 15
#define LCD_G4 16
#define LCD_G5 4

// Red channel (5 bits)
#define LCD_R0 45
#define LCD_R1 48
#define LCD_R2 47
#define LCD_R3 21
#define LCD_R4 14

// Sync signals
#define LCD_HSYNC 39
#define LCD_VSYNC 40
#define LCD_DE 41
#define LCD_PCLK 42

// CrowPanel uses direct PWM/GPIO backlight control.
#define LCD_BL 2
#endif

// ========== Display profile selection ==========
#if defined(BOARD_PROFILE_1024X600)
// 1024x600 profile
#define LCD_WIDTH 1024
#define LCD_HEIGHT 600

// RGB timing (common 1024x600 timing values for EK79007/compatible panels)
// Restored 2026-08-23 ("Attempt E") to the values from this project's very first
// commit (477709e, "WIP: Replace LovyanGFX with esp_lcd_new_rgb_panel"), after user
// recalled the display used to work and everything tried this session (Attempts
// A-D: various num_fbs/bounce_buffer/pclk combinations, all built on the "zero-copy
// direct_mode" driver architecture) failed to reproduce that. Tracing git history
// found the zero-copy direct_mode architecture — and this reduced 14MHz timing —
// was introduced together in one commit (b492e07, "Restore full-featured main.cpp")
// that also re-pasted in a large amount of application code; the display driver
// changes look like incidental fallout of that restore, not a deliberate, tested
// fix. See RGB_PANEL_NOTES.md, "Attempt E: revert to the original architecture".
// 30MHz here gives ~32.75Hz refresh (916146 total pclks/frame) — much closer to a
// normal refresh rate than anything tried this session (12.9-16.4Hz).
#define LCD_FREQ_WRITE 30000000
#define LCD_HSYNC_POLARITY 0
#define LCD_HSYNC_FRONT_PORCH 48
#define LCD_HSYNC_PULSE_WIDTH 162
#define LCD_HSYNC_BACK_PORCH 152
#define LCD_VSYNC_POLARITY 0
#define LCD_VSYNC_FRONT_PORCH 3
#define LCD_VSYNC_PULSE_WIDTH 45
#define LCD_VSYNC_BACK_PORCH 13
#define LCD_PCLK_ACTIVE_NEG 1
#define LCD_DE_IDLE_HIGH 0
#define LCD_PCLK_IDLE_HIGH 0
#else
// CrowPanel 800x480 profile
#define LCD_WIDTH 800
#define LCD_HEIGHT 480

// RGB timing (validated for current CrowPanel firmware)
#define LCD_FREQ_WRITE 15000000
#define LCD_HSYNC_POLARITY 0
#define LCD_HSYNC_FRONT_PORCH 40
#define LCD_HSYNC_PULSE_WIDTH 48
#define LCD_HSYNC_BACK_PORCH 40
#define LCD_VSYNC_POLARITY 0
#define LCD_VSYNC_FRONT_PORCH 1
#define LCD_VSYNC_PULSE_WIDTH 31
#define LCD_VSYNC_BACK_PORCH 13
#define LCD_PCLK_ACTIVE_NEG 1
#define LCD_DE_IDLE_HIGH 0
#define LCD_PCLK_IDLE_HIGH 0
#endif

// ========== Touch (GT911 via I2C) ==========
#if defined(BOARD_PROFILE_1024X600)
// Waveshare ESP32-S3-Touch-LCD-7B reference mapping.
#define TOUCH_SDA 8
#define TOUCH_SCL 9
#define TOUCH_INT 4
#define TOUCH_RST -1
#else
#define TOUCH_SDA 19
#define TOUCH_SCL 20
#define TOUCH_INT -1
#define TOUCH_RST -1
#endif

// ========== SD Card (SPI) ==========
#define SD_MOSI 11
#define SD_MISO 13
#define SD_CLK 12
#define SD_CS 10

// ========== I2S Audio ==========
#define I2S_LRCLK 18
#define I2S_BCLK 42
#define I2S_SDIN 17

// ========== UART1 ==========
#define UART1_RX 44
#define UART1_TX 43

// ========== External GPIO ==========
#define GPIO_D 38

#endif // BOARD_PINOUT_H
