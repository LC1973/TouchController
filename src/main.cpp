/**
 * TouchController - ESP32-S3 7" RGB Touch Screen
 *
 * Unified control panel for G7NRU radio network:
 *   - Overview: Peer discovery and system status
 *   - Power: Remote station relay control with battery/MPPT meters
 *   - Antennas: Antenna controller relay states
 *   - Rotator: Bearing display and goto heading
 *   - System: WiFi status, network diagnostics
 *
 * Also runs a web server on port 80 for settings and log viewing.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <SPIFFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <lvgl.h>

#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_rgb.h"
#include "freertos/semphr.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <DebugLogger.h>
#include <WiFiManager.h>
#include <Utils.h>
#include <PeerDiscovery.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <TCIService.h>
#include "board_pinout.h"
#include "coastline_data.h"

#define debugLog(msg) DebugLogger::log(msg)
#define debugLogf(...) DebugLogger::logf(__VA_ARGS__)

const char *buildDate = __DATE__;
const char *buildTime = __TIME__;

namespace
{
    // Waveshare IO expander protocol (CH422/IO_EXTENSION compatible).
    constexpr uint8_t kCh422Addr = 0x24;
    constexpr uint8_t kCh422RegMode = 0x02;
    constexpr uint8_t kCh422RegOutput = 0x03;
    constexpr uint8_t kCh422BacklightBit = (1 << 2); // IO2: backlight control
    constexpr uint8_t kCh422UsbCanBit = (1 << 5);    // IO5: 0=USB, 1=CAN

#ifndef CH422_BACKLIGHT_ACTIVE_LOW
#define CH422_BACKLIGHT_ACTIVE_LOW 0
#endif

#ifndef FALLBACK_BL_PIN
#define FALLBACK_BL_PIN 2
#endif

#ifndef WAVESHARE_STRICT_MODE
#if defined(BOARD_PROFILE_1024X600)
#define WAVESHARE_STRICT_MODE 1
#else
#define WAVESHARE_STRICT_MODE 0
#endif
#endif

    uint8_t gCh422OutputState = 0xFF;
    bool gCh422Ready = false;
    TwoWire gCh422Wire = TwoWire(1);

    struct I2cPinPair
    {
        int sda;
        int scl;
    };

#if WAVESHARE_STRICT_MODE
    constexpr I2cPinPair kI2cPinCandidates[] = {
        {TOUCH_SDA, TOUCH_SCL},
    };
#else
    constexpr I2cPinPair kI2cPinCandidates[] = {
        {TOUCH_SDA, TOUCH_SCL},
        {8, 9},
        {17, 18},
    };
#endif

    static bool writeCh422Reg(uint8_t reg, uint8_t value)
    {
        gCh422Wire.beginTransmission(kCh422Addr);
        gCh422Wire.write(reg);
        gCh422Wire.write(value);
        return gCh422Wire.endTransmission() == 0;
    }

    static bool pingI2cAddress(uint8_t i2cAddr)
    {
        gCh422Wire.beginTransmission(i2cAddr);
        return gCh422Wire.endTransmission() == 0;
    }

    static void logI2cScan()
    {
        Serial.print("[BL] I2C devices:");
        bool any = false;
        for (uint8_t addr = 0x08; addr <= 0x77; ++addr)
        {
            if (pingI2cAddress(addr))
            {
                Serial.printf(" 0x%02X", addr);
                any = true;
            }
        }
        if (!any)
        {
            Serial.print(" none");
        }
        Serial.println();
    }

    static bool tryInitCh422()
    {
        gCh422OutputState = 0xFF;
        gCh422OutputState &= ~kCh422UsbCanBit;
#if CH422_BACKLIGHT_ACTIVE_LOW
        gCh422OutputState &= ~kCh422BacklightBit;
#else
        gCh422OutputState |= kCh422BacklightBit;
#endif
        bool modeOk = writeCh422Reg(kCh422RegMode, 0xFF);
        bool outputOk = writeCh422Reg(kCh422RegOutput, gCh422OutputState);

        Serial.printf("[BL] CH422 try addr=0x%02X modeReg=0x%02X outReg=0x%02X mode=%d outok=%d state=0x%02X\n",
                      kCh422Addr,
                      kCh422RegMode,
                      kCh422RegOutput,
                      modeOk ? 1 : 0,
                      outputOk ? 1 : 0,
                      gCh422OutputState);

        return modeOk && outputOk;
    }

    static bool setCh422Bit(uint8_t bitIndex, bool high)
    {
        if (!gCh422Ready || bitIndex > 7)
        {
            return false;
        }

        uint8_t mask = static_cast<uint8_t>(1U << bitIndex);
        if (high)
        {
            gCh422OutputState |= mask;
        }
        else
        {
            gCh422OutputState &= static_cast<uint8_t>(~mask);
        }

        return writeCh422Reg(kCh422RegOutput, gCh422OutputState);
    }

    static void runWaveshareTouchWakeSequence()
    {
        // Waveshare reference: IO1 low -> TOUCH_INT low -> IO1 high.
        if (!gCh422Ready || TOUCH_INT < 0)
        {
            return;
        }

        pinMode(TOUCH_INT, OUTPUT);
        setCh422Bit(1, false);
        delay(100);
        digitalWrite(TOUCH_INT, LOW);
        delay(100);
        setCh422Bit(1, true);
        delay(200);
        pinMode(TOUCH_INT, INPUT);
        Serial.println("[BL] Waveshare touch wake sequence complete");
    }

    bool initCh422Control()
    {
        gCh422Ready = false;

        for (const auto &pins : kI2cPinCandidates)
        {
            gCh422Wire.begin(pins.sda, pins.scl, 400000);
            Serial.printf("[BL] Probe I2C SDA=%d SCL=%d\n", pins.sda, pins.scl);
            logI2cScan();

            if (tryInitCh422())
            {
                gCh422Ready = true;
#if WAVESHARE_STRICT_MODE
                runWaveshareTouchWakeSequence();
#endif
                break;
            }
        }

        Serial.printf("[BL] CH422G init ready=%d activeLow=%d outAddr=0x%02X state=0x%02X\n",
                      gCh422Ready ? 1 : 0,
                      CH422_BACKLIGHT_ACTIVE_LOW ? 1 : 0,
                      kCh422Addr,
                      gCh422OutputState);

        return gCh422Ready;
    }

    void setBacklightViaCh422(bool on)
    {
        if (!gCh422Ready)
        {
            return;
        }

#if CH422_BACKLIGHT_ACTIVE_LOW
        bool driveHigh = !on;
#else
        bool driveHigh = on;
#endif

        if (driveHigh)
        {
            gCh422OutputState |= kCh422BacklightBit;
        }
        else
        {
            gCh422OutputState &= ~kCh422BacklightBit;
        }

        // Always keep USB mode selected while debugging serial/flash.
        gCh422OutputState &= ~kCh422UsbCanBit;
        writeCh422Reg(kCh422RegOutput, gCh422OutputState);
    }
} // namespace

static void blinkBacklightProbe(uint8_t cycles = 6, uint16_t intervalMs = 250)
{
    bool expanderReady = false;
    bool fallbackPinReady = false;

    if (LCD_BL < 0)
    {
        expanderReady = initCh422Control();
    }

    if (!expanderReady && LCD_BL >= 0)
    {
        // Fallback for older boards where backlight is directly wired to MCU GPIO.
        pinMode(LCD_BL, OUTPUT);
        fallbackPinReady = true;
        Serial.printf("[BL] Using direct LCD_BL pin %d\n", LCD_BL);
    }
    else if (!expanderReady && LCD_BL < 0 && FALLBACK_BL_PIN >= 0 && !WAVESHARE_STRICT_MODE)
    {
        // Rescue path for CrowPanel variants that use a direct BL pin.
        pinMode(FALLBACK_BL_PIN, OUTPUT);
        fallbackPinReady = true;
        Serial.printf("[BL] Using fallback GPIO backlight pin %d\n", FALLBACK_BL_PIN);
    }

    for (uint8_t index = 0; index < cycles; ++index)
    {
        bool on = (index % 2) != 0;

        if (expanderReady)
        {
            setBacklightViaCh422(on);
        }
        else if (fallbackPinReady && LCD_BL < 0 && FALLBACK_BL_PIN >= 0)
        {
            digitalWrite(FALLBACK_BL_PIN, on ? HIGH : LOW);
        }
        else if (LCD_BL >= 0)
        {
            digitalWrite(LCD_BL, on ? HIGH : LOW);
        }

        delay(intervalMs);
    }

    if (expanderReady)
    {
        setBacklightViaCh422(true);
    }
    else if (fallbackPinReady && LCD_BL < 0 && FALLBACK_BL_PIN >= 0)
    {
        digitalWrite(FALLBACK_BL_PIN, HIGH);
    }
    else if (LCD_BL >= 0)
    {
        digitalWrite(LCD_BL, HIGH);
    }
}

// ============================================================
// Display Driver (ESP-IDF RGB panel, single buffer + bounce buffer)
//
// LovyanGFX Bus_RGB is incompatible with ESP32-S3 OPI PSRAM: it bypasses
// esp_lcd_new_rgb_panel() and uses direct GDMA without going through the
// documented driver, causing DMA reads of stale PSRAM data due to cache
// coherency issues on chip rev v0.2 (ESP_ROM_HAS_CACHE_WRITEBACK_BUG).
//
// 2026-08-23 ("Attempt E"): reverted to the architecture from this project's
// very first commit (477709e), after a whole session (2026-08-22, Attempts
// A-D) of tuning num_fbs / bounce_buffer_size_px / pclk on top of a
// completely different "zero-copy direct_mode" architecture failed to
// reproduce a display the user remembered as having worked. Git history
// traced the zero-copy architecture — LVGL rendering directly into the
// panel's own PSRAM frame buffer(s) via esp_lcd_rgb_panel_get_frame_buffer(),
// disp_drv.direct_mode=1, and a hand-rolled vsync-semaphore blocking scheme
// in flush_cb — to commit b492e07 ("Restore full-featured main.cpp"), which
// also quietly changed pclk from 30MHz to 14MHz and rewrote the porches, none
// of which is mentioned in that commit's message (focused on application
// features). That whole architecture is the common ancestor of every symptom
// chased this session (roll, shift, flicker, glitching) across every
// buffer/pclk combination tried.
//
// This reverts to the simple, standard LVGL integration pattern instead:
// LVGL owns small SEPARATE partial-render buffers in PSRAM (not the panel's
// own frame buffer), flush_cb hands each rendered strip to
// esp_lcd_panel_draw_bitmap() with no manual vsync blocking, and the
// ESP-IDF driver's own num_fbs=1 + bounce_buffer_size_px combination — one of
// Espressif's two officially documented (and NOT mutually exclusive at
// num_fbs=1) anti-tearing schemes — handles all PSRAM/DMA synchronization
// internally. No direct_mode dual-buffer sync, no foreign-buffer driver
// internals to reason about, no custom ISR. See RGB_PANEL_NOTES.md, "Attempt
// E: revert to the original architecture" for the full trace before changing
// this again.
// ============================================================

static esp_lcd_panel_handle_t s_panel_handle = NULL;

// Height, in rows, of each LVGL partial-render strip buffer (see buf1/buf2 below).
#define LVGL_BUF_LINES 48

// Detected GT911 I2C address (0x5D or 0x14, depends on INT state at reset).
static uint8_t gGt911Addr = 0x5D;
// GT911 configured coordinate range (read from config registers 0x8048-0x804B).
static uint16_t gGt911MaxX = 1024;
static uint16_t gGt911MaxY = 600;

// Read N bytes from GT911 at 16-bit register address over gCh422Wire.
static bool gt911ReadReg(uint16_t reg, uint8_t *buf, uint8_t len)
{
    gCh422Wire.beginTransmission(gGt911Addr);
    gCh422Wire.write((uint8_t)(reg >> 8));
    gCh422Wire.write((uint8_t)(reg & 0xFF));
    if (gCh422Wire.endTransmission(false) != 0)
        return false;
    gCh422Wire.requestFrom(gGt911Addr, len);
    for (uint8_t i = 0; i < len; i++)
        buf[i] = gCh422Wire.read();
    return true;
}

// Probe GT911 at both possible addresses, set gGt911Addr, and read configured resolution.
static void gt911Detect()
{
    const uint8_t candidates[] = {0x5D, 0x14};
    bool found = false;
    for (uint8_t addr : candidates)
    {
        gCh422Wire.beginTransmission(addr);
        if (gCh422Wire.endTransmission() == 0)
        {
            gGt911Addr = addr;
            Serial.printf("[GT911] Found at I2C address 0x%02X\n", addr);
            found = true;
            break;
        }
    }
    if (!found)
    {
        Serial.println("[GT911] WARNING: not found at 0x5D or 0x14 - touch may not work");
        return;
    }

    // Read configured coordinate resolution from GT911 config registers:
    // 0x8048-0x8049: X_Resolution (little-endian)
    // 0x804A-0x804B: Y_Resolution (little-endian)
    uint8_t res[4] = {0};
    if (gt911ReadReg(0x8048, res, 4))
    {
        uint16_t cfgX = (uint16_t)res[0] | ((uint16_t)res[1] << 8);
        uint16_t cfgY = (uint16_t)res[2] | ((uint16_t)res[3] << 8);
        Serial.printf("[GT911] Config resolution: %ux%u (screen: %dx%d)\n",
                      cfgX, cfgY, LCD_WIDTH, LCD_HEIGHT);
    }
    else
    {
        Serial.println("[GT911] WARNING: failed to read resolution config, using defaults");
    }
    // The GT911 on this panel reports cfgX=1280 but the physical touch sensor
    // outputs raw X coordinates in 0..(LCD_WIDTH-1) screen-pixel space, NOT
    // in 0..(cfgX-1) space.  Using cfgX=1280 with the scaling formula:
    //   x = rawX * LCD_WIDTH / cfgX = rawX * 0.8
    // shifts every touch 20% to the left — exactly one tab-width on a 5-tab bar.
    // Force 1:1 mapping so rawX == screen pixel with no scaling.
    gGt911MaxX = LCD_WIDTH;
    gGt911MaxY = LCD_HEIGHT;
    Serial.printf("[GT911] Forced coordinate mapping to %ux%u (1:1 screen pixels)\n",
                  gGt911MaxX, gGt911MaxY);
}

// Poll GT911 for a touch point. Returns true if touched.
static bool gt911GetTouch(uint16_t &x, uint16_t &y)
{
    static uint32_t lastDiagMs = 0;
    static uint32_t pollCount = 0;
    static uint32_t i2cErrCount = 0;
    static uint8_t lastStatus = 0;

    pollCount++;

    uint8_t status = 0;
    bool readOk = gt911ReadReg(0x814E, &status, 1);

    // Periodic diagnostic: every 3 seconds log poll count, I2C errors, last status
    if (millis() - lastDiagMs >= 3000)
    {
        lastDiagMs = millis();
        Serial.printf("[GT911] polls=%u i2cErr=%u lastStatus=0x%02X addr=0x%02X\n",
                      pollCount, i2cErrCount, lastStatus, gGt911Addr);
    }

    if (!readOk)
    {
        i2cErrCount++;
        return false;
    }

    lastStatus = status;

    // Log any non-zero status immediately (even if not a valid touch)
    if (status != 0)
    {
        Serial.printf("[GT911] status=0x%02X (ready=%d pts=%d)\n",
                      status, (status >> 7) & 1, status & 0x0F);
    }

    // Always clear buffer-ready flag when bit 7 is set - if we don't,
    // GT911 stays stuck and never reports new touch points.
    if (status & 0x80)
    {
        uint8_t zero = 0;
        gCh422Wire.beginTransmission(gGt911Addr);
        gCh422Wire.write(0x81);
        gCh422Wire.write(0x4E);
        gCh422Wire.write(zero);
        gCh422Wire.endTransmission();
    }

    if (!(status & 0x80) || (status & 0x0F) == 0)
        return false;

    // GT911 standard little-endian coordinate format starting at 0x8150:
    //   tp[0] = 0x8150: X position low byte  [7:0]
    //   tp[1] = 0x8151: X position high byte [11:8] (bits [3:0] only)
    //   tp[2] = 0x8152: Y position low byte  [7:0]
    //   tp[3] = 0x8153: Y position high byte [11:8] (bits [3:0] only)
    //   tp[4] = 0x8154: touch area size
    uint8_t tp[5] = {0};
    gt911ReadReg(0x8150, tp, 5);
    uint16_t rawX = (uint16_t)tp[0] | (((uint16_t)tp[1] & 0x0F) << 8);
    uint16_t rawY = (uint16_t)tp[2] | (((uint16_t)tp[3] & 0x0F) << 8);

    // Scale to screen coordinates.
    x = (uint16_t)((uint32_t)rawX * LCD_WIDTH / gGt911MaxX);
    y = (uint16_t)((uint32_t)rawY * LCD_HEIGHT / gGt911MaxY);

    Serial.printf("[GT911] raw=(%u,%u) scaled=(%u,%u) (tp: %02X %02X %02X %02X %02X)\n",
                  rawX, rawY, x, y, tp[0], tp[1], tp[2], tp[3], tp[4]);

    return true;
}

// Initialise the RGB panel using the proper ESP-IDF API with bounce buffer.
// bounce_buffer_size_px allocates internal SRAM as a DMA intermediary so
// DMA never reads PSRAM directly — the ESP-IDF ISR handles the cache-safe
// PSRAM→SRAM copy, which is the correct fix for ESP32-S3 rev v0.2.
static void initRgbPanel()
{
    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src = LCD_CLK_SRC_DEFAULT;
    panel_config.timings.pclk_hz = LCD_FREQ_WRITE;
    panel_config.timings.h_res = LCD_WIDTH;
    panel_config.timings.v_res = LCD_HEIGHT;
    panel_config.timings.hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH;
    panel_config.timings.hsync_back_porch = LCD_HSYNC_BACK_PORCH;
    panel_config.timings.hsync_front_porch = LCD_HSYNC_FRONT_PORCH;
    panel_config.timings.vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH;
    panel_config.timings.vsync_back_porch = LCD_VSYNC_BACK_PORCH;
    panel_config.timings.vsync_front_porch = LCD_VSYNC_FRONT_PORCH;
    panel_config.timings.flags.pclk_active_neg = LCD_PCLK_ACTIVE_NEG;
    panel_config.data_width = 16;
    panel_config.bits_per_pixel = 16;
    panel_config.num_fbs = 1;
    // Bounce buffer in internal SRAM: 10 rows x 1024 pixels.
    // The ESP-IDF ISR copies PSRAM framebuffer -> bounce buffer each VSYNC.
    panel_config.bounce_buffer_size_px = LCD_WIDTH * 10;
    panel_config.sram_trans_align = 4;
    panel_config.psram_trans_align = 64;
    panel_config.hsync_gpio_num = LCD_HSYNC;
    panel_config.vsync_gpio_num = LCD_VSYNC;
    panel_config.de_gpio_num = LCD_DE;
    panel_config.pclk_gpio_num = LCD_PCLK;
    panel_config.disp_gpio_num = GPIO_NUM_NC;
    // Data bus: B[0:4], G[0:5], R[0:4]  (matches Waveshare official order)
    panel_config.data_gpio_nums[0] = LCD_B0;
    panel_config.data_gpio_nums[1] = LCD_B1;
    panel_config.data_gpio_nums[2] = LCD_B2;
    panel_config.data_gpio_nums[3] = LCD_B3;
    panel_config.data_gpio_nums[4] = LCD_B4;
    panel_config.data_gpio_nums[5] = LCD_G0;
    panel_config.data_gpio_nums[6] = LCD_G1;
    panel_config.data_gpio_nums[7] = LCD_G2;
    panel_config.data_gpio_nums[8] = LCD_G3;
    panel_config.data_gpio_nums[9] = LCD_G4;
    panel_config.data_gpio_nums[10] = LCD_G5;
    panel_config.data_gpio_nums[11] = LCD_R0;
    panel_config.data_gpio_nums[12] = LCD_R1;
    panel_config.data_gpio_nums[13] = LCD_R2;
    panel_config.data_gpio_nums[14] = LCD_R3;
    panel_config.data_gpio_nums[15] = LCD_R4;
    panel_config.flags.fb_in_psram = 1;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    Serial.printf("[LCD] esp_lcd RGB panel OK  handle=%p\n", (void *)s_panel_handle);
}

// ============================================================
// Globals
// ============================================================

// LVGL draw buffers: small partial-render strips in PSRAM, separate from the
// panel's own frame buffer. Allocated in setup() (LCD_WIDTH * LVGL_BUF_LINES
// each). lvgl_flush_cb() hands each rendered strip to esp_lcd_panel_draw_bitmap(),
// which copies it into the panel's single internal frame buffer via the
// bounce-buffer pipeline — no direct access to the hardware frame buffer here.
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;

static lv_disp_drv_t disp_drv;
static lv_indev_drv_t indev_drv;

// Web server
WebServer server(80);
bool rebootPending = false;
unsigned long rebootStartTime = 0;

// Configuration
String wifiSSID = "";
String wifiPassword = "";
String syslogServerIP = "";
IPAddress syslogIP;
String deviceName = "touch-controller";
String tciHost = "";
uint16_t tciPort = 40001;
bool tciEnabled = false;

// mDNS peer discovery
PeerDiscovery peerDiscovery;

// Remote station data (polled via HTTP from LoRa gateway)
String relayLabels[6] = {"Relay 1", "Relay 2", "Relay 3", "Relay 4", "Relay 5", "Relay 6"};
bool relayStates[6] = {false};
bool relayDataReady = false;
bool remoteSleeping = false;
String lastStatusMessage = "";

// Battery/MPPT data (received from gateway)
struct BmsData
{
    bool connected = false;
    float voltage = 0;
    float soc = 0;
    float temp = -127;
    uint16_t cellMv[4] = {0};
    int cellCount = 0;
};
BmsData remoteBms[2];
float remoteMPPTPower = 0;
int remoteMPPTState = 0;
String remoteMPPTStateName = "Unknown";
bool remoteMPPTValid = false;
float remoteMPPTSystemVoltage = -1;
float remoteTemp1 = -127.0;
float remoteTemp2 = -127.0;
String remoteTemp1Label = "Enclosure";
String remoteTemp2Label = "Air";
unsigned long remoteStatusLastUpdate = 0;

// LoRa signal info
float lastLoRaRssi = 0.0f;
float lastLoRaSnr = 0.0f;
unsigned long lastLoRaRxTime = 0;
bool hasLoRaRx = false;
// Remote (paddock) gateway LoRa signal — what the remote side reports receiving
float remoteGwLoRaRssi = 0.0f;
float remoteGwLoRaSnr = 0.0f;
unsigned long remoteGwLoRaRxTime = 0;
bool hasRemoteGwLoRaRx = false;
static lv_obj_t *tabview = nullptr;
static lv_obj_t *tab_overview = nullptr;
static lv_obj_t *tab_power = nullptr;
static lv_obj_t *tab_antennas = nullptr;
static lv_obj_t *tab_rotator = nullptr;
static lv_obj_t *tab_propagation = nullptr;
static lv_obj_t *custom_tab_btns[5] = {nullptr};

static lv_obj_t *lbl_peers = nullptr;
static lv_obj_t *lbl_wifi = nullptr;
static lv_obj_t *lbl_uptime = nullptr;
static lv_obj_t *lbl_build = nullptr;
static lv_obj_t *lbl_overview_hw = nullptr;
static lv_obj_t *lbl_rotator_bearing = nullptr;
static lv_obj_t *btn_touch_reboot = nullptr;
#define MAX_PEER_ROWS 8
static lv_obj_t *peer_row_objs[MAX_PEER_ROWS] = {nullptr};
static lv_obj_t *peer_name_labels[MAX_PEER_ROWS] = {nullptr};
static lv_obj_t *peer_ip_labels[MAX_PEER_ROWS] = {nullptr};
static lv_obj_t *peer_site_labels[MAX_PEER_ROWS] = {nullptr};
static lv_obj_t *peer_status_labels[MAX_PEER_ROWS] = {nullptr};
static lv_obj_t *peer_reboot_btns[MAX_PEER_ROWS] = {nullptr};
static lv_obj_t *peer_uptime_labels[MAX_PEER_ROWS] = {nullptr};
static lv_obj_t *peer_build_labels[MAX_PEER_ROWS] = {nullptr};
static uint32_t peerUptimeSecs[PeerDiscovery::MAX_PEERS] = {0};
static char peerBuildDate[PeerDiscovery::MAX_PEERS][32] = {};

// Power tab UI elements
static lv_obj_t *power_relay_btns[6] = {nullptr};
static lv_obj_t *power_relay_labels[6] = {nullptr};
static lv_obj_t *btn_all_on_g  = nullptr; // global handles so pulse timer can reach them
static lv_obj_t *btn_all_off_g = nullptr;
static lv_obj_t *lbl_bat1_soc = nullptr;
static lv_obj_t *lbl_bat1_voltage = nullptr;
static lv_obj_t *lbl_bat2_soc = nullptr;
static lv_obj_t *lbl_bat2_voltage = nullptr;
static lv_obj_t *lbl_mppt_power = nullptr;
static lv_obj_t *lbl_mppt_state = nullptr;
static lv_obj_t *lbl_signal = nullptr;
static lv_obj_t *lbl_signal_rssi = nullptr;
static lv_obj_t *lbl_remote_signal = nullptr;
static lv_obj_t *lbl_remote_signal_rssi = nullptr;
static lv_obj_t *bar_remote_signal = nullptr;
static lv_obj_t *btn_gw_override = nullptr;   // tappable gateway override button
static lv_obj_t *lbl_gateway_route = nullptr; // inner label of btn_gw_override
static lv_obj_t *lbl_power_status = nullptr;
// 0=auto, 1=force-remote, 2=force-local
static volatile int loraGwOverride = 0;
static lv_obj_t *bar_bat1_soc = nullptr;
static lv_obj_t *bar_bat2_soc = nullptr;
static lv_obj_t *bar_signal = nullptr;
// WiFi link monitor card widgets (power tab)
static lv_obj_t *lbl_wifi_house_rssi        = nullptr;
static lv_obj_t *lbl_wifi_house_snr         = nullptr;
static lv_obj_t *lbl_wifi_house_tx          = nullptr;
static lv_obj_t *lbl_wifi_house_rx          = nullptr;
static lv_obj_t *bar_wifi_house_sig         = nullptr;
static lv_obj_t *meter_wifi_house           = nullptr;
static lv_meter_indicator_t *indic_house_tx = nullptr;
static lv_meter_indicator_t *indic_house_rx = nullptr;
static lv_obj_t *lbl_wifi_pad_rssi          = nullptr;
static lv_obj_t *lbl_wifi_pad_snr           = nullptr;
static lv_obj_t *lbl_wifi_pad_tx            = nullptr;
static lv_obj_t *lbl_wifi_pad_rx            = nullptr;
static lv_obj_t *bar_wifi_pad_sig           = nullptr;
static lv_obj_t *meter_wifi_pad             = nullptr;
static lv_meter_indicator_t *indic_pad_tx   = nullptr;
static lv_meter_indicator_t *indic_pad_rx   = nullptr;
static lv_obj_t *lbl_link_bolt              = nullptr;

// Antenna tab UI elements
#define MAX_ANTENNAS 8
#define MAX_ANT_GROUPS 8
static lv_obj_t *antenna_btns[MAX_ANTENNAS] = {nullptr};
static lv_obj_t *antenna_labels[MAX_ANTENNAS] = {nullptr};
static lv_obj_t *lbl_antenna_status = nullptr;
static lv_obj_t *antenna_group_rows[3] = {nullptr};
static lv_obj_t *antenna_group_labels[3] = {nullptr}; // group name labels
static lv_obj_t *lbl_vfo_a = nullptr;                 // VFO-A frequency display
static lv_obj_t *lbl_vfo_b = nullptr;                 // VFO-B frequency display
static lv_obj_t *band_tile_a = nullptr;               // band indicator card for VFO-A
static lv_obj_t *band_lbl_a  = nullptr;
static lv_obj_t *band_tile_b = nullptr;               // band indicator card for VFO-B
static lv_obj_t *band_lbl_b  = nullptr;
static double antennaVfoA = 0.0;                      // MHz from TCI
static double antennaVfoB = 0.0;
static bool antennaTciConnected = false;
static char antennaGroupNames[MAX_ANT_GROUPS][24] = {"VHF", "HF-A", "HF-B", "Group D", "Group E", "Group F", "Group G", "Group H"};
static lv_obj_t *antennaPendingBtn = nullptr;
static lv_timer_t *antennaPulseTimer = nullptr;
static bool antennaPulseState = false;

// Rotator tab UI elements
static lv_obj_t *lbl_rotator_status = nullptr;
static lv_obj_t *lbl_rotator_target = nullptr;
static lv_obj_t *lbl_rotator_speed = nullptr;
static lv_obj_t *btn_rotator_stop = nullptr;
static lv_obj_t *btn_rotator_enable = nullptr;
static lv_obj_t *canvas_map = nullptr;
// Two pixel buffers for the map:
//   map_base_buf — clean pixels (coastlines + city labels as pixels, no bearing lines)
//   map_buf      — what LVGL displays (base + bearing lines on top)
// updateBearingLinesDirect() writes bearing-line pixels straight into map_buf
// (MAP_SIZE-strided, MAP-relative coordinates) and calls lv_obj_invalidate() to
// have LVGL redraw canvas_map through the normal render pipeline — no absolute
// screen-coordinate tracking needed (see fbFillRect/fbDrawText/updateBearingLinesDirect).
static lv_obj_t *btn_manual_ccw = nullptr;
static lv_obj_t *btn_manual_cw = nullptr;
static lv_obj_t *btn_manual_stop = nullptr;
static lv_obj_t *rotator_memory_groups[3] = {nullptr};
static lv_obj_t *rotator_memory_grids[3]  = {nullptr}; // inner grid inside each group

// Rotator button pulse (flash until rotator confirms command)
static lv_obj_t *rotatorPendingBtn = nullptr;
static lv_timer_t *rotatorPulseTimer = nullptr;
static bool rotatorPulseState = false;
static lv_color_t gPendingBtnRestoreBg = {}; // bg to restore when pulse stops
static bool gRotatorAvailable = false;        // mirrors rotatorAvailable for use in callbacks

static int peerRowToIndex[MAX_PEER_ROWS] = {-1};
static bool manualRotating = false;
static int manualDirection = 0; // -1 CCW, +1 CW
static int pendingMapBearing = -1;
static lv_obj_t *pendingMapDialog = nullptr;

// Path selection dialog state (shown when a memory entry has both SP and LP bearings)
static lv_obj_t *pendingPathDialog = nullptr;
static int       pendingPathSP     = -1;
static int       pendingPathLP     = -1;
static lv_obj_t *pendingPathBtn    = nullptr;

// Path label badge drawn at top-right of map: "SP 355\xC2\xB0" or "LP 175\xC2\xB0"
static char g_pathLabel[20]  = "";
static bool g_pathLabelVisible = false;
static bool g_pathIsLP        = false; // colours the badge (blue=SP, purple=LP)

// Map city labels (LVGL objects overlaid on the map canvas)
#define MAX_MAP_LABELS 20
static lv_obj_t *map_city_labels[MAX_MAP_LABELS] = {nullptr};
// Indices into mapPoints[] for labelled cities
// London, Edinburgh, Reykjavik, Oslo, Paris, Berlin, Moscow, Istanbul, New York, Anchorage, Tokyo, Seoul, Beijing, Mumbai, Dubai, Sydney, CapeTown, BuenosAires, Nairobi, Singapore
static const int labeledCityIdx[MAX_MAP_LABELS] = {0, 10, 33, 20, 38, 41, 85, 79, 173, 197, 155, 154, 150, 128, 122, 161, 248, 227, 244, 145};

// Toast notification
static lv_obj_t *toast_label = nullptr;
static unsigned long toast_hide_time = 0;
static lv_obj_t *btn_zoom[3] = {nullptr};
static int currentZoom = 1; // 0=UK, 1=Europe, 2=World
static unsigned long lastTouchActivity = 0;

// QTH location (G7NRU)
static const float QTH_LAT = 53.00234f;
static const float QTH_LNG = -0.62763f;

#define MEM_PER_GROUP 15
struct RotatorMemoryPoint
{
    char name[24];
    int bearing;    // short-path bearing (0-359)
    int bearingLP = -1; // long-path bearing (0-359), or -1 if not stored
    bool active = true; // false = slot empty/not configured
};

static unsigned long lastUiUpdate = 0;
#define UI_UPDATE_INTERVAL 300
static volatile bool otaInProgress = false;

// ============================================================
// IP-based peer polling (discovered via mDNS)
// ============================================================

static unsigned long lastPeerPoll = 0;
#define PEER_POLL_INTERVAL 5000 // Poll each peer every 5s

#define HTTP_TIMEOUT_TOUCH_MS 350
#define HTTP_TIMEOUT_ROTATOR_MS 1000
#define HTTP_TIMEOUT_POLL_ONLINE_MS 300
#define HTTP_TIMEOUT_POLL_OFFLINE_MS 250
#define HTTP_TIMEOUT_PROP_MS 450

enum HttpCmdMethod
{
    HTTP_CMD_GET,
    HTTP_CMD_POST,
};

struct HttpCommand
{
    String url;
    HttpCmdMethod method;
    String body;
    uint16_t timeoutMs;
    bool isRotatorCmd; // if true, write HTTP result back to g_rotatorCmdHttpCode
    bool isJsonBody;   // if true, send Content-Type: application/json
    bool isRelayCmd;   // if true, write relay HTTP result back to g_relayCmdResult
    int8_t relayId;    // 0-5 = individual relay, 6 = all-on, 7 = all-off
    bool relayTarget;  // expected new state (true=ON) for individual relay; unused for all-on/off
};

#define HTTP_CMD_QUEUE_SIZE 16
static HttpCommand httpCmdQueue[HTTP_CMD_QUEUE_SIZE];
static uint8_t httpCmdHead = 0;
static uint8_t httpCmdTail = 0;

// Rotator data (polled from rotator-controller via HTTP)
static float rotatorBearing = -1;
static float rotatorTargetBearing = -1;
static float rotatorVoltage = 0;
static bool rotatorCalibrated = false;
static bool rotatorEnabled = false;
static bool rotatorMoving = false;
static int rotatorMotorSpeed = 0;     // 0-255 PWM
static int rotatorMotorDirection = 0; // 0=stopped, 1=CW, -1=CCW
static unsigned long rotatorLastUpdate = 0;
static unsigned long rotatorFastPollUntil = 0; // epoch ms: fast-poll active until this time
static unsigned long lastRotatorFastPoll = 0;
static unsigned long rotatorCommandSentAt = 0; // epoch ms of last goto/manual command
// HTTP result of the last rotator command (0=pending, -1=conn fail, else HTTP code).
// Written by poll task, read by UI. Volatile is sufficient: single writer, single reader.
static volatile int g_rotatorCmdHttpCode = 0;

// Relay command confirmation state.
// Written atomically by the poll task; read by update_power_tab on the main loop.
// relayPendingMask: bitmask of relays currently waiting for confirmation (0 = none pending).
// relayHttpCode:    HTTP result of the last relay command (-2=not yet sent, 0=in-flight,
//                   -1=connection failure, else HTTP status code).
// relaySentAt:      millis() when the command was enqueued (for timeout detection).
// relayAllPending:  6=all-on pending, 7=all-off pending, -1=no all-command pending.
#define RELAY_CMD_TIMEOUT_MS 8000 // 8 s before showing failure toast
static volatile uint8_t g_relayPendingMask = 0;    // bitmask, bits 0-5
static volatile int8_t  g_relayAllPending = -1;     // 6=all-on, 7=all-off, -1=none
static volatile int     g_relayHttpCode   = -2;     // -2=idle
static unsigned long    g_relaySentAt     = 0;
// Expected target state per relay (true=ON) — set at command time, cleared on confirm/timeout.
static bool             g_relayPendingTarget[6] = {false};
// Last server-confirmed states (copied from relayStates only after a successful poll).
// The buttons show these values when no command is pending, so they can't flicker.
static bool             g_relayConfirmed[6] = {false};
static bool             g_relayConfirmedReady = false; // becomes true after first poll
// Pulse state for the flashing relay buttons
static bool             g_relayPulseState = false;
static lv_timer_t      *g_relayPulseTimer = nullptr;
static unsigned long lastVfoPoll = 0; // dedicated fast VFO frequency poll
static bool mapDirty = true;
static bool mapBaseDirty = true;
static float lastMapBearing = -9999.0f;
static float lastMapTargetBearing = -9999.0f;
static bool lastMapMoving = false;
static bool lastMapOnline = false;
static int lastMapZoom = -1;

// Antenna data (polled from antenna-controller via HTTP)
struct AntennaInfo
{
    char name[32];
    int id;
    int group;
    bool active;
    bool freqMatch; // true = antenna is suitable for the current VFO-A frequency
};
static AntennaInfo antennas[MAX_ANTENNAS];
static int antennaBtnIds[MAX_ANTENNAS] = {-1};
static int8_t prevAntennaState[MAX_ANTENNAS] = {-1, -1, -1, -1, -1, -1, -1, -1};
static int antennaCount = 0;
static bool antennaDataReady = false;
static unsigned long antennaLastUpdate = 0;

static unsigned long lastPropProxyPoll = 0;
static unsigned long lastLinkPoll = 0;
#define PROP_PROXY_POLL_INTERVAL 30000
#define LINK_POLL_INTERVAL       30000

// Rotator memory bank: fetched from rotator /api/memory on startup and periodically
static unsigned long lastRotatorMemoryFetch = 0;
static bool rotatorMemoryLoaded = false;
static volatile bool rotatorMemoryDirty = false;
#define ROTATOR_MEMORY_FETCH_INTERVAL 300000 // re-fetch every 5 minutes

// Mutex protecting all state shared between the main loop (core 1) and the
// background poll task (core 0): peer table, relay/rotator/antenna/solar data.
static SemaphoreHandle_t g_dataMutex = nullptr;

// ============================================================
// Propagation Data
// ============================================================

#define PROP_NUM_BANDS 12

struct PropBandInfo
{
    const char *name;
    uint32_t freqLow;
    uint32_t freqHigh;
    int hfGroupIndex; // 0-3 for hamqsl band groups, -1 = none
};

static const PropBandInfo propBands[PROP_NUM_BANDS] = {
    {"160m", 1800000, 2000000, -1},
    {"80m", 3500000, 4000000, 0},
    {"60m", 5300000, 5400000, -1},
    {"40m", 7000000, 7300000, 0},
    {"30m", 10100000, 10150000, 1},
    {"20m", 14000000, 14350000, 1},
    {"17m", 18068000, 18168000, 2},
    {"15m", 21000000, 21450000, 2},
    {"12m", 24890000, 24990000, 3},
    {"10m", 28000000, 29700000, 3},
    {"6m", 50000000, 54000000, -1},
    {"2m", 144000000, 148000000, -1},
};

struct PSKBandData
{
    int maxTxKm  = 0;
    int maxRxKm  = 0;
    char condition[12] = "";
};

struct SolarPropData
{
    int sfi = 0;
    int kIndex = 0;
    int aIndex = 0;
    int ssn = 0;
    char geoMag[20] = "";
    char signalNoise[8] = "";
    char hfCondDay[4][12] = {{0}, {0}, {0}, {0}};
    char hfCondNight[4][12] = {{0}, {0}, {0}, {0}};
    char vhfESkipEU[24] = "";
    char vhfAurora[24] = "";
    // PSK Reporter per-band data (index matches propBands[])
    PSKBandData psk[PROP_NUM_BANDS] = {};
    // Space weather (NOAA SWPC, 1-min cadence)
    bool  recentlyDisturbed = false;
    float bz = 0.0f;
    int   bzAgeSeconds = -1;
    char  xrayClass[12] = "";
    int   xrayAgeSeconds = -1;
    // Gray line (server-computed from home QTH)
    char sunriseUtc[8] = "";
    char sunsetUtc[8]  = "";
    int  minsToGrayLine = 0;
    // Data age (server-reported)
    int  ageSeconds    = -1;
    int  pskAgeSeconds = -1;
    bool valid = false;
    unsigned long lastUpdate = 0;
};
static SolarPropData solarData;
static uint32_t s_propDataVersion = 0;  // incremented each time prop data is parsed

// WiFi link monitor data (from PropProxy /api/link, Ubiquiti airOS SNMP)
struct LinkEndpointData {
    char name[32];
    char ip[20];
    int  rssi;        // dBm (negative)
    int  noise;       // dBm (negative)
    int  snr;         // dB
    int  txRateMbps;
    int  rxRateMbps;
    bool connected;
};
struct LinkMonData {
    LinkEndpointData house;
    LinkEndpointData paddock;
    bool valid;
};
static LinkMonData s_linkData;
static uint32_t s_linkDataVersion = 0;

static RotatorMemoryPoint rotatorMemories[3][MEM_PER_GROUP] = {
    // UK (bearings from G7NRU, Leadenham, Lincs)
    {{"London", 157}, {"Belfast", 308}, {"Edinburgh", 347}, {"Cardiff", 250}, {"Plymouth", 228}, {"Norwich", 118}, {"Leeds", 339}, {"Newcastle", 357}, {"Bristol", 232}, {"Dover", 140}, {"Manchester", 328}, {"Glasgow", 343}, {"Birmingham", 236}, {"Liverpool", 295}, {"Hull", 73}},
    // Europe
    {{"Paris", 149}, {"Berlin", 92}, {"Rome", 139}, {"Madrid", 212}, {"Prague", 106}, {"Warsaw", 84}, {"Stockholm", 49}, {"Vienna", 111}, {"Lisbon", 221}, {"Helsinki", 39}, {"Reykjavik", 331}, {"Istanbul", 118}, {"Amsterdam", 102}, {"Zurich", 130}, {"Athens", 129}},
    // World
    {{"New York", 287}, {"Los Angeles", 323}, {"Tokyo", 34}, {"Sydney", 60}, {"Moscow", 70}, {"Delhi", 78}, {"Cairo", 132}, {"Nairobi", 146}, {"Sao Paulo", 234}, {"Singapore", 70}, {"Honolulu", 347}, {"Cape Town", 173}, {"Beijing", 45}, {"Buenos Aires", 233}, {"Toronto", 302}}};

static void copyText(char *dst, size_t dstSize, const String &src)
{
    if (!dst || dstSize == 0)
        return;
    strncpy(dst, src.c_str(), dstSize - 1);
    dst[dstSize - 1] = '\0';
}

static void drawAzimuthalMap();
static void rebuildRotatorMemoryButtons();
static void showToast(const char *msg, lv_color_t bg_color, uint32_t duration_ms = 2000);

static void parsePropagationPayload(const JsonDocument &doc)
{
    JsonVariantConst root = doc.as<JsonVariantConst>();
    JsonVariantConst solar = root;
    JsonVariantConst hf = root;
    JsonVariantConst vhf = root;

    if (!root["solar"].isNull())
        solar = root["solar"];
    if (!root["hf"].isNull())
        hf = root["hf"];
    if (!root["vhf"].isNull())
        vhf = root["vhf"];

    solarData.sfi = solar["sfi"] | solarData.sfi;
    solarData.kIndex = solar["kIndex"] | (solar["k"] | solarData.kIndex);
    solarData.aIndex = solar["aIndex"] | (solar["a"] | solarData.aIndex);
    solarData.ssn = solar["ssn"] | solarData.ssn;

    for (int i = 0; i < 4; i++)
    {
        // Nested format: hf.day[i] / hf.night[i]
        if (hf["day"].is<JsonArrayConst>() && i < (int)hf["day"].as<JsonArrayConst>().size())
            copyText(solarData.hfCondDay[i], sizeof(solarData.hfCondDay[i]), hf["day"][i].as<String>());
        else if (root["hfCondDay"].is<JsonArrayConst>() && i < (int)root["hfCondDay"].as<JsonArrayConst>().size())
            copyText(solarData.hfCondDay[i], sizeof(solarData.hfCondDay[i]), root["hfCondDay"][i].as<String>());

        if (hf["night"].is<JsonArrayConst>() && i < (int)hf["night"].as<JsonArrayConst>().size())
            copyText(solarData.hfCondNight[i], sizeof(solarData.hfCondNight[i]), hf["night"][i].as<String>());
        else if (root["hfCondNight"].is<JsonArrayConst>() && i < (int)root["hfCondNight"].as<JsonArrayConst>().size())
            copyText(solarData.hfCondNight[i], sizeof(solarData.hfCondNight[i]), root["hfCondNight"][i].as<String>());
    }

    // Nested format: vhf.es / vhf.aurora  — or flat: vhfESkipEU / vhfAurora
    if (vhf["es"])
        copyText(solarData.vhfESkipEU, sizeof(solarData.vhfESkipEU), vhf["es"].as<String>());
    else if (root["vhfESkipEU"])
        copyText(solarData.vhfESkipEU, sizeof(solarData.vhfESkipEU), root["vhfESkipEU"].as<String>());
    if (vhf["aurora"])
        copyText(solarData.vhfAurora, sizeof(solarData.vhfAurora), vhf["aurora"].as<String>());
    else if (root["vhfAurora"])
        copyText(solarData.vhfAurora, sizeof(solarData.vhfAurora), root["vhfAurora"].as<String>());

    if (root["geoMag"])
        copyText(solarData.geoMag, sizeof(solarData.geoMag), root["geoMag"].as<String>());
    if (root["signalNoise"])
        copyText(solarData.signalNoise, sizeof(solarData.signalNoise), root["signalNoise"].as<String>());

    // Space weather
    if (!root["recentlyDisturbed"].isNull())
        solarData.recentlyDisturbed = root["recentlyDisturbed"].as<bool>();
    if (!root["bz"].isNull())
        solarData.bz = root["bz"].as<float>();
    if (!root["bzAgeSeconds"].isNull())
        solarData.bzAgeSeconds = root["bzAgeSeconds"].as<int>();
    if (root["xrayClass"])
        copyText(solarData.xrayClass, sizeof(solarData.xrayClass), root["xrayClass"].as<String>());
    if (!root["xrayAgeSeconds"].isNull())
        solarData.xrayAgeSeconds = root["xrayAgeSeconds"].as<int>();

    // Gray line (computed server-side from home QTH coordinates)
    if (root["sunriseUtc"])
        copyText(solarData.sunriseUtc, sizeof(solarData.sunriseUtc), root["sunriseUtc"].as<String>());
    if (root["sunsetUtc"])
        copyText(solarData.sunsetUtc, sizeof(solarData.sunsetUtc), root["sunsetUtc"].as<String>());
    if (!root["minsToGrayLine"].isNull())
        solarData.minsToGrayLine = root["minsToGrayLine"].as<int>();

    // Server-reported data age
    if (!root["ageSeconds"].isNull())
        solarData.ageSeconds = root["ageSeconds"].as<int>();
    if (!root["pskAgeSeconds"].isNull())
        solarData.pskAgeSeconds = root["pskAgeSeconds"].as<int>();

    // PSK Reporter per-band propagation (object keyed by band name, e.g. "20m")
    if (root["propagation"].is<JsonObjectConst>())
    {
        JsonObjectConst prop = root["propagation"].as<JsonObjectConst>();
        for (int i = 0; i < PROP_NUM_BANDS; i++)
        {
            JsonVariantConst b = prop[propBands[i].name];
            if (!b.isNull())
            {
                if (!b["maxTxKm"].isNull())
                    solarData.psk[i].maxTxKm = b["maxTxKm"].as<int>();
                if (!b["maxRxKm"].isNull())
                    solarData.psk[i].maxRxKm = b["maxRxKm"].as<int>();
                if (b["condition"])
                    copyText(solarData.psk[i].condition, sizeof(solarData.psk[i].condition),
                             b["condition"].as<String>());
            }
        }
    }

    solarData.valid = true;
    solarData.lastUpdate = millis();
    s_propDataVersion++;
}

// Propagation tab UI
static lv_obj_t *prop_band_cards[PROP_NUM_BANDS]    = {nullptr};
static lv_obj_t *prop_band_cond_lbl[PROP_NUM_BANDS] = {nullptr};
static lv_obj_t *prop_band_tx_bar[PROP_NUM_BANDS]   = {nullptr};  // TX km bar
static lv_obj_t *prop_band_rx_bar[PROP_NUM_BANDS]   = {nullptr};  // RX km bar
static lv_obj_t *prop_sfi_val = nullptr;
static lv_obj_t *prop_k_val = nullptr;
static lv_obj_t *prop_a_val = nullptr;
static lv_obj_t *prop_ssn_val = nullptr;
static lv_obj_t *prop_sfi_meter = nullptr;
static lv_obj_t *prop_k_meter = nullptr;
static lv_obj_t *prop_a_meter = nullptr;
static lv_obj_t *prop_ssn_meter = nullptr;
static lv_meter_indicator_t *prop_sfi_needle = nullptr;
static lv_meter_indicator_t *prop_k_needle = nullptr;
static lv_meter_indicator_t *prop_a_needle = nullptr;
static lv_meter_indicator_t *prop_ssn_needle = nullptr;
static lv_obj_t *prop_footer_solar_lbl = nullptr;  // Bz, X-ray, gray line, sunrise/sunset
static lv_obj_t *prop_vhf_lbl = nullptr;            // Es, Aurora, S/N
static lv_obj_t *prop_updated_lbl = nullptr;        // data freshness

// Find a reachable LoRa gateway/remote peer
// Prefer the remote (paddock) gateway - it's the direct controller and faster.
// Fall back to local (thelimes) gateway if remote is unreachable.
static DiscoveredPeer *findLoRaPeer()
{
    // Scan all peers to find remote vs local gateways
    DiscoveredPeer *remoteGw = nullptr;
    DiscoveredPeer *localGw = nullptr;
    int count = peerDiscovery.peerCount();
    const DiscoveredPeer *peers = peerDiscovery.peers();
    for (int i = 0; i < count; i++)
    {
        if (strcmp(peers[i].role, "lora-gateway") == 0 || strcmp(peers[i].role, "lora-remote") == 0)
        {
            if (strcmp(peers[i].site, "paddock") == 0)
                remoteGw = const_cast<DiscoveredPeer *>(&peers[i]);
            else
                localGw = const_cast<DiscoveredPeer *>(&peers[i]);
        }
    }
    // Respect manual override (volatile read is safe on 32-bit ESP32)
    int ovr = loraGwOverride;
    if (ovr == 1)
        return remoteGw ? remoteGw : localGw; // force remote, fall back if missing
    if (ovr == 2)
        return localGw ? localGw : remoteGw; // force local, fall back if missing
    // Auto: prefer remote if reachable
    if (remoteGw && remoteGw->reachable)
        return remoteGw;
    // Fall back to local if reachable
    if (localGw && localGw->reachable)
        return localGw;
    // Return whichever exists (even if unreachable)
    return remoteGw ? remoteGw : localGw;
}

static bool enqueueHttpCommand(const String &url, HttpCmdMethod method, const String &body = "", uint16_t timeoutMs = HTTP_TIMEOUT_TOUCH_MS)
{
    uint8_t nextTail = (httpCmdTail + 1) % HTTP_CMD_QUEUE_SIZE;
    if (nextTail == httpCmdHead)
    {
        debugLog("[HTTPQ] Queue full, dropping command");
        return false;
    }

    httpCmdQueue[httpCmdTail].url = url;
    httpCmdQueue[httpCmdTail].method = method;
    httpCmdQueue[httpCmdTail].body = body;
    httpCmdQueue[httpCmdTail].timeoutMs = timeoutMs;
    httpCmdQueue[httpCmdTail].isRotatorCmd = false;
    httpCmdQueue[httpCmdTail].isJsonBody = false;
    httpCmdQueue[httpCmdTail].isRelayCmd = false;
    httpCmdQueue[httpCmdTail].relayId = -1;
    httpCmdQueue[httpCmdTail].relayTarget = false;
    httpCmdTail = nextTail;
    return true;
}

static void processHttpCommandQueue()
{
    // Dequeue one command under the mutex, then perform the HTTP call without
    // holding it. The queue is a single-producer (main loop) / single-consumer
    // (poll task) ring buffer; only advancing the head index needs protection.
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    if (httpCmdHead == httpCmdTail)
    {
        xSemaphoreGive(g_dataMutex);
        return;
    }
    HttpCommand cmd = httpCmdQueue[httpCmdHead];
    httpCmdHead = (httpCmdHead + 1) % HTTP_CMD_QUEUE_SIZE;
    xSemaphoreGive(g_dataMutex);

    HTTPClient http;
    http.setTimeout(cmd.timeoutMs);
    if (!http.begin(cmd.url))
    {
        debugLogf("[HTTPQ] begin failed: %s", cmd.url.c_str());
        return;
    }

    int code = -1;
    if (cmd.method == HTTP_CMD_POST)
    {
        if (cmd.body.length() > 0)
            http.addHeader("Content-Type", cmd.isJsonBody ? "application/json" : "application/x-www-form-urlencoded");
        code = http.POST(cmd.body);
    }
    else
    {
        code = http.GET();
    }
    http.end();

    if (code < 0)
        debugLogf("[HTTPQ] request failed (%d): %s", code, cmd.url.c_str());

    if (cmd.isRotatorCmd)
        g_rotatorCmdHttpCode = (code < 0) ? -1 : code;

    if (cmd.isRelayCmd)
        g_relayHttpCode = (code < 0) ? -1 : code;
}

// Poll a peer's /api/status via HTTP and update local state.
// CALLED WITH g_dataMutex HELD. Snapshots peer address fields, releases
// the mutex for the HTTP call, then reacquires before writing results.
// Mutex is held on return; the caller is responsible for releasing it.
static void pollPeerStatus(DiscoveredPeer &peer, int peerIdx = -1)
{
    if (strlen(peer.ip) == 0)
        return;

    // Snapshot while mutex is held
    char snapIp[16], snapRole[32], snapName[32], snapSite[16];
    uint16_t snapPort;
    bool snapReachable;
    memcpy(snapIp, peer.ip, sizeof(snapIp));
    memcpy(snapRole, peer.role, sizeof(snapRole));
    memcpy(snapName, peer.name, sizeof(snapName));
    memcpy(snapSite, peer.site, sizeof(snapSite));
    snapPort = peer.port;
    snapReachable = peer.reachable;

    xSemaphoreGive(g_dataMutex); // release during HTTP

    HTTPClient http;
    String url = "http://" + String(snapIp) + ":" + String(snapPort) + "/api/status";
    http.setTimeout(snapReachable ? HTTP_TIMEOUT_POLL_ONLINE_MS : HTTP_TIMEOUT_POLL_OFFLINE_MS);
    http.begin(url);
    int code = http.GET();
    String body;
    if (code == 200)
        body = http.getString();
    http.end();

    xSemaphoreTake(g_dataMutex, portMAX_DELAY); // reacquire for writes

    peer.lastPolled = millis();

    if (code != 200)
    {
        // Prop peers (PropProxy) don't serve /api/status — treat as reachable if TCP connected
        bool isPropPeer = strstr(snapRole, "prop") != nullptr || strstr(snapName, "propproxy") != nullptr;
        if (!isPropPeer)
            peer.reachable = false;
        return; // mutex held; caller releases
    }

    peer.reachable = true;
    peer.lastSuccess = millis();

    JsonDocument doc;
    if (deserializeJson(doc, body))
        return; // mutex held; caller releases

    // Store uptime and build date
    if (peerIdx >= 0 && peerIdx < PeerDiscovery::MAX_PEERS)
    {
        peerUptimeSecs[peerIdx] = (doc["uptimeMs"] | (uint32_t)0) / 1000;
        const char *bd = doc["buildDate"] | "";
        strncpy(peerBuildDate[peerIdx], bd, 31);
        peerBuildDate[peerIdx][31] = '\0';
    }

    // LoRa Remote/Gateway: update relay states, battery, MPPT, signal
    if (strcmp(snapRole, "lora-remote") == 0 || strcmp(snapRole, "lora-gateway") == 0)
    {
        // If responding to HTTP, it's awake and has data
        relayDataReady = true;
        remoteSleeping = false;

        // Relay states
        if (doc["states"].is<JsonArray>())
        {
            for (int i = 0; i < 6; i++)
            {
                if (doc["states"][i].is<bool>())
                    relayStates[i] = doc["states"][i].as<bool>();
                if (doc["labels"][i].is<const char *>())
                    relayLabels[i] = doc["labels"][i].as<String>();
            }
        }

        // Dual BMS
        if (doc["dalyBms"].is<JsonArray>())
        {
            JsonArray bmsArr = doc["dalyBms"].as<JsonArray>();
            for (int i = 0; i < 2 && i < (int)bmsArr.size(); i++)
            {
                JsonObject bms = bmsArr[i];
                remoteBms[i].connected = bms["connected"] | false;
                remoteBms[i].voltage = bms["voltage"] | 0.0f;
                remoteBms[i].soc = bms["soc"] | 0.0f;
                remoteBms[i].temp = bms["tempHigh"] | -127.0f;
                if (bms["cells"].is<JsonArray>())
                {
                    JsonArray cells = bms["cells"].as<JsonArray>();
                    remoteBms[i].cellCount = cells.size();
                    for (int c = 0; c < 4 && c < (int)cells.size(); c++)
                        remoteBms[i].cellMv[c] = cells[c] | 0;
                }
            }
        }

        // MPPT
        if (doc["mppt"].is<JsonObject>())
        {
            remoteMPPTPower = doc["mppt"]["power"] | 0.0f;
            remoteMPPTState = doc["mppt"]["state"] | 0;
            remoteMPPTStateName = doc["mppt"]["stateName"] | "Unknown";
            remoteMPPTValid = doc["mppt"]["valid"] | false;
            remoteMPPTSystemVoltage = doc["mppt"]["systemVoltage"] | -1.0f;
        }

        // Temperatures
        if (doc["temperatures"].is<JsonArray>())
        {
            JsonArray temps = doc["temperatures"].as<JsonArray>();
            if (temps.size() > 0)
            {
                remoteTemp1 = temps[0]["celsius"] | -127.0f;
                remoteTemp1Label = temps[0]["label"] | "Enclosure";
            }
            if (temps.size() > 1)
            {
                remoteTemp2 = temps[1]["celsius"] | -127.0f;
                remoteTemp2Label = temps[1]["label"] | "Air";
            }
        }

        // LoRa signal (split by site so we track local and remote views separately)
        if (doc["lora"].is<JsonObject>())
        {
            bool valid = doc["lora"]["valid"] | false;
            float rssi = doc["lora"]["rssi"] | 0.0f;
            float snr = doc["lora"]["snr"] | 0.0f;
            if (strcmp(snapSite, "paddock") == 0)
            {
                hasRemoteGwLoRaRx = valid;
                remoteGwLoRaRssi = rssi;
                remoteGwLoRaSnr = snr;
                if (valid)
                    remoteGwLoRaRxTime = millis();
            }
            else
            {
                hasLoRaRx = valid;
                lastLoRaRssi = rssi;
                lastLoRaSnr = snr;
                if (valid)
                    lastLoRaRxTime = millis();
            }
        }

        remoteStatusLastUpdate = millis();
    }

    // Rotator Controller: update bearing, enabled state
    if (strcmp(snapRole, "rotator-controller") == 0)
    {
        if (doc["rotator"].is<JsonObject>())
        {
            float newBearing = doc["rotator"]["currentPosition"] | -1.0f;
            float newTarget = doc["rotator"]["targetPosition"] | -1.0f;
            float newVoltage = doc["rotator"]["positionVoltage"] | 0.0f;
            bool newCalibrated = doc["rotator"]["calibrated"] | false;
            bool newEnabled = doc["rotator"]["enabled"] | false;
            bool newMoving = doc["rotator"]["motorRunning"] | false;
            int newMotorSpeed = doc["rotator"]["motorSpeed"] | 0;
            int newMotorDirection = doc["rotator"]["motorDirection"] | 0;

            // Only flag a mandatory redraw for discrete state changes (moving on/off).
            // Continuous bearing drift is handled by the 1.0° threshold in the
            // update_ui guard — setting mapDirty here on every 0.5° ADC wobble
            // was bypassing that guard and causing visible flicker.
            if (newMoving != rotatorMoving)
                mapDirty = true;

            rotatorBearing = newBearing;
            rotatorTargetBearing = newTarget;
            rotatorVoltage = newVoltage;
            rotatorCalibrated = newCalibrated;
            rotatorEnabled = newEnabled;
            rotatorMoving = newMoving;
            rotatorMotorSpeed = newMotorSpeed;
            rotatorMotorDirection = newMotorDirection;
        }
        rotatorLastUpdate = millis();
    }

    // Antenna Controller: update antenna states
    if (strcmp(snapRole, "antenna-controller") == 0)
    {
        // Parse group names from API
        if (doc["groups"].is<JsonArray>())
        {
            JsonArray grps = doc["groups"].as<JsonArray>();
            for (int i = 0; i < (int)grps.size() && i < MAX_ANT_GROUPS; i++)
            {
                JsonObject g = grps[i];
                int gid = g["id"] | i;
                const char *gname = g["name"] | (const char *)nullptr;
                if (gname && gid < MAX_ANT_GROUPS)
                    strncpy(antennaGroupNames[gid], gname, sizeof(antennaGroupNames[0]) - 1);
            }
        }
        if (doc["antennas"].is<JsonArray>())
        {
            JsonArray arr = doc["antennas"].as<JsonArray>();
            antennaCount = 0;
            for (int i = 0; i < (int)arr.size() && i < MAX_ANTENNAS; i++)
            {
                JsonObject a = arr[i];
                strncpy(antennas[i].name, a["name"] | "Antenna", sizeof(antennas[i].name) - 1);
                antennas[i].id = a["id"] | i;
                antennas[i].group = a["group"] | 255;
                if (antennas[i].group == 255)
                    continue; // skip antennas not assigned to any relay group
                antennas[i].active = a["active"] | false;
                antennas[i].freqMatch = a["freqMatch"] | true;
                antennaCount++;
            }
            antennaDataReady = true;
        }
        // Parse VFO frequencies from TCI data
        if (doc["tci"].is<JsonObject>())
        {
            JsonObject tci = doc["tci"].as<JsonObject>();
            antennaVfoA = tci["frequencyA"] | 0.0;
            antennaVfoB = tci["frequencyB"] | 0.0;
            antennaTciConnected = tci["connected"] | false;
        }
        antennaLastUpdate = millis();
    }

    // Pi5 propagation proxy
    if (strstr(snapRole, "prop") != nullptr || strstr(snapName, "propproxy") != nullptr)
    {
        parsePropagationPayload(doc);
    }
    // mutex held; caller releases
}

// Poll all discovered peers
static int pollRoundRobinIdx = 0;
static void pollAllPeers()
{
    unsigned long now = millis();
    if (now - lastPeerPoll < PEER_POLL_INTERVAL)
        return;
    lastPeerPoll = now;

    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    int count = peerDiscovery.peerCount();
    if (count > 0)
    {
        const DiscoveredPeer *peers = peerDiscovery.peers();
        // Round-robin: start search from where we left off so every peer gets polled
        for (int n = 0; n < count; n++)
        {
            int i = (pollRoundRobinIdx + n) % count;
            DiscoveredPeer &peer = const_cast<DiscoveredPeer &>(peers[i]);
            unsigned long interval = peer.reachable ? PEER_POLL_INTERVAL : (PEER_POLL_INTERVAL * 4);
            if (now - peer.lastPolled >= interval)
            {
                pollPeerStatus(peer, i); // releases mutex during HTTP, reacquires after
                pollRoundRobinIdx = (i + 1) % count;
                break;
            }
        }
    }
    xSemaphoreGive(g_dataMutex);
}

// Poll the rotator every 1s when a command was recently sent or motor is moving,
// every 2s otherwise.
static void pollRotatorFast()
{
    unsigned long now = millis();
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    bool active = (now < rotatorFastPollUntil) || rotatorMoving;
    unsigned long interval = active ? 300 : 2000;
    if (now - lastRotatorFastPoll < interval)
    {
        xSemaphoreGive(g_dataMutex);
        return;
    }
    lastRotatorFastPoll = now;

    DiscoveredPeer *rot = peerDiscovery.findByRole("rotator-controller");
    if (!rot || strlen(rot->ip) == 0)
    {
        xSemaphoreGive(g_dataMutex);
        return;
    }

    // Find peer index so peerUptimeSecs is updated too
    int count = peerDiscovery.peerCount();
    const DiscoveredPeer *peers = peerDiscovery.peers();
    int idx = -1;
    for (int i = 0; i < count; i++)
    {
        if (&peers[i] == rot)
        {
            idx = i;
            break;
        }
    }
    pollPeerStatus(*rot, idx); // releases mutex during HTTP, reacquires after
    // Extend fast-poll while still moving
    if (rotatorMoving)
        rotatorFastPollUntil = max(rotatorFastPollUntil, now + 3000);
    xSemaphoreGive(g_dataMutex);
}

// Poll /api/tci on the antenna-controller every 300 ms for near-real-time VFO display
static void pollVfoFast()
{
    unsigned long now = millis();
    if (now - lastVfoPoll < 300)
        return;
    lastVfoPoll = now;

    // Snapshot peer address under mutex
    char ip[16] = {0};
    uint16_t port = 80;
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    DiscoveredPeer *ant = peerDiscovery.findByRole("antenna-controller");
    if (ant && strlen(ant->ip) > 0)
    {
        strncpy(ip, ant->ip, sizeof(ip) - 1);
        port = ant->port;
    }
    xSemaphoreGive(g_dataMutex);

    if (ip[0] == 0)
        return;

    String url = "http://" + String(ip) + ":" + String(port) + "/api/tci";
    HTTPClient http;
    http.begin(url);
    http.setTimeout(HTTP_TIMEOUT_POLL_ONLINE_MS);
    int code = http.GET();
    if (code == 200)
    {
        String body = http.getString();
        http.end();
        JsonDocument doc;
        if (deserializeJson(doc, body) == DeserializationError::Ok)
        {
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            antennaTciConnected = doc["connected"] | false;
            antennaVfoA = doc["frequencyA"] | 0.0;
            antennaVfoB = doc["frequencyB"] | 0.0;

            // Fast-path: update active antenna highlights without waiting for the
            // 5-second /api/status peer poll.  Antenna names/groups are still
            // populated by the full poll; this only refreshes the active flags.
            if (antennaDataReady && doc["activeAntennas"].is<JsonArray>())
            {
                for (int i = 0; i < antennaCount; i++)
                    antennas[i].active = false;
                for (JsonVariant v : doc["activeAntennas"].as<JsonArray>())
                {
                    int id = v.as<int>();
                    for (int i = 0; i < antennaCount; i++)
                    {
                        if (antennas[i].id == id)
                        {
                            antennas[i].active = true;
                            break;
                        }
                    }
                }
                antennaLastUpdate = millis(); // keep stale-data check satisfied
            }
            xSemaphoreGive(g_dataMutex);
        }
    }
    else
    {
        http.end();
    }
}

static void pollPropagationProxy()
{
    if (millis() - lastPropProxyPoll < PROP_PROXY_POLL_INTERVAL)
        return;
    lastPropProxyPoll = millis();

    // Snapshot all prop-peer addresses under the mutex so HTTP calls are unlocked.
    struct PropSnap
    {
        char ip[16];
        uint16_t port;
    };
    PropSnap snaps[PeerDiscovery::MAX_PEERS];
    int snapCount = 0;
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    {
        int count = peerDiscovery.peerCount();
        const DiscoveredPeer *peers = peerDiscovery.peers();
        debugLogf("[PROP] poll: %d total peers", count);
        for (int i = 0; i < count && snapCount < PeerDiscovery::MAX_PEERS; i++)
        {
            const DiscoveredPeer &p = peers[i];
            debugLogf("[PROP] peer[%d] name=%s role=%s ip=%s port=%d", i, p.name, p.role, p.ip, p.port);
            if (!strlen(p.ip))
                continue;
            bool isPropPeer = strstr(p.role, "prop") != nullptr || strstr(p.name, "propproxy") != nullptr || strstr(p.name, "pi5") != nullptr;
            if (!isPropPeer)
                continue;
            memcpy(snaps[snapCount].ip, p.ip, 16);
            snaps[snapCount].port = p.port;
            snapCount++;
        }
    }
    xSemaphoreGive(g_dataMutex);

    debugLogf("[PROP] snapCount=%d", snapCount);
    for (int s = 0; s < snapCount; s++)
    {
        HTTPClient http;
        String base = "http://" + String(snaps[s].ip) + ":" + String(snaps[s].port);
        String urls[2] = {base + "/api/propagation", base + "/api/status"};
        for (int u = 0; u < 2; u++)
        {
            debugLogf("[PROP] GET %s", urls[u].c_str());
            http.setTimeout(HTTP_TIMEOUT_PROP_MS);
            if (!http.begin(urls[u]))
            {
                debugLog("[PROP] begin() failed");
                continue;
            }
            int code = http.GET();
            debugLogf("[PROP] HTTP %d", code);
            if (code == 200)
            {
                String body = http.getString();
                http.end();
                JsonDocument doc;
                DeserializationError err = deserializeJson(doc, body);
                if (!err)
                {
                    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
                    parsePropagationPayload(doc);
                    xSemaphoreGive(g_dataMutex);
                    debugLogf("[PROP] parsed OK, sfi=%d valid=%d", doc["sfi"].as<int>(), doc["valid"].as<bool>());
                    return;
                }
                debugLogf("[PROP] JSON parse error: %s", err.c_str());
                continue;
            }
            http.end();
        }
    }
}

static void pollLinkMonitor()
{
    if (millis() - lastLinkPoll < LINK_POLL_INTERVAL)
        return;
    lastLinkPoll = millis();

    struct LinkSnap { char ip[16]; uint16_t port; };
    LinkSnap snaps[PeerDiscovery::MAX_PEERS];
    int snapCount = 0;
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    {
        int count = peerDiscovery.peerCount();
        const DiscoveredPeer *peers = peerDiscovery.peers();
        for (int i = 0; i < count && snapCount < PeerDiscovery::MAX_PEERS; i++)
        {
            const DiscoveredPeer &p = peers[i];
            if (!strlen(p.ip)) continue;
            bool isPropPeer = strstr(p.role, "prop") != nullptr
                           || strstr(p.name, "propproxy") != nullptr
                           || strstr(p.name, "pi5") != nullptr;
            if (!isPropPeer) continue;
            memcpy(snaps[snapCount].ip, p.ip, 16);
            snaps[snapCount].port = p.port;
            snapCount++;
        }
    }
    xSemaphoreGive(g_dataMutex);

    for (int s = 0; s < snapCount; s++)
    {
        HTTPClient http;
        String url = "http://" + String(snaps[s].ip) + ":" + String(snaps[s].port) + "/api/link";
        http.setTimeout(HTTP_TIMEOUT_PROP_MS);
        if (!http.begin(url)) continue;
        int code = http.GET();
        if (code == 200)
        {
            String body = http.getString();
            http.end();
            JsonDocument doc;
            if (!deserializeJson(doc, body))
            {
                xSemaphoreTake(g_dataMutex, portMAX_DELAY);
                JsonObject h = doc["house"].as<JsonObject>();
                JsonObject p = doc["paddock"].as<JsonObject>();
                strncpy(s_linkData.house.name, h["name"] | "", sizeof(s_linkData.house.name)-1);
                strncpy(s_linkData.house.ip,   h["ip"]   | "", sizeof(s_linkData.house.ip)-1);
                s_linkData.house.rssi        = h["rssi"].as<int>();
                s_linkData.house.noise       = h["noise"].as<int>();
                s_linkData.house.snr         = h["snr"].as<int>();
                s_linkData.house.txRateMbps  = h["txRateMbps"].as<int>();
                s_linkData.house.rxRateMbps  = h["rxRateMbps"].as<int>();
                s_linkData.house.connected   = h["connected"].as<bool>();
                strncpy(s_linkData.paddock.name, p["name"] | "", sizeof(s_linkData.paddock.name)-1);
                strncpy(s_linkData.paddock.ip,   p["ip"]   | "", sizeof(s_linkData.paddock.ip)-1);
                s_linkData.paddock.rssi        = p["rssi"].as<int>();
                s_linkData.paddock.noise       = p["noise"].as<int>();
                s_linkData.paddock.snr         = p["snr"].as<int>();
                s_linkData.paddock.txRateMbps  = p["txRateMbps"].as<int>();
                s_linkData.paddock.rxRateMbps  = p["rxRateMbps"].as<int>();
                s_linkData.paddock.connected   = p["connected"].as<bool>();
                s_linkData.valid = doc["valid"].as<bool>();
                s_linkDataVersion++;
                xSemaphoreGive(g_dataMutex);
                debugLogf("[LINK] House %s RSSI=%d SNR=%d TX=%dMbps | Paddock %s RSSI=%d SNR=%d TX=%dMbps",
                    s_linkData.house.name, s_linkData.house.rssi, s_linkData.house.snr, s_linkData.house.txRateMbps,
                    s_linkData.paddock.name, s_linkData.paddock.rssi, s_linkData.paddock.snr, s_linkData.paddock.txRateMbps);
                return;
            }
            continue;
        }
        http.end();
    }
}

// ============================================================
// LVGL Callbacks
// ============================================================

static void lvgl_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p)
{
    esp_lcd_panel_draw_bitmap(s_panel_handle,
                              area->x1, area->y1,
                              area->x2 + 1, area->y2 + 1,
                              color_p);
    lv_disp_flush_ready(drv);
}

static void lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    uint16_t x = 0, y = 0;
    if (gt911GetTouch(x, y))
    {
        lastTouchActivity = millis();
        data->state = LV_INDEV_STATE_PRESSED;
        data->point.x = x;
        data->point.y = y;
        Serial.printf("[TOUCH] x=%u y=%u\n", x, y);
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ============================================================
// SPIFFS HTML Helper
// ============================================================

String loadHTMLPart(const String &path)
{
    File f = SPIFFS.open(path, "r");
    if (!f)
        return "";
    String content = f.readString();
    f.close();
    return content;
}

// ============================================================
// Configuration
// ============================================================

void loadConfig()
{
    String raw;
    Preferences prefs;
    prefs.begin("tcfg", true);
    raw = prefs.getString("json", "");
    prefs.end();

    // Fallback to SPIFFS
    if (raw.isEmpty())
    {
        File f = SPIFFS.open("/config.json", "r");
        if (f)
        {
            raw = f.readString();
            f.close();
        }
    }

    if (raw.isEmpty())
    {
        debugLog("[CONFIG] No config found, using defaults");
        return;
    }

    JsonDocument doc;
    if (deserializeJson(doc, raw))
    {
        debugLog("[CONFIG] JSON parse error");
        return;
    }

    wifiSSID = doc["wifi"]["ssid"] | "";
    wifiPassword = doc["wifi"]["password"] | "";
    deviceName = doc["deviceName"] | "touch-controller";

    syslogServerIP = doc["syslog"] | "";
    syslogServerIP.trim();
    if (!syslogServerIP.isEmpty() && syslogIP.fromString(syslogServerIP))
    {
        DebugLogger::setSyslog(syslogIP, deviceName);
        debugLog("[CONFIG] Syslog enabled: " + syslogIP.toString());
    }

    for (int i = 0; i < 6; i++)
    {
        if (doc["relayLabels"][i].is<const char *>())
            relayLabels[i] = doc["relayLabels"][i].as<String>();
    }

    if (doc["rotatorMemories"].is<JsonArrayConst>())
    {
        JsonArrayConst groups = doc["rotatorMemories"].as<JsonArrayConst>();
        for (int g = 0; g < 3 && g < (int)groups.size(); g++)
        {
            if (!groups[g].is<JsonArrayConst>())
                continue;
            JsonArrayConst entries = groups[g].as<JsonArrayConst>();
            for (int i = 0; i < 10 && i < (int)entries.size(); i++)
            {
                if (!entries[i].is<JsonObjectConst>())
                    continue;
                JsonObjectConst e = entries[i].as<JsonObjectConst>();
                if (e["label"].is<const char *>())
                {
                    strncpy(rotatorMemories[g][i].name, e["label"].as<const char *>(), sizeof(rotatorMemories[g][i].name) - 1);
                    rotatorMemories[g][i].name[sizeof(rotatorMemories[g][i].name) - 1] = '\0';
                }
                if (e["bearing"].is<int>())
                {
                    rotatorMemories[g][i].bearing = constrain(e["bearing"].as<int>(), 0, 359);
                }
            }
        }
    }

    tciHost = doc["tci"]["host"] | "";
    tciPort = doc["tci"]["port"] | 40001;
    tciEnabled = doc["tci"]["enabled"] | false;
    if (!tciHost.isEmpty())
    {
        debugLog("[CONFIG] TCI Host: " + tciHost + ":" + String(tciPort) + (tciEnabled ? " (enabled)" : " (disabled)"));
    }

    debugLogf("[CONFIG] WiFi SSID: %s, device: %s",
              wifiSSID.c_str(), deviceName.c_str());
}

void saveConfig()
{
    JsonDocument doc;
    doc["deviceName"] = deviceName;

    auto wifi = doc["wifi"].to<JsonObject>();
    wifi["ssid"] = wifiSSID;
    wifi["password"] = wifiPassword;

    doc["syslog"] = syslogServerIP;

    auto labels = doc["relayLabels"].to<JsonArray>();
    for (int i = 0; i < 6; i++)
        labels.add(relayLabels[i]);

    auto memGroups = doc["rotatorMemories"].to<JsonArray>();
    for (int g = 0; g < 3; g++)
    {
        JsonArray group = memGroups.add<JsonArray>();
        for (int i = 0; i < 10; i++)
        {
            JsonObject e = group.add<JsonObject>();
            e["label"] = rotatorMemories[g][i].name;
            e["bearing"] = rotatorMemories[g][i].bearing;
        }
    }

    auto tciObj = doc["tci"].to<JsonObject>();
    tciObj["host"] = tciHost;
    tciObj["port"] = tciPort;
    tciObj["enabled"] = tciEnabled;

    // Save to NVS
    String json;
    serializeJson(doc, json);
    Preferences prefs;
    prefs.begin("tcfg", false);
    prefs.putString("json", json);
    prefs.end();

    // Also save to SPIFFS
    File f = SPIFFS.open("/config.json", "w");
    if (f)
    {
        f.print(json);
        f.close();
    }

    debugLog("[CONFIG] Saved");
}

// ============================================================
// Web Server Handlers
// ============================================================

void handleRoot()
{
    String html = loadHTMLPart("/header.html");
    html += R"rawliteral(
<section class="dashboard">
  <div class="hero">
    <div>
      <div class="hero-title">Touch Controller</div>
      <div class="hero-subtitle">CrowPanel 7" control panel</div>
      <div class="hero-meta">
        <span id="status-age" class="pill">--</span>
        <span id="signal-pill" class="pill">Signal --</span>
      </div>
    </div>
    <div class="hero-actions all-buttons-row">
      <button class="all-on-button" onclick="allRelaysOn()">All On</button>
      <button class="all-off-button" onclick="allRelaysOff()">All Off</button>
    </div>
  </div>

  <div class="status-cards">
    <div class="status-card" id="battery-card-0">
      <div class="card-icon" id="bat0-icon" title="">
        <svg viewBox="0 0 28 48" width="28" height="48">
          <rect x="8" y="0" width="12" height="3" rx="1" fill="#555"/>
          <rect x="2" y="3" width="24" height="44" rx="3" stroke="#555" stroke-width="2" fill="none"/>
          <rect id="bat0-c3" x="5" y="6" width="18" height="8" rx="1" fill="#333"/>
          <rect id="bat0-c2" x="5" y="16" width="18" height="8" rx="1" fill="#333"/>
          <rect id="bat0-c1" x="5" y="26" width="18" height="8" rx="1" fill="#333"/>
          <rect id="bat0-c0" x="5" y="36" width="18" height="8" rx="1" fill="#333"/>
        </svg>
      </div>
      <div class="card-content">
        <div class="card-value" id="card-bat0-soc" style="color:#888">Waiting&#8230;</div>
        <div class="card-label" id="card-bat0-name">Battery 1</div>
        <div class="card-detail"><span id="card-bat0-voltage"></span><span id="card-bat0-temp"></span></div>
      </div>
    </div>
    <div class="status-card" id="battery-card-1">
      <div class="card-icon" id="bat1-icon" title="">
        <svg viewBox="0 0 28 48" width="28" height="48">
          <rect x="8" y="0" width="12" height="3" rx="1" fill="#555"/>
          <rect x="2" y="3" width="24" height="44" rx="3" stroke="#555" stroke-width="2" fill="none"/>
          <rect id="bat1-c3" x="5" y="6" width="18" height="8" rx="1" fill="#333"/>
          <rect id="bat1-c2" x="5" y="16" width="18" height="8" rx="1" fill="#333"/>
          <rect id="bat1-c1" x="5" y="26" width="18" height="8" rx="1" fill="#333"/>
          <rect id="bat1-c0" x="5" y="36" width="18" height="8" rx="1" fill="#333"/>
        </svg>
      </div>
      <div class="card-content">
        <div class="card-value" id="card-bat1-soc" style="color:#888">Waiting&#8230;</div>
        <div class="card-label" id="card-bat1-name">Battery 2</div>
        <div class="card-detail"><span id="card-bat1-voltage"></span><span id="card-bat1-temp"></span></div>
      </div>
    </div>
    <div class="status-card" id="solar-card">
      <div class="card-icon">&#x26A1;</div>
      <div class="card-content">
        <div class="card-value" id="card-mppt-power">--</div>
        <div class="card-label">Solar</div>
        <div class="card-detail" id="card-mppt-state">--</div>
      </div>
    </div>
  </div>

  <div class="relay-grid">
)rawliteral";

    for (int i = 0; i < 6; i++)
    {
        String buttonClass;
        if (remoteSleeping || !relayDataReady)
        {
            buttonClass = "grey inactive";
        }
        else
        {
            buttonClass = relayStates[i] ? "green active" : "red active";
        }
        html += "<button id='relay" + String(i) + "' class='" + buttonClass + "' onclick='toggleRelay(" + String(i) + ")'>" + ESP32Utils::htmlEscape(relayLabels[i]) + "</button>";
    }

    html += R"rawliteral(
  </div>

  <div class="tools-menu">
    <button class="tool-button" onclick="location.href='/settings'" title="Settings">&#x2699;&#xFE0F; Settings</button>
    <button class="tool-button" onclick="location.href='/log'" title="View Log">&#x1F4CB; Logs</button>
    <button class="tool-button reboot" onclick="location.href='/reboot'" title="Reboot">&#x21BB; Reboot</button>
  </div>

  <div id="status-message" class="status-message">
)rawliteral";

    if (lastStatusMessage.length() > 0)
        html += "<p style='margin:5px;'>" + lastStatusMessage + "</p>";
    else
        html += "<p style='margin:5px; color:#888;'>No status messages</p>";

    html += "</div></section>\n";
    html += loadHTMLPart("/footer.html");
    html += "<script>window.BUILD_DATE='" + String(buildDate) + " " + String(buildTime) + "';window.ESP_UPTIME=" + String(millis()) + ";</script>";

    // Battery card update script (same pattern as LoRa Client)
    html += R"rawliteral(<script>
(function(){
  var origUpdate = window.updateStatus;
  if (!origUpdate) return;
  function applyBat(d) {
    if (d.dalyBms) {
      for (var bi=0; bi<d.dalyBms.length && bi<2; bi++) {
        var bms = d.dalyBms[bi];
        var socEl = document.getElementById('card-bat'+bi+'-soc');
        var vEl = document.getElementById('card-bat'+bi+'-voltage');
        var tEl = document.getElementById('card-bat'+bi+'-temp');
        if (socEl) {
          if (bms.connected && bms.voltage > 0) {
            socEl.textContent = bms.soc.toFixed(0)+'%';
            socEl.style.color = bms.soc>50?'#4CAF50':(bms.soc>20?'#ff9800':'#f44336');
          } else {
            socEl.textContent = '--%'; socEl.style.color = '#888';
          }
        }
        if (vEl) vEl.textContent = (bms.connected && bms.voltage > 0) ? bms.voltage.toFixed(2)+'V' : '';
        if (tEl) {
          if (bms.connected && bms.voltage > 0 && bms.tempHigh > -40) {
            tEl.textContent = ' \u00b7 '+bms.tempHigh.toFixed(0)+'\u00b0C';
          } else { tEl.textContent = ''; }
        }
        // Cell segment colors
        var cells = bms.cells;
        var iconEl = document.getElementById('bat'+bi+'-icon');
        if (cells && cells.length >= 4 && iconEl) {
          var avg = 0;
          for (var ci=0; ci<cells.length; ci++) avg += cells[ci];
          avg /= cells.length;
          var tip = '';
          for (var ci=0; ci<4; ci++) {
            var seg = document.getElementById('bat'+bi+'-c'+ci);
            if (!seg) continue;
            var dev = Math.abs(cells[ci] - avg) / avg * 100;
            seg.setAttribute('fill', dev > 0.1 ? '#ff9800' : '#4CAF50');
            tip += 'Cell '+(ci+1)+': '+(cells[ci]/1000).toFixed(3)+'V\n';
          }
          iconEl.title = tip.trim();
        }
      }
    }
    if (d.mppt) {
      var pwEl = document.getElementById('card-mppt-power');
      var stEl = document.getElementById('card-mppt-state');
      if (pwEl && d.mppt.valid) {
        pwEl.textContent = d.mppt.power.toFixed(1)+'W';
        pwEl.style.color = d.mppt.power>10?'#4CAF50':(d.mppt.power>0?'#ff9800':'#888');
      }
      if (stEl) stEl.textContent = d.mppt.stateName || '--';
    }
  }
  window.updateStatus = function() {
    return origUpdate.apply(this, arguments).then(function() {
      fetch('/api/status').then(function(r){return r.json()}).then(applyBat).catch(function(){});
    });
  };
})();
</script>)rawliteral";

    server.send(200, "text/html", html);
}

void handleStatusApi()
{
    JsonDocument doc;
    doc["uptimeMs"] = millis();
    doc["buildDate"] = String(buildDate) + " " + String(buildTime);

    auto states = doc["states"].to<JsonArray>();
    auto labels = doc["labels"].to<JsonArray>();

    for (int i = 0; i < 6; i++)
    {
        states.add(relayStates[i]);
        labels.add(relayLabels[i]);
    }

    doc["lastStatus"] = lastStatusMessage;
    doc["remoteSleeping"] = remoteSleeping;
    doc["relayDataReady"] = relayDataReady;

    // Dual BMS data
    JsonArray dalyBms = doc["dalyBms"].to<JsonArray>();
    for (int i = 0; i < 2; i++)
    {
        JsonObject bms = dalyBms.add<JsonObject>();
        bms["name"] = String("Battery ") + String(i + 1);
        bms["connected"] = remoteBms[i].connected;
        bms["voltage"] = remoteBms[i].voltage;
        bms["soc"] = remoteBms[i].soc;
        bms["tempHigh"] = remoteBms[i].temp;
        if (remoteBms[i].cellCount > 0)
        {
            JsonArray cells = bms["cells"].to<JsonArray>();
            for (int c = 0; c < remoteBms[i].cellCount; c++)
                cells.add(remoteBms[i].cellMv[c]);
        }
    }

    JsonObject mppt = doc["mppt"].to<JsonObject>();
    mppt["power"] = remoteMPPTPower;
    mppt["state"] = remoteMPPTState;
    mppt["stateName"] = remoteMPPTStateName;
    mppt["valid"] = remoteMPPTValid;
    mppt["systemVoltage"] = remoteMPPTSystemVoltage;

    JsonObject lora = doc["lora"].to<JsonObject>();
    lora["valid"] = hasLoRaRx;
    lora["rssi"] = lastLoRaRssi;
    lora["snr"] = lastLoRaSnr;
    if (hasLoRaRx && lastLoRaRxTime > 0)
        lora["age"] = (millis() - lastLoRaRxTime) / 1000;
    else
        lora["age"] = -1;

    // Temperatures
    JsonArray temperatures = doc["temperatures"].to<JsonArray>();
    if (remoteTemp1 > -127.0)
    {
        JsonObject t = temperatures.add<JsonObject>();
        t["label"] = remoteTemp1Label;
        t["celsius"] = remoteTemp1;
    }
    if (remoteTemp2 > -127.0)
    {
        JsonObject t = temperatures.add<JsonObject>();
        t["label"] = remoteTemp2Label;
        t["celsius"] = remoteTemp2;
    }

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

void handleToggle()
{
    if (!server.hasArg("id"))
    {
        server.send(400, "text/plain", "Missing id");
        return;
    }
    int id = server.arg("id").toInt();
    if (id < 0 || id > 5)
    {
        server.send(400, "text/plain", "Invalid id");
        return;
    }

    // Send toggle command via HTTP to the LoRa remote
    DiscoveredPeer *gw = findLoRaPeer();
    if (gw && strlen(gw->ip) > 0)
    {
        HTTPClient http;
        String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/toggle?id=" + String(id);
        http.setTimeout(2000);
        http.begin(url);
        http.GET();
        http.end();
    }

    // Optimistic update
    relayStates[id] = !relayStates[id];
    server.send(200, "text/plain", "OK");
    debugLogf("[WEB] Toggle relay %d -> %s", id, relayStates[id] ? "ON" : "OFF");
}

void handleAllOn()
{
    DiscoveredPeer *gw = findLoRaPeer();
    if (gw && strlen(gw->ip) > 0)
    {
        HTTPClient http;
        String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/all_on";
        http.setTimeout(2000);
        http.begin(url);
        http.GET();
        http.end();
    }
    for (int i = 0; i < 6; i++)
        relayStates[i] = true;
    server.send(200, "text/plain", "OK");
    debugLog("[WEB] All relays ON requested");
}

void handleAllOff()
{
    DiscoveredPeer *gw = findLoRaPeer();
    if (gw && strlen(gw->ip) > 0)
    {
        HTTPClient http;
        String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/all_off";
        http.setTimeout(2000);
        http.begin(url);
        http.GET();
        http.end();
    }
    for (int i = 0; i < 6; i++)
        relayStates[i] = false;
    server.send(200, "text/plain", "OK");
    debugLog("[WEB] All relays OFF requested");
}

void handleSettings()
{
    String html = loadHTMLPart("/header.html");
    html += R"rawliteral(
  <div class="container">
    <form action='/save' id='settingsForm'>
      <table class='settings-table'>
        <thead><tr><th colspan='2'>WiFi Configuration</th></tr></thead>
        <tbody>
          <tr><td>SSID</td><td><input type='text' name='ssid' value=')rawliteral";
    html += ESP32Utils::htmlEscape(wifiSSID);
    html += R"rawliteral(' maxlength='32'></td></tr>
          <tr><td>Password</td><td><input type='password' name='password' value=')rawliteral";
    html += ESP32Utils::htmlEscape(wifiPassword);
    html += R"rawliteral(' maxlength='63'></td></tr>
        </tbody>
      </table>
      <br>
      <table class='settings-table'>
        <thead><tr><th colspan='2'>Device Settings</th></tr></thead>
        <tbody>
          <tr><td>Device Name</td><td><input type='text' name='deviceName' value=')rawliteral";
    html += ESP32Utils::htmlEscape(deviceName);
    html += R"rawliteral(' maxlength='32'></td></tr>
          <tr><td>Syslog Server IP</td><td><input type='text' name='syslog' value=')rawliteral";
    html += ESP32Utils::htmlEscape(syslogServerIP);
    html += R"rawliteral(' maxlength='15' placeholder='192.168.2.11'></td></tr>
        </tbody>
      </table>
      <br>
      <table class='settings-table'>
        <thead><tr><th colspan='2'>TCI Radio Connection</th></tr></thead>
        <tbody>
          <tr><td>TCI Host (radio IP)</td><td><input type='text' name='tciHost' value=')rawliteral";
    html += ESP32Utils::htmlEscape(tciHost);
    html += R"rawliteral(' maxlength='64' placeholder='192.168.1.44'></td></tr>
          <tr><td>TCI Port</td><td><input type='number' name='tciPort' value=')rawliteral";
    html += String(tciPort);
    html += R"rawliteral(' min='1' max='65535'></td></tr>
          <tr><td>Enable TCI</td><td><label><input type='checkbox' name='tciEnabled')rawliteral";
    html += tciEnabled ? " checked" : "";
    html += R"rawliteral(> Enable direct TCI connection</label></td></tr>
        </tbody>
      </table>
      <br>
      <table class='settings-table'>
        <thead><tr><th>Relay #</th><th>Label</th></tr></thead>
        <tbody>
  )rawliteral";

    for (int i = 0; i < 6; i++)
    {
        html += "<tr><td>" + String(i + 1) + "</td>";
        html += "<td><input type='text' name='label" + String(i) + "' value='" + ESP32Utils::htmlEscape(relayLabels[i]) + "' maxlength='20'></td></tr>";
    }

    const char *groupNames[] = {"UK", "Europe", "World"};
    for (int g = 0; g < 3; g++)
    {
        html += "</tbody></table><br><table class='settings-table'><thead><tr><th colspan='3'>Rotator " + String(groupNames[g]) + " Memories</th></tr><tr><th>#</th><th>Label</th><th>Bearing</th></tr></thead><tbody>";
        for (int i = 0; i < 10; i++)
        {
            html += "<tr><td>" + String(i + 1) + "</td>";
            html += "<td><input type='text' name='mem_label_" + String(g) + "_" + String(i) + "' value='" + ESP32Utils::htmlEscape(String(rotatorMemories[g][i].name)) + "' maxlength='23'></td>";
            html += "<td><input type='number' min='0' max='359' step='1' name='mem_bearing_" + String(g) + "_" + String(i) + "' value='" + String(rotatorMemories[g][i].bearing) + "'></td></tr>";
        }
    }

    html += R"rawliteral(
        </tbody>
      </table>
      <div class='form-buttons'>
        <button class='settings-button' type='submit'>Save Settings</button>
        <a href="/"><button class="settings-button" type="button">Back</button></a>
      </div>
    </form>
  </div>
  )rawliteral";

    html += loadHTMLPart("/footer.html");
    html += "<script>window.BUILD_DATE='" + String(buildDate) + " " + String(buildTime) + "';window.ESP_UPTIME=" + String(millis()) + ";</script>";
    server.send(200, "text/html", html);
}

void handleSave()
{
    if (server.hasArg("ssid"))
        wifiSSID = server.arg("ssid");
    if (server.hasArg("password"))
        wifiPassword = server.arg("password");
    if (server.hasArg("deviceName"))
        deviceName = server.arg("deviceName");

    if (server.hasArg("syslog"))
    {
        syslogServerIP = server.arg("syslog");
        syslogServerIP.trim();
        if (!syslogServerIP.isEmpty() && syslogIP.fromString(syslogServerIP))
        {
            DebugLogger::setSyslog(syslogIP, deviceName);
        }
        else if (syslogServerIP.isEmpty())
        {
            DebugLogger::disableSyslog();
        }
    }

    if (server.hasArg("tciHost"))
        tciHost = server.arg("tciHost");
    if (server.hasArg("tciPort"))
        tciPort = (uint16_t)constrain(server.arg("tciPort").toInt(), 1, 65535);
    tciEnabled = server.hasArg("tciEnabled");

    for (int i = 0; i < 6; i++)
    {
        if (server.hasArg("label" + String(i)))
            relayLabels[i] = server.arg("label" + String(i));
    }

    for (int g = 0; g < 3; g++)
    {
        for (int i = 0; i < 10; i++)
        {
            String labelArg = "mem_label_" + String(g) + "_" + String(i);
            String bearingArg = "mem_bearing_" + String(g) + "_" + String(i);
            if (server.hasArg(labelArg))
            {
                String label = server.arg(labelArg);
                label.trim();
                if (label.length() == 0)
                    label = "Mem " + String(i + 1);
                strncpy(rotatorMemories[g][i].name, label.c_str(), sizeof(rotatorMemories[g][i].name) - 1);
                rotatorMemories[g][i].name[sizeof(rotatorMemories[g][i].name) - 1] = '\0';
            }
            if (server.hasArg(bearingArg))
            {
                rotatorMemories[g][i].bearing = constrain(server.arg(bearingArg).toInt(), 0, 359);
            }
        }
    }

    saveConfig();
    debugLog("[SAVE] Configuration saved");

    server.sendHeader("Location", "/");
    server.send(303);
}

void handleLogPage()
{
    String html = loadHTMLPart("/header.html");
    html += R"rawliteral(<div class="log-box">)rawliteral";

    String logText = DebugLogger::getLogText();
    if (logText.length() > 0)
        html += logText;
    else
        html += "<p>No log entries yet.</p>";

    html += R"rawliteral(
</div>
<div class="controls">
  <button class="settings-button" onclick="clearLog()">Clear Log</button>
  <button class="settings-button" onclick="location.href='/'">Back</button>
</div>
)rawliteral";

    html += loadHTMLPart("/footer.html");
    html += "<script>window.BUILD_DATE='" + String(buildDate) + " " + String(buildTime) + "';window.ESP_UPTIME=" + String(millis()) + ";</script>";
    server.send(200, "text/html", html);
}

void handleClearLog()
{
    DebugLogger::clearLog();
    debugLog("Log cleared");
    server.send(200, "text/plain", "Log cleared");
}

void handleReboot()
{
    String html = loadHTMLPart("/header.html");
    html += "<h1>Rebooting...</h1><p>Page will reload shortly.</p><script>setTimeout(function(){window.location.href='/';},5000);</script>";
    html += loadHTMLPart("/footer.html");
    html += "<script>window.BUILD_DATE='" + String(buildDate) + " " + String(buildTime) + "';window.ESP_UPTIME=" + String(millis()) + ";</script>";
    server.send(200, "text/html", html);
    rebootPending = true;
    rebootStartTime = millis();
}

void handleDownloadConfig()
{
    File f = SPIFFS.open("/config.json", "r");
    if (!f)
    {
        server.send(404, "text/plain", "No config file");
        return;
    }
    server.streamFile(f, "application/json");
    f.close();
}

// ============================================================
// Web Server Setup
// ============================================================

void setupWebServer()
{
    // Static files from SPIFFS
    server.on("/style.css", HTTP_GET, []()
              {
        File f = SPIFFS.open("/style.css", "r");
        if (!f) { server.send(404, "text/css", ""); return; }
        server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        server.streamFile(f, "text/css");
        f.close(); });

    server.on("/script.js", HTTP_GET, []()
              {
        File f = SPIFFS.open("/script.js", "r");
        if (!f) { server.send(404, "application/javascript", ""); return; }
        server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
        server.streamFile(f, "application/javascript");
        f.close(); });

    server.on("/logo.png", HTTP_GET, []()
              {
        File f = SPIFFS.open("/logo.png", "r");
        if (!f) { server.send(404, "image/png", ""); return; }
        server.sendHeader("Cache-Control", "max-age=86400");
        server.streamFile(f, "image/png");
        f.close(); });

    // Pages and API
    server.on("/", handleRoot);
    server.on("/toggle", handleToggle);
    server.on("/all_on", handleAllOn);
    server.on("/all_off", handleAllOff);
    server.on("/settings", handleSettings);
    server.on("/save", handleSave);
    server.on("/log", handleLogPage);
    server.on("/clearlog", HTTP_GET, handleClearLog);
    server.on("/api/status", handleStatusApi);
    // Receive push notification from antenna-controller on antenna change.
    // Resets the VFO poll timer so the next loop() iteration fetches immediately.
    server.on("/api/notify", HTTP_POST, []() {
        if (server.hasArg("role") && server.arg("role").indexOf("antenna") >= 0)
            lastVfoPoll = 0;
        server.send(200, "application/json", "{\"ok\":true}");
    });
    server.on("/reboot", handleReboot);
    server.on("/download_config", HTTP_GET, handleDownloadConfig);

    // Peer discovery status API
    server.on("/api/peers/status", HTTP_GET, []()
              {
        JsonDocument doc;
        JsonObject root = doc.to<JsonObject>();
        peerDiscovery.toJson(root);
        String json;
        serializeJson(doc, json);
        server.send(200, "application/json", json); });

    server.begin();
    debugLogf("[WEB] Server started on port 80 - http://%s", WiFi.localIP().toString().c_str());
}

// ============================================================
// Power Tab - Relay command helpers
// ============================================================

// Forward-declared: defined after update_power_tab so it can access the button objects.
static void relay_pulse_timer_cb(lv_timer_t *t);

static void stopRelayPulse()
{
    if (g_relayPulseTimer)
    {
        lv_timer_del(g_relayPulseTimer);
        g_relayPulseTimer = nullptr;
    }
    g_relayPulseState = false;
}

// Enqueue a relay HTTP command and arm the confirmation machinery.
// relayId: 0-5 = single relay, 6 = all-on, 7 = all-off.
// expectedState: true=ON, false=OFF (only meaningful for single relay).
static void enqueueRelayCommand(const String &url, int8_t relayId, bool expectedState)
{
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    uint8_t nextTail = (httpCmdTail + 1) % HTTP_CMD_QUEUE_SIZE;
    if (nextTail == httpCmdHead)
    {
        xSemaphoreGive(g_dataMutex);
        showToast("Command queue busy", lv_color_hex(0xCC0000));
        return;
    }
    HttpCommand &cmd = httpCmdQueue[httpCmdTail];
    cmd.url          = url;
    cmd.method       = HTTP_CMD_GET;
    cmd.body         = "";
    cmd.timeoutMs    = HTTP_TIMEOUT_TOUCH_MS;
    cmd.isRotatorCmd = false;
    cmd.isJsonBody   = false;
    cmd.isRelayCmd   = true;
    cmd.relayId      = relayId;
    cmd.relayTarget  = expectedState;
    httpCmdTail = nextTail;

    // Mark pending under the same mutex slice
    g_relayHttpCode = 0; // in-flight
    if (relayId <= 5)
    {
        g_relayPendingTarget[relayId] = expectedState;
        g_relayPendingMask |= (uint8_t)(1U << relayId);
        g_relayAllPending = -1;
    }
    else
    {
        for (int i = 0; i < 6; i++)
            g_relayPendingTarget[i] = (relayId == 6); // 6=all-on → true, 7=all-off → false
        g_relayPendingMask = 0x3F; // all 6 bits
        g_relayAllPending  = relayId; // 6=all-on, 7=all-off
    }
    xSemaphoreGive(g_dataMutex);

    g_relaySentAt = millis();

    // Start/restart flash pulse timer (LVGL timer must be created on main/LVGL thread,
    // but relay callbacks are always called from the main thread via LVGL events)
    stopRelayPulse();
    g_relayPulseState = true;
    g_relayPulseTimer = lv_timer_create(relay_pulse_timer_cb, 250, nullptr);
}

static void relay_btn_event_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id < 0 || id > 5)
        return;
    if (remoteSleeping || !relayDataReady)
        return;
    // Ignore tap if this relay is already pending
    if (g_relayPendingMask & (1U << id))
        return;

    bool expected = !relayStates[id]; // toggle from last confirmed state
    debugLogf("[TOUCH] Toggle relay %d -> %s (pending)", id, expected ? "ON" : "OFF");

    DiscoveredPeer *gw = findLoRaPeer();
    if (!gw || strlen(gw->ip) == 0)
    {
        showToast("No gateway reachable", lv_color_hex(0xCC0000));
        return;
    }

    String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/toggle?id=" + String(id);
    enqueueRelayCommand(url, (int8_t)id, expected);
}

static void all_on_btn_event_cb(lv_event_t *e)
{
    if (g_relayPendingMask || g_relayAllPending >= 0)
        return; // already pending
    debugLog("[TOUCH] All relays ON (pending)");
    DiscoveredPeer *gw = findLoRaPeer();
    if (!gw || strlen(gw->ip) == 0)
    {
        showToast("No gateway reachable", lv_color_hex(0xCC0000));
        return;
    }
    String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/all_on";
    enqueueRelayCommand(url, 6, true);
}

static void all_off_btn_event_cb(lv_event_t *e)
{
    if (g_relayPendingMask || g_relayAllPending >= 0)
        return; // already pending
    debugLog("[TOUCH] All relays OFF (pending)");
    DiscoveredPeer *gw = findLoRaPeer();
    if (!gw || strlen(gw->ip) == 0)
    {
        showToast("No gateway reachable", lv_color_hex(0xCC0000));
        return;
    }
    String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/all_off";
    enqueueRelayCommand(url, 7, false);
}


// ============================================================
// UI Feedback Helpers
// ============================================================

// Show a brief toast notification at the top-right of the screen
static void showToast(const char *msg, lv_color_t bg_color, uint32_t duration_ms)
{
    if (!toast_label)
    {
        toast_label = lv_label_create(lv_scr_act());
        lv_obj_set_style_bg_opa(toast_label, LV_OPA_90, 0);
        lv_obj_set_style_radius(toast_label, 8, 0);
        lv_obj_set_style_pad_all(toast_label, 8, 0);
        lv_obj_set_style_text_color(toast_label, lv_color_white(), 0);
        lv_obj_set_style_text_font(toast_label, &lv_font_montserrat_14, 0);
    }
    lv_label_set_text(toast_label, msg);
    lv_obj_set_style_bg_color(toast_label, bg_color, 0);
    lv_obj_align(toast_label, LV_ALIGN_TOP_RIGHT, -10, 50);
    lv_obj_clear_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
    toast_hide_time = millis() + duration_ms;
}

// Brief button flash to confirm touch was registered
static void flashButton(lv_obj_t *btn, lv_color_t flash_color)
{
    if (!btn)
        return;
    lv_obj_set_style_bg_color(btn, flash_color, 0);
}

static bool postToPeer(const DiscoveredPeer &peer, const char *endpoint, uint16_t timeoutMs = HTTP_TIMEOUT_TOUCH_MS)
{
    if (strlen(peer.ip) == 0)
        return false;

    String url = "http://" + String(peer.ip) + ":" + String(peer.port) + endpoint;
    return enqueueHttpCommand(url, HTTP_CMD_POST, "", timeoutMs);
}

static void reboot_touch_cb(lv_event_t *e)
{
    (void)e;
    showToast("Rebooting touch controller...", lv_color_hex(0xCC3333), 1200);
    rebootPending = true;
    rebootStartTime = millis();
}

static void peer_reboot_btn_cb(lv_event_t *e)
{
    int row = (int)(intptr_t)lv_event_get_user_data(e);
    if (row < 0 || row >= MAX_PEER_ROWS)
        return;

    int peerIndex = peerRowToIndex[row];
    if (peerIndex < 0 || peerIndex >= peerDiscovery.peerCount())
        return;

    const DiscoveredPeer *peers = peerDiscovery.peers();
    const DiscoveredPeer &peer = peers[peerIndex];
    // Try /api/reboot (POST, JSON response) first — all G7NRU devices support this.
    // Use a longer timeout since the peer sends a response before restarting.
    if (postToPeer(peer, "/api/reboot", 1500))
    {
        showToast("Reboot command sent", lv_color_hex(0x4CAF50));
    }
    else
    {
        showToast("Peer reboot failed", lv_color_hex(0xCC0000));
    }
}

// ============================================================
// Antenna Tab - Select antenna callback
// ============================================================

static void stopAntennaPulse()
{
    if (antennaPulseTimer)
    {
        lv_timer_del(antennaPulseTimer);
        antennaPulseTimer = nullptr;
    }
    antennaPendingBtn = nullptr;
    antennaPulseState = false;
}

// ============================================================
// Rotator Tab - Pulse (flash until rotator confirms command)
// ============================================================

static void stopRotatorPulse()
{
    if (rotatorPulseTimer)
    {
        lv_timer_del(rotatorPulseTimer);
        rotatorPulseTimer = nullptr;
    }
    if (rotatorPendingBtn)
    {
        lv_obj_set_style_bg_color(rotatorPendingBtn, gPendingBtnRestoreBg, 0);
        rotatorPendingBtn = nullptr;
    }
    rotatorPulseState = false;
}

static void rotator_pulse_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!rotatorPendingBtn)
    {
        stopRotatorPulse();
        return;
    }
    rotatorPulseState = !rotatorPulseState;
    lv_obj_set_style_bg_color(rotatorPendingBtn,
                              rotatorPulseState ? lv_color_hex(0x4caf50) : gPendingBtnRestoreBg, 0);
}

static void antenna_pulse_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!antennaPendingBtn)
    {
        stopAntennaPulse();
        return;
    }
    antennaPulseState = !antennaPulseState;
    lv_obj_set_style_bg_color(antennaPendingBtn,
                              antennaPulseState ? lv_color_hex(0xE65100) : lv_color_hex(0x1a2128), 0);
}

static void antenna_btn_event_cb(lv_event_t *e)
{
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    if (index < 0 || index >= MAX_ANTENNAS)
        return;

    int id = antennaBtnIds[index];
    if (id < 0)
        return;

    if (!antennaDataReady)
        return;

    lv_obj_t *btn = lv_event_get_target(e);

    // Start pulsing the button until the next data refresh confirms the change
    stopAntennaPulse();
    antennaPendingBtn = btn;
    antennaPulseState = true;
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xE65100), 0);
    antennaPulseTimer = lv_timer_create(antenna_pulse_timer_cb, 300, nullptr);

    debugLogf("[TOUCH] Select antenna %d", id);

    DiscoveredPeer *ant = peerDiscovery.findByRole("antenna-controller");
    if (ant && strlen(ant->ip) > 0)
    {
        String url = "http://" + String(ant->ip) + ":" + String(ant->port) + "/api/antenna?id=" + String(id);
        enqueueHttpCommand(url, HTTP_CMD_GET, "", HTTP_TIMEOUT_TOUCH_MS);
        showToast("Antenna selected", lv_color_hex(0x4CAF50), 1500);
    }
    else
    {
        showToast("Antenna controller not found", lv_color_hex(0xCC0000));
    }

    // Optimistic update - mark this antenna active, others in same group inactive
    int selGroup = -1;
    for (int i = 0; i < antennaCount; i++)
    {
        if (antennas[i].id == id)
        {
            selGroup = antennas[i].group;
            break;
        }
    }
    if (selGroup >= 0)
    {
        for (int i = 0; i < antennaCount; i++)
        {
            if (antennas[i].group == selGroup)
                antennas[i].active = (antennas[i].id == id);
        }
    }
}

// ============================================================
// Rotator Tab - Callbacks
// ============================================================

static bool sendRotatorCommand(const char *endpoint, const char *postBody = nullptr, bool jsonBody = false)
{
    DiscoveredPeer *rot = peerDiscovery.findByRole("rotator-controller");
    if (!rot || strlen(rot->ip) == 0)
    {
        showToast("Rotator not found", lv_color_hex(0xCC0000));
        return false;
    }

    String url = "http://" + String(rot->ip) + ":" + String(rot->port) + endpoint;
    String body = postBody ? String(postBody) : String("");
    // Reset result before queuing so badge shows "pending" immediately
    g_rotatorCmdHttpCode  = 0;
    HttpCommand cmd;
    cmd.url           = url;
    cmd.method        = HTTP_CMD_POST;
    cmd.body          = body;
    cmd.timeoutMs     = HTTP_TIMEOUT_ROTATOR_MS;
    cmd.isRotatorCmd  = true;
    cmd.isJsonBody    = jsonBody;
    cmd.isRelayCmd    = false;
    cmd.relayId       = -1;
    cmd.relayTarget   = false;
    // Enqueue directly instead of via enqueueHttpCommand so we can set isRotatorCmd
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    uint8_t nextTail = (httpCmdTail + 1) % HTTP_CMD_QUEUE_SIZE;
    if (nextTail == httpCmdHead)
    {
        xSemaphoreGive(g_dataMutex);
        showToast("Command queue busy", lv_color_hex(0xCC0000));
        return false;
    }
    httpCmdQueue[httpCmdTail] = cmd;
    httpCmdTail = nextTail;
    xSemaphoreGive(g_dataMutex);
    // Fast-poll for 15s; reset last-poll time so the poll task fires immediately
    // after the command is sent rather than waiting up to 1s.
    rotatorFastPollUntil  = millis() + 15000;
    lastRotatorFastPoll   = 0;
    rotatorCommandSentAt  = millis();
    return true;
}

static void sendGotoBearing(int heading)
{
    bool wasDisabled = !rotatorEnabled;
    if (wasDisabled)
    {
        // Auto-enable the rotator before issuing the goto command
        sendRotatorCommand("/api/rotator/enable");
    }
    char body[32];
    snprintf(body, sizeof(body), "{\"bearing\":%d}", heading);
    if (sendRotatorCommand("/api/rotator/goto", body, /*jsonBody=*/true))
    {
        showToast(wasDisabled ? "Enabling & rotating..." : "Goto sent",
                  lv_color_hex(0x2196F3), 1500);
    }
}

static void rotator_goto_cb(lv_event_t *e)
{
    int heading = (int)(intptr_t)lv_event_get_user_data(e);
    debugLogf("[TOUCH] Rotator goto %d", heading);
    lv_obj_t *btn = lv_event_get_target(e);
    flashButton(btn, lv_color_hex(0xFFFFFF));
    sendGotoBearing(heading);
    if (btn)
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
}

static void rotator_memory_cb(lv_event_t *e)
{
    int heading = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);

    if (!gRotatorAvailable)
    {
        showToast("Rotator not ready", lv_color_hex(0xCC0000), 1200);
        return;
    }

    // Pulse bright green ⇔ normal green until rotator confirms it is moving
    gPendingBtnRestoreBg = lv_color_hex(0x1B5E20);
    stopRotatorPulse();
    rotatorPendingBtn = btn;
    rotatorPulseState = true;
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x4caf50), 0);
    rotatorPulseTimer = lv_timer_create(rotator_pulse_timer_cb, 300, nullptr);

    sendGotoBearing(heading);
}

static void rotator_stop_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    debugLog("[TOUCH] Rotator stop");
    stopRotatorPulse();
    lv_obj_set_style_bg_color(btn_manual_stop, lv_color_hex(0xC62828), 0); // restore stop red
    sendRotatorCommand("/api/rotator/stop");
    manualRotating = false;
    manualDirection = 0;
    showToast("Stop sent", lv_color_hex(0xff4d4d), 1500);
}

static void rotator_manual_ccw_cb(lv_event_t *e)
{
    (void)e;
    if (manualRotating && manualDirection == -1)
        return;
    bool wasDisabled = !rotatorEnabled;
    if (wasDisabled)
        sendRotatorCommand("/api/rotator/enable");
    if (sendRotatorCommand("/api/rotator/manual", "direction=ccw"))
    {
        manualRotating = true;
        manualDirection = -1;
        gPendingBtnRestoreBg = lv_color_hex(gRotatorAvailable ? 0x1565C0 : 0x2a2a2a);
        stopRotatorPulse();
        rotatorPendingBtn = btn_manual_ccw;
        rotatorPulseState = true;
        lv_obj_set_style_bg_color(btn_manual_ccw, lv_color_hex(0x4caf50), 0);
        rotatorPulseTimer = lv_timer_create(rotator_pulse_timer_cb, 300, nullptr);
        showToast(wasDisabled ? "Enabling & CCW" : "Manual CCW", lv_color_hex(0x1E88E5), 1200);
    }
}

static void rotator_manual_cw_cb(lv_event_t *e)
{
    (void)e;
    if (manualRotating && manualDirection == 1)
        return;
    bool wasDisabled = !rotatorEnabled;
    if (wasDisabled)
        sendRotatorCommand("/api/rotator/enable");
    if (sendRotatorCommand("/api/rotator/manual", "direction=cw"))
    {
        manualRotating = true;
        manualDirection = 1;
        gPendingBtnRestoreBg = lv_color_hex(gRotatorAvailable ? 0x2E7D32 : 0x2a2a2a);
        stopRotatorPulse();
        rotatorPendingBtn = btn_manual_cw;
        rotatorPulseState = true;
        lv_obj_set_style_bg_color(btn_manual_cw, lv_color_hex(0x4caf50), 0);
        rotatorPulseTimer = lv_timer_create(rotator_pulse_timer_cb, 300, nullptr);
        showToast(wasDisabled ? "Enabling & CW" : "Manual CW", lv_color_hex(0x43A047), 1200);
    }
}

static void map_dialog_close()
{
    if (pendingMapDialog)
    {
        lv_obj_del(pendingMapDialog);
        pendingMapDialog = nullptr;
    }
    pendingMapBearing = -1;
}

static void map_dialog_rotate_cb(lv_event_t *e)
{
    (void)e;
    if (pendingMapBearing >= 0)
        sendGotoBearing(pendingMapBearing);
    map_dialog_close();
}

static void map_dialog_cancel_cb(lv_event_t *e)
{
    (void)e;
    map_dialog_close();
}

static void showMapConfirmDialog(int bearing)
{
    map_dialog_close();
    pendingMapBearing = bearing;

    // Full-screen semi-transparent overlay on the top layer
    lv_obj_t *overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(overlay, 160, 0);
    lv_obj_set_style_border_width(overlay, 0, 0);
    lv_obj_set_style_pad_all(overlay, 0, 0);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    pendingMapDialog = overlay;

    // Centered dialog box
    lv_obj_t *box = lv_obj_create(overlay);
    lv_obj_set_size(box, 380, 190);
    lv_obj_center(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1a2535), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x3366cc), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, 14, 0);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_set_style_pad_gap(box, 16, 0);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Map Target");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x8fa0ae), 0);

    char msg[64];
    snprintf(msg, sizeof(msg), "Rotate to %d\xC2\xB0?", bearing);
    lv_obj_t *lbl = lv_label_create(box);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xffffff), 0);

    // Button row
    lv_obj_t *btn_row = lv_obj_create(box);
    lv_obj_set_size(btn_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_style_pad_gap(btn_row, 20, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_rotate = lv_btn_create(btn_row);
    lv_obj_set_size(btn_rotate, 150, 54);
    lv_obj_set_style_bg_color(btn_rotate, lv_color_hex(0x1565C0), 0);
    lv_obj_set_style_radius(btn_rotate, 10, 0);
    lv_obj_add_event_cb(btn_rotate, map_dialog_rotate_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_r = lv_label_create(btn_rotate);
    lv_label_set_text(lbl_r, LV_SYMBOL_RIGHT " Rotate");
    lv_obj_set_style_text_font(lbl_r, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_r);

    lv_obj_t *btn_cancel = lv_btn_create(btn_row);
    lv_obj_set_size(btn_cancel, 150, 54);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x424242), 0);
    lv_obj_set_style_radius(btn_cancel, 10, 0);
    lv_obj_add_event_cb(btn_cancel, map_dialog_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_c = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_c, "Cancel");
    lv_obj_set_style_text_font(lbl_c, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl_c);
}

static void rotator_map_click_cb(lv_event_t *e)
{
    (void)e;
    lv_point_t p;
    lv_indev_get_point(lv_indev_get_act(), &p);
    lv_area_t a;
    lv_obj_get_coords(canvas_map, &a);
    int x = p.x - a.x1;
    int y = p.y - a.y1;

    int mapSize = lv_obj_get_width(canvas_map);
    int mapCx = mapSize / 2;
    int mapCy = mapSize / 2;
    int mapR = mapSize / 2 - 10;

    float dx = (float)(x - mapCx);
    float dy = (float)(mapCy - y);
    float radius = sqrtf(dx * dx + dy * dy);
    if (radius < 16.0f || radius > mapR)
        return;

    int bestBearing = (int)roundf(fmodf((atan2f(dx, dy) * 180.0f / PI) + 360.0f, 360.0f));
    showMapConfirmDialog(bestBearing);
}

static void rotator_enable_cb(lv_event_t *e)
{
    flashButton(btn_rotator_enable, lv_color_hex(0xFFFFFF));
    if (rotatorEnabled)
    {
        debugLog("[TOUCH] Rotator disable");
        showToast("Disabling rotator...", lv_color_hex(0xFF9800), 1500);
        sendRotatorCommand("/api/rotator/disable");
    }
    else
    {
        debugLog("[TOUCH] Rotator enable");
        showToast("Enabling rotator...", lv_color_hex(0x4CAF50), 1500);
        sendRotatorCommand("/api/rotator/enable");
    }
    lv_obj_set_style_bg_color(btn_rotator_enable, lv_color_hex(0x2196F3), 0);
}

static void zoom_btn_cb(lv_event_t *e)
{
    int z = (int)(intptr_t)lv_event_get_user_data(e);
    currentZoom = z;
    // Update zoom button styles
    for (int i = 0; i < 3; i++)
    {
        if (btn_zoom[i])
        {
            if (i == currentZoom)
            {
                lv_obj_set_style_bg_color(btn_zoom[i], lv_color_hex(0x2196F3), 0);
                lv_obj_set_style_border_color(btn_zoom[i], lv_color_hex(0x64B5F6), 0);
            }
            else
            {
                lv_obj_set_style_bg_color(btn_zoom[i], lv_color_hex(0x1a2128), 0);
                lv_obj_set_style_border_color(btn_zoom[i], lv_color_hex(0x2b3541), 0);
            }
        }
    }

    for (int i = 0; i < 3; i++)
    {
        if (!rotator_memory_groups[i])
            continue;
        if (i == currentZoom)
            lv_obj_clear_flag(rotator_memory_groups[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(rotator_memory_groups[i], LV_OBJ_FLAG_HIDDEN);
    }

    mapDirty = true;
    mapBaseDirty = true; // zoom changed — rebuild static base layer
}

// ============================================================
// Azimuthal Map Drawing
// ============================================================

#define MAP_SIZE 460
#define MAP_CX (MAP_SIZE / 2)
#define MAP_CY (MAP_SIZE / 2)
#define MAP_R (MAP_SIZE / 2 - 10)

static lv_color_t *map_buf = nullptr;      // rendered frame (base + bearing lines)
static lv_color_t *map_base_buf = nullptr; // clean pixels (no lines), rebuilt on zoom
static lv_img_dsc_t map_img_dsc;           // registered with LVGL once at first draw

// Convert lat/lon to azimuthal equidistant projection centered on QTH
// Returns true if point is within the visible disc
static bool azimuthalProject(float lat, float lon, float maxDistKm, int &px, int &py)
{
    float dLat = (lat - QTH_LAT) * DEG_TO_RAD;
    float dLon = (lon - QTH_LNG) * DEG_TO_RAD;
    float lat1 = QTH_LAT * DEG_TO_RAD;
    float lat2 = lat * DEG_TO_RAD;

    // Great circle distance
    float a = sinf(dLat / 2) * sinf(dLat / 2) +
              cosf(lat1) * cosf(lat2) * sinf(dLon / 2) * sinf(dLon / 2);
    float c = 2 * atan2f(sqrtf(a), sqrtf(1 - a));
    float distKm = 6371.0f * c;

    if (distKm > maxDistKm)
        return false;

    // Bearing from QTH to point
    float y = sinf(dLon) * cosf(lat2);
    float x = cosf(lat1) * sinf(lat2) - sinf(lat1) * cosf(lat2) * cosf(dLon);
    float bearing = atan2f(y, x);

    // Map to pixel coordinates
    float r = (distKm / maxDistKm) * MAP_R;
    px = MAP_CX + (int)(r * sinf(bearing));
    py = MAP_CY - (int)(r * cosf(bearing));

    return (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE);
}

// Draw a filled circle on the map buffer
static void drawDot(int cx, int cy, int radius, lv_color_t color)
{
    for (int dy = -radius; dy <= radius; dy++)
    {
        for (int dx = -radius; dx <= radius; dx++)
        {
            if (dx * dx + dy * dy <= radius * radius)
            {
                int px = cx + dx, py = cy + dy;
                if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE)
                    map_buf[py * MAP_SIZE + px] = color;
            }
        }
    }
}

// Draw a line on the map buffer using Bresenham
static void drawLine(int x0, int y0, int x1, int y1, lv_color_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    while (true)
    {
        if (x0 >= 0 && x0 < MAP_SIZE && y0 >= 0 && y0 < MAP_SIZE)
            map_buf[y0 * MAP_SIZE + x0] = color;
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

static void drawWideLine(int x0, int y0, int x1, int y1, lv_color_t color)
{
    drawLine(x0, y0, x1, y1, color);
    drawLine(x0 + 1, y0, x1 + 1, y1, color);
    drawLine(x0, y0 + 1, x1, y1 + 1, color);
}

// Major cities/landmarks for the map
struct MapPoint
{
    float lat, lon;
    const char *label;
};

static const MapPoint mapPoints[] = {
    // UK (0-19)
    {51.507f, -0.128f, "London"},
    {53.483f, -2.244f, "Manchester"},
    {52.486f, -1.890f, "Birmingham"},
    {53.800f, -1.549f, "Leeds"},
    {53.408f, -2.991f, "Liverpool"},
    {51.454f, -2.587f, "Bristol"},
    {50.719f, -1.880f, "Bournemouth"},
    {52.630f, 1.297f, "Norwich"},
    {50.822f, -0.137f, "Brighton"},
    {54.978f, -1.617f, "Newcastle"},
    {55.953f, -3.189f, "Edinburgh"},
    {51.481f, -3.179f, "Cardiff"},
    {54.597f, -5.930f, "Belfast"},
    {57.149f, -2.094f, "Aberdeen"},
    {51.752f, -1.257f, "Oxford"},
    {51.454f, -0.978f, "Reading"},
    {52.205f, 0.121f, "Cambridge"},
    {52.954f, -1.158f, "Nottingham"},
    {53.381f, -1.470f, "Sheffield"},
    {50.909f, -1.404f, "Southampton"},
    // Scandinavia & Baltic (20-32)
    {59.913f, 10.752f, "Oslo"},
    {59.329f, 18.069f, "Stockholm"},
    {55.676f, 12.568f, "Copenhagen"},
    {60.170f, 24.938f, "Helsinki"},
    {57.706f, 11.967f, "Gothenburg"},
    {60.392f, 5.325f, "Bergen"},
    {63.430f, 10.395f, "Trondheim"},
    {70.663f, 23.681f, "Hammerfest"},
    {59.437f, 24.754f, "Tallinn"},
    {56.949f, 24.106f, "Riga"},
    {54.687f, 25.279f, "Vilnius"},
    {60.499f, 22.266f, "Turku"},
    {65.013f, 25.472f, "Oulu"},
    // Greenland & Arctic (33-36)
    {64.146f, -21.942f, "Reykjavik"},
    {78.216f, 15.635f, "Longyearbyen"},
    {64.175f, -51.738f, "Nuuk"},
    {69.650f, 18.956f, "Tromsoe"},
    // Western Europe (37-58)
    {53.349f, -6.260f, "Dublin"},
    {48.857f, 2.352f, "Paris"},
    {50.850f, 4.351f, "Brussels"},
    {52.367f, 4.904f, "Amsterdam"},
    {52.520f, 13.405f, "Berlin"},
    {53.551f, 9.993f, "Hamburg"},
    {50.938f, 6.960f, "Cologne"},
    {50.110f, 8.682f, "Frankfurt"},
    {49.453f, 11.077f, "Nuremberg"},
    {48.137f, 11.575f, "Munich"},
    {47.376f, 8.541f, "Zurich"},
    {46.948f, 7.447f, "Bern"},
    {48.208f, 16.374f, "Vienna"},
    {50.075f, 14.438f, "Prague"},
    {52.230f, 21.012f, "Warsaw"},
    {52.406f, 16.925f, "Poznan"},
    {50.061f, 19.938f, "Krakow"},
    {38.722f, -9.139f, "Lisbon"},
    {40.417f, -3.704f, "Madrid"},
    {41.387f, 2.170f, "Barcelona"},
    {47.498f, 19.040f, "Budapest"},
    {43.296f, 5.369f, "Marseille"},
    // Southern & Med Europe (59-72)
    {43.710f, 7.262f, "Nice"},
    {45.070f, 7.686f, "Turin"},
    {45.464f, 9.190f, "Milan"},
    {41.903f, 12.496f, "Rome"},
    {40.851f, 14.268f, "Naples"},
    {37.498f, 15.090f, "Catania"},
    {37.984f, 23.728f, "Athens"},
    {40.634f, 22.943f, "Thessaloniki"},
    {42.697f, 23.321f, "Sofia"},
    {44.787f, 20.448f, "Belgrade"},
    {45.815f, 15.982f, "Zagreb"},
    {44.426f, 26.102f, "Bucharest"},
    {46.771f, 23.623f, "Cluj"},
    {35.897f, 14.514f, "Valletta"},
    // Eastern Europe & Turkey (73-84)
    {50.450f, 30.523f, "Kyiv"},
    {46.482f, 30.723f, "Odessa"},
    {53.905f, 27.561f, "Minsk"},
    {47.005f, 28.857f, "Chisinau"},
    {50.004f, 36.229f, "Kharkiv"},
    {41.008f, 28.978f, "Istanbul"},
    {39.927f, 32.855f, "Ankara"},
    {36.898f, 30.713f, "Antalya"},
    {40.185f, 44.515f, "Yerevan"},
    {41.693f, 44.802f, "Tbilisi"},
    {40.409f, 49.867f, "Baku"},
    {39.927f, 32.855f, "Izmir"},
    // Russia & Caucasus (85-96)
    {55.756f, 37.617f, "Moscow"},
    {59.931f, 30.360f, "StPetersburg"},
    {56.326f, 44.006f, "NizhniyNov"},
    {55.787f, 49.124f, "Kazan"},
    {48.708f, 44.516f, "Volgograd"},
    {47.227f, 39.723f, "Rostov"},
    {69.008f, 33.074f, "Murmansk"},
    {56.838f, 60.597f, "Ekaterinburg"},
    {54.984f, 73.368f, "Omsk"},
    {55.030f, 82.920f, "Novosibirsk"},
    {56.010f, 92.852f, "Krasnoyarsk"},
    {52.290f, 104.297f, "Irkutsk"},
    // Siberia & Far East (97-104)
    {61.799f, 129.474f, "Yakutsk"},
    {51.672f, 135.066f, "Khabarovsk"},
    {43.133f, 131.906f, "Vladivostok"},
    {59.561f, 150.793f, "Magadan"},
    {69.350f, 88.202f, "Norilsk"},
    {71.630f, 128.870f, "Tiksi"},
    {46.959f, 142.738f, "Sakhalinsk"},
    {53.750f, 87.117f, "Novokuznetsk"},
    // Central Asia (105-112)
    {51.180f, 71.446f, "Astana"},
    {43.257f, 76.947f, "Almaty"},
    {41.299f, 69.240f, "Tashkent"},
    {42.870f, 74.590f, "Bishkek"},
    {37.940f, 58.380f, "Ashgabat"},
    {34.525f, 69.178f, "Kabul"},
    {33.729f, 73.094f, "Islamabad"},
    {31.549f, 74.344f, "Lahore"},
    // Middle East (113-126)
    {33.893f, 35.502f, "Beirut"},
    {33.510f, 36.291f, "Damascus"},
    {31.963f, 35.930f, "Amman"},
    {31.768f, 35.214f, "Jerusalem"},
    {30.044f, 31.236f, "Cairo"},
    {31.203f, 29.919f, "Alexandria"},
    {24.713f, 46.675f, "Riyadh"},
    {21.485f, 39.192f, "Jeddah"},
    {25.285f, 51.531f, "Doha"},
    {25.205f, 55.271f, "Dubai"},
    {35.676f, 51.389f, "Tehran"},
    {33.342f, 44.401f, "Baghdad"},
    {29.358f, 47.990f, "Kuwait"},
    {15.369f, 44.191f, "Sanaa"},
    // South Asia (127-137)
    {24.860f, 67.001f, "Karachi"},
    {19.076f, 72.878f, "Mumbai"},
    {28.614f, 77.209f, "Delhi"},
    {22.572f, 88.363f, "Kolkata"},
    {23.810f, 90.412f, "Dhaka"},
    {6.927f, 79.861f, "Colombo"},
    {27.717f, 85.314f, "Kathmandu"},
    {13.090f, 80.279f, "Chennai"},
    {17.385f, 78.487f, "Hyderabad"},
    {12.971f, 77.595f, "Bangalore"},
    {23.022f, 72.572f, "Ahmedabad"},
    // Southeast Asia (138-148)
    {16.866f, 96.195f, "Yangon"},
    {13.756f, 100.501f, "Bangkok"},
    {21.028f, 105.834f, "Hanoi"},
    {10.823f, 106.629f, "HoChiMinh"},
    {11.556f, 104.917f, "PhnomPenh"},
    {17.974f, 102.630f, "Vientiane"},
    {3.139f, 101.687f, "KualaLumpur"},
    {1.352f, 103.820f, "Singapore"},
    {14.600f, 120.984f, "Manila"},
    {-6.200f, 106.816f, "Jakarta"},
    {-8.559f, 115.178f, "Bali"},
    // East Asia (149-160)
    {22.319f, 114.169f, "HongKong"},
    {25.047f, 121.532f, "Taipei"},
    {39.916f, 116.397f, "Beijing"},
    {31.230f, 121.474f, "Shanghai"},
    {29.564f, 106.551f, "Chongqing"},
    {23.129f, 113.260f, "Guangzhou"},
    {37.566f, 126.978f, "Seoul"},
    {35.689f, 139.692f, "Tokyo"},
    {34.693f, 135.502f, "Osaka"},
    {43.063f, 141.354f, "Sapporo"},
    {47.921f, 106.905f, "Ulaanbaatar"},
    {45.754f, 126.642f, "Harbin"},
    // Australasia & Pacific (161-172)
    {-33.869f, 151.209f, "Sydney"},
    {-37.814f, 144.963f, "Melbourne"},
    {-27.468f, 153.028f, "Brisbane"},
    {-31.953f, 115.857f, "Perth"},
    {-34.929f, 138.601f, "Adelaide"},
    {-12.463f, 130.843f, "Darwin"},
    {-36.848f, 174.763f, "Auckland"},
    {-41.286f, 174.776f, "Wellington"},
    {-43.532f, 172.637f, "Christchurch"},
    {21.306f, -157.858f, "Honolulu"},
    {-9.432f, 160.064f, "Honiara"},
    {-9.446f, 147.181f, "PortMoresby"},
    // North America (173-204)
    {40.713f, -74.006f, "New York"},
    {42.360f, -71.058f, "Boston"},
    {39.952f, -75.164f, "Philadelphia"},
    {38.907f, -77.037f, "Washington"},
    {25.762f, -80.192f, "Miami"},
    {30.332f, -81.655f, "Jacksonville"},
    {33.749f, -84.388f, "Atlanta"},
    {35.227f, -80.843f, "Charlotte"},
    {36.174f, -86.768f, "Nashville"},
    {29.760f, -95.369f, "Houston"},
    {32.787f, -96.797f, "Dallas"},
    {29.951f, -90.072f, "NewOrleans"},
    {41.878f, -87.629f, "Chicago"},
    {42.331f, -83.046f, "Detroit"},
    {44.980f, -93.270f, "Minneapolis"},
    {38.627f, -90.199f, "StLouis"},
    {39.099f, -94.578f, "KansasCity"},
    {39.739f, -104.990f, "Denver"},
    {33.448f, -112.074f, "Phoenix"},
    {36.175f, -115.137f, "Las Vegas"},
    {37.773f, -122.419f, "SanFrancisco"},
    {34.052f, -118.244f, "LosAngeles"},
    {47.606f, -122.332f, "Seattle"},
    {45.523f, -122.676f, "Portland"},
    {61.218f, -149.900f, "Anchorage"},
    {64.838f, -147.716f, "Fairbanks"},
    {49.282f, -123.121f, "Vancouver"},
    {51.045f, -114.071f, "Calgary"},
    {53.546f, -113.491f, "Edmonton"},
    {49.899f, -97.138f, "Winnipeg"},
    {43.653f, -79.383f, "Toronto"},
    {45.501f, -73.567f, "Montreal"},
    {45.421f, -75.697f, "Ottawa"},
    // Central America & Caribbean (205-214)
    {19.433f, -99.133f, "MexicoCity"},
    {23.133f, -82.383f, "Havana"},
    {17.997f, -76.793f, "Kingston"},
    {18.472f, -66.110f, "SanJuan"},
    {18.486f, -69.931f, "SantoDomingo"},
    {9.000f, -79.500f, "Panama"},
    {9.936f, -84.084f, "SanJoseCR"},
    {14.641f, -90.513f, "Guatemala"},
    {14.093f, -87.207f, "Tegucigalpa"},
    {17.250f, -88.768f, "BelizeCity"},
    // South America (215-229)
    {4.711f, -74.072f, "Bogota"},
    {10.491f, -66.902f, "Caracas"},
    {-0.180f, -78.468f, "Quito"},
    {-12.047f, -77.043f, "Lima"},
    {-16.500f, -68.150f, "LaPaz"},
    {-17.800f, -63.160f, "SantaCruz"},
    {-3.119f, -60.021f, "Manaus"},
    {-3.717f, -38.543f, "Fortaleza"},
    {-8.063f, -34.871f, "Recife"},
    {-15.779f, -47.930f, "Brasilia"},
    {-23.551f, -46.634f, "SaoPaulo"},
    {-22.906f, -43.173f, "Rio"},
    {-34.603f, -58.382f, "BuenosAires"},
    {-33.448f, -70.669f, "Santiago"},
    {-34.901f, -56.165f, "Montevideo"},
    // Africa (230-251)
    {33.573f, -7.589f, "Casablanca"},
    {31.628f, -7.987f, "Marrakech"},
    {36.737f, 3.086f, "Algiers"},
    {36.819f, 10.168f, "Tunis"},
    {32.902f, 13.180f, "Tripoli"},
    {15.552f, 32.532f, "Khartoum"},
    {14.690f, -17.447f, "Dakar"},
    {5.559f, -0.197f, "Accra"},
    {6.524f, 3.379f, "Lagos"},
    {4.061f, 9.768f, "Douala"},
    {5.354f, -4.008f, "Abidjan"},
    {-4.325f, 15.322f, "Kinshasa"},
    {-8.839f, 13.289f, "Luanda"},
    {-1.286f, 36.817f, "Nairobi"},
    {-6.792f, 39.209f, "DarEsSalaam"},
    {9.005f, 38.763f, "AddisAbaba"},
    {-26.204f, 28.047f, "Johannesburg"},
    {-33.925f, 18.424f, "CapeTown"},
    {-25.891f, 32.605f, "Maputo"},
    {-18.914f, 47.536f, "Antananarivo"},
    {-15.417f, 28.283f, "Lusaka"},
    {-17.830f, 31.053f, "Harare"},
};
static const int mapPointCount = sizeof(mapPoints) / sizeof(mapPoints[0]);

static void drawAzimuthalMap()
{
    // Only rebuild base when zoom changes or on first call.
    if (!mapBaseDirty)
        return;
    if (!map_buf || !map_base_buf)
        return;

    mapBaseDirty = false;

    // Zoom levels: UK ~800km, Europe ~3000km, World ~20000km
    float maxDist[] = {800.0f, 3000.0f, 20000.0f};
    float dist = maxDist[currentZoom];

    {
        // Clear to dark background
        lv_color_t bgColor = lv_color_hex(0x0a0a1a);
        for (int i = 0; i < MAP_SIZE * MAP_SIZE; i++)
            map_buf[i] = bgColor;

        // Draw range rings
        lv_color_t ringColor = lv_color_hex(0x1a1a3a);
        for (int ring = 1; ring <= 6; ring++)
        {
            int r = MAP_R * ring / 6;
            for (int a = 0; a < 360; a++)
            {
                float rad = a * DEG_TO_RAD;
                int px = MAP_CX + (int)(r * sinf(rad));
                int py = MAP_CY - (int)(r * cosf(rad));
                if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE)
                    map_buf[py * MAP_SIZE + px] = ringColor;
            }
        }

        // Draw compass lines and spokes
        lv_color_t compassColor = lv_color_hex(0x222244);
        drawWideLine(MAP_CX, MAP_CY - MAP_R, MAP_CX, MAP_CY + MAP_R, compassColor);
        drawWideLine(MAP_CX - MAP_R, MAP_CY, MAP_CX + MAP_R, MAP_CY, compassColor);
        for (int a = 45; a < 360; a += 45)
        {
            float rad = a * DEG_TO_RAD;
            int ex = MAP_CX + (int)(MAP_R * sinf(rad));
            int ey = MAP_CY - (int)(MAP_R * cosf(rad));
            drawLine(MAP_CX, MAP_CY, ex, ey, lv_color_hex(0x1a2238));
        }

        // Draw outer circle (double-pixel)
        lv_color_t edgeColor = lv_color_hex(0x333366);
        for (int a = 0; a < 360; a++)
        {
            float rad = a * DEG_TO_RAD;
            int px = MAP_CX + (int)(MAP_R * sinf(rad));
            int py = MAP_CY - (int)(MAP_R * cosf(rad));
            int px2 = MAP_CX + (int)((MAP_R - 1) * sinf(rad));
            int py2 = MAP_CY - (int)((MAP_R - 1) * cosf(rad));
            if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE)
                map_buf[py * MAP_SIZE + px] = edgeColor;
            if (px2 >= 0 && px2 < MAP_SIZE && py2 >= 0 && py2 < MAP_SIZE)
                map_buf[py2 * MAP_SIZE + px2] = edgeColor;
        }

        // Draw coastlines
        lv_color_t coastColor = lv_color_hex(0xc8c8c8);
        int prevPx = -1, prevPy = -1;
        for (int i = 0; i < COASTLINE_POINTS; i++)
        {
            float clat = COASTLINE_DATA[i * 2];
            float clon = COASTLINE_DATA[i * 2 + 1];
            if (clat > 900.0f)
            {
                prevPx = prevPy = -1;
                continue;
            }
            int cpx, cpy;
            if (azimuthalProject(clat, clon, dist, cpx, cpy))
            {
                if (prevPx >= 0)
                {
                    int ddx = cpx - prevPx, ddy = cpy - prevPy;
                    if (ddx * ddx + ddy * ddy < MAP_SIZE * MAP_SIZE / 4)
                        drawWideLine(prevPx, prevPy, cpx, cpy, coastColor);
                }
                prevPx = cpx;
                prevPy = cpy;
            }
            else
            {
                prevPx = prevPy = -1;
            }
        }

        // Draw city dots (clipped to 85% of MAP_R in pixel space)
        lv_color_t cityColor = lv_color_hex(0x5588aa);
        lv_color_t ukColor = lv_color_hex(0x88ccff);
        const float clipR = MAP_R * 0.85f;
        if (currentZoom == 0)
        {
            for (int i = 0; i < 20; i++)
            {
                int px, py;
                if (azimuthalProject(mapPoints[i].lat, mapPoints[i].lon, dist, px, py))
                {
                    float dx = px - MAP_CX, dy = py - MAP_CY;
                    if (dx * dx + dy * dy <= clipR * clipR)
                        drawDot(px, py, 3, ukColor);
                }
            }
        }
        else
        {
            for (int k = 0; k < MAX_MAP_LABELS; k++)
            {
                int i = labeledCityIdx[k];
                int px, py;
                if (azimuthalProject(mapPoints[i].lat, mapPoints[i].lon, dist, px, py))
                {
                    float dx = px - MAP_CX, dy = py - MAP_CY;
                    if (dx * dx + dy * dy <= clipR * clipR)
                    {
                        bool isUK = (i < 20);
                        drawDot(px, py, isUK ? 3 : 2, isUK ? ukColor : cityColor);
                    }
                }
            }
        }

        // Draw rotator memory bearing markers
        static const lv_color_t memGroupColors[3] = {
            lv_color_hex(0x88ccff),
            lv_color_hex(0xCE93D8),
            lv_color_hex(0x81C784),
        };
        int markerR = MAP_R - 8;
        int g = currentZoom;
        for (int m = 0; m < MEM_PER_GROUP; m++)
        {
            if (rotatorMemories[g][m].name[0] == '\0')
                continue;
            float bearRad = rotatorMemories[g][m].bearing * DEG_TO_RAD;
            int mx = MAP_CX + (int)(markerR * sinf(bearRad));
            int my = MAP_CY - (int)(markerR * cosf(bearRad));
            if (mx >= 0 && mx < MAP_SIZE && my >= 0 && my < MAP_SIZE)
                drawDot(mx, my, 4, memGroupColors[g]);
        }

        // Draw QTH centre
        drawDot(MAP_CX, MAP_CY, 4, lv_color_hex(0xff4444));

        // Save clean base — bearing updates restore from this then draw lines on top.
        memcpy(map_base_buf, map_buf, MAP_SIZE * MAP_SIZE * sizeof(lv_color_t));
    } // end base render

    // Register pixel buffer with LVGL once per zoom change. lv_img_set_src()
    // invalidates canvas_map itself; updateBearingLinesDirect() (called right
    // after this by the caller) overwrites map_buf again and invalidates once
    // more, so no manual lv_timer_handler() nudge is needed here.
    if (canvas_map)
    {
        map_img_dsc.header.always_zero = 0;
        map_img_dsc.header.w  = MAP_SIZE;
        map_img_dsc.header.h  = MAP_SIZE;
        map_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        map_img_dsc.data_size = MAP_SIZE * MAP_SIZE * sizeof(lv_color_t);
        map_img_dsc.data      = (const uint8_t *)map_buf;
        lv_img_set_src(canvas_map, &map_img_dsc);
    }
}

// Fill a rectangle directly into map_buf (MAP_SIZE-relative coords). Only ever
// used on map_buf, which LVGL redraws normally via lv_obj_invalidate() — see
// updateBearingLinesDirect().
static void fbFillRect(lv_color_t *fb, int x, int y, int w, int h, lv_color_t col)
{
    for (int r = y; r < y + h; r++)
    {
        if (r < 0 || r >= MAP_SIZE) continue;
        for (int c = x; c < x + w; c++)
        {
            if (c < 0 || c >= MAP_SIZE) continue;
            fb[r * MAP_SIZE + c] = col;
        }
    }
}

// Render ASCII text into map_buf (MAP_SIZE-relative coords) using lv_font_montserrat_12.
// (x, y) is the top-left of the first character's bounding box (NOT the baseline).
static void fbDrawText(lv_color_t *fb, int x, int y, const char *text, lv_color_t color,
                       const lv_font_t *font = &lv_font_montserrat_16)
{
    if (!fb || !text) return;
    int cx = x;
    for (int i = 0; text[i]; i++)
    {
        uint32_t letter      = (uint8_t)text[i];
        uint32_t letter_next = text[i + 1] ? (uint8_t)text[i + 1] : 0;
        lv_font_glyph_dsc_t g;
        if (!lv_font_get_glyph_dsc(font, &g, letter, letter_next)) { cx += g.adv_w >> 4; continue; }
        if (g.box_w == 0 || g.box_h == 0) { cx += g.adv_w >> 4; continue; }

        // Use resolved_font to match lv_draw_sw_letter.c (handles font fallback)
        const lv_font_t *rfont = g.resolved_font ? g.resolved_font : font;
        const uint8_t *bmp = lv_font_get_glyph_bitmap(rfont, letter);
        if (!bmp) { cx += g.adv_w >> 4; continue; }

        // LVGL treats bpp=3 as bpp=4 (see lv_draw_sw_letter.c)
        uint32_t bpp = g.bpp;
        if (bpp == 3 || bpp == 0) bpp = 4;

        // Glyph top-left on screen — exact formula from lv_draw_sw_letter.c:
        //   gpos.y = pos.y + (line_height - base_line) - box_h - ofs_y
        int glyph_top = y + ((int)font->line_height - (int)font->base_line)
                          - (int)g.box_h - (int)g.ofs_y;

        // LVGL bitmap is a flat MSB-first bitstream (NO row padding).
        // Use sliding bitmask exactly as lv_draw_sw_letter.c does.
        uint32_t bitmask_init = (0xFFu << (8u - bpp)) & 0xFFu;
        uint32_t col_bit_max  = 8u - bpp;
        const uint8_t *map_p  = bmp;
        uint32_t col_bit      = 0;

        for (int row = 0; row < (int)g.box_h; row++)
        {
            for (int col = 0; col < (int)g.box_w; col++)
            {
                uint8_t alpha = (*map_p & (bitmask_init >> col_bit)) >> (col_bit_max - col_bit);
                // Draw any non-transparent pixel (includes anti-aliased edges)
                if (alpha > 0)
                {
                    int px = cx + (int)g.ofs_x + col;
                    int py = glyph_top + row;
                    if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE)
                        fb[py * MAP_SIZE + px] = color;
                }
                // Advance bit pointer through flat bitstream
                if (col_bit < col_bit_max) {
                    col_bit += bpp;
                } else {
                    col_bit = 0;
                    map_p++;
                }
            }
            // No row-padding reset: LVGL bitstream is flat, rows share bytes
        }
        // lv_font_get_glyph_dsc already converts adv_w from 1/16-px to whole pixels
        cx += (int)g.adv_w;
    }
}

// Update bearing + target lines by writing pixels directly into map_buf
// (MAP_SIZE-relative coordinates), then invalidating canvas_map so LVGL redraws
// it through the normal render pipeline. map_base_buf contains clean pixels
// (coastlines + city label text + city dots), so restoring from it correctly
// preserves city names.
static void updateBearingLinesDirect()
{
    if (!map_buf || !map_base_buf || !canvas_map)
        return;

    bool rotOnline = (rotatorBearing >= 0) && (millis() - rotatorLastUpdate < 60000);

    lv_color_t *fb = map_buf;

    // Restore clean map from base (erases old bearing line, keeps labels)
    memcpy(fb, map_base_buf, MAP_SIZE * MAP_SIZE * sizeof(lv_color_t));

    // Draw green bearing line (3px wide Bresenham)
    if (rotOnline)
    {
        float rad = rotatorBearing * (float)DEG_TO_RAD;
        int ex = MAP_CX + (int)(MAP_R * sinf(rad));
        int ey = MAP_CY - (int)(MAP_R * cosf(rad));
        auto fbPix = [&](int px, int py, lv_color_t col) {
            if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE)
                fb[py * MAP_SIZE + px] = col;
        };
        lv_color_t green = lv_color_hex(0x4caf50);
        int x0 = MAP_CX, y0 = MAP_CY, x1 = ex, y1 = ey;
        int dx = abs(x1-x0), sx2 = x0<x1?1:-1;
        int dy = -abs(y1-y0), sy2 = y0<y1?1:-1;
        int err = dx+dy;
        for (;;) {
            fbPix(x0,   y0,   green);
            fbPix(x0+1, y0,   green);
            fbPix(x0,   y0+1, green);
            if (x0==x1 && y0==y1) break;
            int e2 = 2*err;
            if (e2 >= dy) { err += dy; x0 += sx2; }
            if (e2 <= dx) { err += dx; y0 += sy2; }
        }
    }

    // Draw orange dashed target line
    if (rotOnline && rotatorMoving && rotatorTargetBearing >= 0)
    {
        float rad = rotatorTargetBearing * (float)DEG_TO_RAD;
        lv_color_t orange = lv_color_hex(0xff9800);
        for (int r = 0; r < MAP_R; r += 2)
        {
            int px = MAP_CX + (int)(r * sinf(rad));
            int py = MAP_CY - (int)(r * cosf(rad));
            if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE)
                fb[py * MAP_SIZE + px] = orange;
        }
    }

    // Status badge at top-left of map.
    // Background colour reflects state; "Rotating..." flashes at ~600ms period.
    {
        const char *statusText;
        lv_color_t bgCol;
        bool flash = rotOnline && rotatorEnabled && rotatorMoving;

        // Detect command failure: non-2xx HTTP response, or rotator still idle
        // 5 seconds after a goto/manual command was sent.
        bool cmdPending    = (rotatorCommandSentAt > 0);
        bool cmdErrorCode  = cmdPending && (g_rotatorCmdHttpCode != 0)
                                && (g_rotatorCmdHttpCode < 200 || g_rotatorCmdHttpCode >= 300);
        bool cmdTimeout    = cmdPending && !rotatorMoving
                                && (millis() - rotatorCommandSentAt > 5000)
                                && g_rotatorCmdHttpCode != 0; // result arrived but no motion
        static char errBuf[20];

        if (!rotOnline)
        {
            statusText = "Waiting..."; bgCol = lv_color_hex(0x7B1414);
        }
        else if (cmdErrorCode)
        {
            snprintf(errBuf, sizeof(errBuf), "Error %d", (int)g_rotatorCmdHttpCode);
            statusText = errBuf; bgCol = lv_color_hex(0xB71C1C);
        }
        else if (cmdTimeout)
        {
            statusText = "No response"; bgCol = lv_color_hex(0xB71C1C);
        }
        else if (!rotatorEnabled)
        {
            statusText = "Disabled";   bgCol = lv_color_hex(0x2a2a2a);
        }
        else if (rotatorMoving)
        {
            statusText = "Rotating..."; bgCol = lv_color_hex(0x1565C0);
        }
        else if (!rotatorCalibrated)
        {
            statusText = "No calibration"; bgCol = lv_color_hex(0x7B4F00);
        }
        else
        {
            statusText = "Ready"; bgCol = lv_color_hex(0x1B5E20);
        }

        // Badge: 4px from top-left corner of map, 28px tall (montserrat_16 line_height~20)
        int bx = 4;
        int by = 4;
        int bw = 148;
        int bh = 28;
        fbFillRect(fb, bx, by, bw, bh, bgCol);

        // Swoosh: when rotating, draw a bright stripe sweeping left→right over 1.2s
        if (rotatorMoving && rotOnline && rotatorEnabled)
        {
            uint32_t sweep_ms   = millis() % 1200u;
            // stripe centre travels across badge interior (skip 1-px rim each side)
            int inner_w         = bw - 2;
            int stripe_cx       = bx + 1 + (int)((uint32_t)sweep_ms * (uint32_t)inner_w / 1200u);
            const int STRIPE_HW = 10; // half-width in pixels
            for (int sy = by + 1; sy < by + bh - 1; sy++)
            {
                for (int dx2 = -STRIPE_HW; dx2 <= STRIPE_HW; dx2++)
                {
                    int sx = stripe_cx + dx2;
                    if (sx <= bx || sx >= bx + bw - 1) continue;
                    // Fade: full bright at centre, normal at edge
                    int dist = abs(dx2);
                    lv_color_t sc = (dist <= STRIPE_HW / 3)
                        ? lv_color_hex(0x90CAF9)  // near-white blue at core
                        : lv_color_hex(0x42A5F5); // medium-light blue at shoulders
                    fb[sy * MAP_SIZE + sx] = sc;
                }
            }
        }

        // 1-px white rim
        lv_color_t rim = lv_color_hex(0xffffff);
        fbFillRect(fb, bx,        by,        bw, 1,  rim);
        fbFillRect(fb, bx,        by+bh-1,   bw, 1,  rim);
        fbFillRect(fb, bx,        by,        1,  bh, rim);
        fbFillRect(fb, bx+bw-1,   by,        1,  bh, rim);
        // Text: 5px left padding, top of text area = by + 4
        fbDrawText(fb, bx + 6, by + 4, statusText, lv_color_hex(0xffffff));
    }

    // map_buf is updated; tell LVGL to redraw canvas_map through the normal
    // render pipeline (partial-buffer flush via lvgl_flush_cb -> draw_bitmap).
    lv_obj_invalidate(canvas_map);
}

// ============================================================
// UI Construction
// ============================================================

static void create_overview_tab(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_set_style_pad_gap(parent, 5, 0);
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE); // prevent whole-tab vertical scroll

    lv_obj_t *title_row = lv_obj_create(parent);
    lv_obj_set_size(title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE);

    btn_touch_reboot = lv_btn_create(title_row);
    lv_obj_set_size(btn_touch_reboot, 138, 36);
    lv_obj_set_style_bg_color(btn_touch_reboot, lv_color_hex(0xB71C1C), 0);
    lv_obj_set_style_radius(btn_touch_reboot, 10, 0);
    lv_obj_add_event_cb(btn_touch_reboot, reboot_touch_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reboot_lbl = lv_label_create(btn_touch_reboot);
    lv_label_set_text(reboot_lbl, LV_SYMBOL_REFRESH " Reboot Touch");
    lv_obj_center(reboot_lbl);

    // Info row 1: WiFi + Uptime side by side
    lv_obj_t *info_row1 = lv_obj_create(parent);
    lv_obj_set_size(info_row1, LV_PCT(100), 28);
    lv_obj_set_flex_flow(info_row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(info_row1, 4, 0);
    lv_obj_set_style_bg_color(info_row1, lv_color_hex(0x151b22), 0);
    lv_obj_set_style_bg_opa(info_row1, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(info_row1, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_border_width(info_row1, 1, 0);
    lv_obj_set_style_radius(info_row1, 6, 0);
    lv_obj_clear_flag(info_row1, LV_OBJ_FLAG_SCROLLABLE);

    lbl_wifi = lv_label_create(info_row1);
    lv_label_set_text(lbl_wifi, "WiFi: not connected");
    lv_obj_set_style_text_font(lbl_wifi, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(lbl_wifi, LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(lbl_wifi, 1);
    lv_obj_set_height(lbl_wifi, 20);

    lbl_uptime = lv_label_create(info_row1);
    lv_label_set_text(lbl_uptime, "Uptime: 0s");
    lv_obj_set_style_text_font(lbl_uptime, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(lbl_uptime, lv_color_hex(0x8fa0ae), 0);
    lv_label_set_long_mode(lbl_uptime, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(lbl_uptime, 280, 20);

    // Info row 2: Build + Heap
    lv_obj_t *info_row2 = lv_obj_create(parent);
    lv_obj_set_size(info_row2, LV_PCT(100), 24);
    lv_obj_set_flex_flow(info_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info_row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(info_row2, 4, 0);
    lv_obj_set_style_bg_opa(info_row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info_row2, 0, 0);
    lv_obj_clear_flag(info_row2, LV_OBJ_FLAG_SCROLLABLE);

    lbl_build = lv_label_create(info_row2);
    lv_label_set_text(lbl_build, "Build: --");
    lv_obj_set_style_text_font(lbl_build, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_build, lv_color_hex(0x8fa0ae), 0);
    lv_label_set_long_mode(lbl_build, LV_LABEL_LONG_CLIP);
    lv_obj_set_flex_grow(lbl_build, 1);
    lv_obj_set_height(lbl_build, 16);

    lbl_overview_hw = lv_label_create(info_row2);
    lv_label_set_text(lbl_overview_hw, "Heap: -- KB   PSRAM: -- KB");
    lv_obj_set_style_text_font(lbl_overview_hw, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_overview_hw, lv_color_hex(0x8fa0ae), 0);
    lv_label_set_long_mode(lbl_overview_hw, LV_LABEL_LONG_CLIP);
    lv_obj_set_size(lbl_overview_hw, 350, 16);

    // Peer count label
    lbl_peers = lv_label_create(parent);
    lv_label_set_text(lbl_peers, "Peers: scanning...");
    lv_obj_set_style_text_font(lbl_peers, &lv_font_montserrat_16, 0);
    lv_obj_set_size(lbl_peers, LV_PCT(100), 24);
    lv_label_set_long_mode(lbl_peers, LV_LABEL_LONG_CLIP);

    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(hdr, 4, 0);
    lv_obj_set_style_pad_gap(hdr, 8, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(0x2b3541), 0);
    lv_obj_clear_flag(hdr, LV_OBJ_FLAG_SCROLLABLE);

    const char *headers[] = {"Name", "IP Address", "Site", "Uptime", "Build Date", "Status", ""};
    const int widths[] = {200, 140, 75, 110, 190, 90, 95};
    for (int i = 0; i < 7; i++)
    {
        lv_obj_t *lbl = lv_label_create(hdr);
        lv_obj_set_width(lbl, widths[i]);
        lv_label_set_text(lbl, headers[i]);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x8fa0ae), 0);
    }

    for (int i = 0; i < MAX_PEER_ROWS; i++)
    {
        peer_row_objs[i] = lv_obj_create(parent);
        lv_obj_set_size(peer_row_objs[i], lv_pct(100), 44);
        lv_obj_set_flex_flow(peer_row_objs[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(peer_row_objs[i], LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(peer_row_objs[i], 4, 0);
        lv_obj_set_style_pad_gap(peer_row_objs[i], 8, 0);
        lv_obj_set_style_bg_color(peer_row_objs[i], i % 2 == 0 ? lv_color_hex(0x151b22) : lv_color_hex(0x111518), 0);
        lv_obj_set_style_border_color(peer_row_objs[i], lv_color_hex(0x2b3541), 0);
        lv_obj_clear_flag(peer_row_objs[i], LV_OBJ_FLAG_SCROLLABLE); // prevent accidental horizontal scroll corrupting Name column
        lv_obj_add_flag(peer_row_objs[i], LV_OBJ_FLAG_HIDDEN);

        peer_name_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_size(peer_name_labels[i], widths[0], 20);
        lv_label_set_long_mode(peer_name_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_name_labels[i], &lv_font_montserrat_14, 0);
        lv_label_set_text(peer_name_labels[i], "");

        peer_ip_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_size(peer_ip_labels[i], widths[1], 20);
        lv_label_set_long_mode(peer_ip_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_ip_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(peer_ip_labels[i], lv_color_hex(0xaabbcc), 0);
        lv_label_set_text(peer_ip_labels[i], "");

        peer_site_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_size(peer_site_labels[i], widths[2], 20);
        lv_label_set_long_mode(peer_site_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_site_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(peer_site_labels[i], lv_color_hex(0x8fa0ae), 0);
        lv_label_set_text(peer_site_labels[i], "");

        peer_uptime_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_size(peer_uptime_labels[i], widths[3], 20);
        lv_label_set_long_mode(peer_uptime_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_uptime_labels[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(peer_uptime_labels[i], lv_color_hex(0x8fa0ae), 0);
        lv_label_set_text(peer_uptime_labels[i], "-");

        peer_build_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_size(peer_build_labels[i], widths[4], 16);
        lv_label_set_long_mode(peer_build_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_build_labels[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(peer_build_labels[i], lv_color_hex(0x6a7c8a), 0);
        lv_label_set_text(peer_build_labels[i], "-");

        peer_status_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_size(peer_status_labels[i], widths[5], 20);
        lv_label_set_long_mode(peer_status_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_status_labels[i], &lv_font_montserrat_14, 0);
        lv_label_set_text(peer_status_labels[i], "");

        peer_reboot_btns[i] = lv_btn_create(peer_row_objs[i]);
        lv_obj_set_size(peer_reboot_btns[i], widths[6], 36);
        lv_obj_set_style_bg_color(peer_reboot_btns[i], lv_color_hex(0xB71C1C), 0);
        lv_obj_set_style_radius(peer_reboot_btns[i], 6, 0);
        lv_obj_add_event_cb(peer_reboot_btns[i], peer_reboot_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *icon = lv_label_create(peer_reboot_btns[i]);
        lv_label_set_text(icon, LV_SYMBOL_REFRESH " Reboot");
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_12, 0);
        lv_obj_center(icon);
    }
}

// Callback: tap the gateway indicator to cycle override mode (auto → remote → local → auto)
static void gw_override_btn_cb(lv_event_t *e)
{
    loraGwOverride = (loraGwOverride + 1) % 3;
}

static void create_power_tab(lv_obj_t *parent)
{
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    // Status cards row (Battery 1, Battery 2, Solar, LoRa Signal)
    lv_obj_t *cards_row = lv_obj_create(parent);
    lv_obj_set_size(cards_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cards_row, 4, 0);
    lv_obj_set_style_bg_opa(cards_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards_row, 0, 0);
    lv_obj_clear_flag(cards_row, LV_OBJ_FLAG_SCROLLABLE);

    // Battery 1 card
    lv_obj_t *bat1_card = lv_obj_create(cards_row);
    lv_obj_set_size(bat1_card, LV_PCT(19), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bat1_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(bat1_card, 8, 0);
    lv_obj_set_style_pad_gap(bat1_card, 2, 0);
    lv_obj_set_style_bg_color(bat1_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(bat1_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(bat1_card, 12, 0);
    lv_obj_clear_flag(bat1_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bat1_title = lv_label_create(bat1_card);
    lv_label_set_text(bat1_title, "Battery 1");
    lv_obj_set_style_text_color(bat1_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(bat1_title, &lv_font_montserrat_12, 0);

    lbl_bat1_soc = lv_label_create(bat1_card);
    lv_label_set_text(lbl_bat1_soc, "--%");
    lv_obj_set_style_text_font(lbl_bat1_soc, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_bat1_soc, lv_color_hex(0x888888), 0);
    lv_obj_set_size(lbl_bat1_soc, LV_PCT(100), 32);
    lv_label_set_long_mode(lbl_bat1_soc, LV_LABEL_LONG_CLIP);

    lbl_bat1_voltage = lv_label_create(bat1_card);
    lv_label_set_text(lbl_bat1_voltage, "");
    lv_obj_set_style_text_font(lbl_bat1_voltage, &lv_font_montserrat_12, 0);
    lv_obj_set_size(lbl_bat1_voltage, LV_PCT(100), 18);
    lv_label_set_long_mode(lbl_bat1_voltage, LV_LABEL_LONG_CLIP);

    bar_bat1_soc = lv_bar_create(bat1_card);
    lv_obj_set_size(bar_bat1_soc, LV_PCT(100), 10);
    lv_bar_set_range(bar_bat1_soc, 0, 100);
    lv_bar_set_value(bar_bat1_soc, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_bat1_soc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_bat1_soc, lv_color_hex(0x888888), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_bat1_soc, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_bat1_soc, 4, LV_PART_INDICATOR);

    // Battery 2 card
    lv_obj_t *bat2_card = lv_obj_create(cards_row);
    lv_obj_set_size(bat2_card, LV_PCT(19), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bat2_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(bat2_card, 8, 0);
    lv_obj_set_style_pad_gap(bat2_card, 2, 0);
    lv_obj_set_style_bg_color(bat2_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(bat2_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(bat2_card, 12, 0);
    lv_obj_clear_flag(bat2_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bat2_title = lv_label_create(bat2_card);
    lv_label_set_text(bat2_title, "Battery 2");
    lv_obj_set_style_text_color(bat2_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(bat2_title, &lv_font_montserrat_12, 0);

    lbl_bat2_soc = lv_label_create(bat2_card);
    lv_label_set_text(lbl_bat2_soc, "--%");
    lv_obj_set_style_text_font(lbl_bat2_soc, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_bat2_soc, lv_color_hex(0x888888), 0);
    lv_obj_set_size(lbl_bat2_soc, LV_PCT(100), 32);
    lv_label_set_long_mode(lbl_bat2_soc, LV_LABEL_LONG_CLIP);

    lbl_bat2_voltage = lv_label_create(bat2_card);
    lv_label_set_text(lbl_bat2_voltage, "");
    lv_obj_set_style_text_font(lbl_bat2_voltage, &lv_font_montserrat_12, 0);
    lv_obj_set_size(lbl_bat2_voltage, LV_PCT(100), 18);
    lv_label_set_long_mode(lbl_bat2_voltage, LV_LABEL_LONG_CLIP);

    bar_bat2_soc = lv_bar_create(bat2_card);
    lv_obj_set_size(bar_bat2_soc, LV_PCT(100), 10);
    lv_bar_set_range(bar_bat2_soc, 0, 100);
    lv_bar_set_value(bar_bat2_soc, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_bat2_soc, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_bat2_soc, lv_color_hex(0x888888), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_bat2_soc, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_bat2_soc, 4, LV_PART_INDICATOR);

    // Solar/MPPT card
    lv_obj_t *mppt_card = lv_obj_create(cards_row);
    lv_obj_set_size(mppt_card, LV_PCT(19), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mppt_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(mppt_card, 8, 0);
    lv_obj_set_style_pad_gap(mppt_card, 2, 0);
    lv_obj_set_style_bg_color(mppt_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(mppt_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(mppt_card, 12, 0);
    lv_obj_clear_flag(mppt_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *mppt_title = lv_label_create(mppt_card);
    lv_label_set_text(mppt_title, "Solar");
    lv_obj_set_style_text_color(mppt_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(mppt_title, &lv_font_montserrat_12, 0);

    lbl_mppt_power = lv_label_create(mppt_card);
    lv_label_set_text(lbl_mppt_power, "--W");
    lv_obj_set_style_text_font(lbl_mppt_power, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_mppt_power, lv_color_hex(0x888888), 0);
    lv_obj_set_size(lbl_mppt_power, LV_PCT(100), 32);
    lv_label_set_long_mode(lbl_mppt_power, LV_LABEL_LONG_CLIP);

    lbl_mppt_state = lv_label_create(mppt_card);
    lv_label_set_text(lbl_mppt_state, "--");
    lv_obj_set_style_text_font(lbl_mppt_state, &lv_font_montserrat_12, 0);
    lv_obj_set_size(lbl_mppt_state, LV_PCT(100), 18);
    lv_label_set_long_mode(lbl_mppt_state, LV_LABEL_LONG_CLIP);

    // LoRa Signal card
    lv_obj_t *signal_card = lv_obj_create(cards_row);
    lv_obj_set_size(signal_card, LV_PCT(19), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(signal_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(signal_card, 8, 0);
    lv_obj_set_style_pad_gap(signal_card, 2, 0);
    lv_obj_set_style_bg_color(signal_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(signal_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(signal_card, 12, 0);
    lv_obj_clear_flag(signal_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *signal_title = lv_label_create(signal_card);
    lv_label_set_text(signal_title, "LoRa Local");
    lv_obj_set_style_text_color(signal_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(signal_title, &lv_font_montserrat_12, 0);

    lbl_signal_rssi = lv_label_create(signal_card);
    lv_label_set_text(lbl_signal_rssi, "-- dBm");
    lv_obj_set_style_text_font(lbl_signal_rssi, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_signal_rssi, lv_color_hex(0x888888), 0);
    lv_obj_set_size(lbl_signal_rssi, LV_PCT(100), 32);
    lv_label_set_long_mode(lbl_signal_rssi, LV_LABEL_LONG_CLIP);

    lbl_signal = lv_label_create(signal_card);
    lv_label_set_text(lbl_signal, "SNR: --");
    lv_obj_set_style_text_font(lbl_signal, &lv_font_montserrat_12, 0);
    lv_obj_set_size(lbl_signal, LV_PCT(100), 18);
    lv_label_set_long_mode(lbl_signal, LV_LABEL_LONG_CLIP);

    bar_signal = lv_bar_create(signal_card);
    lv_obj_set_size(bar_signal, LV_PCT(100), 10);
    lv_bar_set_range(bar_signal, 0, 100);
    lv_bar_set_value(bar_signal, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_signal, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_signal, lv_color_hex(0x888888), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_signal, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_signal, 4, LV_PART_INDICATOR);

    // LoRa Remote card — signal quality as seen by the remote (paddock) gateway
    lv_obj_t *remote_signal_card = lv_obj_create(cards_row);
    lv_obj_set_size(remote_signal_card, LV_PCT(19), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(remote_signal_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(remote_signal_card, 8, 0);
    lv_obj_set_style_pad_gap(remote_signal_card, 2, 0);
    lv_obj_set_style_bg_color(remote_signal_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(remote_signal_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(remote_signal_card, 12, 0);
    lv_obj_clear_flag(remote_signal_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *remote_signal_title = lv_label_create(remote_signal_card);
    lv_label_set_text(remote_signal_title, "LoRa Remote");
    lv_obj_set_style_text_color(remote_signal_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(remote_signal_title, &lv_font_montserrat_12, 0);

    lbl_remote_signal_rssi = lv_label_create(remote_signal_card);
    lv_label_set_text(lbl_remote_signal_rssi, "-- dBm");
    lv_obj_set_style_text_font(lbl_remote_signal_rssi, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(lbl_remote_signal_rssi, lv_color_hex(0x888888), 0);
    lv_obj_set_size(lbl_remote_signal_rssi, LV_PCT(100), 28);
    lv_label_set_long_mode(lbl_remote_signal_rssi, LV_LABEL_LONG_CLIP);

    lbl_remote_signal = lv_label_create(remote_signal_card);
    lv_label_set_text(lbl_remote_signal, "SNR: --");
    lv_obj_set_style_text_font(lbl_remote_signal, &lv_font_montserrat_12, 0);
    lv_obj_set_size(lbl_remote_signal, LV_PCT(100), 18);
    lv_label_set_long_mode(lbl_remote_signal, LV_LABEL_LONG_CLIP);

    bar_remote_signal = lv_bar_create(remote_signal_card);
    lv_obj_set_size(bar_remote_signal, LV_PCT(100), 10);
    lv_bar_set_range(bar_remote_signal, 0, 100);
    lv_bar_set_value(bar_remote_signal, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_remote_signal, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_remote_signal, lv_color_hex(0x888888), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_remote_signal, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_remote_signal, 4, LV_PART_INDICATOR);

    // All On / All Off / Gateway Override buttons
    lv_obj_t *all_row = lv_obj_create(parent);
    lv_obj_set_size(all_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(all_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(all_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(all_row, 4, 0);
    lv_obj_set_style_pad_gap(all_row, 12, 0);
    lv_obj_set_style_bg_opa(all_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(all_row, 0, 0);
    lv_obj_clear_flag(all_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_all_on = lv_btn_create(all_row);
    btn_all_on_g = btn_all_on;
    lv_obj_set_size(btn_all_on, 140, 52);
    lv_obj_set_style_bg_color(btn_all_on, lv_color_hex(0x4caf50), 0);
    lv_obj_add_event_cb(btn_all_on, all_on_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_aon = lv_label_create(btn_all_on);
    lv_label_set_text(lbl_aon, "All On");
    lv_obj_center(lbl_aon);

    lv_obj_t *btn_all_off = lv_btn_create(all_row);
    btn_all_off_g = btn_all_off;
    lv_obj_set_size(btn_all_off, 140, 52);
    lv_obj_set_style_bg_color(btn_all_off, lv_color_hex(0xff4d4d), 0);
    lv_obj_add_event_cb(btn_all_off, all_off_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_aoff = lv_label_create(btn_all_off);
    lv_label_set_text(lbl_aoff, "All Off");
    lv_obj_center(lbl_aoff);

    btn_gw_override = lv_btn_create(all_row);
    lv_obj_set_size(btn_gw_override, 140, 52);
    lv_obj_set_style_bg_color(btn_gw_override, lv_color_hex(0x555555), 0);
    lv_obj_set_style_shadow_width(btn_gw_override, 0, 0);
    lv_obj_add_event_cb(btn_gw_override, gw_override_btn_cb, LV_EVENT_CLICKED, NULL);
    lbl_gateway_route = lv_label_create(btn_gw_override);
    lv_label_set_text(lbl_gateway_route, "Auto: --");
    lv_obj_set_style_text_font(lbl_gateway_route, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_gateway_route, lv_color_white(), 0);
    lv_obj_set_style_text_align(lbl_gateway_route, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_size(lbl_gateway_route, LV_PCT(100), 20);
    lv_label_set_long_mode(lbl_gateway_route, LV_LABEL_LONG_CLIP);
    lv_obj_center(lbl_gateway_route);

    // Relay buttons grid (3 columns x 2 rows)
    lv_obj_t *relay_grid = lv_obj_create(parent);
    lv_obj_set_size(relay_grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(relay_grid, LV_LAYOUT_GRID);
    static lv_coord_t col_dsc[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t row_dsc[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(relay_grid, col_dsc, row_dsc);
    lv_obj_set_style_pad_all(relay_grid, 4, 0);
    lv_obj_set_style_pad_gap(relay_grid, 8, 0);
    lv_obj_set_style_bg_opa(relay_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(relay_grid, 0, 0);
    lv_obj_clear_flag(relay_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 6; i++)
    {
        lv_obj_t *btn = lv_btn_create(relay_grid);
        lv_obj_set_size(btn, LV_PCT(100), 64);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, i % 3, 1,
                             LV_GRID_ALIGN_CENTER, i / 3, 1);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_add_event_cb(btn, relay_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, relayLabels[i].c_str());
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        // LV_SIZE_CONTENT shrinks label to text height so lv_obj_center gives true V+H centre
        lv_obj_set_size(lbl, LV_PCT(95), LV_SIZE_CONTENT);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);

        power_relay_btns[i] = btn;
        power_relay_labels[i] = lbl;
    }

    // Status message
    lbl_power_status = lv_label_create(parent);
    lv_label_set_text(lbl_power_status, "Waiting for data...");
    lv_obj_set_style_text_color(lbl_power_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_power_status, &lv_font_montserrat_12, 0);
    lv_obj_set_size(lbl_power_status, LV_PCT(100), 18);
    lv_label_set_long_mode(lbl_power_status, LV_LABEL_LONG_CLIP);

    // WiFi link row — House card | ⚡ bolt | Paddock card
    // Outer container matches relay_grid padding so card edges align with relay buttons.
    lv_obj_t *wifi_row = lv_obj_create(parent);
    lv_obj_set_size(wifi_row, LV_PCT(100), 164);
    lv_obj_set_flex_flow(wifi_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wifi_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(wifi_row, 4, 0);
    lv_obj_set_style_pad_gap(wifi_row, 8, 0);
    lv_obj_set_style_bg_opa(wifi_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_row, 0, 0);
    lv_obj_clear_flag(wifi_row, LV_OBJ_FLAG_SCROLLABLE);

    // Build one card helper: i=0 House, i=1 Paddock
    lv_obj_t *wifi_cards[2];
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *card = lv_obj_create(wifi_row);
        wifi_cards[i] = card;
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_height(card, 156);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_gap(card, 8, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2128), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x2b3541), 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Left: text info column
        lv_obj_t *info = lv_obj_create(card);
        lv_obj_set_flex_grow(info, 1);
        lv_obj_set_height(info, 140);
        lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(info, 0, 0);
        lv_obj_set_style_pad_gap(info, 3, 0);
        lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(info, 0, 0);
        lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(info);
        lv_label_set_text(title, i == 0 ? "WiFi House" : "WiFi Paddock");
        lv_obj_set_style_text_color(title, lv_color_hex(0x8fa0ae), 0);
        lv_obj_set_style_text_font(title, &lv_font_montserrat_12, 0);

        lv_obj_t *rssi_l = lv_label_create(info);
        lv_label_set_text(rssi_l, "-- dBm");
        lv_obj_set_style_text_font(rssi_l, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(rssi_l, lv_color_hex(0x888888), 0);
        lv_obj_set_size(rssi_l, LV_PCT(100), 32);
        lv_label_set_long_mode(rssi_l, LV_LABEL_LONG_CLIP);

        lv_obj_t *snr_l = lv_label_create(info);
        lv_label_set_text(snr_l, "SNR: -- dB");
        lv_obj_set_style_text_font(snr_l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(snr_l, lv_color_hex(0x888888), 0);
        lv_obj_set_size(snr_l, LV_PCT(100), 18);
        lv_label_set_long_mode(snr_l, LV_LABEL_LONG_CLIP);

        lv_obj_t *sig_bar = lv_bar_create(info);
        lv_obj_set_size(sig_bar, LV_PCT(100), 10);
        lv_bar_set_range(sig_bar, 0, 100);
        lv_bar_set_value(sig_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(sig_bar, lv_color_hex(0x333333), LV_PART_MAIN);
        lv_obj_set_style_bg_color(sig_bar, lv_color_hex(0x888888), LV_PART_INDICATOR);
        lv_obj_set_style_radius(sig_bar, 4, LV_PART_MAIN);
        lv_obj_set_style_radius(sig_bar, 4, LV_PART_INDICATOR);

        lv_obj_t *tx_l = lv_label_create(info);
        lv_label_set_text(tx_l, "TX: -- Mbps");
        lv_obj_set_style_text_font(tx_l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(tx_l, lv_color_hex(0x4caf50), 0);
        lv_label_set_long_mode(tx_l, LV_LABEL_LONG_CLIP);

        lv_obj_t *rx_l = lv_label_create(info);
        lv_label_set_text(rx_l, "RX: -- Mbps");
        lv_obj_set_style_text_font(rx_l, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(rx_l, lv_color_hex(0x42a5f5), 0);
        lv_label_set_long_mode(rx_l, LV_LABEL_LONG_CLIP);

        // Right: bandwidth gauge (0–140 Mbps, two needles — prop-panel style)
        lv_obj_t *m = lv_meter_create(card);
        lv_obj_set_size(m, 140, 140);
        lv_obj_set_style_bg_color(m, lv_color_hex(0x0d1117), 0);
        lv_obj_set_style_border_width(m, 0, 0);
        lv_obj_set_style_text_font(m, &lv_font_montserrat_12, LV_PART_TICKS);
        lv_obj_set_style_text_color(m, lv_color_hex(0xcccccc), LV_PART_TICKS);

        lv_meter_scale_t *sc = lv_meter_add_scale(m);
        lv_meter_set_scale_range(m, sc, 0, 140, 270, 135);
        lv_meter_set_scale_ticks(m, sc, 29, 1, 6, lv_color_hex(0x505050));
        lv_meter_set_scale_major_ticks(m, sc, 7, 2, 14, lv_color_hex(0xcccccc), 4);

        // Background colour arcs: red → amber → green (same palette as solar gauges)
        lv_meter_indicator_t *arc_lo = lv_meter_add_arc(m, sc, 13, lv_color_hex(0xf44336), 0);
        lv_meter_set_indicator_start_value(m, arc_lo, 0);
        lv_meter_set_indicator_end_value(m, arc_lo, 47);
        lv_meter_indicator_t *arc_mi = lv_meter_add_arc(m, sc, 13, lv_color_hex(0xff9800), 0);
        lv_meter_set_indicator_start_value(m, arc_mi, 47);
        lv_meter_set_indicator_end_value(m, arc_mi, 93);
        lv_meter_indicator_t *arc_hi = lv_meter_add_arc(m, sc, 13, lv_color_hex(0x4caf50), 0);
        lv_meter_set_indicator_start_value(m, arc_hi, 93);
        lv_meter_set_indicator_end_value(m, arc_hi, 140);

        // TX needle (green, thicker — link rate / potential bandwidth)
        lv_meter_indicator_t *n_tx = lv_meter_add_needle_line(m, sc, 3, lv_color_hex(0x4caf50), -10);
        // RX needle (blue, thinner — received rate / used bandwidth)
        lv_meter_indicator_t *n_rx = lv_meter_add_needle_line(m, sc, 2, lv_color_hex(0x42a5f5), -8);

        if (i == 0)
        {
            lbl_wifi_house_rssi  = rssi_l;
            lbl_wifi_house_snr   = snr_l;
            lbl_wifi_house_tx    = tx_l;
            lbl_wifi_house_rx    = rx_l;
            bar_wifi_house_sig   = sig_bar;
            meter_wifi_house     = m;
            indic_house_tx       = n_tx;
            indic_house_rx       = n_rx;
        }
        else
        {
            lbl_wifi_pad_rssi   = rssi_l;
            lbl_wifi_pad_snr    = snr_l;
            lbl_wifi_pad_tx     = tx_l;
            lbl_wifi_pad_rx     = rx_l;
            bar_wifi_pad_sig    = sig_bar;
            meter_wifi_pad      = m;
            indic_pad_tx        = n_tx;
            indic_pad_rx        = n_rx;
        }

        // Insert lightning bolt between the two cards (after house card, before paddock card)
        if (i == 0)
        {
            lbl_link_bolt = lv_label_create(wifi_row);
            lv_label_set_text(lbl_link_bolt, LV_SYMBOL_CHARGE);
            lv_obj_set_size(lbl_link_bolt, 28, LV_SIZE_CONTENT);
            lv_obj_set_style_text_color(lbl_link_bolt, lv_color_hex(0x444444), 0);
            lv_obj_set_style_text_align(lbl_link_bolt, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(lbl_link_bolt, &lv_font_montserrat_24, 0);
        }
    }
    (void)wifi_cards;
}

static void create_antennas_tab(lv_obj_t *parent)
{
    // Fixed-height layout throughout — avoids LV_SIZE_CONTENT cascades that
    // cause repeated invalidations and visible artifacts on the RGB DMA panel.
    //
    // Tab content: LCD_WIDTH x (LCD_HEIGHT-64) = 1024x536
    // Padding 10px each side → inner = 1004x516
    // VFO bar 58px fixed + gap 6px = 64px overhead
    // Columns get the remaining ~452px
    static const lv_coord_t VFO_BAR_H = 58;
    static const lv_coord_t COLS_H = (lv_coord_t)(LCD_HEIGHT - 64 - 20 - VFO_BAR_H - 6);

    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    // VFO frequency bar — dark LCD-style panel, sits above the antenna columns
    lv_obj_t *vfo_bar = lv_obj_create(parent);
    lv_obj_set_size(vfo_bar, LV_PCT(100), VFO_BAR_H);
    lv_obj_set_flex_flow(vfo_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(vfo_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(vfo_bar, 8, 0);
    lv_obj_set_style_pad_ver(vfo_bar, 0, 0);
    lv_obj_set_style_pad_gap(vfo_bar, 8, 0);
    lv_obj_set_style_bg_color(vfo_bar, lv_color_hex(0x050500), 0); // near-black amber tint
    lv_obj_set_style_border_color(vfo_bar, lv_color_hex(0x6B4800), 0);
    lv_obj_set_style_border_width(vfo_bar, 2, 0);
    lv_obj_set_style_radius(vfo_bar, 8, 0);
    lv_obj_clear_flag(vfo_bar, LV_OBJ_FLAG_SCROLLABLE);

    // Each VFO slot: [band_tile (62 px fixed)] + [freq label (flex_grow)]
    // Tile is green on amateur/CB, red+OOB otherwise.
    static const lv_coord_t TILE_W   = 62;
    static const lv_coord_t TILE_H   = (lv_coord_t)(VFO_BAR_H - 12);
    static const lv_coord_t VFO_LBL_H = (lv_coord_t)(VFO_BAR_H - 8);

    for (int v = 0; v < 2; v++)
    {
        lv_obj_t *grp = lv_obj_create(vfo_bar);
        lv_obj_set_height(grp, VFO_BAR_H);
        lv_obj_set_flex_grow(grp, 1);
        lv_obj_set_flex_flow(grp, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(grp, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(grp, 4, 0);
        lv_obj_set_style_pad_ver(grp, 0, 0);
        lv_obj_set_style_pad_gap(grp, 6, 0);
        lv_obj_set_style_bg_opa(grp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(grp, 0, 0);
        lv_obj_clear_flag(grp, LV_OBJ_FLAG_SCROLLABLE);

        // Band indicator tile
        lv_obj_t *tile = lv_obj_create(grp);
        lv_obj_set_size(tile, TILE_W, TILE_H);
        lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tile, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(tile, 2, 0);
        lv_obj_set_style_bg_color(tile, lv_color_hex(0x1a1a1a), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x444444), 0);
        lv_obj_set_style_border_width(tile, 2, 0);
        lv_obj_set_style_radius(tile, 6, 0);
        lv_obj_clear_flag(tile, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *tileLbl = lv_label_create(tile);
        lv_label_set_text(tileLbl, "--");
        lv_obj_set_style_text_font(tileLbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(tileLbl, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_align(tileLbl, LV_TEXT_ALIGN_CENTER, 0);

        // Frequency label
        lv_obj_t *freqLbl = lv_label_create(grp);
        lv_obj_set_height(freqLbl, VFO_LBL_H);
        lv_obj_set_flex_grow(freqLbl, 1);
        lv_label_set_long_mode(freqLbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(freqLbl, v == 0 ? "VFO A: ---.--- MHz" : "VFO B: ---.--- MHz");
        lv_obj_set_style_text_font(freqLbl, &lv_font_montserrat_36, 0);
        lv_obj_set_style_text_color(freqLbl, lv_color_hex(0x40E060), 0);
        lv_obj_set_style_text_align(freqLbl, LV_TEXT_ALIGN_LEFT, 0);

        if (v == 0) { lbl_vfo_a = freqLbl; band_tile_a = tile; band_lbl_a = tileLbl; }
        else        { lbl_vfo_b = freqLbl; band_tile_b = tile; band_lbl_b = tileLbl; }
    }

    // 3-column container — fixed height so no child can force a re-layout
    lv_obj_t *cols = lv_obj_create(parent);
    lv_obj_set_size(cols, LV_PCT(100), COLS_H);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(cols, 0, 0);
    lv_obj_set_style_pad_gap(cols, 8, 0);
    lv_obj_set_style_bg_opa(cols, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cols, 0, 0);
    lv_obj_clear_flag(cols, LV_OBJ_FLAG_SCROLLABLE);

    static const uint32_t groupColorValues[3] = {0x64B5F6, 0xCE93D8, 0x81C784};

    for (int group = 0; group < 3; group++)
    {
        // Column panel — flex_grow(1) works because parent (cols) is fixed height
        lv_obj_t *col = lv_obj_create(cols);
        lv_obj_set_flex_grow(col, 1);
        lv_obj_set_height(col, LV_PCT(100)); // = COLS_H, resolved from fixed parent
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(col, 8, 0);
        lv_obj_set_style_pad_gap(col, 6, 0);
        lv_obj_set_style_bg_color(col, lv_color_hex(0x151b22), 0);
        lv_obj_set_style_border_color(col, lv_color_hex(0x2b3541), 0);
        lv_obj_set_style_radius(col, 8, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_SCROLLABLE);

        // Group header label
        lv_obj_t *groupLbl = lv_label_create(col);
        lv_obj_set_size(groupLbl, LV_PCT(100), 22);
        lv_label_set_long_mode(groupLbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(groupLbl, antennaGroupNames[group]);
        lv_obj_set_style_text_font(groupLbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(groupLbl, lv_color_hex(groupColorValues[group]), 0);
        lv_obj_set_style_text_align(groupLbl, LV_TEXT_ALIGN_CENTER, 0);
        antenna_group_labels[group] = groupLbl;

        // Button container — flex_grow fills remaining column height (fixed parent)
        antenna_group_rows[group] = lv_obj_create(col);
        lv_obj_set_width(antenna_group_rows[group], LV_PCT(100));
        lv_obj_set_flex_grow(antenna_group_rows[group], 1);
        lv_obj_set_flex_flow(antenna_group_rows[group], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(antenna_group_rows[group], 0, 0);
        lv_obj_set_style_pad_gap(antenna_group_rows[group], 6, 0);
        lv_obj_set_style_bg_opa(antenna_group_rows[group], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(antenna_group_rows[group], 0, 0);
        lv_obj_clear_flag(antenna_group_rows[group], LV_OBJ_FLAG_SCROLLABLE);
    }

    // Status message
    lbl_antenna_status = lv_label_create(parent);
    lv_label_set_text(lbl_antenna_status, "Waiting for antenna controller...");
    lv_obj_set_style_text_color(lbl_antenna_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_antenna_status, &lv_font_montserrat_12, 0);
    lv_obj_set_size(lbl_antenna_status, LV_PCT(100), 18);
    lv_label_set_long_mode(lbl_antenna_status, LV_LABEL_LONG_CLIP);
}

static void create_rotator_tab(lv_obj_t *parent)
{
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    // left_col: transparent wrapper column — dark map frame (flex_grow=1) above, zoom buttons below outside the frame
    lv_obj_t *left_col = lv_obj_create(parent);
    lv_obj_set_size(left_col, MAP_SIZE + 24, LV_PCT(100));
    lv_obj_set_flex_flow(left_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(left_col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(left_col, 0, 0);
    lv_obj_set_style_pad_gap(left_col, 6, 0);
    lv_obj_set_style_bg_opa(left_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left_col, 0, 0);
    lv_obj_clear_flag(left_col, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *map_panel = lv_obj_create(left_col);
    lv_obj_set_width(map_panel, LV_PCT(100));
    lv_obj_set_flex_grow(map_panel, 1);
    lv_obj_set_flex_flow(map_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(map_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(map_panel, 4, 0);
    lv_obj_set_style_pad_gap(map_panel, 0, 0);
    lv_obj_set_style_bg_color(map_panel, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_border_color(map_panel, lv_color_hex(0x333366), 0);
    lv_obj_set_style_radius(map_panel, 12, 0);
    lv_obj_clear_flag(map_panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *map_stack = lv_obj_create(map_panel);
    lv_obj_set_size(map_stack, MAP_SIZE, MAP_SIZE);
    lv_obj_set_layout(map_stack, 0);
    lv_obj_set_style_bg_opa(map_stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(map_stack, 0, 0);
    lv_obj_set_style_pad_all(map_stack, 0, 0);

    lv_obj_clear_flag(map_stack, LV_OBJ_FLAG_SCROLLABLE);

    canvas_map = lv_img_create(map_stack);
    lv_obj_set_size(canvas_map, MAP_SIZE, MAP_SIZE);
    lv_obj_align(canvas_map, LV_ALIGN_CENTER, 0, 0);
    // City labels are rendered as pixels into map_base_buf (not as LVGL objects)
    // so they are always present and the direct framebuffer restore preserves them.

    lv_obj_t *map_touch = lv_btn_create(map_stack);
    lv_obj_set_size(map_touch, MAP_SIZE, MAP_SIZE);
    lv_obj_align(map_touch, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(map_touch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(map_touch, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(map_touch, rotator_map_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *zoom_row = lv_obj_create(left_col);
    lv_obj_set_size(zoom_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(zoom_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(zoom_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(zoom_row, 2, 0);
    lv_obj_set_style_pad_top(zoom_row, 0, 0);
    lv_obj_set_style_pad_gap(zoom_row, 6, 0);
    lv_obj_set_style_bg_opa(zoom_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zoom_row, 0, 0);
    lv_obj_clear_flag(zoom_row, LV_OBJ_FLAG_SCROLLABLE);

    const char *zoomLabels[] = {"UK", "Europe", "World"};
    for (int i = 0; i < 3; i++)
    {
        btn_zoom[i] = lv_btn_create(zoom_row);
        lv_obj_set_size(btn_zoom[i], 140, 44);
        lv_obj_set_style_radius(btn_zoom[i], 10, 0);
        lv_obj_set_style_border_width(btn_zoom[i], 1, 0);
        if (i == currentZoom)
        {
            lv_obj_set_style_bg_color(btn_zoom[i], lv_color_hex(0x2196F3), 0);
            lv_obj_set_style_border_color(btn_zoom[i], lv_color_hex(0x64B5F6), 0);
        }
        else
        {
            lv_obj_set_style_bg_color(btn_zoom[i], lv_color_hex(0x1a2128), 0);
            lv_obj_set_style_border_color(btn_zoom[i], lv_color_hex(0x2b3541), 0);
        }
        lv_obj_add_event_cb(btn_zoom[i], zoom_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *lbl = lv_label_create(btn_zoom[i]);
        lv_label_set_text(lbl, zoomLabels[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_center(lbl);
    }

    lv_obj_t *ctrl_panel = lv_obj_create(parent);
    lv_obj_set_flex_grow(ctrl_panel, 1);
    lv_obj_set_height(ctrl_panel, LV_PCT(100));
    lv_obj_set_flex_flow(ctrl_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(ctrl_panel, 6, 0);
    lv_obj_set_style_pad_gap(ctrl_panel, 3, 0);
    lv_obj_set_style_bg_opa(ctrl_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_panel, 0, 0);
    lv_obj_clear_flag(ctrl_panel, LV_OBJ_FLAG_SCROLLABLE);

    // ---- Three status cards: Current / Target / Speed ----
    lv_obj_t *stats_row = lv_obj_create(ctrl_panel);
    lv_obj_set_size(stats_row, LV_PCT(100), 76);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(stats_row, 0, 0);
    lv_obj_set_style_pad_gap(stats_row, 6, 0);
    lv_obj_set_style_bg_opa(stats_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(stats_row, 0, 0);
    lv_obj_clear_flag(stats_row, LV_OBJ_FLAG_SCROLLABLE);

    const char *cardTitles[] = {"Current", "Target", "Speed"};
    lv_obj_t **cardLabels[] = {&lbl_rotator_bearing, &lbl_rotator_target, &lbl_rotator_speed};
    uint32_t cardColors[] = {0x4caf50, 0xff9800, 0x64b5f6};

    for (int ci = 0; ci < 3; ci++)
    {
        lv_obj_t *card = lv_obj_create(stats_row);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_height(card, 76);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_set_style_pad_gap(card, 2, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2128), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x2b3541), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *ctitle = lv_label_create(card);
        lv_label_set_text(ctitle, cardTitles[ci]);
        lv_obj_set_style_text_color(ctitle, lv_color_hex(0x8fa0ae), 0);
        lv_obj_set_style_text_font(ctitle, &lv_font_montserrat_12, 0);
        lv_obj_set_size(ctitle, LV_PCT(100), 16);
        lv_label_set_long_mode(ctitle, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(ctitle, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t *cval = lv_label_create(card);
        lv_label_set_text(cval, "-");
        lv_obj_set_style_text_font(cval, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(cval, lv_color_hex(cardColors[ci]), 0);
        lv_obj_set_size(cval, LV_PCT(100), 28);
        lv_label_set_long_mode(cval, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(cval, LV_TEXT_ALIGN_CENTER, 0);
        *cardLabels[ci] = cval;
    }

    struct MemoryGroup
    {
        const char *title;
        const RotatorMemoryPoint *points;
    };
    const MemoryGroup groups[] = {
        {"UK Memories", rotatorMemories[0]},
        {"Europe Memories", rotatorMemories[1]},
        {"World Memories", rotatorMemories[2]},
    };

    for (int g = 0; g < 3; g++)
    {
        rotator_memory_groups[g] = lv_obj_create(ctrl_panel);
        lv_obj_set_size(rotator_memory_groups[g], LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(rotator_memory_groups[g], LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(rotator_memory_groups[g], 0, 0);
        lv_obj_set_style_pad_gap(rotator_memory_groups[g], 2, 0);
        lv_obj_set_style_bg_opa(rotator_memory_groups[g], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rotator_memory_groups[g], 0, 0);
        lv_obj_clear_flag(rotator_memory_groups[g], LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *gtitle = lv_label_create(rotator_memory_groups[g]);
        lv_label_set_text(gtitle, groups[g].title);
        lv_obj_set_style_text_color(gtitle, lv_color_hex(0x8fa0ae), 0);
        lv_obj_set_style_text_font(gtitle, &lv_font_montserrat_12, 0);

        lv_obj_t *grid = lv_obj_create(rotator_memory_groups[g]);
        lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);
        static lv_coord_t mem_col[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
        static lv_coord_t mem_row[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
        lv_obj_set_grid_dsc_array(grid, mem_col, mem_row);
        lv_obj_set_style_pad_all(grid, 2, 0);
        lv_obj_set_style_pad_row(grid, 4, 0);
        lv_obj_set_style_pad_column(grid, 8, 0);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(grid, 0, 0);
        lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        rotator_memory_grids[g] = grid;

        for (int i = 0; i < MEM_PER_GROUP; i++)
        {
            lv_obj_t *btn = lv_btn_create(grid);
            lv_obj_set_size(btn, LV_PCT(100), 58);
            lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, i % 3, 1,
                                 LV_GRID_ALIGN_CENTER, i / 3, 1);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a2128), 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0x2b3541), 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_add_event_cb(btn, rotator_memory_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)((g << 8) | i));
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, groups[g].points[i].name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lbl, LV_PCT(96));
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(lbl);
        }

        if (g != currentZoom)
            lv_obj_add_flag(rotator_memory_groups[g], LV_OBJ_FLAG_HIDDEN);
    }

    lbl_rotator_status = lv_label_create(ctrl_panel);
    lv_label_set_text(lbl_rotator_status, "Tap map to request rotation target");
    lv_obj_set_style_text_color(lbl_rotator_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_rotator_status, &lv_font_montserrat_12, 0);
    lv_obj_set_size(lbl_rotator_status, LV_PCT(100), 18);
    lv_label_set_long_mode(lbl_rotator_status, LV_LABEL_LONG_CLIP);

    // Flex spacer — pushes << Stop >> row to the bottom of ctrl_panel,
    // aligning it visually with the zoom buttons in the map frame
    lv_obj_t *ctrl_spacer = lv_obj_create(ctrl_panel);
    lv_obj_set_size(ctrl_spacer, LV_PCT(100), 0);
    lv_obj_set_flex_grow(ctrl_spacer, 1);
    lv_obj_set_style_bg_opa(ctrl_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_spacer, 0, 0);
    lv_obj_set_style_pad_all(ctrl_spacer, 0, 0);

    lv_obj_t *manual_row = lv_obj_create(ctrl_panel);
    lv_obj_set_size(manual_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(manual_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(manual_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(manual_row, 2, 0);
    lv_obj_set_style_pad_gap(manual_row, 8, 0);
    lv_obj_set_style_bg_opa(manual_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(manual_row, 0, 0);
    lv_obj_clear_flag(manual_row, LV_OBJ_FLAG_SCROLLABLE);

    btn_manual_ccw = lv_btn_create(manual_row);
    lv_obj_set_size(btn_manual_ccw, 140, 44);
    lv_obj_set_style_radius(btn_manual_ccw, 10, 0);
    lv_obj_set_style_bg_color(btn_manual_ccw, lv_color_hex(0x1565C0), 0);
    lv_obj_add_event_cb(btn_manual_ccw, rotator_manual_ccw_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ccw = lv_label_create(btn_manual_ccw);
    lv_label_set_text(lbl_ccw, "<<");
    lv_obj_set_style_text_font(lbl_ccw, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_ccw);

    btn_manual_stop = lv_btn_create(manual_row);
    lv_obj_set_size(btn_manual_stop, 140, 44);
    lv_obj_set_style_radius(btn_manual_stop, 10, 0);
    lv_obj_set_style_bg_color(btn_manual_stop, lv_color_hex(0xC62828), 0);
    lv_obj_add_event_cb(btn_manual_stop, rotator_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_stop = lv_label_create(btn_manual_stop);
    lv_label_set_text(lbl_stop, "Stop");
    lv_obj_set_style_text_font(lbl_stop, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_stop);

    btn_manual_cw = lv_btn_create(manual_row);
    lv_obj_set_size(btn_manual_cw, 140, 44);
    lv_obj_set_style_radius(btn_manual_cw, 10, 0);
    lv_obj_set_style_bg_color(btn_manual_cw, lv_color_hex(0x2E7D32), 0);
    lv_obj_add_event_cb(btn_manual_cw, rotator_manual_cw_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cw = lv_label_create(btn_manual_cw);
    lv_label_set_text(lbl_cw, ">>");
    lv_obj_set_style_text_font(lbl_cw, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl_cw);

    drawAzimuthalMap();
    mapDirty = false;
}

static void create_propagation_tab(lv_obj_t *parent)
{
    lv_obj_clear_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    // 800x480 screen math: 44px tab header leaves ~436px vertical. Layout below targets ~420px total.
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 4, 0);
    lv_obj_set_style_pad_gap(parent, 4, 0);

    // Solar gauge row - 4 dials
    // Solar gauge row - 4 dials with green/amber/red zones
    lv_obj_t *solar_row = lv_obj_create(parent);
    lv_obj_set_size(solar_row, LV_PCT(100), 220);
    lv_obj_set_flex_flow(solar_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(solar_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(solar_row, 1, 0);
    lv_obj_set_style_pad_gap(solar_row, 4, 0);
    lv_obj_set_style_bg_opa(solar_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(solar_row, 0, 0);
    lv_obj_clear_flag(solar_row, LV_OBJ_FLAG_SCROLLABLE);

    struct PropGaugeDef
    {
        const char *label;
        lv_obj_t **val_ptr;
        lv_obj_t **meter_ptr;
        lv_meter_indicator_t **needle_ptr;
        int smin, smax, b1, b2;
        uint32_t col1, col2, col3;
        int tick_cnt, tick_nth; // minor tick count, every nth is a labeled major tick
    };
    // col1=[smin..b1], col2=[b1..b2], col3=[b2..smax]
    // SFI/SSN: good=high → green on right (red,amber,green)
    // K/A:     good=low  → green on left (green,amber,red)
    // tick_cnt / tick_nth → labels at: SFI=60,120,180,240,300  K=0,3,6,9  A=0,25,50,75,100  SSN=0,100,200,300
    PropGaugeDef gaugeDefs[] = {
        {"SFI", &prop_sfi_val, &prop_sfi_meter, &prop_sfi_needle, 60, 300, 100, 150, 0xf44336, 0xff9800, 0x4caf50, 9, 2},
        {"K-Index", &prop_k_val, &prop_k_meter, &prop_k_needle, 0, 9, 2, 4, 0x4caf50, 0xff9800, 0xf44336, 10, 3},
        {"A-Index", &prop_a_val, &prop_a_meter, &prop_a_needle, 0, 100, 8, 20, 0x4caf50, 0xff9800, 0xf44336, 21, 5},
        {"SSN", &prop_ssn_val, &prop_ssn_meter, &prop_ssn_needle, 0, 300, 50, 150, 0xf44336, 0xff9800, 0x4caf50, 13, 4},
    };

    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *card = lv_obj_create(solar_row);
        lv_obj_set_size(card, LV_PCT(24), 218);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_set_style_pad_gap(card, 2, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2128), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x2b3541), 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Meter (gauge dial)
        lv_obj_t *meter = lv_meter_create(card);
        lv_obj_set_size(meter, 190, 190);
        lv_obj_set_style_bg_color(meter, lv_color_hex(0x0d1117), 0);
        lv_obj_set_style_border_width(meter, 0, 0);
        *gaugeDefs[i].meter_ptr = meter;

        lv_meter_scale_t *scale = lv_meter_add_scale(meter);
        lv_meter_set_scale_range(meter, scale, gaugeDefs[i].smin, gaugeDefs[i].smax, 270, 135);
        lv_meter_set_scale_ticks(meter, scale, gaugeDefs[i].tick_cnt, 1, 6, lv_color_hex(0x505050));
        lv_meter_set_scale_major_ticks(meter, scale, gaugeDefs[i].tick_nth, 2, 14, lv_color_hex(0xcccccc), 4);
        lv_obj_set_style_text_font(meter, &lv_font_montserrat_12, LV_PART_TICKS);
        lv_obj_set_style_text_color(meter, lv_color_hex(0xcccccc), LV_PART_TICKS);

        // Three colored zone arcs
        lv_meter_indicator_t *arc1 = lv_meter_add_arc(meter, scale, 13, lv_color_hex(gaugeDefs[i].col1), 0);
        lv_meter_set_indicator_start_value(meter, arc1, gaugeDefs[i].smin);
        lv_meter_set_indicator_end_value(meter, arc1, gaugeDefs[i].b1);

        lv_meter_indicator_t *arc2 = lv_meter_add_arc(meter, scale, 13, lv_color_hex(gaugeDefs[i].col2), 0);
        lv_meter_set_indicator_start_value(meter, arc2, gaugeDefs[i].b1);
        lv_meter_set_indicator_end_value(meter, arc2, gaugeDefs[i].b2);

        lv_meter_indicator_t *arc3 = lv_meter_add_arc(meter, scale, 13, lv_color_hex(gaugeDefs[i].col3), 0);
        lv_meter_set_indicator_start_value(meter, arc3, gaugeDefs[i].b2);
        lv_meter_set_indicator_end_value(meter, arc3, gaugeDefs[i].smax);

        // White needle indicator
        *gaugeDefs[i].needle_ptr = lv_meter_add_needle_line(meter, scale, 3, lv_color_hex(0xffffff), -10);
        lv_meter_set_indicator_value(meter, *gaugeDefs[i].needle_ptr, gaugeDefs[i].smin);

        // Numeric value label below the needle pivot point
        *gaugeDefs[i].val_ptr = lv_label_create(meter);
        lv_label_set_text(*gaugeDefs[i].val_ptr, "--");
        lv_obj_set_style_text_font(*gaugeDefs[i].val_ptr, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(*gaugeDefs[i].val_ptr, lv_color_hex(0x888888), 0);
        lv_obj_set_size(*gaugeDefs[i].val_ptr, 80, 28);
        lv_label_set_long_mode(*gaugeDefs[i].val_ptr, LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(*gaugeDefs[i].val_ptr, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(*gaugeDefs[i].val_ptr, LV_ALIGN_CENTER, 0, 40);

        // Title label below the meter
        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, gaugeDefs[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x8fa0ae), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    }

    // Band cards grid — 4 columns x 3 rows.
    // Each tile: band name / condition (PSK-derived) / TX-RX distances.
    lv_obj_t *bands_grid = lv_obj_create(parent);
    lv_obj_set_size(bands_grid, LV_PCT(100), 250);
    lv_obj_set_layout(bands_grid, LV_LAYOUT_GRID);
    static lv_coord_t bcol[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t brow[] = {80, 80, 80, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(bands_grid, bcol, brow);
    lv_obj_set_style_pad_all(bands_grid, 1, 0);
    lv_obj_set_style_pad_gap(bands_grid, 4, 0);
    lv_obj_set_style_bg_opa(bands_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bands_grid, 0, 0);
    lv_obj_clear_flag(bands_grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < PROP_NUM_BANDS; i++)
    {
        lv_obj_t *card = lv_obj_create(bands_grid);
        lv_obj_set_grid_cell(card, LV_GRID_ALIGN_STRETCH, i % 4, 1,
                             LV_GRID_ALIGN_STRETCH, i / 4, 1);
        lv_obj_set_height(card, 80);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 2, 0);
        lv_obj_set_style_pad_gap(card, 1, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1a1a2e), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x333355), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 6, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

        // Band name
        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, propBands[i].name);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xdddddd), 0);

        // Condition text — Good/Fair/Poor from PSK, or hamqsl HF groups, or VHF status
        prop_band_cond_lbl[i] = lv_label_create(card);
        lv_label_set_text(prop_band_cond_lbl[i], "");
        lv_obj_set_style_text_font(prop_band_cond_lbl[i], &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(prop_band_cond_lbl[i], lv_color_hex(0x888888), 0);
        lv_obj_set_size(prop_band_cond_lbl[i], LV_PCT(100), 18);
        lv_label_set_long_mode(prop_band_cond_lbl[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_align(prop_band_cond_lbl[i], LV_TEXT_ALIGN_CENTER, 0);

        // TX / RX mini bar charts (PSK Reporter spot distances)
        lv_obj_t *bar_cont = lv_obj_create(card);
        lv_obj_set_size(bar_cont, LV_PCT(96), 20);
        lv_obj_set_flex_flow(bar_cont, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(bar_cont, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(bar_cont, 0, 0);
        lv_obj_set_style_pad_gap(bar_cont, 2, 0);
        lv_obj_set_style_bg_opa(bar_cont, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bar_cont, 0, 0);
        lv_obj_clear_flag(bar_cont, LV_OBJ_FLAG_SCROLLABLE);

        // TX bar (blue — outbound spots)
        prop_band_tx_bar[i] = lv_bar_create(bar_cont);
        lv_obj_set_size(prop_band_tx_bar[i], LV_PCT(100), 7);
        lv_bar_set_range(prop_band_tx_bar[i], 0, 20000);
        lv_bar_set_value(prop_band_tx_bar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(prop_band_tx_bar[i], lv_color_hex(0x0d2233), 0);
        lv_obj_set_style_bg_opa(prop_band_tx_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(prop_band_tx_bar[i], lv_color_hex(0x1565c0), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(prop_band_tx_bar[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(prop_band_tx_bar[i], 2, 0);
        lv_obj_set_style_radius(prop_band_tx_bar[i], 2, LV_PART_INDICATOR);

        // RX bar (green — inbound spots)
        prop_band_rx_bar[i] = lv_bar_create(bar_cont);
        lv_obj_set_size(prop_band_rx_bar[i], LV_PCT(100), 7);
        lv_bar_set_range(prop_band_rx_bar[i], 0, 20000);
        lv_bar_set_value(prop_band_rx_bar[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(prop_band_rx_bar[i], lv_color_hex(0x0d2010), 0);
        lv_obj_set_style_bg_opa(prop_band_rx_bar[i], LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(prop_band_rx_bar[i], lv_color_hex(0x2e7d32), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(prop_band_rx_bar[i], LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_set_style_radius(prop_band_rx_bar[i], 2, 0);
        lv_obj_set_style_radius(prop_band_rx_bar[i], 2, LV_PART_INDICATOR);

        prop_band_cards[i] = card;
    }

    // Two-row footer
    lv_obj_t *footer = lv_obj_create(parent);
    lv_obj_set_size(footer, LV_PCT(100), 44);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(footer, 2, 0);
    lv_obj_set_style_pad_gap(footer, 2, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);
    lv_obj_clear_flag(footer, LV_OBJ_FLAG_SCROLLABLE);

    // Row 1: Bz | X-ray | gray line | sunrise/sunset | GeoMag
    prop_footer_solar_lbl = lv_label_create(footer);
    lv_label_set_text(prop_footer_solar_lbl, "Waiting for space weather...");
    lv_obj_set_style_text_font(prop_footer_solar_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prop_footer_solar_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_size(prop_footer_solar_lbl, LV_PCT(100), 20);
    lv_label_set_long_mode(prop_footer_solar_lbl, LV_LABEL_LONG_CLIP);

    // Row 2: VHF Es/Aurora (left) + data freshness (right)
    lv_obj_t *footer_row2 = lv_obj_create(footer);
    lv_obj_set_size(footer_row2, LV_PCT(100), 20);
    lv_obj_set_flex_flow(footer_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer_row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(footer_row2, 0, 0);
    lv_obj_set_style_bg_opa(footer_row2, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer_row2, 0, 0);
    lv_obj_clear_flag(footer_row2, LV_OBJ_FLAG_SCROLLABLE);

    prop_vhf_lbl = lv_label_create(footer_row2);
    lv_label_set_text(prop_vhf_lbl, "");
    lv_obj_set_style_text_font(prop_vhf_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prop_vhf_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_size(prop_vhf_lbl, 660, 20);
    lv_label_set_long_mode(prop_vhf_lbl, LV_LABEL_LONG_CLIP);

    prop_updated_lbl = lv_label_create(footer_row2);
    lv_label_set_text(prop_updated_lbl, "");
    lv_obj_set_style_text_font(prop_updated_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(prop_updated_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_size(prop_updated_lbl, 340, 20);
    lv_label_set_long_mode(prop_updated_lbl, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(prop_updated_lbl, LV_TEXT_ALIGN_RIGHT, 0);
}

static void create_ui()
{
    lv_theme_t *th = lv_theme_default_init(
        lv_disp_get_default(),
        lv_palette_main(LV_PALETTE_BLUE),
        lv_palette_main(LV_PALETTE_RED),
        true,
        LV_FONT_DEFAULT);
    lv_disp_set_theme(lv_disp_get_default(), th);

    // Prevent the root screen from being scrolled by unhandled touch events.
    lv_obj_clear_flag(lv_scr_act(), LV_OBJ_FLAG_SCROLLABLE);

    // Create tabview with NO built-in tab bar (height=0) to avoid btnmatrix hit-test
    // issues. We create our own custom tab bar with individual lv_btn objects which
    // have exact, predictable click zones at 1024/5 = ~204px each.
    tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 0);
    lv_obj_clear_flag(tabview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(tabview, 0, 64);
    lv_obj_set_size(tabview, LCD_WIDTH, LCD_HEIGHT - 64);
    // Ensure exact per-tab scroll positions: no padding/gap on content, no snap.
    // This guarantees tab N is at exactly x = N * 1024 in the content area.
    lv_obj_set_style_pad_all(lv_tabview_get_content(tabview), 0, 0);
    lv_obj_set_style_pad_gap(lv_tabview_get_content(tabview), 0, 0);
    lv_obj_set_scroll_snap_x(lv_tabview_get_content(tabview), LV_SCROLL_SNAP_NONE);
    // Disable swipe-to-change on content — our custom buttons handle navigation.
    lv_obj_set_scroll_dir(lv_tabview_get_content(tabview), LV_DIR_NONE);

    tab_overview    = lv_tabview_add_tab(tabview, "Overview");
    tab_power       = lv_tabview_add_tab(tabview, "Power");
    tab_antennas    = lv_tabview_add_tab(tabview, "Antennas");
    tab_rotator     = lv_tabview_add_tab(tabview, "Rotator");
    tab_propagation = lv_tabview_add_tab(tabview, "Prop");
    // Prevent each tab panel from being accidentally scrolled by touch.
    lv_obj_clear_flag(tab_overview,    LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tab_power,       LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tab_antennas,    LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tab_rotator,     LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(tab_propagation, LV_OBJ_FLAG_SCROLLABLE);

    // Custom tab bar — individual lv_btn objects guarantee exact click zones
    lv_obj_t *tab_bar = lv_obj_create(lv_scr_act());
    lv_obj_set_size(tab_bar, LCD_WIDTH, 64);
    lv_obj_set_pos(tab_bar, 0, 0);
    lv_obj_clear_flag(tab_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(tab_bar, lv_color_hex(0x151b22), 0);
    lv_obj_set_style_border_width(tab_bar, 0, 0);
    lv_obj_set_style_radius(tab_bar, 0, 0);
    lv_obj_set_style_pad_all(tab_bar, 0, 0);
    lv_obj_set_style_pad_gap(tab_bar, 0, 0);
    lv_obj_set_flex_flow(tab_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    const char *tabNames[] = {"Overview", "Power", "Antennas", "Rotator", "Prop"};
    for (int ti = 0; ti < 5; ti++)
    {
        custom_tab_btns[ti] = lv_btn_create(tab_bar);
        lv_obj_set_flex_grow(custom_tab_btns[ti], 1);
        lv_obj_set_height(custom_tab_btns[ti], 64);
        lv_obj_set_style_radius(custom_tab_btns[ti], 0, 0);
        lv_obj_set_style_border_width(custom_tab_btns[ti], 0, 0);
        lv_obj_set_style_bg_color(custom_tab_btns[ti],
                                  ti == 0 ? lv_color_hex(0x1e3a5f) : lv_color_hex(0x1a2128), 0);
        lv_obj_set_style_shadow_width(custom_tab_btns[ti], 0, 0);
        lv_obj_add_event_cb(custom_tab_btns[ti], [](lv_event_t *e)
                            {
            int idx = (int)(intptr_t)lv_event_get_user_data(e);
            lv_tabview_set_act(tabview, (uint32_t)idx, LV_ANIM_OFF);
            for (int i = 0; i < 5; i++)
                if (custom_tab_btns[i])
                    lv_obj_set_style_bg_color(custom_tab_btns[i],
                        i == idx ? lv_color_hex(0x1e3a5f) : lv_color_hex(0x1a2128), 0); }, LV_EVENT_CLICKED, (void *)(intptr_t)ti);

        lv_obj_t *lbl = lv_label_create(custom_tab_btns[ti]);
        lv_label_set_text(lbl, tabNames[ti]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xdddddd), 0);
        lv_obj_center(lbl);
    }

    lv_tabview_set_act(tabview, 0, LV_ANIM_OFF); // ensure we start at Overview

    create_overview_tab(tab_overview);
    create_power_tab(tab_power);
    create_antennas_tab(tab_antennas);
    create_rotator_tab(tab_rotator);
    create_propagation_tab(tab_propagation);
}

// ============================================================
// UI Update
// ============================================================

// Only update label text when it actually changed.
// lv_label_set_text() always calls lv_obj_invalidate() regardless of whether
// the text changed — so calling it unconditionally every 300 ms causes a
// dirty-mark + full redraw on every poll cycle, even when nothing has changed.
static inline void lbl_set(lv_obj_t *lbl, const char *text)
{
    if (!lbl || !text)
        return;
    if (strcmp(lv_label_get_text(lbl), text) != 0)
        lv_label_set_text(lbl, text);
}

// Return the amateur/CB band name for a frequency in MHz, or "OOB" if out of band.
static const char *freqGetBandName(float mhz, bool &inBand)
{
    static const struct { float lo, hi; const char *n; } bands[] = {
        {0.1357f, 0.1378f, "2200m"},
        {0.472f,  0.479f,  "630m"},
        {1.8f,    2.0f,    "160m"},
        {3.5f,    4.0f,    "80m"},
        {5.351f,  5.367f,  "60m"},
        {7.0f,    7.3f,    "40m"},
        {10.1f,   10.15f,  "30m"},
        {14.0f,   14.35f,  "20m"},
        {18.068f, 18.168f, "17m"},
        {21.0f,   21.45f,  "15m"},
        {24.89f,  24.99f,  "12m"},
        {26.0f,   28.0f,   "CB"},
        {28.0f,   29.7f,   "10m"},
        {50.0f,   54.0f,   "6m"},
        {70.0f,   70.5f,   "4m"},
        {144.0f,  148.0f,  "2m"},
        {430.0f,  440.0f,  "70cm"},
    };
    for (const auto &b : bands)
        if (mhz >= b.lo && mhz < b.hi) { inBand = true; return b.n; }
    inBand = false;
    return "OOB";
}

// Update a band indicator tile — green for amateur/CB, red for OOB, grey when no data.
static void updateBandTile(lv_obj_t *tile, lv_obj_t *lbl, float mhz)
{
    if (!tile || !lbl) return;
    if (mhz <= 0.0f) {
        lbl_set(lbl, "--");
        lv_obj_set_style_bg_color(tile,     lv_color_hex(0x1a1a1a), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x444444), 0);
        lv_obj_set_style_text_color(lbl,    lv_color_hex(0x888888), 0);
        return;
    }
    bool inBand;
    const char *name = freqGetBandName(mhz, inBand);
    lbl_set(lbl, name);
    if (inBand) {
        lv_obj_set_style_bg_color(tile,     lv_color_hex(0x0a2a0a), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0x4caf50), 0);
        lv_obj_set_style_text_color(lbl,    lv_color_hex(0x4caf50), 0);
    } else {
        lv_obj_set_style_bg_color(tile,     lv_color_hex(0x2a0a0a), 0);
        lv_obj_set_style_border_color(tile, lv_color_hex(0xf44336), 0);
        lv_obj_set_style_text_color(lbl,    lv_color_hex(0xf44336), 0);
    }
}

static void update_power_tab()
{
    // ----------------------------------------------------------------
    // Relay confirmation / timeout logic
    // ----------------------------------------------------------------
    // First: copy current server-confirmed relay states when new poll data arrives.
    // We do this so that buttons can show the last-known-good state without flickering
    // back while a command is pending.
    if (relayDataReady)
    {
        for (int i = 0; i < 6; i++)
            g_relayConfirmed[i] = relayStates[i];
        g_relayConfirmedReady = true;
    }

    // Check for HTTP failure / confirmation / timeout when a command is pending
    if (g_relayPendingMask || g_relayAllPending >= 0)
    {
        int httpCode = g_relayHttpCode;
        if (httpCode < 0)
        {
            // Network failure — abort pending, show error toast
            debugLogf("[RELAY] HTTP failure %d, aborting pending", httpCode);
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            g_relayPendingMask = 0;
            g_relayAllPending  = -1;
            g_relayHttpCode    = -2;
            xSemaphoreGive(g_dataMutex);
            stopRelayPulse();
            showToast("Relay command failed (network error)", lv_color_hex(0xCC0000), 4000);
        }
        else if (millis() - g_relaySentAt > RELAY_CMD_TIMEOUT_MS)
        {
            // Timed out waiting for confirmation — abort, show toast
            debugLog("[RELAY] Confirmation timeout");
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            g_relayPendingMask = 0;
            g_relayAllPending  = -1;
            g_relayHttpCode    = -2;
            xSemaphoreGive(g_dataMutex);
            stopRelayPulse();
            showToast("Relay command not confirmed — check gateway", lv_color_hex(0xFF8800), 4000);
        }
        else if (g_relayConfirmedReady && relayDataReady && httpCode > 0)
        {
            // HTTP succeeded; now check if the polled state matches our expected target
            bool allConfirmed = true;
            uint8_t pending = g_relayPendingMask;
            for (int i = 0; i < 6; i++)
            {
                if (!(pending & (1U << i))) continue;
                if (relayStates[i] != g_relayPendingTarget[i])
                {
                    allConfirmed = false;
                    break;
                }
            }
            if (allConfirmed)
            {
                debugLog("[RELAY] Confirmed by poll data");
                xSemaphoreTake(g_dataMutex, portMAX_DELAY);
                g_relayPendingMask = 0;
                g_relayAllPending  = -1;
                g_relayHttpCode    = -2;
                xSemaphoreGive(g_dataMutex);
                stopRelayPulse();
            }
        }
    } // end if pending

    // ----------------------------------------------------------------
    // Update relay buttons
    // ----------------------------------------------------------------
    // Cache: -1=unset, 0=sleeping/notready, 1=on, 2=off, 3=pending-pulse-A, 4=pending-pulse-B
    static int8_t prevRelayDrawState[6] = {-1, -1, -1, -1, -1, -1};
    for (int i = 0; i < 6; i++)
    {
        if (!power_relay_btns[i])
            continue;

        lbl_set(power_relay_labels[i], relayLabels[i].c_str());

        bool isPending = (g_relayPendingMask & (1U << i)) != 0;

        if (isPending)
        {
            // Pulse between amber and dark — use g_relayPulseState toggled by timer
            int8_t pulseState = g_relayPulseState ? 3 : 4;
            if (pulseState != prevRelayDrawState[i])
            {
                prevRelayDrawState[i] = pulseState;
                if (g_relayPulseState)
                {
                    lv_obj_set_style_bg_color(power_relay_btns[i], lv_color_hex(0x7a5500), 0);
                    lv_obj_set_style_border_color(power_relay_btns[i], lv_color_hex(0xffa500), 0);
                }
                else
                {
                    lv_obj_set_style_bg_color(power_relay_btns[i], lv_color_hex(0x2a2000), 0);
                    lv_obj_set_style_border_color(power_relay_btns[i], lv_color_hex(0x555500), 0);
                }
            }
            continue; // don't overwrite with server state while pending
        }

        // Not pending — use confirmed server state (or grey if unavailable)
        int8_t newState = (remoteSleeping || !relayDataReady) ? 0
                        : (g_relayConfirmedReady ? (g_relayConfirmed[i] ? 1 : 2)
                                                 : (relayStates[i] ? 1 : 2));
        if (newState != prevRelayDrawState[i])
        {
            prevRelayDrawState[i] = newState;
            if (newState == 0)
            {
                lv_obj_set_style_bg_color(power_relay_btns[i], lv_color_hex(0x333333), 0);
                lv_obj_set_style_border_color(power_relay_btns[i], lv_color_hex(0x666666), 0);
            }
            else if (newState == 1)
            {
                lv_obj_set_style_bg_color(power_relay_btns[i], lv_color_hex(0x1b3a1b), 0);
                lv_obj_set_style_border_color(power_relay_btns[i], lv_color_hex(0x4caf50), 0);
            }
            else
            {
                lv_obj_set_style_bg_color(power_relay_btns[i], lv_color_hex(0x3a1b1b), 0);
                lv_obj_set_style_border_color(power_relay_btns[i], lv_color_hex(0xff4d4d), 0);
            }
        }
    }

    // All On / All Off pulse when all-cmd pending
    if (g_relayAllPending >= 0 && (btn_all_on_g || btn_all_off_g))
    {
        lv_obj_t *activeAllBtn = (g_relayAllPending == 6) ? btn_all_on_g : btn_all_off_g;
        lv_obj_t *otherAllBtn  = (g_relayAllPending == 6) ? btn_all_off_g : btn_all_on_g;
        if (activeAllBtn)
        {
            if (g_relayPulseState)
                lv_obj_set_style_bg_color(activeAllBtn, lv_color_hex(0x7a5500), 0);
            else
                lv_obj_set_style_bg_color(activeAllBtn, lv_color_hex(0x2a2000), 0);
        }
        if (otherAllBtn)
        {
            lv_color_t restoreCol = (g_relayAllPending == 6) ? lv_color_hex(0xff4d4d) : lv_color_hex(0x4caf50);
            lv_obj_set_style_bg_color(otherAllBtn, restoreCol, 0);
        }
    }
    else
    {
        // Restore All On/Off buttons to normal colours when not pending
        if (btn_all_on_g)
            lv_obj_set_style_bg_color(btn_all_on_g, lv_color_hex(0x4caf50), 0);
        if (btn_all_off_g)
            lv_obj_set_style_bg_color(btn_all_off_g, lv_color_hex(0xff4d4d), 0);
    }

    // Update battery cards
    // Cache SOC colour band per battery: -1=unset, 0=disconnected, 1=red(<=20%), 2=orange(<=50%), 3=green(>50%)
    static int8_t prevBatBand[2] = {-1, -1};
    static int prevBatBarVal[2] = {-2, -2};
    char buf[64];
    for (int i = 0; i < 2; i++)
    {
        lv_obj_t *soc_lbl = (i == 0) ? lbl_bat1_soc : lbl_bat2_soc;
        lv_obj_t *v_lbl = (i == 0) ? lbl_bat1_voltage : lbl_bat2_voltage;
        if (!soc_lbl || !v_lbl)
            continue;

        if (remoteBms[i].connected && remoteBms[i].voltage > 0)
        {
            snprintf(buf, sizeof(buf), "%d%%", (int)remoteBms[i].soc);
            lbl_set(soc_lbl, buf);

            int8_t band = (remoteBms[i].soc > 50) ? 3 : (remoteBms[i].soc > 20) ? 2
                                                                                : 1;
            if (band != prevBatBand[i])
            {
                prevBatBand[i] = band;
                lv_color_t col = (band == 3) ? lv_color_hex(0x4caf50) : (band == 2) ? lv_color_hex(0xff9800)
                                                                                    : lv_color_hex(0xf44336);
                lv_obj_set_style_text_color(soc_lbl, col, 0);
                lv_obj_t *bar = (i == 0) ? bar_bat1_soc : bar_bat2_soc;
                if (bar)
                    lv_obj_set_style_bg_color(bar, col, LV_PART_INDICATOR);
            }

            snprintf(buf, sizeof(buf), "%.2fV", remoteBms[i].voltage);
            lbl_set(v_lbl, buf);

            lv_obj_t *bar = (i == 0) ? bar_bat1_soc : bar_bat2_soc;
            if (bar)
            {
                int bval = (int)remoteBms[i].soc;
                if (bval != prevBatBarVal[i])
                {
                    prevBatBarVal[i] = bval;
                    lv_bar_set_value(bar, bval, LV_ANIM_OFF);
                }
            }
        }
        else
        {
            if (prevBatBand[i] != 0)
            {
                prevBatBand[i] = 0;
                prevBatBarVal[i] = 0;
                lv_obj_set_style_text_color(soc_lbl, lv_color_hex(0x888888), 0);
                lv_obj_t *bar = (i == 0) ? bar_bat1_soc : bar_bat2_soc;
                if (bar)
                {
                    lv_bar_set_value(bar, 0, LV_ANIM_OFF);
                    lv_obj_set_style_bg_color(bar, lv_color_hex(0x888888), LV_PART_INDICATOR);
                }
            }
            lbl_set(soc_lbl, "--%");
            lbl_set(v_lbl, "");
        }
    }

    // Update MPPT
    // Cache: -1=unset, 0=invalid, 1=zero, 2=low(>0W), 3=good(>10W)
    static int8_t prevMpptBand = -1;
    if (lbl_mppt_power)
    {
        int8_t mpptBand = !remoteMPPTValid ? 0 : (remoteMPPTPower > 10) ? 3
                                             : (remoteMPPTPower > 0)    ? 2
                                                                        : 1;
        if (remoteMPPTValid)
        {
            snprintf(buf, sizeof(buf), "%.1fW", remoteMPPTPower);
            lbl_set(lbl_mppt_power, buf);
        }
        else
        {
            lbl_set(lbl_mppt_power, "--W");
        }
        if (mpptBand != prevMpptBand)
        {
            prevMpptBand = mpptBand;
            lv_color_t col = (mpptBand == 3) ? lv_color_hex(0x4caf50) : (mpptBand == 2) ? lv_color_hex(0xff9800)
                                                                                        : lv_color_hex(0x888888);
            lv_obj_set_style_text_color(lbl_mppt_power, col, 0);
        }
    }
    if (lbl_mppt_state)
    {
        lbl_set(lbl_mppt_state, remoteMPPTStateName.c_str());
    }

    // Update LoRa signal card
    // Cache colour band: -1=unset, 0=no signal, 1=poor(pct<=30), 2=mid(pct<=60), 3=good(pct>60)
    static int8_t prevSignalBand = -1;
    static int prevSignalPct = -2;
    {
        int8_t band = 0;
        int pct = 0;
        if (hasLoRaRx)
        {
            pct = constrain((int)((lastLoRaRssi + 130) * 100 / 100), 0, 100);
            band = (pct > 60) ? 3 : (pct > 30) ? 2
                                               : 1;
            snprintf(buf, sizeof(buf), "%.0f dBm", lastLoRaRssi);
            lbl_set(lbl_signal_rssi, buf);
            snprintf(buf, sizeof(buf), "SNR: %.1f dB", lastLoRaSnr);
            lbl_set(lbl_signal, buf);
        }
        else
        {
            lbl_set(lbl_signal_rssi, "-- dBm");
            lbl_set(lbl_signal, "SNR: --");
        }
        if (band != prevSignalBand)
        {
            prevSignalBand = band;
            lv_color_t col = (band == 3) ? lv_color_hex(0x4caf50) : (band == 2) ? lv_color_hex(0xff9800)
                                                                : (band == 1)   ? lv_color_hex(0xf44336)
                                                                                : lv_color_hex(0x888888);
            if (lbl_signal_rssi)
                lv_obj_set_style_text_color(lbl_signal_rssi, col, 0);
            if (bar_signal)
                lv_obj_set_style_bg_color(bar_signal, col, LV_PART_INDICATOR);
        }
        if (bar_signal && pct != prevSignalPct)
        {
            prevSignalPct = pct;
            lv_bar_set_value(bar_signal, pct, LV_ANIM_OFF);
        }
    }

    // Update LoRa Remote signal card
    static int8_t prevRemoteSignalBand = -1;
    static int prevRemoteSignalPct = -2;
    {
        int8_t band = 0;
        int pct = 0;
        if (hasRemoteGwLoRaRx)
        {
            pct = constrain((int)((remoteGwLoRaRssi + 130) * 100 / 100), 0, 100);
            band = (pct > 60) ? 3 : (pct > 30) ? 2
                                               : 1;
            snprintf(buf, sizeof(buf), "%.0f dBm", remoteGwLoRaRssi);
            lbl_set(lbl_remote_signal_rssi, buf);
            snprintf(buf, sizeof(buf), "SNR: %.1f dB", remoteGwLoRaSnr);
            lbl_set(lbl_remote_signal, buf);
        }
        else
        {
            lbl_set(lbl_remote_signal_rssi, "-- dBm");
            lbl_set(lbl_remote_signal, "SNR: --");
        }
        if (band != prevRemoteSignalBand)
        {
            prevRemoteSignalBand = band;
            lv_color_t col = (band == 3) ? lv_color_hex(0x4caf50) : (band == 2) ? lv_color_hex(0xff9800)
                                                                : (band == 1)   ? lv_color_hex(0xf44336)
                                                                                : lv_color_hex(0x888888);
            if (lbl_remote_signal_rssi)
                lv_obj_set_style_text_color(lbl_remote_signal_rssi, col, 0);
            if (bar_remote_signal)
                lv_obj_set_style_bg_color(bar_remote_signal, col, LV_PART_INDICATOR);
        }
        if (bar_remote_signal && pct != prevRemoteSignalPct)
        {
            prevRemoteSignalPct = pct;
            lv_bar_set_value(bar_remote_signal, pct, LV_ANIM_OFF);
        }
    }

    if (btn_gw_override && lbl_gateway_route)
    {
        // Scan both gateways directly so Lock mode checks the *specific* requested
        // peer's reachability — findLoRaPeer() silently falls back to the other
        // gateway, which would leave the button blue even when the forced one is down.
        DiscoveredPeer *remoteGwBtn = nullptr;
        DiscoveredPeer *localGwBtn = nullptr;
        {
            int cnt = peerDiscovery.peerCount();
            const DiscoveredPeer *pp = peerDiscovery.peers();
            for (int i = 0; i < cnt; i++)
            {
                if (strcmp(pp[i].role, "lora-gateway") == 0 || strcmp(pp[i].role, "lora-remote") == 0)
                {
                    if (strcmp(pp[i].site, "paddock") == 0)
                        remoteGwBtn = const_cast<DiscoveredPeer *>(&pp[i]);
                    else
                        localGwBtn = const_cast<DiscoveredPeer *>(&pp[i]);
                }
            }
        }
        int ovr = loraGwOverride;
        const char *text;
        uint32_t bgColor;
        if (ovr == 1)
        {
            bool ok = remoteGwBtn && remoteGwBtn->reachable;
            text = "Lock: Remote";
            bgColor = ok ? 0x1565c0 : 0x555555;
        }
        else if (ovr == 2)
        {
            bool ok = localGwBtn && localGwBtn->reachable;
            text = "Lock: Local";
            bgColor = ok ? 0x6a1b9a : 0x555555;
        }
        else
        {
            // Auto mode — prefer remote, fall back to local
            bool remoteOk = remoteGwBtn && remoteGwBtn->reachable;
            bool localOk = localGwBtn && localGwBtn->reachable;
            if (remoteOk)
            {
                text = "Auto: Remote";
                bgColor = 0x2e7d32;
            }
            else if (localOk)
            {
                text = "Auto: Local";
                bgColor = 0xe65100;
            }
            else
            {
                text = "Auto: None";
                bgColor = 0x555555;
            }
        }
        lbl_set(lbl_gateway_route, text);
        static uint32_t prevGwBgColor = 0xFFFFFFFF;
        if (bgColor != prevGwBgColor)
        {
            prevGwBgColor = bgColor;
            lv_obj_set_style_bg_color(btn_gw_override, lv_color_hex(bgColor), 0);
        }
    }

    // Status message
    if (lbl_power_status)
    {
        if (remoteSleeping)
            lbl_set(lbl_power_status, "Remote device sleeping");
        else if (!relayDataReady)
            lbl_set(lbl_power_status, "Waiting for relay data...");
        else
            lbl_set(lbl_power_status, lastStatusMessage.length() > 0 ? lastStatusMessage.c_str() : "");
    }

    // Update WiFi link cards
    static uint32_t prevLinkVersion = 0xFFFFFFFFu;
    if (s_linkDataVersion != prevLinkVersion)
    {
        prevLinkVersion = s_linkDataVersion;

        struct WifiWidgetSet {
            lv_obj_t **rssi; lv_obj_t **snr; lv_obj_t **tx; lv_obj_t **rx;
            lv_obj_t **bar_sig;
            lv_obj_t **meter; lv_meter_indicator_t **itx; lv_meter_indicator_t **irx;
        };
        WifiWidgetSet ws[2] = {
            { &lbl_wifi_house_rssi, &lbl_wifi_house_snr, &lbl_wifi_house_tx, &lbl_wifi_house_rx,
              &bar_wifi_house_sig, &meter_wifi_house, &indic_house_tx, &indic_house_rx },
            { &lbl_wifi_pad_rssi,   &lbl_wifi_pad_snr,   &lbl_wifi_pad_tx,   &lbl_wifi_pad_rx,
              &bar_wifi_pad_sig,  &meter_wifi_pad,   &indic_pad_tx,   &indic_pad_rx }
        };
        const LinkEndpointData *eps[2] = { &s_linkData.house, &s_linkData.paddock };
        char buf[48];

        for (int i = 0; i < 2; i++)
        {
            const LinkEndpointData &ep = *eps[i];
            WifiWidgetSet &w = ws[i];
            if (!*w.rssi || !*w.snr || !*w.meter) continue;

            if (ep.connected && s_linkData.valid)
            {
                // RSSI (colour + bar matching LoRa band thresholds)
                snprintf(buf, sizeof(buf), "%d dBm", ep.rssi);
                lbl_set(*w.rssi, buf);
                int pct = constrain((ep.rssi + 90) * 2, 0, 100);
                int8_t band = (pct > 60) ? 3 : (pct > 30) ? 2 : 1;
                lv_color_t sig_col = (band == 3) ? lv_color_hex(0x4caf50)
                                   : (band == 2) ? lv_color_hex(0xff9800)
                                                 : lv_color_hex(0xf44336);
                lv_obj_set_style_text_color(*w.rssi, sig_col, 0);
                if (*w.bar_sig)
                {
                    lv_bar_set_value(*w.bar_sig, pct, LV_ANIM_OFF);
                    lv_obj_set_style_bg_color(*w.bar_sig, sig_col, LV_PART_INDICATOR);
                }

                // SNR (colour by quality)
                snprintf(buf, sizeof(buf), "SNR %d dB", ep.snr);
                lbl_set(*w.snr, buf);
                lv_color_t snr_col = (ep.snr >= 25) ? lv_color_hex(0x4caf50)
                                   : (ep.snr >= 15) ? lv_color_hex(0xff9800)
                                                    : lv_color_hex(0xf44336);
                lv_obj_set_style_text_color(*w.snr, snr_col, 0);

                // TX / RX labels
                snprintf(buf, sizeof(buf), "TX %d Mbps", ep.txRateMbps);
                lbl_set(*w.tx, buf);
                snprintf(buf, sizeof(buf), "RX %d Mbps", ep.rxRateMbps);
                lbl_set(*w.rx, buf);

                // Gauge needles (clamped to 0–140)
                int tx_val = constrain(ep.txRateMbps, 0, 140);
                int rx_val = constrain(ep.rxRateMbps, 0, 140);
                lv_meter_set_indicator_value(*w.meter, *w.itx, tx_val);
                lv_meter_set_indicator_value(*w.meter, *w.irx, rx_val);
            }
            else
            {
                lbl_set(*w.rssi, "-- dBm");
                lbl_set(*w.snr,  "SNR: -- dB");
                lbl_set(*w.tx,   "TX: -- Mbps");
                lbl_set(*w.rx,   "RX: -- Mbps");
                lv_obj_set_style_text_color(*w.rssi, lv_color_hex(0x888888), 0);
                lv_obj_set_style_text_color(*w.snr,  lv_color_hex(0x888888), 0);
                if (*w.bar_sig)
                {
                    lv_bar_set_value(*w.bar_sig, 0, LV_ANIM_OFF);
                    lv_obj_set_style_bg_color(*w.bar_sig, lv_color_hex(0x888888), LV_PART_INDICATOR);
                }
                lv_meter_set_indicator_value(*w.meter, *w.itx, 0);
                lv_meter_set_indicator_value(*w.meter, *w.irx, 0);
            }
        }

        // Lightning bolt: yellow = both ends up, grey = only house (or neither)
        if (lbl_link_bolt)
        {
            bool both = s_linkData.house.connected && s_linkData.paddock.connected && s_linkData.valid;
            bool house_only = s_linkData.house.connected && !s_linkData.paddock.connected && s_linkData.valid;
            lv_color_t bolt_col = both      ? lv_color_hex(0xffcc00)
                                : house_only ? lv_color_hex(0xff8800)
                                             : lv_color_hex(0x444444);
            lv_obj_set_style_text_color(lbl_link_bolt, bolt_col, 0);
        }
    }
}

// Relay pulse timer callback — fires every 250 ms while a relay command is pending.
// Toggles g_relayPulseState so update_power_tab() will repaint the pending buttons.
static void relay_pulse_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!g_relayPendingMask && g_relayAllPending < 0)
    {
        // Nothing pending any more — the timer should have been deleted already,
        // but guard here just in case.
        stopRelayPulse();
        return;
    }
    g_relayPulseState = !g_relayPulseState;
    // Force a repaint of pending buttons immediately rather than waiting for the
    // next 300ms update_ui cycle.
    for (int i = 0; i < 6; i++)
    {
        if (power_relay_btns[i] && (g_relayPendingMask & (1U << i)))
            lv_obj_invalidate(power_relay_btns[i]);
    }
    if (g_relayAllPending == 6 && btn_all_on_g)
        lv_obj_invalidate(btn_all_on_g);
    if (g_relayAllPending == 7 && btn_all_off_g)
        lv_obj_invalidate(btn_all_off_g);
}

static void update_propagation_tab()
{
    char buf[64];

    // Cache solar indicator values — meters and text-colour only update when values change.
    // Solar data changes at most every few minutes, so these would otherwise fire
    // lv_meter_set_indicator_value (which calls invalidate) every 300ms for nothing.
    static int prevSfi = -1, prevK = -1, prevA = -1, prevSsn = -1;
    static bool prevSolarValid = false;

    // Solar gauge dials
    if (solarData.valid)
    {
        if (prop_sfi_meter && prop_sfi_needle && solarData.sfi != prevSfi)
            lv_meter_set_indicator_value(prop_sfi_meter, prop_sfi_needle, solarData.sfi);
        if (prop_k_meter && prop_k_needle && solarData.kIndex != prevK)
            lv_meter_set_indicator_value(prop_k_meter, prop_k_needle, solarData.kIndex);
        if (prop_a_meter && prop_a_needle && solarData.aIndex != prevA)
            lv_meter_set_indicator_value(prop_a_meter, prop_a_needle, solarData.aIndex);
        if (prop_ssn_meter && prop_ssn_needle && solarData.ssn != prevSsn)
            lv_meter_set_indicator_value(prop_ssn_meter, prop_ssn_needle, solarData.ssn);

        if (prop_sfi_val && solarData.sfi != prevSfi)
        {
            snprintf(buf, sizeof(buf), "%d", solarData.sfi);
            lbl_set(prop_sfi_val, buf);
            lv_color_t c = solarData.sfi < 100   ? lv_color_hex(0xf44336)
                           : solarData.sfi < 150 ? lv_color_hex(0xff9800)
                                                 : lv_color_hex(0x4caf50);
            lv_obj_set_style_text_color(prop_sfi_val, c, 0);
        }
        if (prop_k_val && solarData.kIndex != prevK)
        {
            snprintf(buf, sizeof(buf), "%d", solarData.kIndex);
            lbl_set(prop_k_val, buf);
            lv_color_t c = solarData.kIndex <= 2   ? lv_color_hex(0x4caf50)
                           : solarData.kIndex <= 4 ? lv_color_hex(0xff9800)
                                                   : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(prop_k_val, c, 0);
        }
        if (prop_a_val && solarData.aIndex != prevA)
        {
            snprintf(buf, sizeof(buf), "%d", solarData.aIndex);
            lbl_set(prop_a_val, buf);
            lv_color_t c = solarData.aIndex <= 8    ? lv_color_hex(0x4caf50)
                           : solarData.aIndex <= 20 ? lv_color_hex(0xff9800)
                                                    : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(prop_a_val, c, 0);
        }
        if (prop_ssn_val && solarData.ssn != prevSsn)
        {
            snprintf(buf, sizeof(buf), "%d", solarData.ssn);
            lbl_set(prop_ssn_val, buf);
            lv_color_t c = solarData.ssn < 50    ? lv_color_hex(0xf44336)
                           : solarData.ssn < 150 ? lv_color_hex(0xff9800)
                                                 : lv_color_hex(0x4caf50);
            lv_obj_set_style_text_color(prop_ssn_val, c, 0);
        }
        prevSfi = solarData.sfi;
        prevK = solarData.kIndex;
        prevA = solarData.aIndex;
        prevSsn = solarData.ssn;
        prevSolarValid = true;
    }
    else if (prevSolarValid)
    {
        // Transition to invalid — reset indicators once, not every 300ms
        prevSolarValid = false;
        prevSfi = prevK = prevA = prevSsn = -1;
        if (prop_sfi_val)
        {
            lbl_set(prop_sfi_val, "--");
            lv_obj_set_style_text_color(prop_sfi_val, lv_color_hex(0x888888), 0);
        }
        if (prop_k_val)
        {
            lbl_set(prop_k_val, "--");
            lv_obj_set_style_text_color(prop_k_val, lv_color_hex(0x888888), 0);
        }
        if (prop_a_val)
        {
            lbl_set(prop_a_val, "--");
            lv_obj_set_style_text_color(prop_a_val, lv_color_hex(0x888888), 0);
        }
        if (prop_ssn_val)
        {
            lbl_set(prop_ssn_val, "--");
            lv_obj_set_style_text_color(prop_ssn_val, lv_color_hex(0x888888), 0);
        }
    }

    // Band cards + footer rows 1-2 left: only redraw when new data arrives.
    // This prevents 12+ simultaneous card redraws (flicker) on every 300ms UI tick.
    static uint32_t s_propRenderedVersion = 0xFFFFFFFFu;
    const bool propDataChanged = (s_propDataVersion != s_propRenderedVersion);
    if (propDataChanged)
        s_propRenderedVersion = s_propDataVersion;

    // Band cards — colour from PSK Reporter condition; distances from maxTxKm/maxRxKm.
    // 30m and 60m have no PSK band in the proxy; they fall back to hamqsl HF group conditions.
    if (propDataChanged) for (int i = 0; i < PROP_NUM_BANDS; i++)
    {
        if (!prop_band_cards[i])
            continue;

        // Determine condition text:
        //   1. PSK-derived condition (Good/Fair/Poor/Unknown) when available
        //   2. VHF special text for 6m (E-Skip) and 2m (Aurora) when no PSK
        //   3. hamqsl HF group day/night fallback for 30m, 60m
        const char *cond = "";
        bool hasPsk = solarData.valid && strlen(solarData.psk[i].condition) > 0;
        if (hasPsk)
        {
            cond = solarData.psk[i].condition;
        }
        else if (solarData.valid)
        {
            if (i == 10 && strlen(solarData.vhfESkipEU) > 0)
                cond = solarData.vhfESkipEU;
            else if (i == 11 && strlen(solarData.vhfAurora) > 0)
                cond = solarData.vhfAurora;
            else
            {
                struct tm timeinfo = {};
                bool isDay = true;
                if (getLocalTime(&timeinfo, 0))
                    isDay = (timeinfo.tm_hour >= 6 && timeinfo.tm_hour < 20);
                int g = (propBands[i].hfGroupIndex >= 0) ? propBands[i].hfGroupIndex : 0;
                cond = isDay ? solarData.hfCondDay[g] : solarData.hfCondNight[g];
            }
        }

        // Map condition text to colours
        bool isGood = (strcmp(cond, "Good") == 0) ||
                      (i == 10 && strstr(cond, "Open")   != nullptr) ||
                      (i == 11 && (strstr(cond, "Active") != nullptr ||
                                   strstr(cond, "Aurora") != nullptr));
        bool isFair = (strcmp(cond, "Fair") == 0);
        bool isPoor = (strcmp(cond, "Poor") == 0);

        lv_color_t cardBg, cardBorder, condColor;
        if (isGood) {
            cardBg     = lv_color_hex(0x1a2e1a);
            cardBorder = lv_color_hex(0x4caf50);
            condColor  = lv_color_hex(0x4caf50);
        } else if (isFair) {
            cardBg     = lv_color_hex(0x2e2a1a);
            cardBorder = lv_color_hex(0xff9800);
            condColor  = lv_color_hex(0xff9800);
        } else if (isPoor) {
            cardBg     = lv_color_hex(0x2e1a1a);
            cardBorder = lv_color_hex(0xf44336);
            condColor  = lv_color_hex(0xf44336);
        } else {
            cardBg     = lv_color_hex(0x1a1a2e);
            cardBorder = lv_color_hex(0x333355);
            condColor  = lv_color_hex(0x888888);
        }

        // Only redraw when condition string actually changes
        static char prevCond[PROP_NUM_BANDS][24] = {};
        if (strcmp(cond, prevCond[i]) != 0)
        {
            strncpy(prevCond[i], cond, sizeof(prevCond[i]) - 1);
            if (prop_band_cond_lbl[i])
            {
                lbl_set(prop_band_cond_lbl[i], cond);
                lv_obj_set_style_text_color(prop_band_cond_lbl[i], condColor, 0);
            }
            lv_obj_set_style_bg_color(prop_band_cards[i], cardBg, 0);
            lv_obj_set_style_border_color(prop_band_cards[i], cardBorder, 0);
        }

        // TX / RX mini bars
        if (prop_band_tx_bar[i] && prop_band_rx_bar[i])
        {
            int txVal = 0, rxVal = 0;
            if (hasPsk)
            {
                txVal = (solarData.psk[i].maxTxKm > 0) ? solarData.psk[i].maxTxKm : 0;
                rxVal = (solarData.psk[i].maxRxKm > 0) ? solarData.psk[i].maxRxKm : 0;
            }
            lv_bar_set_value(prop_band_tx_bar[i], txVal, LV_ANIM_OFF);
            lv_bar_set_value(prop_band_rx_bar[i], rxVal, LV_ANIM_OFF);
        }
    }  // end if (propDataChanged) for band cards

    // Footer rows 1 and 2-left also only change when data changes
    if (!propDataChanged)
        goto prop_age_only;

    // Footer row 1: space weather — Bz, X-ray class, gray line, sunrise/sunset, GeoMag
    if (prop_footer_solar_lbl)
    {
        if (solarData.valid)
        {
            char fsbuf[160];
            char bzStr[24] = "Bz: --";
            char xrStr[18] = "";
            char glStr[28] = "";
            char ssStr[28] = "";
            char gmStr[28] = "";

            if (solarData.bzAgeSeconds >= 0)
                snprintf(bzStr, sizeof(bzStr), "Bz: %+.1f nT", solarData.bz);
            if (solarData.xrayAgeSeconds >= 0 && strlen(solarData.xrayClass) > 0)
                snprintf(xrStr, sizeof(xrStr), "  X-ray: %s", solarData.xrayClass);
            if (solarData.minsToGrayLine != 0)
            {
                int m = abs(solarData.minsToGrayLine);
                if (solarData.minsToGrayLine > 0)
                    snprintf(glStr, sizeof(glStr), "  GL in %dh%02dm", m / 60, m % 60);
                else
                    snprintf(glStr, sizeof(glStr), "  GL -%dh%02dm ago", m / 60, m % 60);
            }
            if (strlen(solarData.sunriseUtc) > 0 && strlen(solarData.sunsetUtc) > 0)
                snprintf(ssStr, sizeof(ssStr), "  Rise: %s  Set: %s",
                         solarData.sunriseUtc, solarData.sunsetUtc);
            if (strlen(solarData.geoMag) > 0)
                snprintf(gmStr, sizeof(gmStr), "  Geo: %s%s",
                         solarData.geoMag, solarData.recentlyDisturbed ? "!" : "");

            snprintf(fsbuf, sizeof(fsbuf), "%s%s%s%s%s", bzStr, xrStr, glStr, ssStr, gmStr);
            lbl_set(prop_footer_solar_lbl, fsbuf);

            // Colour by Bz: green (northward), amber (slight south), red (storm threshold)
            lv_color_t bzColor = (solarData.bzAgeSeconds < 0) ? lv_color_hex(0x888888) :
                                 (solarData.bz < -10.0f)       ? lv_color_hex(0xf44336) :
                                 (solarData.bz <   0.0f)       ? lv_color_hex(0xff9800) :
                                                                  lv_color_hex(0x4caf50);
            lv_obj_set_style_text_color(prop_footer_solar_lbl, bzColor, 0);
        }
        else
        {
            lbl_set(prop_footer_solar_lbl, "Waiting for propagation data...");
            lv_obj_set_style_text_color(prop_footer_solar_lbl, lv_color_hex(0x888888), 0);
        }
    }

    // Footer row 2 left: VHF Es / Aurora / S/N
    if (prop_vhf_lbl)
    {
        if (solarData.valid)
        {
            snprintf(buf, sizeof(buf), "Es: %s  |  Aurora: %s  |  S/N: %s",
                     strlen(solarData.vhfESkipEU)  > 0 ? solarData.vhfESkipEU  : "--",
                     strlen(solarData.vhfAurora)   > 0 ? solarData.vhfAurora   : "--",
                     strlen(solarData.signalNoise)  > 0 ? solarData.signalNoise  : "--");
            lbl_set(prop_vhf_lbl, buf);
        }
        else
            lbl_set(prop_vhf_lbl, "");
    }

    prop_age_only:
    // Footer row 2 right: data freshness — always update (ticks every second)
    if (prop_updated_lbl)
    {
        if (solarData.valid)
        {
            auto fmtAge = [](int s, char *out, int sz) {
                if (s < 0)       snprintf(out, sz, "--");
                else if (s < 60) snprintf(out, sz, "%ds", s);
                else             snprintf(out, sz, "%dm%02ds", s / 60, s % 60);
            };
            char sa[12], pa[12];
            fmtAge(solarData.ageSeconds,    sa, sizeof(sa));
            fmtAge(solarData.pskAgeSeconds, pa, sizeof(pa));
            snprintf(buf, sizeof(buf), "Solar: %s  PSK: %s", sa, pa);
        }
        else
            snprintf(buf, sizeof(buf), "No data");
        lbl_set(prop_updated_lbl, buf);
    }
}

static void update_ui()
{
    unsigned long secs = millis() / 1000;
    char buf[512];

    // Uptime - show only relevant units
    {
        unsigned long h = secs / 3600;
        unsigned long m = (secs % 3600) / 60;
        unsigned long s = secs % 60;
        if (h > 0)
            snprintf(buf, sizeof(buf), "Uptime: %luh %lum %lus", h, m, s);
        else if (m > 0)
            snprintf(buf, sizeof(buf), "Uptime: %lum %lus", m, s);
        else
            snprintf(buf, sizeof(buf), "Uptime: %lus", s);
    }
    if (lbl_uptime)
        lbl_set(lbl_uptime, buf);

    if (lbl_build)
    {
        snprintf(buf, sizeof(buf), "Build: %s %s", buildDate, buildTime);
        lbl_set(lbl_build, buf);
    }

    if (lbl_overview_hw)
    {
        snprintf(buf, sizeof(buf), "Heap: %lu KB   PSRAM: %lu KB",
                 ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
        lbl_set(lbl_overview_hw, buf);
    }

    // WiFi — omit raw RSSI (fluctuates ±2 dBm every poll, causing 3Hz dirty marks)
    if (lbl_wifi)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            // Round RSSI to nearest 5 dBm to suppress noise-driven redraws
            int rssi5 = (WiFi.RSSI() / 5) * 5;
            snprintf(buf, sizeof(buf), "WiFi: %s  IP: %s  RSSI: %d dBm",
                     WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), rssi5);
        }
        else
        {
            snprintf(buf, sizeof(buf), "WiFi: not connected");
        }
        lbl_set(lbl_wifi, buf);
    }

    // Overview peer rows
    if (lbl_peers)
    {
        int count = peerDiscovery.peerCount();
        const DiscoveredPeer *peers = peerDiscovery.peers();
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  Peers: %d discovered", count);
        lbl_set(lbl_peers, buf);

        // Track per-row status category so we only call lv_obj_set_style_text_color
        // when the category actually changes (not every 300ms unconditionally).
        static int8_t prevPeerCat[MAX_PEER_ROWS] = {};

        // KEY FIX: do NOT hide-all then re-show. That causes a hide→show cycle
        // on every visible row every 300ms, which is exactly what causes the
        // table to flicker. Instead: only hide rows that are now past the peer
        // count, and only show rows that are newly becoming visible.
        for (int i = 0; i < MAX_PEER_ROWS; i++)
            peerRowToIndex[i] = -1;

        // Hide rows that are now beyond the active peer count.
        for (int i = count; i < MAX_PEER_ROWS; i++)
        {
            if (peer_row_objs[i] && !lv_obj_has_flag(peer_row_objs[i], LV_OBJ_FLAG_HIDDEN))
                lv_obj_add_flag(peer_row_objs[i], LV_OBJ_FLAG_HIDDEN);
            prevPeerCat[i] = -1; // reset so style reapplies if peer reappears here
        }

        int row = 0;
        for (int i = 0; i < count && row < MAX_PEER_ROWS; i++)
        {
            const DiscoveredPeer &p = peers[i];
            if (!peer_row_objs[row])
                continue;

            peerRowToIndex[row] = i;
            // Only unhide if actually hidden — lv_obj_clear_flag always invalidates in LVGL 8.
            if (lv_obj_has_flag(peer_row_objs[row], LV_OBJ_FLAG_HIDDEN))
                lv_obj_clear_flag(peer_row_objs[row], LV_OBJ_FLAG_HIDDEN);
            lbl_set(peer_name_labels[row], p.name);
            lbl_set(peer_ip_labels[row], strlen(p.ip) > 0 ? p.ip : "-");
            lbl_set(peer_site_labels[row], p.site);

            // Uptime from last API poll
            if (peer_uptime_labels[row])
            {
                uint32_t u = (i < PeerDiscovery::MAX_PEERS) ? peerUptimeSecs[i] : 0;
                if (u > 0)
                {
                    if (u >= 86400)
                        snprintf(buf, sizeof(buf), "%ud %uh", u / 86400, (u % 86400) / 3600);
                    else if (u >= 3600)
                        snprintf(buf, sizeof(buf), "%uh %um", u / 3600, (u % 3600) / 60);
                    else if (u >= 60)
                        snprintf(buf, sizeof(buf), "%um %us", u / 60, u % 60);
                    else
                        snprintf(buf, sizeof(buf), "%us", u);
                    lbl_set(peer_uptime_labels[row], buf);
                }
                else
                {
                    lbl_set(peer_uptime_labels[row], "-");
                }
            }
            if (peer_build_labels[row])
            {
                const char *bd = (i < PeerDiscovery::MAX_PEERS) ? peerBuildDate[i] : "";
                lbl_set(peer_build_labels[row], bd[0] ? bd : "-");
            }

            unsigned long age = millis() - p.lastSeen;
            // Use per-row status category cache declared above.
            int8_t cat = (p.reachable && age < 30000) ? 0 : (age < 120000) ? 1
                                                                           : 2;
            bool catChanged = (cat != prevPeerCat[row]);
            prevPeerCat[row] = cat;
            if (peer_status_labels[row])
            {
                if (cat == 0)
                {
                    lbl_set(peer_status_labels[row], LV_SYMBOL_OK " Online");
                    if (catChanged)
                        lv_obj_set_style_text_color(peer_status_labels[row], lv_color_hex(0x4caf50), 0);
                }
                else if (cat == 1)
                {
                    // Show age bucketed to 5s to halve the dirty-mark rate
                    snprintf(buf, sizeof(buf), "%lus ago", (age / 5000) * 5);
                    lbl_set(peer_status_labels[row], buf);
                    if (catChanged)
                        lv_obj_set_style_text_color(peer_status_labels[row], lv_color_hex(0xff9800), 0);
                }
                else
                {
                    lbl_set(peer_status_labels[row], "Stale");
                    if (catChanged)
                        lv_obj_set_style_text_color(peer_status_labels[row], lv_color_hex(0x888888), 0);
                }
            }

            row++;
        }
    }

    // Rotator tab update
    // Sync manual rotation state with actual server state
    if (!rotatorMoving)
    {
        manualRotating = false;
        manualDirection = 0;
    }
    bool rotatorOnline = rotatorBearing >= 0 && (millis() - rotatorLastUpdate < 60000);

    // Stop pulse animation when rotator confirms command (new data arrived).
    // Grace period: ignore the first poll responses for 3s after sending a command
    // because the rotator may not have started moving yet.
    {
        static unsigned long lastRotatorUpdateSeen = 0;
        bool cmdGrace = (millis() - rotatorCommandSentAt < 3000);
        if (rotatorLastUpdate != lastRotatorUpdateSeen)
        {
            lastRotatorUpdateSeen = rotatorLastUpdate;
            if (rotatorMoving)
            {
                stopRotatorPulse();       // confirmed moving
                rotatorCommandSentAt = 0; // clear pending — command acknowledged
                g_rotatorCmdHttpCode = 0;
            }
            else if (!cmdGrace)
                stopRotatorPulse();       // confirmed idle and grace period over
        }
    }

    static bool prevRotatorOnlineColor = false;
    if (lbl_rotator_bearing)
    {
        if (rotatorOnline)
        {
            snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", rotatorBearing);
            if (!prevRotatorOnlineColor)
            {
                prevRotatorOnlineColor = true;
                lv_obj_set_style_text_color(lbl_rotator_bearing, lv_color_hex(0x4caf50), 0);
            }
        }
        else
        {
            snprintf(buf, sizeof(buf), "---\xC2\xB0");
            if (prevRotatorOnlineColor)
            {
                prevRotatorOnlineColor = false;
                lv_obj_set_style_text_color(lbl_rotator_bearing, lv_color_hex(0x888888), 0);
            }
        }
        lbl_set(lbl_rotator_bearing, buf);
    }
    if (lbl_rotator_target)
    {
        if (rotatorMoving && rotatorTargetBearing >= 0)
        {
            snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", rotatorTargetBearing);
            lbl_set(lbl_rotator_target, buf);
        }
        else
        {
            lbl_set(lbl_rotator_target, "-");
        }
    }
    if (lbl_rotator_speed)
    {
        if (rotatorMoving && rotatorMotorSpeed > 0)
        {
            const char *dirStr = (rotatorMotorDirection > 0) ? "CW" : (rotatorMotorDirection < 0) ? "CCW"
                                                                                                  : "";
            int pct = (rotatorMotorSpeed * 100) / 255;
            snprintf(buf, sizeof(buf), "%d%% %s", pct, dirStr);
            lbl_set(lbl_rotator_speed, buf);
        }
        else
        {
            lbl_set(lbl_rotator_speed, "-");
        }
    }
    if (lbl_rotator_status)
        lbl_set(lbl_rotator_status, ""); // status shown on map badge instead

    // Update memory/direction button colours to reflect rotator availability.
    // "Available" = online + enabled + calibrated (same bar as "Ready" status).
    {
        bool rotatorAvailable = rotatorOnline && rotatorEnabled && rotatorCalibrated;
        gRotatorAvailable = rotatorAvailable; // keep global in sync for use in callbacks
        static bool lastRotatorAvailable = true; // initialised != false to force first update
        if (rotatorAvailable != lastRotatorAvailable)
        {
            lastRotatorAvailable = rotatorAvailable;
            // Memory buttons: green when available, dark-grey when not
            for (int g = 0; g < 3; g++)
            {
                if (!rotator_memory_grids[g]) continue;
                uint32_t cnt = lv_obj_get_child_cnt(rotator_memory_grids[g]);
                for (uint32_t i = 0; i < cnt; i++)
                {
                    lv_obj_t *mbtn = lv_obj_get_child(rotator_memory_grids[g], i);
                    if (!mbtn) continue;
                    lv_obj_set_style_bg_color(mbtn, lv_color_hex(rotatorAvailable ? 0x1B5E20 : 0x2a2a2a), 0);
                    lv_obj_set_style_border_color(mbtn, lv_color_hex(rotatorAvailable ? 0x4caf50 : 0x444444), 0);
                }
            }
            // Direction buttons: restore normal colours when available, grey when not
            if (btn_manual_ccw)
                lv_obj_set_style_bg_color(btn_manual_ccw,  lv_color_hex(rotatorAvailable ? 0x1565C0 : 0x2a2a2a), 0);
            if (btn_manual_stop)
                lv_obj_set_style_bg_color(btn_manual_stop, lv_color_hex(rotatorAvailable ? 0xC62828 : 0x2a2a2a), 0);
            if (btn_manual_cw)
                lv_obj_set_style_bg_color(btn_manual_cw,   lv_color_hex(rotatorAvailable ? 0x2E7D32 : 0x2a2a2a), 0);
            mapDirty = true; // refresh ready indicator dot
        }
    }
    // Rebuild memory buttons when fresh data has arrived from /api/memory
    if (rotatorMemoryDirty)
        rebuildRotatorMemoryButtons();
    // Update enable button label to reflect current state
    if (btn_rotator_enable)
    {
        lv_obj_t *lbl = lv_obj_get_child(btn_rotator_enable, 0);
        if (lbl)
            lbl_set(lbl, rotatorEnabled ? LV_SYMBOL_POWER " Disable" : LV_SYMBOL_POWER " Enable");
    }
    // Only update map/bearing when the Rotator tab (index 3) is visible — no point
    // recomputing it while the user can't see it.
    bool rotatorTabActive = (tabview && lv_tabview_get_tab_act(tabview) == 3);
    {
        static bool wasRotatorTabActive = false;
        if (rotatorTabActive && !wasRotatorTabActive)
        {
            // Just switched to rotator tab — force full map + bearing redraw.
            mapBaseDirty = true;
            mapDirty     = true;
        }
        wasRotatorTabActive = rotatorTabActive;
    }

    if (rotatorTabActive)
    {
        // Map static image: rebuild pixel buffer only when zoom changes (or first draw).
        // lv_img_set_src is called inside drawAzimuthalMap only when mapBaseDirty is set.
        if (mapBaseDirty)
        {
            drawAzimuthalMap(); // renders map_buf + saves map_base_buf, calls lv_img_set_src once
            mapDirty = true;    // force bearing re-draw now base is fresh
            lastMapZoom = currentZoom;
        }

        // Keep swoosh animating while rotating (60ms → ~16fps is smooth enough)
        if (rotatorMoving)
        {
            static unsigned long lastFlashTick = 0;
            if (millis() - lastFlashTick >= 60)
            {
                lastFlashTick = millis();
                mapDirty = true;
            }
        }

        {
            bool bearingChanged = fabsf(rotatorBearing - lastMapBearing) >= 0.5f ||
                                  fabsf(rotatorTargetBearing - lastMapTargetBearing) >= 0.5f ||
                                  (rotatorMoving != lastMapMoving) ||
                                  (rotatorOnline != lastMapOnline) ||
                                  mapDirty;
            // Skip while the map-confirm dialog is open — no need to churn the
            // map underneath it.
            if (bearingChanged && !pendingMapDialog)
            {
                updateBearingLinesDirect();
                lastMapBearing = rotatorBearing;
                lastMapTargetBearing = rotatorTargetBearing;
                lastMapMoving = rotatorMoving;
                lastMapOnline = rotatorOnline;
                mapDirty = false;
            }
        }
    }

    // Antenna tab update
    // VFO labels: update whenever TCI is connected (direct WebSocket path) OR
    // antenna-controller data is fresh.  They must NOT be gated behind
    // antennaDataReady — the TCI WebSocket delivers freq independently of whether
    // the antenna-controller peer has been discovered yet.
    if (lbl_vfo_a)
    {
        char vbuf[32];
        if (antennaTciConnected && antennaVfoA > 0.0)
            snprintf(vbuf, sizeof(vbuf), "VFO A: %.3f MHz", antennaVfoA);
        else
            snprintf(vbuf, sizeof(vbuf), "VFO A: ---.--- MHz");
        if (strcmp(lv_label_get_text(lbl_vfo_a), vbuf) != 0)
            lbl_set(lbl_vfo_a, vbuf);
    }
    updateBandTile(band_tile_a, band_lbl_a, antennaTciConnected ? (float)antennaVfoA : 0.0f);
    if (lbl_vfo_b)
    {
        char vbuf[32];
        if (antennaTciConnected && antennaVfoB > 0.0)
            snprintf(vbuf, sizeof(vbuf), "VFO B: %.3f MHz", antennaVfoB);
        else
            snprintf(vbuf, sizeof(vbuf), "VFO B: ---.--- MHz");
        if (strcmp(lv_label_get_text(lbl_vfo_b), vbuf) != 0)
            lbl_set(lbl_vfo_b, vbuf);
    }
    updateBandTile(band_tile_b, band_lbl_b, antennaTciConnected ? (float)antennaVfoB : 0.0f);

    if (antennaDataReady && (millis() - antennaLastUpdate < 30000))
    {
        // Refresh group name labels from live API data — only if changed
        for (int g = 0; g < 3; g++)
        {
            if (antenna_group_labels[g] &&
                strcmp(lv_label_get_text(antenna_group_labels[g]), antennaGroupNames[g]) != 0)
                lbl_set(antenna_group_labels[g], antennaGroupNames[g]);
        }

        // Lazy-create antenna buttons in the correct group rows on first data
        if (!antenna_btns[0])
        {
            for (int i = 0; i < antennaCount && i < MAX_ANTENNAS; i++)
            {
                int g = constrain(antennas[i].group, 0, 2);
                if (!antenna_group_rows[g])
                    continue;
                lv_obj_t *btn = lv_btn_create(antenna_group_rows[g]);
                lv_obj_set_size(btn, LV_PCT(100), 70);
                lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a2128), 0);
                lv_obj_set_style_border_color(btn, lv_color_hex(0x2b3541), 0);
                lv_obj_set_style_border_width(btn, 2, 0);
                lv_obj_set_style_radius(btn, 12, 0);
                lv_obj_add_event_cb(btn, antenna_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
                lv_obj_t *lbl = lv_label_create(btn);
                lbl_set(lbl, "");
                lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
                lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
                lv_obj_set_size(lbl, LV_PCT(95), 26);
                lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
                lv_obj_center(lbl);
                antenna_btns[i] = btn;
                antenna_labels[i] = lbl;
            }
        }

        // Stop any pending pulse animation — confirmed by new data
        stopAntennaPulse();

        for (int i = 0; i < MAX_ANTENNAS; i++)
        {
            if (i < antennaCount && antenna_btns[i])
            {
                int groupRaw = antennas[i].group;
                int group = (groupRaw < 0 || groupRaw > 2) ? 0 : groupRaw;

                // Guard: clear_flag always invalidates in LVGL 8 even if already visible
                if (lv_obj_has_flag(antenna_btns[i], LV_OBJ_FLAG_HIDDEN))
                    lv_obj_clear_flag(antenna_btns[i], LV_OBJ_FLAG_HIDDEN);

                // Only update label text when it actually changed
                if (antenna_labels[i] &&
                    strcmp(lv_label_get_text(antenna_labels[i]), antennas[i].name) != 0)
                    lbl_set(antenna_labels[i], antennas[i].name);
                antennaBtnIds[i] = antennas[i].id;

                // Encode button state: 1=active+freqMatch, 2=active+noMatch, 0=inactive
                int8_t newState = antennas[i].active ? (antennas[i].freqMatch ? 1 : 2) : 0;
                if (newState != prevAntennaState[i])
                {
                    prevAntennaState[i] = newState;
                    if (newState == 1)
                    {
                        // Active and frequency-compatible — green
                        lv_obj_set_style_bg_color(antenna_btns[i], lv_color_hex(0x1b3a1b), 0);
                        lv_obj_set_style_border_color(antenna_btns[i], lv_color_hex(0x4caf50), 0);
                        lv_obj_set_style_text_color(antenna_labels[i], lv_color_hex(0xffffff), 0);
                    }
                    else if (newState == 2)
                    {
                        // Active but wrong band — amber/yellow warning
                        lv_obj_set_style_bg_color(antenna_btns[i], lv_color_hex(0x2e2200), 0);
                        lv_obj_set_style_border_color(antenna_btns[i], lv_color_hex(0xF6D470), 0);
                        lv_obj_set_style_text_color(antenna_labels[i], lv_color_hex(0xF6D470), 0);
                    }
                    else
                    {
                        lv_color_t gcol = lv_color_hex(0x2b3541);
                        if (group == 0)
                            gcol = lv_color_hex(0x1565C0);
                        if (group == 1)
                            gcol = lv_color_hex(0x6A1B9A);
                        if (group == 2)
                            gcol = lv_color_hex(0x2E7D32);
                        lv_obj_set_style_bg_color(antenna_btns[i], lv_color_hex(0x1a2128), 0);
                        lv_obj_set_style_border_color(antenna_btns[i], gcol, 0);
                        lv_obj_set_style_text_color(antenna_labels[i], lv_color_hex(0xffffff), 0);
                    }
                }
            }
            else if (antenna_btns[i])
            {
                if (!lv_obj_has_flag(antenna_btns[i], LV_OBJ_FLAG_HIDDEN))
                    lv_obj_add_flag(antenna_btns[i], LV_OBJ_FLAG_HIDDEN);
                antennaBtnIds[i] = -1;
                prevAntennaState[i] = -1; // reset so styles reapply if button becomes visible again
            }
        }
        if (lbl_antenna_status)
            lbl_set(lbl_antenna_status, "Connected");
    }
    else if (lbl_antenna_status)
    {
        lbl_set(lbl_antenna_status, "Waiting for antenna controller...");
    }

    // Power tab
    update_power_tab();

    // Propagation tab
    update_propagation_tab();
}

// ============================================================
// Rotator Memory Bank: fetch from /api/memory and rebuild UI
// ============================================================

// Fetch the rotator's memory bank from /api/memory and store into rotatorMemories.
// Non-blocking: checks internal interval before making a network call.
// Safe to call from the poll task (core 0); writes under g_dataMutex.
static void fetchRotatorMemory()
{
    unsigned long now = millis();
    if (rotatorMemoryLoaded && now - lastRotatorMemoryFetch < ROTATOR_MEMORY_FETCH_INTERVAL)
        return;

    // Snapshot IP under mutex
    char ip[16] = {0};
    uint16_t port = 80;
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    DiscoveredPeer *rot = peerDiscovery.findByRole("rotator-controller");
    if (rot && strlen(rot->ip) > 0)
    {
        strncpy(ip, rot->ip, sizeof(ip) - 1);
        port = rot->port;
    }
    xSemaphoreGive(g_dataMutex);

    if (ip[0] == 0)
        return; // rotator not yet discovered

    String url = "http://" + String(ip) + ":" + String(port) + "/api/memory";
    HTTPClient http;
    http.setTimeout(HTTP_TIMEOUT_ROTATOR_MS);
    if (!http.begin(url))
        return;
    int code = http.GET();
    if (code != 200)
    {
        http.end();
        return;
    }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body) != DeserializationError::Ok)
    {
        debugLog("[MEM] Failed to parse /api/memory response");
        return;
    }

    const char *groupKeys[] = {"UK", "Europe", "World"};
    xSemaphoreTake(g_dataMutex, portMAX_DELAY);
    for (int g = 0; g < 3; g++)
    {
        JsonArrayConst slots = doc["banks"][groupKeys[g]].as<JsonArrayConst>();
        if (slots.isNull())
            continue;

        // Reset all slots to inactive before applying new data
        for (int i = 0; i < MEM_PER_GROUP; i++)
        {
            rotatorMemories[g][i].active = false;
            rotatorMemories[g][i].name[0] = '\0';
            rotatorMemories[g][i].bearing = 0;
            rotatorMemories[g][i].bearingLP = -1;
        }

        for (JsonVariantConst slot : slots)
        {
            int slotNum = slot["slot"] | 0;
            if (slotNum < 1 || slotNum > MEM_PER_GROUP)
                continue;
            bool active = slot["active"] | false;
            if (!active)
                continue;
            int idx = slotNum - 1;
            const char *name = slot["name"] | "";
            int bearing = slot["bearing"] | 0;
            int bearingLP = slot["bearingLP"] | -1;
            strncpy(rotatorMemories[g][idx].name, name, sizeof(rotatorMemories[g][idx].name) - 1);
            rotatorMemories[g][idx].name[sizeof(rotatorMemories[g][idx].name) - 1] = '\0';
            rotatorMemories[g][idx].bearing = constrain(bearing, 0, 359);
            rotatorMemories[g][idx].bearingLP = (bearingLP >= 0 && bearingLP <= 359) ? bearingLP : -1;
            rotatorMemories[g][idx].active = true;
        }
    }
    rotatorMemoryDirty = true;
    rotatorMemoryLoaded = true;
    xSemaphoreGive(g_dataMutex);

    lastRotatorMemoryFetch = millis();
    debugLog("[MEM] Rotator memory bank fetched from /api/memory");
}

// Rebuild the LVGL memory buttons to match rotatorMemories[].
// Must be called from the LVGL thread (main loop / update_ui).
// Shows only active slots; hides the rest. Updates label text and bearing.
static void rebuildRotatorMemoryButtons()
{
    bool rotatorAvailable = gRotatorAvailable;
    for (int g = 0; g < 3; g++)
    {
        if (!rotator_memory_grids[g])
            continue;
        uint32_t cnt = lv_obj_get_child_cnt(rotator_memory_grids[g]);
        for (uint32_t i = 0; i < cnt && i < (uint32_t)MEM_PER_GROUP; i++)
        {
            lv_obj_t *btn = lv_obj_get_child(rotator_memory_grids[g], i);
            if (!btn)
                continue;
            bool active = rotatorMemories[g][i].active && rotatorMemories[g][i].name[0] != '\0';
            if (active)
            {
                lv_obj_clear_flag(btn, LV_OBJ_FLAG_HIDDEN);
                // Update label: "Name\n000°" or "Name\n000° / 000°" when LP available
                lv_obj_t *lbl = lv_obj_get_child(btn, 0);
                if (lbl)
                {
                    char text[56];
                    if (rotatorMemories[g][i].bearingLP >= 0)
                        snprintf(text, sizeof(text), "%s\n%d\xC2\xB0 / %d\xC2\xB0",
                                 rotatorMemories[g][i].name,
                                 rotatorMemories[g][i].bearing,
                                 rotatorMemories[g][i].bearingLP);
                    else
                        snprintf(text, sizeof(text), "%s\n%d\xC2\xB0",
                                 rotatorMemories[g][i].name, rotatorMemories[g][i].bearing);
                    lv_label_set_text(lbl, text);
                }
                // Re-register callback — user data encodes group+index so callback can look up LP
                lv_obj_remove_event_cb(btn, rotator_memory_cb);
                lv_obj_add_event_cb(btn, rotator_memory_cb, LV_EVENT_CLICKED,
                                    (void *)(intptr_t)((g << 8) | i));
                // Apply availability colour
                lv_obj_set_style_bg_color(btn, lv_color_hex(rotatorAvailable ? 0x1B5E20 : 0x2a2a2a), 0);
                lv_obj_set_style_border_color(btn, lv_color_hex(rotatorAvailable ? 0x4caf50 : 0x444444), 0);
            }
            else
            {
                lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }
    rotatorMemoryDirty = false;
}

// ============================================================
// Background poll task (core 0)
// All HTTP status polling runs here so the main loop (core 1) is never
// stalled by network I/O and lv_timer_handler() stays responsive.
// g_dataMutex protects all shared state; poll functions release it during
// the actual HTTP call and reacquire before writing results.
// ============================================================
static void pollTaskFn(void *pv)
{
    for (;;)
    {
        if (otaInProgress)
        {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        processHttpCommandQueue();
        pollAllPeers();
        pollRotatorFast();
        pollVfoFast();
        pollPropagationProxy();
        pollLinkMonitor();
        fetchRotatorMemory();
        if (tciEnabled)
            TCIService.loop();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

// ============================================================
// Setup & Loop
// ============================================================

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== TouchController ===");
    Serial.printf("[Profile] %s (%dx%d)\n",
#if defined(BOARD_PROFILE_1024X600)
                  "Waveshare-like 1024x600",
#else
                  "CrowPanel 800x480",
#endif
                  LCD_WIDTH, LCD_HEIGHT);
    Serial.printf("[Touch] SDA=%d SCL=%d INT=%d RST=%d strict=%d\n",
                  TOUCH_SDA, TOUCH_SCL, TOUCH_INT, TOUCH_RST, WAVESHARE_STRICT_MODE ? 1 : 0);

#if defined(EARLY_HEARTBEAT_DIAG) && (EARLY_HEARTBEAT_DIAG == 1)
    blinkBacklightProbe(8, 120);
    Serial.println("[DIAG] EARLY_HEARTBEAT_DIAG active - holding before app init");

    uint32_t beat = 0;
    while (true)
    {
        Serial.printf("[DIAG] heartbeat %lu ms=%lu\r\n", static_cast<unsigned long>(beat++), static_cast<unsigned long>(millis()));
        delay(200);
    }
#endif

    // Boot delay: allows serial monitor to connect before output starts
    delay(1500);
    Serial.println("[BOOT] Starting setup...");

    // Temporary boot probe: blink the backlight before display init so we can
    // confirm the MCU is alive even if LCD or USB startup fails later.
    blinkBacklightProbe();

    Serial.printf("[LCD] Free heap: %u  PSRAM: %u\n",
                  esp_get_free_heap_size(),
                  heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // Initialize display using ESP-IDF RGB panel API with bounce buffer.
    // This replaces LovyanGFX Bus_RGB which is incompatible with ESP32-S3
    // OPI PSRAM (no bounce buffer → cache-incoherent DMA on chip rev v0.2).
    initRgbPanel();
    Serial.printf("[LCD] RGB panel init OK (%dx%d)\n", LCD_WIDTH, LCD_HEIGHT);

    // gCh422Wire is already up from blinkBacklightProbe(). Since we no longer
    // call lcd.init(), the LGFX GT911 ESP-IDF driver never ran and CH422 state
    // is intact. Just ensure backlight is on.
    if (LCD_BL < 0)
    {
        setBacklightViaCh422(true);
        Serial.printf("[BL] CH422 backlight on state=0x%02X\n", gCh422OutputState);
    }
    Serial.printf("[Display] ESP-IDF RGB panel initialized (%dx%d)\n", LCD_WIDTH, LCD_HEIGHT);

    // Detect GT911 touch controller I2C address
    gt911Detect();

    // Mount SPIFFS
    if (!SPIFFS.begin(true))
    {
        Serial.println("[SETUP] SPIFFS mount failed");
    }
    else
    {
        Serial.println("[SETUP] SPIFFS mounted");
    }

    // Initialize logger
    DebugLogger::begin("/debug_log.txt", 50000);

    // Load configuration
    loadConfig();
    debugLog("[SETUP] Configuration loaded");

    // Initialize LVGL
    lv_init();

    size_t buf_size = LCD_WIDTH * LVGL_BUF_LINES * sizeof(lv_color_t);
    buf1 = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    buf2 = (lv_color_t *)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2)
    {
        debugLog("[LVGL] PSRAM alloc failed, falling back to internal RAM");
        buf1 = (lv_color_t *)malloc(buf_size);
        buf2 = nullptr;
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, LCD_WIDTH * LVGL_BUF_LINES);

    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = LCD_WIDTH;
    disp_drv.ver_res = LCD_HEIGHT;
    disp_drv.flush_cb = lvgl_flush_cb;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = lvgl_touch_cb;
    indev_drv.scroll_limit = 24; // Require 24px movement before scroll (default 10) - makes taps reliable
    lv_indev_drv_register(&indev_drv);

    debugLog("[LVGL] Initialized with PSRAM buffers");

    // Allocate azimuthal map buffers in PSRAM
    map_buf = (lv_color_t *)heap_caps_malloc(MAP_SIZE * MAP_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!map_buf)
    {
        debugLog("[MAP] PSRAM alloc failed for map_buf, using internal RAM");
        map_buf = (lv_color_t *)malloc(MAP_SIZE * MAP_SIZE * sizeof(lv_color_t));
    }
    map_base_buf = (lv_color_t *)heap_caps_malloc(MAP_SIZE * MAP_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!map_base_buf)
    {
        debugLog("[MAP] PSRAM alloc failed for map_base_buf, using internal RAM");
        map_base_buf = (lv_color_t *)malloc(MAP_SIZE * MAP_SIZE * sizeof(lv_color_t));
    }
    memset(&map_img_dsc, 0, sizeof(map_img_dsc));

    // Build UI
    create_ui();
    debugLog("[UI] Tab view created");

    // Start WiFi
    // ESP32-S3 with RGB panel + OPI PSRAM: the bounce-buffer ISR runs at interrupt
    // level 7 (~2000 times/sec) and causes WiFi beacon misses when modem sleep is
    // enabled (WIFI_PS_MIN_MODEM). Disable modem sleep so the WiFi modem stays
    // active and beacon timing is not interrupt-latency sensitive.
    WiFiManager::setPowerSaveMode(WIFI_PS_NONE);

    if (!wifiSSID.isEmpty())
    {
        WiFiManager::onWiFiReconnect([]()
                                     {
            debugLog("[WiFi] Reconnect - re-registering mDNS");
            MDNS.end();
            if (MDNS.begin(deviceName.c_str())) {
                MDNS.addService("http", "tcp", 80);
                debugLogf("[WiFi] mDNS re-registered as %s.local", deviceName.c_str());
            }
            peerDiscovery.begin(deviceName.c_str(), 80, "touch-controller", "thelimes"); });

        WiFiManager::begin(wifiSSID, wifiPassword, "TouchPanel-Setup", "12345678", deviceName.c_str());
        debugLogf("[WIFI] Connecting to %s", wifiSSID.c_str());
    }
    else
    {
        WiFiManager::begin("", "", "TouchPanel-Setup", "12345678");
        debugLog("[WIFI] No SSID configured, starting in AP mode");
    }

    // NTP
    configTzTime("GMT0BST,M3.5.0/1,M10.5.0", "pool.ntp.org");

    // mDNS
    if (MDNS.begin(deviceName.c_str()))
    {
        MDNS.addService("http", "tcp", 80);
        debugLogf("[MDNS] %s.local", deviceName.c_str());
    }

    // OTA
    ArduinoOTA.onStart([]()
                       {
                           otaInProgress = true;
                           debugLog("[OTA] Update started"); });
    ArduinoOTA.onEnd([]()
                     {
                         debugLog("[OTA] Update finished");
                         otaInProgress = false; });
    ArduinoOTA.onError([](ota_error_t err)
                       {
                           debugLogf("[OTA] Error %u", (unsigned)err);
                           otaInProgress = false; });
    ArduinoOTA.setHostname(deviceName.c_str());
    ArduinoOTA.setPassword("otapass");
    ArduinoOTA.begin();
    debugLog("[OTA] Ready");

    // Web server
    setupWebServer();

    // Peer discovery
    peerDiscovery.begin(deviceName.c_str(), 80, "touch-controller", "thelimes");

    // Direct TCI connection to radio (event-driven, replaces HTTP /api/tci polling)
    if (tciEnabled && !tciHost.isEmpty())
    {
        TCIService.onFrequencyChange([](double freqMHz, int vfo)
                                     {
            if (g_dataMutex)
                xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            if (vfo == 0)
                antennaVfoA = freqMHz;
            else if (vfo == 1)
                antennaVfoB = freqMHz;
            if (g_dataMutex)
                xSemaphoreGive(g_dataMutex); });
        TCIService.onConnectionChange([](bool connected)
                                      {
            if (g_dataMutex)
                xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            antennaTciConnected = connected;
            if (!connected)
            {
                antennaVfoA = 0.0;
                antennaVfoB = 0.0;
            }
            if (g_dataMutex)
                xSemaphoreGive(g_dataMutex);
            debugLogf("[TCI] Connection: %s", connected ? "connected" : "disconnected"); });
        TCIService.begin(tciHost.c_str(), tciPort);
        debugLogf("[TCI] Connecting to %s:%d", tciHost.c_str(), tciPort);
    }
    else
    {
        debugLog("[TCI] Direct TCI disabled or no host configured");
    }

    // Create shared-data mutex then launch background poll task on core 0.
    // All HTTP polling runs there; the main loop (core 1) is never blocked.
    g_dataMutex = xSemaphoreCreateMutex();
    xTaskCreatePinnedToCore(pollTaskFn, "pollTask", 8192, nullptr, 1, nullptr, 0);
    debugLog("[POLL] Background poll task started on core 0");

    debugLogf("[SETUP] Free heap: %lu KB, PSRAM: %lu KB",
              ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
    debugLog("=== Setup complete ===");
}

void loop()
{
    // LVGL task handler.  flush_cb blocks internally until the vsync DMA swap
    // completes, so lv_timer_handler() returns only after the frame is on screen.
    lv_timer_handler();

    // Web server
    server.handleClient();

    // WiFi management
    WiFiManager::loop();

    // OTA
    ArduinoOTA.handle();

    // Peer discovery — writes to the shared peer table; protect with mutex
    if (g_dataMutex)
    {
        xSemaphoreTake(g_dataMutex, portMAX_DELAY);
        peerDiscovery.loop();
        xSemaphoreGive(g_dataMutex);
    }
    else
    {
        peerDiscovery.loop();
    }

    // Logger maintenance
    DebugLogger::periodicFlush();

    // Periodic UI update — reads shared state; protect with mutex
    if (millis() - lastUiUpdate > UI_UPDATE_INTERVAL)
    {
        if (g_dataMutex)
        {
            xSemaphoreTake(g_dataMutex, portMAX_DELAY);
            update_ui();
            xSemaphoreGive(g_dataMutex);
        }
        else
        {
            update_ui();
        }
        lastUiUpdate = millis();
    }

    // Reboot handler
    if (rebootPending && millis() - rebootStartTime > 1000)
    {
        ESP.restart();
    }

    // Auto-hide toast
    if (toast_label && toast_hide_time && millis() > toast_hide_time)
    {
        lv_obj_add_flag(toast_label, LV_OBJ_FLAG_HIDDEN);
        toast_hide_time = 0;
    }

    delay(1);
}
