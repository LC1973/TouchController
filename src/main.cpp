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

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <DebugLogger.h>
#include <WiFiManager.h>
#include <Utils.h>
#include <PeerDiscovery.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
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
// Display Driver (ESP-IDF RGB panel with bounce buffer)
//
// LovyanGFX Bus_RGB is incompatible with ESP32-S3 OPI PSRAM: it bypasses
// esp_lcd_new_rgb_panel() and uses direct GDMA without a bounce buffer,
// causing DMA reads of stale PSRAM data due to cache coherency issues on
// chip rev v0.2 (ESP_ROM_HAS_CACHE_WRITEBACK_BUG).
//
// The official Waveshare fix uses esp_lcd_new_rgb_panel() with
// bounce_buffer_size_px: the ESP-IDF RGB driver ISR copies PSRAM→internal
// SRAM bounce buffer before each DMA transfer, bypassing the cache issue.
// ============================================================

static esp_lcd_panel_handle_t s_panel_handle = NULL;

// Read N bytes from GT911 at 16-bit register address over gCh422Wire.
static bool gt911ReadReg(uint16_t reg, uint8_t *buf, uint8_t len)
{
    gCh422Wire.beginTransmission(0x5D);
    gCh422Wire.write((uint8_t)(reg >> 8));
    gCh422Wire.write((uint8_t)(reg & 0xFF));
    if (gCh422Wire.endTransmission(false) != 0)
        return false;
    gCh422Wire.requestFrom((uint8_t)0x5D, len);
    for (uint8_t i = 0; i < len; i++)
        buf[i] = gCh422Wire.read();
    return true;
}

// Poll GT911 for a touch point. Returns true if touched.
static bool gt911GetTouch(uint16_t &x, uint16_t &y)
{
    uint8_t status = 0;
    if (!gt911ReadReg(0x814E, &status, 1))
        return false;
    if (!(status & 0x80) || (status & 0x0F) == 0)
        return false;

    // Touch point 1: track_id(1), x_lo(1), x_hi(1), y_lo(1), y_hi(1)
    uint8_t tp[5] = {0};
    gt911ReadReg(0x8150, tp, 5);
    x = (uint16_t)tp[1] | ((uint16_t)tp[2] << 8);
    y = (uint16_t)tp[3] | ((uint16_t)tp[4] << 8);

    // Clear buffer-ready flag so GT911 reports the next event
    uint8_t zero = 0;
    gCh422Wire.beginTransmission(0x5D);
    gCh422Wire.write(0x81);
    gCh422Wire.write(0x4E);
    gCh422Wire.write(zero);
    gCh422Wire.endTransmission();
    return true;
}

// Initialise the RGB panel using the proper ESP-IDF API with bounce buffer.
// bounce_buffer_size_px allocates internal SRAM as a DMA intermediary so
// DMA never reads PSRAM directly — the ESP-IDF ISR handles the cache-safe
// PSRAM→SRAM copy, which is the correct fix for ESP32-S3 rev v0.2.
static void initRgbPanel()
{
    esp_lcd_rgb_panel_config_t panel_config = {};
    panel_config.clk_src                 = LCD_CLK_SRC_DEFAULT;
    panel_config.timings.pclk_hz         = LCD_FREQ_WRITE;
    panel_config.timings.h_res           = LCD_WIDTH;
    panel_config.timings.v_res           = LCD_HEIGHT;
    panel_config.timings.hsync_pulse_width = LCD_HSYNC_PULSE_WIDTH;
    panel_config.timings.hsync_back_porch  = LCD_HSYNC_BACK_PORCH;
    panel_config.timings.hsync_front_porch = LCD_HSYNC_FRONT_PORCH;
    panel_config.timings.vsync_pulse_width = LCD_VSYNC_PULSE_WIDTH;
    panel_config.timings.vsync_back_porch  = LCD_VSYNC_BACK_PORCH;
    panel_config.timings.vsync_front_porch = LCD_VSYNC_FRONT_PORCH;
    panel_config.timings.flags.pclk_active_neg = LCD_PCLK_ACTIVE_NEG;
    panel_config.data_width              = 16;
    panel_config.bits_per_pixel          = 16;
    panel_config.num_fbs                 = 1;
    // Bounce buffer in internal SRAM: 10 rows × 1024 pixels.
    // The ESP-IDF ISR copies PSRAM framebuffer → bounce buffer each VSYNC.
    panel_config.bounce_buffer_size_px   = LCD_WIDTH * 10;
    panel_config.sram_trans_align        = 4;
    panel_config.psram_trans_align       = 64;
    panel_config.hsync_gpio_num          = LCD_HSYNC;
    panel_config.vsync_gpio_num          = LCD_VSYNC;
    panel_config.de_gpio_num             = LCD_DE;
    panel_config.pclk_gpio_num           = LCD_PCLK;
    panel_config.disp_gpio_num           = GPIO_NUM_NC;
    // Data bus: B[0:4], G[0:5], R[0:4]  (matches Waveshare official order)
    panel_config.data_gpio_nums[0]  = LCD_B0;
    panel_config.data_gpio_nums[1]  = LCD_B1;
    panel_config.data_gpio_nums[2]  = LCD_B2;
    panel_config.data_gpio_nums[3]  = LCD_B3;
    panel_config.data_gpio_nums[4]  = LCD_B4;
    panel_config.data_gpio_nums[5]  = LCD_G0;
    panel_config.data_gpio_nums[6]  = LCD_G1;
    panel_config.data_gpio_nums[7]  = LCD_G2;
    panel_config.data_gpio_nums[8]  = LCD_G3;
    panel_config.data_gpio_nums[9]  = LCD_G4;
    panel_config.data_gpio_nums[10] = LCD_G5;
    panel_config.data_gpio_nums[11] = LCD_R0;
    panel_config.data_gpio_nums[12] = LCD_R1;
    panel_config.data_gpio_nums[13] = LCD_R2;
    panel_config.data_gpio_nums[14] = LCD_R3;
    panel_config.data_gpio_nums[15] = LCD_R4;
    panel_config.flags.fb_in_psram  = 1;

    ESP_ERROR_CHECK(esp_lcd_new_rgb_panel(&panel_config, &s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel_handle));

    Serial.printf("[LCD] esp_lcd RGB panel OK  handle=%p\n", (void *)s_panel_handle);
}

// ============================================================
// Globals
// ============================================================

// LVGL draw buffers
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1 = nullptr;
static lv_color_t *buf2 = nullptr;
#define LVGL_BUF_LINES 48

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
static lv_obj_t *tabview = nullptr;
static lv_obj_t *tab_overview = nullptr;
static lv_obj_t *tab_power = nullptr;
static lv_obj_t *tab_antennas = nullptr;
static lv_obj_t *tab_rotator = nullptr;
static lv_obj_t *tab_propagation = nullptr;

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

// Power tab UI elements
static lv_obj_t *power_relay_btns[6] = {nullptr};
static lv_obj_t *power_relay_labels[6] = {nullptr};
static lv_obj_t *lbl_bat1_soc = nullptr;
static lv_obj_t *lbl_bat1_voltage = nullptr;
static lv_obj_t *lbl_bat2_soc = nullptr;
static lv_obj_t *lbl_bat2_voltage = nullptr;
static lv_obj_t *lbl_mppt_power = nullptr;
static lv_obj_t *lbl_mppt_state = nullptr;
static lv_obj_t *lbl_signal = nullptr;
static lv_obj_t *lbl_signal_rssi = nullptr;
static lv_obj_t *lbl_power_status = nullptr;
static lv_obj_t *bar_bat1_soc = nullptr;
static lv_obj_t *bar_bat2_soc = nullptr;
static lv_obj_t *bar_signal = nullptr;

// Antenna tab UI elements
#define MAX_ANTENNAS 8
static lv_obj_t *antenna_btns[MAX_ANTENNAS] = {nullptr};
static lv_obj_t *antenna_labels[MAX_ANTENNAS] = {nullptr};
static lv_obj_t *lbl_antenna_status = nullptr;
static lv_obj_t *antenna_group_rows[3] = {nullptr};

// Rotator tab UI elements
static lv_obj_t *lbl_rotator_status = nullptr;
static lv_obj_t *lbl_rotator_target = nullptr;
static lv_obj_t *btn_rotator_stop = nullptr;
static lv_obj_t *btn_rotator_enable = nullptr;
static lv_obj_t *canvas_map = nullptr;
static lv_obj_t *btn_manual_ccw = nullptr;
static lv_obj_t *btn_manual_cw = nullptr;
static lv_obj_t *btn_manual_stop = nullptr;
static lv_obj_t *rotator_memory_groups[3] = {nullptr};

static int peerRowToIndex[MAX_PEER_ROWS] = {-1};
static bool manualRotating = false;
static int manualDirection = 0; // -1 CCW, +1 CW
static int pendingMapBearing = -1;
static lv_obj_t *pendingMapDialog = nullptr;

// Toast notification
static lv_obj_t *toast_label = nullptr;
static unsigned long toast_hide_time = 0;
static lv_obj_t *btn_zoom[3] = {nullptr};
static int currentZoom = 1; // 0=UK, 1=Europe, 2=World
static unsigned long lastTouchActivity = 0;

// QTH location (G7NRU)
static const float QTH_LAT = 53.00234f;
static const float QTH_LNG = -0.62763f;

struct RotatorMemoryPoint
{
    char name[24];
    int bearing;
};

static unsigned long lastUiUpdate = 0;
#define UI_UPDATE_INTERVAL 1000
static volatile bool otaInProgress = false;

// ============================================================
// IP-based peer polling (discovered via mDNS)
// ============================================================

static unsigned long lastPeerPoll = 0;
#define PEER_POLL_INTERVAL 5000 // Poll each peer every 5s

#define HTTP_TIMEOUT_TOUCH_MS 350
#define HTTP_TIMEOUT_ROTATOR_MS 350
#define HTTP_TIMEOUT_POLL_ONLINE_MS 450
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
static unsigned long rotatorLastUpdate = 0;
static bool mapDirty = true;
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
};
static AntennaInfo antennas[MAX_ANTENNAS];
static int antennaBtnIds[MAX_ANTENNAS] = {-1};
static int antennaCount = 0;
static bool antennaDataReady = false;
static unsigned long antennaLastUpdate = 0;

static unsigned long lastPropProxyPoll = 0;
#define PROP_PROXY_POLL_INTERVAL 30000

// ============================================================
// Propagation Data
// ============================================================

#define PROP_NUM_BANDS 13

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
    {"70cm", 430000000, 440000000, -1},
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
    bool valid = false;
    unsigned long lastUpdate = 0;
};
static SolarPropData solarData;

static RotatorMemoryPoint rotatorMemories[3][10] = {
    {
        {"London", 157}, {"Belfast", 308}, {"Edinburgh", 347}, {"Cardiff", 250}, {"Plymouth", 228},
        {"Norwich", 118}, {"Leeds", 339}, {"Newcastle", 357}, {"Bristol", 232}, {"Dover", 140}
    },
    {
        {"Paris", 149}, {"Berlin", 92}, {"Rome", 139}, {"Madrid", 212}, {"Prague", 106},
        {"Warsaw", 84}, {"Stockholm", 49}, {"Vienna", 111}, {"Lisbon", 221}, {"Helsinki", 39}
    },
    {
        {"New York", 287}, {"Los Angeles", 323}, {"Tokyo", 34}, {"Sydney", 60}, {"Moscow", 70},
        {"Delhi", 78}, {"Cairo", 132}, {"Nairobi", 146}, {"Sao Paulo", 234}, {"Singapore", 70}
    }
};

static void copyText(char *dst, size_t dstSize, const String &src)
{
    if (!dst || dstSize == 0)
        return;
    strncpy(dst, src.c_str(), dstSize - 1);
    dst[dstSize - 1] = '\0';
}

static void drawAzimuthalMap();

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
        if (hf["day"].is<JsonArrayConst>() && i < (int)hf["day"].as<JsonArrayConst>().size())
        {
            copyText(solarData.hfCondDay[i], sizeof(solarData.hfCondDay[i]), hf["day"][i].as<String>());
        }
        if (hf["night"].is<JsonArrayConst>() && i < (int)hf["night"].as<JsonArrayConst>().size())
        {
            copyText(solarData.hfCondNight[i], sizeof(solarData.hfCondNight[i]), hf["night"][i].as<String>());
        }
    }

    if (vhf["es"])
        copyText(solarData.vhfESkipEU, sizeof(solarData.vhfESkipEU), vhf["es"].as<String>());
    if (vhf["aurora"])
        copyText(solarData.vhfAurora, sizeof(solarData.vhfAurora), vhf["aurora"].as<String>());

    if (root["geoMag"])
        copyText(solarData.geoMag, sizeof(solarData.geoMag), root["geoMag"].as<String>());
    if (root["signalNoise"])
        copyText(solarData.signalNoise, sizeof(solarData.signalNoise), root["signalNoise"].as<String>());

    solarData.valid = true;
    solarData.lastUpdate = millis();
}



// Propagation tab UI
static lv_obj_t *prop_band_cards[PROP_NUM_BANDS] = {nullptr};
static lv_obj_t *prop_band_cond_lbl[PROP_NUM_BANDS] = {nullptr};
static lv_obj_t *prop_sfi_val = nullptr;
static lv_obj_t *prop_k_val = nullptr;
static lv_obj_t *prop_a_val = nullptr;
static lv_obj_t *prop_ssn_val = nullptr;
static lv_obj_t *prop_vhf_lbl = nullptr;
static lv_obj_t *prop_updated_lbl = nullptr;

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
    // Prefer remote if reachable
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
    httpCmdTail = nextTail;
    return true;
}

static void processHttpCommandQueue()
{
    if (httpCmdHead == httpCmdTail)
        return;

    HttpCommand cmd = httpCmdQueue[httpCmdHead];
    httpCmdHead = (httpCmdHead + 1) % HTTP_CMD_QUEUE_SIZE;

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
            http.addHeader("Content-Type", "application/x-www-form-urlencoded");
        code = http.POST(cmd.body);
    }
    else
    {
        code = http.GET();
    }
    http.end();

    if (code < 0)
        debugLogf("[HTTPQ] request failed (%d): %s", code, cmd.url.c_str());
}

// Poll a peer's /api/status via HTTP and update local state
static void pollPeerStatus(DiscoveredPeer &peer)
{
    if (strlen(peer.ip) == 0)
        return;

    HTTPClient http;
    String url = "http://" + String(peer.ip) + ":" + String(peer.port) + "/api/status";
    // Use shorter timeout for peers that have been unreachable
    http.setTimeout(peer.reachable ? HTTP_TIMEOUT_POLL_ONLINE_MS : HTTP_TIMEOUT_POLL_OFFLINE_MS);
    http.begin(url);

    int code = http.GET();
    peer.lastPolled = millis();

    if (code != 200)
    {
        peer.reachable = false;
        http.end();
        return;
    }

    peer.reachable = true;
    peer.lastSuccess = millis();

    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body))
        return;

    // LoRa Remote/Gateway: update relay states, battery, MPPT, signal
    if (strcmp(peer.role, "lora-remote") == 0 || strcmp(peer.role, "lora-gateway") == 0)
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

        // LoRa signal
        if (doc["lora"].is<JsonObject>())
        {
            hasLoRaRx = doc["lora"]["valid"] | false;
            lastLoRaRssi = doc["lora"]["rssi"] | 0.0f;
            lastLoRaSnr = doc["lora"]["snr"] | 0.0f;
            if (hasLoRaRx)
                lastLoRaRxTime = millis();
        }

        remoteStatusLastUpdate = millis();
    }

    // Rotator Controller: update bearing, enabled state
    if (strcmp(peer.role, "rotator-controller") == 0)
    {
        if (doc["rotator"].is<JsonObject>())
        {
            float newBearing = doc["rotator"]["currentPosition"] | -1.0f;
            float newTarget = doc["rotator"]["targetPosition"] | -1.0f;
            float newVoltage = doc["rotator"]["positionVoltage"] | 0.0f;
            bool newCalibrated = doc["rotator"]["calibrated"] | false;
            bool newEnabled = doc["rotator"]["enabled"] | false;
            bool newMoving = doc["rotator"]["motorRunning"] | false;

            if (fabsf(newBearing - rotatorBearing) >= 0.5f ||
                fabsf(newTarget - rotatorTargetBearing) >= 0.5f ||
                newMoving != rotatorMoving)
            {
                mapDirty = true;
            }

            rotatorBearing = newBearing;
            rotatorTargetBearing = newTarget;
            rotatorVoltage = newVoltage;
            rotatorCalibrated = newCalibrated;
            rotatorEnabled = newEnabled;
            rotatorMoving = newMoving;
        }
        rotatorLastUpdate = millis();
    }

    // Antenna Controller: update antenna states
    if (strcmp(peer.role, "antenna-controller") == 0)
    {
        if (doc["antennas"].is<JsonArray>())
        {
            JsonArray arr = doc["antennas"].as<JsonArray>();
            antennaCount = 0;
            for (int i = 0; i < (int)arr.size() && i < MAX_ANTENNAS; i++)
            {
                JsonObject a = arr[i];
                strncpy(antennas[i].name, a["name"] | "Antenna", sizeof(antennas[i].name) - 1);
                antennas[i].id = a["id"] | i;
                antennas[i].group = a["group"] | 0;
                antennas[i].active = a["active"] | false;
                antennaCount++;
            }
            antennaDataReady = true;
        }
        antennaLastUpdate = millis();
    }

    // Pi5 propagation proxy
    if (strstr(peer.role, "prop") != nullptr || strstr(peer.name, "propproxy") != nullptr)
    {
        parsePropagationPayload(doc);
    }
}

// Poll all discovered peers
static void pollAllPeers()
{
    unsigned long now = millis();
    if (now - lastPeerPoll < PEER_POLL_INTERVAL)
        return;
    lastPeerPoll = now;

    int count = peerDiscovery.peerCount();
    const DiscoveredPeer *peers = peerDiscovery.peers();

    for (int i = 0; i < count; i++)
    {
        // Stagger polls - don't poll all at once
        // Cast away const for lastPolled update
        DiscoveredPeer &peer = const_cast<DiscoveredPeer &>(peers[i]);
        // Back off unreachable peers - poll 4x less often
        unsigned long interval = peer.reachable ? PEER_POLL_INTERVAL : (PEER_POLL_INTERVAL * 4);
        if (now - peer.lastPolled >= interval)
        {
            pollPeerStatus(peer);
            break; // One per cycle to avoid blocking
        }
    }
}

static void pollPropagationProxy()
{
    if (millis() - lastPropProxyPoll < PROP_PROXY_POLL_INTERVAL)
        return;
    lastPropProxyPoll = millis();

    int count = peerDiscovery.peerCount();
    const DiscoveredPeer *peers = peerDiscovery.peers();
    for (int i = 0; i < count; i++)
    {
        const DiscoveredPeer &p = peers[i];
        if (!p.reachable || strlen(p.ip) == 0)
            continue;

        bool isPropPeer = strstr(p.role, "prop") != nullptr || strstr(p.name, "propproxy") != nullptr || strstr(p.name, "pi5") != nullptr;
        if (!isPropPeer)
            continue;

        HTTPClient http;
        String base = "http://" + String(p.ip) + ":" + String(p.port);
        String urls[2] = {base + "/api/propagation", base + "/api/status"};
        for (int u = 0; u < 2; u++)
        {
            http.setTimeout(HTTP_TIMEOUT_PROP_MS);
            if (!http.begin(urls[u]))
                continue;
            int code = http.GET();
            if (code == 200)
            {
                String body = http.getString();
                JsonDocument doc;
                if (!deserializeJson(doc, body))
                {
                    parsePropagationPayload(doc);
                    http.end();
                    return;
                }
            }
            http.end();
        }
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
// Power Tab - Relay button callback
// ============================================================

static void relay_btn_event_cb(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);
    if (id < 0 || id > 5)
        return;
    if (remoteSleeping || !relayDataReady)
        return;

    relayStates[id] = !relayStates[id];
    debugLogf("[TOUCH] Toggle relay %d -> %s", id, relayStates[id] ? "ON" : "OFF");

    DiscoveredPeer *gw = findLoRaPeer();
    if (gw && strlen(gw->ip) > 0)
    {
        String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/toggle?id=" + String(id);
        enqueueHttpCommand(url, HTTP_CMD_GET, "", HTTP_TIMEOUT_TOUCH_MS);
    }
}

static void all_on_btn_event_cb(lv_event_t *e)
{
    for (int i = 0; i < 6; i++)
        relayStates[i] = true;
    debugLog("[TOUCH] All relays ON");
    DiscoveredPeer *gw = findLoRaPeer();
    if (gw && strlen(gw->ip) > 0)
    {
        String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/all_on";
        enqueueHttpCommand(url, HTTP_CMD_GET, "", HTTP_TIMEOUT_TOUCH_MS);
    }
}

static void all_off_btn_event_cb(lv_event_t *e)
{
    for (int i = 0; i < 6; i++)
        relayStates[i] = false;
    debugLog("[TOUCH] All relays OFF");
    DiscoveredPeer *gw = findLoRaPeer();
    if (gw && strlen(gw->ip) > 0)
    {
        String url = "http://" + String(gw->ip) + ":" + String(gw->port) + "/all_off";
        enqueueHttpCommand(url, HTTP_CMD_GET, "", HTTP_TIMEOUT_TOUCH_MS);
    }
}

// ============================================================
// UI Feedback Helpers
// ============================================================

// Show a brief toast notification at the top-right of the screen
static void showToast(const char *msg, lv_color_t bg_color, uint32_t duration_ms = 2000)
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
    if (!btn) return;
    lv_obj_set_style_bg_color(btn, flash_color, 0);
}

static bool postToPeer(const DiscoveredPeer &peer, const char *endpoint)
{
    if (strlen(peer.ip) == 0)
        return false;

    String url = "http://" + String(peer.ip) + ":" + String(peer.port) + endpoint;
    return enqueueHttpCommand(url, HTTP_CMD_POST, "", HTTP_TIMEOUT_TOUCH_MS);
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
    if (postToPeer(peer, "/reboot"))
    {
        showToast("Peer reboot command sent", lv_color_hex(0x4CAF50));
    }
    else
    {
        showToast("Peer reboot failed", lv_color_hex(0xCC0000));
    }
}

// ============================================================
// Antenna Tab - Select antenna callback
// ============================================================

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
    flashButton(btn, lv_color_hex(0xFFFFFF));
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

static bool sendRotatorCommand(const char *endpoint, const char *postBody = nullptr)
{
    DiscoveredPeer *rot = peerDiscovery.findByRole("rotator-controller");
    if (!rot || strlen(rot->ip) == 0)
    {
        showToast("Rotator not found", lv_color_hex(0xCC0000));
        return false;
    }

    String url = "http://" + String(rot->ip) + ":" + String(rot->port) + endpoint;
    String body = postBody ? String(postBody) : String("");
    if (!enqueueHttpCommand(url, HTTP_CMD_POST, body, HTTP_TIMEOUT_ROTATOR_MS))
    {
        showToast("Command queue busy", lv_color_hex(0xCC0000));
        return false;
    }
    return true;
}

static void sendGotoBearing(int heading)
{
    char body[32];
    snprintf(body, sizeof(body), "position=%d", heading);
    if (sendRotatorCommand("/api/rotator/goto", body))
    {
        showToast("Goto sent", lv_color_hex(0x2196F3), 1500);
    }
}

static void rotator_goto_cb(lv_event_t *e)
{
    int heading = (int)(intptr_t)lv_event_get_user_data(e);
    debugLogf("[TOUCH] Rotator goto %d", heading);
    lv_obj_t *btn = lv_event_get_target(e);
    flashButton(btn, lv_color_hex(0xFFFFFF));
    sendGotoBearing(heading);
    if (btn) lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
}

static void rotator_memory_cb(lv_event_t *e)
{
    int heading = (int)(intptr_t)lv_event_get_user_data(e);
    sendGotoBearing(heading);
}

static void rotator_stop_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    debugLog("[TOUCH] Rotator stop");
    flashButton(btn ? btn : btn_manual_stop, lv_color_hex(0xFFFFFF));
    sendRotatorCommand("/api/rotator/stop");
    manualRotating = false;
    manualDirection = 0;
    showToast("Stop sent", lv_color_hex(0xff4d4d), 1500);
    if (btn)
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xff4d4d), 0);
}

static void rotator_manual_ccw_cb(lv_event_t *e)
{
    (void)e;
    if (manualRotating && manualDirection == -1)
        return;
    if (sendRotatorCommand("/api/rotator/manual", "direction=ccw"))
    {
        manualRotating = true;
        manualDirection = -1;
        showToast("Manual CCW", lv_color_hex(0x1E88E5), 1200);
    }
}

static void rotator_manual_cw_cb(lv_event_t *e)
{
    (void)e;
    if (manualRotating && manualDirection == 1)
        return;
    if (sendRotatorCommand("/api/rotator/manual", "direction=cw"))
    {
        manualRotating = true;
        manualDirection = 1;
        showToast("Manual CW", lv_color_hex(0x43A047), 1200);
    }
}

static void map_confirm_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    const char *txt = lv_msgbox_get_active_btn_text(pendingMapDialog);
    if (txt && strcmp(txt, "Rotate") == 0 && pendingMapBearing >= 0)
    {
        sendGotoBearing(pendingMapBearing);
    }
    pendingMapBearing = -1;
    if (btn)
        lv_msgbox_close(pendingMapDialog);
    pendingMapDialog = nullptr;
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

    pendingMapBearing = bestBearing;
    if (pendingMapDialog)
    {
        lv_msgbox_close(pendingMapDialog);
        pendingMapDialog = nullptr;
    }
    char msg[96];
    snprintf(msg, sizeof(msg), "Rotate to %d\xC2\xB0?", bestBearing);
    static const char *btns[] = {"Rotate", "Cancel", ""};
    pendingMapDialog = lv_msgbox_create(NULL, "Map Target", msg, btns, false);
    lv_obj_center(pendingMapDialog);
    lv_obj_add_event_cb(pendingMapDialog, map_confirm_cb, LV_EVENT_VALUE_CHANGED, NULL);
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
}

// ============================================================
// Azimuthal Map Drawing
// ============================================================

#define MAP_SIZE 360
#define MAP_CX (MAP_SIZE / 2)
#define MAP_CY (MAP_SIZE / 2)
#define MAP_R  (MAP_SIZE / 2 - 10)

static lv_color_t *map_buf = nullptr;

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
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
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
    // UK
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
    // Europe
    {48.857f, 2.352f, "Paris"},
    {52.520f, 13.405f, "Berlin"},
    {40.417f, -3.704f, "Madrid"},
    {41.903f, 12.496f, "Rome"},
    {52.367f, 4.904f, "Amsterdam"},
    {50.850f, 4.351f, "Brussels"},
    {50.110f, 8.682f, "Frankfurt"},
    {45.464f, 9.190f, "Milan"},
    {45.815f, 15.982f, "Zagreb"},
    {44.426f, 26.102f, "Bucharest"},
    {37.984f, 23.728f, "Athens"},
    {59.437f, 24.754f, "Tallinn"},
    {56.949f, 24.106f, "Riga"},
    {54.687f, 25.279f, "Vilnius"},
    {50.450f, 30.523f, "Kyiv"},
    {46.948f, 7.447f, "Bern"},
    {53.349f, -6.260f, "Dublin"},
    {59.913f, 10.752f, "Oslo"},
    {59.329f, 18.069f, "Stockholm"},
    {55.676f, 12.568f, "Copenhagen"},
    {60.170f, 24.938f, "Helsinki"},
    {38.722f, -9.139f, "Lisbon"},
    {50.075f, 14.438f, "Prague"},
    {47.498f, 19.040f, "Budapest"},
    {52.230f, 21.012f, "Warsaw"},
    {48.208f, 16.374f, "Vienna"},
    {43.653f, -79.383f, "Toronto"},
    {45.501f, -73.567f, "Montreal"},
    {64.146f, -21.942f, "Reykjavik"},
    {41.387f, 2.170f, "Barcelona"},
    {43.296f, 5.369f, "Marseille"},
    {52.406f, 16.925f, "Poznan"},
    {50.061f, 19.938f, "Krakow"},
    {42.697f, 23.321f, "Sofia"},
    {44.787f, 20.448f, "Belgrade"},
    {46.771f, 23.623f, "Cluj"},
    {47.376f, 8.541f, "Zurich"},
    {48.137f, 11.575f, "Munich"},
    {53.551f, 9.993f, "Hamburg"},
    {43.710f, 7.262f, "Nice"},
    {45.070f, 7.686f, "Turin"},
    {59.931f, 30.360f, "StPetersburg"},
    // World
    {40.713f, -74.006f, "New York"},
    {34.052f, -118.244f, "Los Angeles"},
    {41.878f, -87.629f, "Chicago"},
    {47.606f, -122.332f, "Seattle"},
    {49.282f, -123.121f, "Vancouver"},
    {19.433f, -99.133f, "Mexico City"},
    {35.689f, 139.692f, "Tokyo"},
    {37.566f, 126.978f, "Seoul"},
    {39.916f, 116.397f, "Beijing"},
    {31.230f, 121.474f, "Shanghai"},
    {22.319f, 114.169f, "Hong Kong"},
    {13.756f, 100.501f, "Bangkok"},
    {10.823f, 106.629f, "Ho Chi Minh"},
    {14.600f, 120.984f, "Manila"},
    {19.076f, 72.878f, "Mumbai"},
    {24.860f, 67.001f, "Karachi"},
    {-33.869f, 151.209f, "Sydney"},
    {-37.814f, 144.963f, "Melbourne"},
    {-36.848f, 174.763f, "Auckland"},
    {55.756f, 37.617f, "Moscow"},
    {39.904f, 116.407f, "Beijing"},
    {28.614f, 77.209f, "Delhi"},
    {25.285f, 51.531f, "Doha"},
    {24.713f, 46.675f, "Riyadh"},
    {-23.551f, -46.634f, "Sao Paulo"},
    {-34.603f, -58.382f, "Buenos Aires"},
    {-33.448f, -70.669f, "Santiago"},
    {-12.047f, -77.043f, "Lima"},
    {30.044f, 31.236f, "Cairo"},
    {33.573f, -7.589f, "Casablanca"},
    {6.524f, 3.379f, "Lagos"},
    {-26.204f, 28.047f, "Johannesburg"},
    {-1.286f, 36.817f, "Nairobi"},
    {1.352f, 103.820f, "Singapore"},
    {25.205f, 55.271f, "Dubai"},
    {35.676f, 51.389f, "Tehran"},
    {41.008f, 28.978f, "Istanbul"},
    {33.893f, 35.502f, "Beirut"},
    {31.768f, 35.214f, "Jerusalem"},
    {9.082f, 8.675f, "Nigeria"},
    {-4.325f, 15.322f, "Kinshasa"},
    {-8.839f, 13.289f, "Luanda"},
    {-22.906f, -43.173f, "Rio"},
    {-0.180f, -78.468f, "Quito"},
    {4.711f, -74.072f, "Bogota"},
    {51.045f, -114.071f, "Calgary"},
    {35.227f, -80.843f, "Charlotte"},
    {33.749f, -84.388f, "Atlanta"},
    {29.760f, -95.369f, "Houston"},
    {25.762f, -80.192f, "Miami"},
    {39.739f, -104.990f, "Denver"},
    {21.306f, -157.858f, "Honolulu"},
    {45.421f, -75.697f, "Ottawa"},
};
static const int mapPointCount = sizeof(mapPoints) / sizeof(mapPoints[0]);

static void drawAzimuthalMap()
{
    // Zoom levels: UK ~800km, Europe ~3000km, World ~20000km
    float maxDist[] = {800.0f, 3000.0f, 20000.0f};
    float dist = maxDist[currentZoom];

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

    // Draw outer circle
    lv_color_t edgeColor = lv_color_hex(0x333366);
    for (int a = 0; a < 360; a++)
    {
        float rad = a * DEG_TO_RAD;
        int px = MAP_CX + (int)(MAP_R * sinf(rad));
        int py = MAP_CY - (int)(MAP_R * cosf(rad));
        if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE)
            map_buf[py * MAP_SIZE + px] = edgeColor;

        int px2 = MAP_CX + (int)((MAP_R - 1) * sinf(rad));
        int py2 = MAP_CY - (int)((MAP_R - 1) * cosf(rad));
        if (px2 >= 0 && px2 < MAP_SIZE && py2 >= 0 && py2 < MAP_SIZE)
            map_buf[py2 * MAP_SIZE + px2] = edgeColor;
    }

    // Draw coastlines
    lv_color_t coastColor = lv_color_hex(0x1a4a2a);
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

    // Draw city dots
    lv_color_t cityColor = lv_color_hex(0x5588aa);
    lv_color_t ukColor = lv_color_hex(0x88ccff);
    for (int i = 0; i < mapPointCount; i++)
    {
        int px, py;
        if (azimuthalProject(mapPoints[i].lat, mapPoints[i].lon, dist, px, py))
        {
            bool isUK = (i < 5);
            drawDot(px, py, isUK ? 3 : 2, isUK ? ukColor : cityColor);
        }
    }

    // Draw QTH center
    drawDot(MAP_CX, MAP_CY, 4, lv_color_hex(0xff4444));

    // Draw current bearing line
    if (rotatorBearing >= 0 && (millis() - rotatorLastUpdate < 30000))
    {
        float bearRad = rotatorBearing * DEG_TO_RAD;
        int ex = MAP_CX + (int)(MAP_R * sinf(bearRad));
        int ey = MAP_CY - (int)(MAP_R * cosf(bearRad));
        drawWideLine(MAP_CX, MAP_CY, ex, ey, lv_color_hex(0x4caf50));
    }

    // Draw target bearing line (dashed effect - every other pixel)
    if (rotatorTargetBearing >= 0 && rotatorMoving && (millis() - rotatorLastUpdate < 30000))
    {
        float bearRad = rotatorTargetBearing * DEG_TO_RAD;
        int len = MAP_R;
        for (int r = 0; r < len; r += 2)
        {
            int px = MAP_CX + (int)(r * sinf(bearRad));
            int py = MAP_CY - (int)(r * cosf(bearRad));
            if (px >= 0 && px < MAP_SIZE && py >= 0 && py < MAP_SIZE)
                map_buf[py * MAP_SIZE + px] = lv_color_hex(0xff9800);
        }
    }

    // Update canvas
    if (canvas_map)
    {
        static lv_img_dsc_t img_dsc;
        memset(&img_dsc, 0, sizeof(img_dsc));
        img_dsc.header.w = MAP_SIZE;
        img_dsc.header.h = MAP_SIZE;
        img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        img_dsc.data_size = MAP_SIZE * MAP_SIZE * sizeof(lv_color_t);
        img_dsc.data = (const uint8_t *)map_buf;
        lv_img_set_src(canvas_map, &img_dsc);
    }
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

    lv_obj_t *title_row = lv_obj_create(parent);
    lv_obj_set_size(title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(title_row, 0, 0);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, LV_SYMBOL_HOME "  G7NRU Network Overview");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    btn_touch_reboot = lv_btn_create(title_row);
    lv_obj_set_size(btn_touch_reboot, 138, 36);
    lv_obj_set_style_bg_color(btn_touch_reboot, lv_color_hex(0xB71C1C), 0);
    lv_obj_set_style_radius(btn_touch_reboot, 10, 0);
    lv_obj_add_event_cb(btn_touch_reboot, reboot_touch_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *reboot_lbl = lv_label_create(btn_touch_reboot);
    lv_label_set_text(reboot_lbl, LV_SYMBOL_REFRESH " Reboot Touch");
    lv_obj_center(reboot_lbl);

    // WiFi + uptime row
    lbl_wifi = lv_label_create(parent);
    lv_label_set_text(lbl_wifi, "WiFi: not connected");
    lv_obj_set_width(lbl_wifi, lv_pct(100));
    lv_label_set_long_mode(lbl_wifi, LV_LABEL_LONG_CLIP);

    lbl_uptime = lv_label_create(parent);
    lv_label_set_text(lbl_uptime, "Uptime: 0s");
    lv_obj_set_width(lbl_uptime, lv_pct(100));
    lv_label_set_long_mode(lbl_uptime, LV_LABEL_LONG_CLIP);

    lbl_build = lv_label_create(parent);
    lv_label_set_text(lbl_build, "Build: --");
    lv_obj_set_width(lbl_build, lv_pct(100));
    lv_label_set_long_mode(lbl_build, LV_LABEL_LONG_CLIP);

    lbl_overview_hw = lv_label_create(parent);
    lv_label_set_text(lbl_overview_hw, "Heap: -- KB   PSRAM: -- KB   Chip: ESP32-S3");
    lv_obj_set_width(lbl_overview_hw, lv_pct(100));
    lv_label_set_long_mode(lbl_overview_hw, LV_LABEL_LONG_CLIP);

    // Peer count label
    lbl_peers = lv_label_create(parent);
    lv_label_set_text(lbl_peers, "Peers: scanning...");
    lv_obj_set_style_text_font(lbl_peers, &lv_font_montserrat_16, 0);

    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(hdr, 3, 0);
    lv_obj_set_style_pad_gap(hdr, 6, 0);
    lv_obj_set_style_bg_color(hdr, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(hdr, lv_color_hex(0x2b3541), 0);

    const char *headers[] = {"Name", "IP", "Site", "Status", ""};
    const int widths[] = {196, 186, 112, 90, 76};
    for (int i = 0; i < 5; i++)
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
        lv_obj_set_size(peer_row_objs[i], lv_pct(100), 32);
        lv_obj_set_flex_flow(peer_row_objs[i], LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(peer_row_objs[i], 2, 0);
        lv_obj_set_style_pad_gap(peer_row_objs[i], 6, 0);
        lv_obj_set_style_bg_color(peer_row_objs[i], lv_color_hex(0x151b22), 0);
        lv_obj_set_style_border_color(peer_row_objs[i], lv_color_hex(0x2b3541), 0);
        lv_obj_add_flag(peer_row_objs[i], LV_OBJ_FLAG_HIDDEN);

        peer_name_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_width(peer_name_labels[i], widths[0]);
        lv_label_set_long_mode(peer_name_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_name_labels[i], &lv_font_montserrat_12, 0);

        peer_ip_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_width(peer_ip_labels[i], widths[1]);
        lv_label_set_long_mode(peer_ip_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_ip_labels[i], &lv_font_montserrat_12, 0);

        peer_site_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_width(peer_site_labels[i], widths[2]);
        lv_label_set_long_mode(peer_site_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_site_labels[i], &lv_font_montserrat_12, 0);

        peer_status_labels[i] = lv_label_create(peer_row_objs[i]);
        lv_obj_set_width(peer_status_labels[i], widths[3]);
        lv_label_set_long_mode(peer_status_labels[i], LV_LABEL_LONG_CLIP);
        lv_obj_set_style_text_font(peer_status_labels[i], &lv_font_montserrat_12, 0);

        peer_reboot_btns[i] = lv_btn_create(peer_row_objs[i]);
        lv_obj_set_size(peer_reboot_btns[i], widths[4], 28);
        lv_obj_set_style_bg_color(peer_reboot_btns[i], lv_color_hex(0xB71C1C), 0);
        lv_obj_add_event_cb(peer_reboot_btns[i], peer_reboot_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *icon = lv_label_create(peer_reboot_btns[i]);
        lv_label_set_text(icon, LV_SYMBOL_REFRESH " Reboot");
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_12, 0);
        lv_obj_center(icon);
    }
}

static void create_power_tab(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    // Title row
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, LV_SYMBOL_POWER "  Station Power Control");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    // Status cards row (Battery 1, Battery 2, Solar, LoRa Signal)
    lv_obj_t *cards_row = lv_obj_create(parent);
    lv_obj_set_size(cards_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cards_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cards_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cards_row, 4, 0);
    lv_obj_set_style_bg_opa(cards_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cards_row, 0, 0);

    // Battery 1 card
    lv_obj_t *bat1_card = lv_obj_create(cards_row);
    lv_obj_set_size(bat1_card, LV_PCT(24), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bat1_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(bat1_card, 8, 0);
    lv_obj_set_style_pad_gap(bat1_card, 2, 0);
    lv_obj_set_style_bg_color(bat1_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(bat1_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(bat1_card, 12, 0);

    lv_obj_t *bat1_title = lv_label_create(bat1_card);
    lv_label_set_text(bat1_title, "Battery 1");
    lv_obj_set_style_text_color(bat1_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(bat1_title, &lv_font_montserrat_12, 0);

    lbl_bat1_soc = lv_label_create(bat1_card);
    lv_label_set_text(lbl_bat1_soc, "--%");
    lv_obj_set_style_text_font(lbl_bat1_soc, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_bat1_soc, lv_color_hex(0x888888), 0);

    lbl_bat1_voltage = lv_label_create(bat1_card);
    lv_label_set_text(lbl_bat1_voltage, "");
    lv_obj_set_style_text_font(lbl_bat1_voltage, &lv_font_montserrat_12, 0);

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
    lv_obj_set_size(bat2_card, LV_PCT(24), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bat2_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(bat2_card, 8, 0);
    lv_obj_set_style_pad_gap(bat2_card, 2, 0);
    lv_obj_set_style_bg_color(bat2_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(bat2_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(bat2_card, 12, 0);

    lv_obj_t *bat2_title = lv_label_create(bat2_card);
    lv_label_set_text(bat2_title, "Battery 2");
    lv_obj_set_style_text_color(bat2_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(bat2_title, &lv_font_montserrat_12, 0);

    lbl_bat2_soc = lv_label_create(bat2_card);
    lv_label_set_text(lbl_bat2_soc, "--%");
    lv_obj_set_style_text_font(lbl_bat2_soc, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_bat2_soc, lv_color_hex(0x888888), 0);

    lbl_bat2_voltage = lv_label_create(bat2_card);
    lv_label_set_text(lbl_bat2_voltage, "");
    lv_obj_set_style_text_font(lbl_bat2_voltage, &lv_font_montserrat_12, 0);

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
    lv_obj_set_size(mppt_card, LV_PCT(24), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(mppt_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(mppt_card, 8, 0);
    lv_obj_set_style_pad_gap(mppt_card, 2, 0);
    lv_obj_set_style_bg_color(mppt_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(mppt_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(mppt_card, 12, 0);

    lv_obj_t *mppt_title = lv_label_create(mppt_card);
    lv_label_set_text(mppt_title, "Solar");
    lv_obj_set_style_text_color(mppt_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(mppt_title, &lv_font_montserrat_12, 0);

    lbl_mppt_power = lv_label_create(mppt_card);
    lv_label_set_text(lbl_mppt_power, "--W");
    lv_obj_set_style_text_font(lbl_mppt_power, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_mppt_power, lv_color_hex(0x888888), 0);

    lbl_mppt_state = lv_label_create(mppt_card);
    lv_label_set_text(lbl_mppt_state, "--");
    lv_obj_set_style_text_font(lbl_mppt_state, &lv_font_montserrat_12, 0);

    // LoRa Signal card
    lv_obj_t *signal_card = lv_obj_create(cards_row);
    lv_obj_set_size(signal_card, LV_PCT(24), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(signal_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(signal_card, 8, 0);
    lv_obj_set_style_pad_gap(signal_card, 2, 0);
    lv_obj_set_style_bg_color(signal_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(signal_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(signal_card, 12, 0);

    lv_obj_t *signal_title = lv_label_create(signal_card);
    lv_label_set_text(signal_title, "LoRa Signal");
    lv_obj_set_style_text_color(signal_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(signal_title, &lv_font_montserrat_12, 0);

    lbl_signal_rssi = lv_label_create(signal_card);
    lv_label_set_text(lbl_signal_rssi, "-- dBm");
    lv_obj_set_style_text_font(lbl_signal_rssi, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(lbl_signal_rssi, lv_color_hex(0x888888), 0);

    lbl_signal = lv_label_create(signal_card);
    lv_label_set_text(lbl_signal, "SNR: --");
    lv_obj_set_style_text_font(lbl_signal, &lv_font_montserrat_12, 0);

    bar_signal = lv_bar_create(signal_card);
    lv_obj_set_size(bar_signal, LV_PCT(100), 10);
    lv_bar_set_range(bar_signal, 0, 100);
    lv_bar_set_value(bar_signal, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(bar_signal, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_signal, lv_color_hex(0x888888), LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_signal, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_signal, 4, LV_PART_INDICATOR);

    // All On / All Off buttons
    lv_obj_t *all_row = lv_obj_create(parent);
    lv_obj_set_size(all_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(all_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(all_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(all_row, 4, 0);
    lv_obj_set_style_pad_gap(all_row, 12, 0);
    lv_obj_set_style_bg_opa(all_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(all_row, 0, 0);

    lv_obj_t *btn_all_on = lv_btn_create(all_row);
    lv_obj_set_size(btn_all_on, 120, 40);
    lv_obj_set_style_bg_color(btn_all_on, lv_color_hex(0x4caf50), 0);
    lv_obj_add_event_cb(btn_all_on, all_on_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_aon = lv_label_create(btn_all_on);
    lv_label_set_text(lbl_aon, "All On");
    lv_obj_center(lbl_aon);

    lv_obj_t *btn_all_off = lv_btn_create(all_row);
    lv_obj_set_size(btn_all_off, 120, 40);
    lv_obj_set_style_bg_color(btn_all_off, lv_color_hex(0xff4d4d), 0);
    lv_obj_add_event_cb(btn_all_off, all_off_btn_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_aoff = lv_label_create(btn_all_off);
    lv_label_set_text(lbl_aoff, "All Off");
    lv_obj_center(lbl_aoff);

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

    for (int i = 0; i < 6; i++)
    {
        lv_obj_t *btn = lv_btn_create(relay_grid);
        lv_obj_set_size(btn, LV_PCT(100), 50);
        lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, i % 3, 1,
                             LV_GRID_ALIGN_CENTER, i / 3, 1);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_add_event_cb(btn, relay_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, relayLabels[i].c_str());
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(lbl, LV_PCT(95));
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
}

static void create_antennas_tab(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 10, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, LV_SYMBOL_WIFI "  Antenna Selection");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    lv_obj_t *legend = lv_label_create(parent);
    lv_label_set_text(legend, "A1 Local   A2 DX   A3 Utility");
    lv_obj_set_style_text_font(legend, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(legend, lv_color_hex(0x8fa0ae), 0);

    const char *groupNames[] = {"A1", "A2", "A3"};
    for (int group = 0; group < 3; group++)
    {
        lv_obj_t *row = lv_obj_create(parent);
        lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_all(row, 3, 0);
        lv_obj_set_style_pad_gap(row, 6, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x151b22), 0);
        lv_obj_set_style_border_color(row, lv_color_hex(0x2b3541), 0);

        lv_obj_t *groupLbl = lv_label_create(row);
        lv_obj_set_width(groupLbl, 54);
        lv_label_set_text(groupLbl, groupNames[group]);
        lv_obj_set_style_text_font(groupLbl, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(groupLbl, lv_color_hex(0x8fa0ae), 0);

        antenna_group_rows[group] = lv_obj_create(row);
        lv_obj_set_flex_grow(antenna_group_rows[group], 1);
        lv_obj_set_height(antenna_group_rows[group], LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(antenna_group_rows[group], LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_all(antenna_group_rows[group], 0, 0);
        lv_obj_set_style_pad_gap(antenna_group_rows[group], 6, 0);
        lv_obj_set_style_bg_opa(antenna_group_rows[group], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(antenna_group_rows[group], 0, 0);
    }

    for (int i = 0; i < MAX_ANTENNAS; i++)
    {
        lv_obj_t *btn = lv_btn_create(antenna_group_rows[0]);
        lv_obj_set_size(btn, 168, 38);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x333333), 0);
        lv_obj_set_style_border_color(btn, lv_color_hex(0x666666), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 12, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(btn, antenna_btn_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, "");
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, LV_PCT(95));
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);

        antenna_btns[i] = btn;
        antenna_labels[i] = lbl;
    }

    // Status message
    lbl_antenna_status = lv_label_create(parent);
    lv_label_set_text(lbl_antenna_status, "Waiting for antenna controller...");
    lv_obj_set_style_text_color(lbl_antenna_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_antenna_status, &lv_font_montserrat_12, 0);
}

static void create_rotator_tab(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(parent, 6, 0);
    lv_obj_set_style_pad_gap(parent, 6, 0);

    lv_obj_t *map_panel = lv_obj_create(parent);
    lv_obj_set_size(map_panel, MAP_SIZE + 24, LV_PCT(100));
    lv_obj_set_flex_flow(map_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(map_panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(map_panel, 6, 0);
    lv_obj_set_style_pad_gap(map_panel, 4, 0);
    lv_obj_set_style_bg_color(map_panel, lv_color_hex(0x0a0a1a), 0);
    lv_obj_set_style_border_color(map_panel, lv_color_hex(0x333366), 0);
    lv_obj_set_style_radius(map_panel, 12, 0);

    lv_obj_t *map_stack = lv_obj_create(map_panel);
    lv_obj_set_size(map_stack, MAP_SIZE, MAP_SIZE);
    lv_obj_set_layout(map_stack, 0);
    lv_obj_set_style_bg_opa(map_stack, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(map_stack, 0, 0);
    lv_obj_set_style_pad_all(map_stack, 0, 0);

    canvas_map = lv_img_create(map_stack);
    lv_obj_set_size(canvas_map, MAP_SIZE, MAP_SIZE);
    lv_obj_align(canvas_map, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *map_touch = lv_btn_create(map_stack);
    lv_obj_set_size(map_touch, MAP_SIZE, MAP_SIZE);
    lv_obj_align(map_touch, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(map_touch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(map_touch, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(map_touch, rotator_map_click_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *zoom_row = lv_obj_create(map_panel);
    lv_obj_set_size(zoom_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(zoom_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(zoom_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(zoom_row, 2, 0);
    lv_obj_set_style_pad_gap(zoom_row, 6, 0);
    lv_obj_set_style_bg_opa(zoom_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(zoom_row, 0, 0);

    const char *zoomLabels[] = {"UK", "Europe", "World"};
    for (int i = 0; i < 3; i++)
    {
        btn_zoom[i] = lv_btn_create(zoom_row);
        lv_obj_set_size(btn_zoom[i], 84, 30);
        lv_obj_set_style_radius(btn_zoom[i], 8, 0);
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
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
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

    lv_obj_t *title_row = lv_obj_create(ctrl_panel);
    lv_obj_set_size(title_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, LV_SYMBOL_GPS "  Rotator");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    btn_rotator_enable = lv_btn_create(title_row);
    lv_obj_set_size(btn_rotator_enable, 110, 30);
    lv_obj_set_style_bg_color(btn_rotator_enable, lv_color_hex(0x2196F3), 0);
    lv_obj_set_style_radius(btn_rotator_enable, 10, 0);
    lv_obj_add_event_cb(btn_rotator_enable, rotator_enable_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_en = lv_label_create(btn_rotator_enable);
    lv_label_set_text(lbl_en, LV_SYMBOL_POWER " Enable");
    lv_obj_center(lbl_en);

    lv_obj_t *bearing_card = lv_obj_create(ctrl_panel);
    lv_obj_set_size(bearing_card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(bearing_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(bearing_card, 8, 0);
    lv_obj_set_style_pad_gap(bearing_card, 2, 0);
    lv_obj_set_style_bg_color(bearing_card, lv_color_hex(0x1a2128), 0);
    lv_obj_set_style_border_color(bearing_card, lv_color_hex(0x2b3541), 0);
    lv_obj_set_style_radius(bearing_card, 12, 0);

    lv_obj_t *bear_title = lv_label_create(bearing_card);
    lv_label_set_text(bear_title, "Current Bearing");
    lv_obj_set_style_text_color(bear_title, lv_color_hex(0x8fa0ae), 0);
    lv_obj_set_style_text_font(bear_title, &lv_font_montserrat_12, 0);

    lbl_rotator_bearing = lv_label_create(bearing_card);
    lv_label_set_text(lbl_rotator_bearing, "---\xC2\xB0");
    lv_obj_set_style_text_font(lbl_rotator_bearing, &lv_font_montserrat_28, 0);

    lbl_rotator_target = lv_label_create(bearing_card);
    lv_label_set_text(lbl_rotator_target, "");
    lv_obj_set_style_text_font(lbl_rotator_target, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(lbl_rotator_target, lv_color_hex(0xff9800), 0);

    struct MemoryGroup { const char *title; const RotatorMemoryPoint *points; };
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

        lv_obj_t *gtitle = lv_label_create(rotator_memory_groups[g]);
        lv_label_set_text(gtitle, groups[g].title);
        lv_obj_set_style_text_color(gtitle, lv_color_hex(0x8fa0ae), 0);
        lv_obj_set_style_text_font(gtitle, &lv_font_montserrat_12, 0);

        lv_obj_t *grid = lv_obj_create(rotator_memory_groups[g]);
        lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_layout(grid, LV_LAYOUT_GRID);
        static lv_coord_t mem_col[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
        static lv_coord_t mem_row[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
        lv_obj_set_grid_dsc_array(grid, mem_col, mem_row);
        lv_obj_set_style_pad_all(grid, 2, 0);
        lv_obj_set_style_pad_gap(grid, 4, 0);
        lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(grid, 0, 0);

        for (int i = 0; i < 10; i++)
        {
            lv_obj_t *btn = lv_btn_create(grid);
            lv_obj_set_size(btn, LV_PCT(100), 40);
            lv_obj_set_grid_cell(btn, LV_GRID_ALIGN_STRETCH, i % 5, 1,
                                 LV_GRID_ALIGN_CENTER, i / 5, 1);
            lv_obj_set_style_bg_color(btn, lv_color_hex(0x1a2128), 0);
            lv_obj_set_style_border_color(btn, lv_color_hex(0x2b3541), 0);
            lv_obj_set_style_border_width(btn, 1, 0);
            lv_obj_set_style_radius(btn, 6, 0);
            lv_obj_add_event_cb(btn, rotator_memory_cb, LV_EVENT_CLICKED,
                                (void *)(intptr_t)groups[g].points[i].bearing);
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, groups[g].points[i].name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lbl, LV_PCT(96));
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(lbl);
        }

        if (g != currentZoom)
            lv_obj_add_flag(rotator_memory_groups[g], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *manual_row = lv_obj_create(ctrl_panel);
    lv_obj_set_size(manual_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(manual_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(manual_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(manual_row, 2, 0);
    lv_obj_set_style_pad_gap(manual_row, 8, 0);
    lv_obj_set_style_bg_opa(manual_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(manual_row, 0, 0);

    btn_manual_ccw = lv_btn_create(manual_row);
    lv_obj_set_size(btn_manual_ccw, 140, 52);
    lv_obj_set_style_bg_color(btn_manual_ccw, lv_color_hex(0x1565C0), 0);
    lv_obj_add_event_cb(btn_manual_ccw, rotator_manual_ccw_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_ccw = lv_label_create(btn_manual_ccw);
    lv_label_set_text(lbl_ccw, "<<");
    lv_obj_set_style_text_font(lbl_ccw, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_ccw);

    btn_manual_stop = lv_btn_create(manual_row);
    lv_obj_set_size(btn_manual_stop, 140, 52);
    lv_obj_set_style_bg_color(btn_manual_stop, lv_color_hex(0xC62828), 0);
    lv_obj_add_event_cb(btn_manual_stop, rotator_stop_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_stop = lv_label_create(btn_manual_stop);
    lv_label_set_text(lbl_stop, "Stop");
    lv_obj_set_style_text_font(lbl_stop, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_stop);

    btn_manual_cw = lv_btn_create(manual_row);
    lv_obj_set_size(btn_manual_cw, 140, 52);
    lv_obj_set_style_bg_color(btn_manual_cw, lv_color_hex(0x2E7D32), 0);
    lv_obj_add_event_cb(btn_manual_cw, rotator_manual_cw_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cw = lv_label_create(btn_manual_cw);
    lv_label_set_text(lbl_cw, ">>");
    lv_obj_set_style_text_font(lbl_cw, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl_cw);

    lbl_rotator_status = lv_label_create(ctrl_panel);
    lv_label_set_text(lbl_rotator_status, "Tap map to request rotation target");
    lv_obj_set_style_text_color(lbl_rotator_status, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl_rotator_status, &lv_font_montserrat_12, 0);

    drawAzimuthalMap();
    mapDirty = false;
}

static void create_propagation_tab(lv_obj_t *parent)
{
    // 800x480 screen math: 44px tab header leaves ~436px vertical. Layout below targets ~420px total.
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 4, 0);
    lv_obj_set_style_pad_gap(parent, 4, 0);

    // Title
    lv_obj_t *title = lv_label_create(parent);
    lv_label_set_text(title, LV_SYMBOL_REFRESH "  Propagation");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    // Solar indices row - 4 mini cards
    lv_obj_t *solar_row = lv_obj_create(parent);
    lv_obj_set_size(solar_row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(solar_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(solar_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(solar_row, 1, 0);
    lv_obj_set_style_pad_gap(solar_row, 4, 0);
    lv_obj_set_style_bg_opa(solar_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(solar_row, 0, 0);

    struct SolarCardDef
    {
        const char *label;
        lv_obj_t **val;
    };
    SolarCardDef solarDefs[] = {
        {"SFI", &prop_sfi_val},
        {"K-Index", &prop_k_val},
        {"A-Index", &prop_a_val},
        {"SSN", &prop_ssn_val},
    };

    for (int i = 0; i < 4; i++)
    {
        lv_obj_t *card = lv_obj_create(solar_row);
        lv_obj_set_size(card, LV_PCT(24), 72);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 1, 0);
        lv_obj_set_style_pad_gap(card, 0, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1a2128), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x2b3541), 0);
        lv_obj_set_style_radius(card, 6, 0);

        *solarDefs[i].val = lv_label_create(card);
        lv_label_set_text(*solarDefs[i].val, "--");
        lv_obj_set_style_text_font(*solarDefs[i].val, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(*solarDefs[i].val, lv_color_hex(0x888888), 0);

        lv_obj_t *lbl = lv_label_create(card);
        lv_label_set_text(lbl, solarDefs[i].label);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0x8fa0ae), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
    }

    // Band cards grid - 4 columns, 4 rows
    lv_obj_t *bands_grid = lv_obj_create(parent);
    lv_obj_set_size(bands_grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_layout(bands_grid, LV_LAYOUT_GRID);
    static lv_coord_t bcol[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t brow[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(bands_grid, bcol, brow);
    lv_obj_set_style_pad_all(bands_grid, 1, 0);
    lv_obj_set_style_pad_gap(bands_grid, 4, 0);
    lv_obj_set_style_bg_opa(bands_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bands_grid, 0, 0);

    for (int i = 0; i < PROP_NUM_BANDS; i++)
    {
        lv_obj_t *card = lv_obj_create(bands_grid);
        lv_obj_set_grid_cell(card, LV_GRID_ALIGN_STRETCH, i % 4, 1,
                     LV_GRID_ALIGN_STRETCH, i / 4, 1);
        lv_obj_set_height(card, 70);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(card, 1, 0);
        lv_obj_set_style_pad_gap(card, 0, 0);
        lv_obj_set_style_bg_color(card, lv_color_hex(0x1a1a2e), 0);
        lv_obj_set_style_border_color(card, lv_color_hex(0x333355), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 6, 0);

        // Band name
        lv_obj_t *name = lv_label_create(card);
        lv_label_set_text(name, propBands[i].name);
        lv_obj_set_style_text_font(name, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xdddddd), 0);

        // Condition text
        prop_band_cond_lbl[i] = lv_label_create(card);
        lv_label_set_text(prop_band_cond_lbl[i], "");
        lv_obj_set_style_text_font(prop_band_cond_lbl[i], &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(prop_band_cond_lbl[i], lv_color_hex(0x888888), 0);

        prop_band_cards[i] = card;
    }

    // VHF conditions + update footer
    lv_obj_t *footer = lv_obj_create(parent);
    lv_obj_set_size(footer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(footer, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(footer, 2, 0);
    lv_obj_set_style_bg_opa(footer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(footer, 0, 0);

    prop_vhf_lbl = lv_label_create(footer);
    lv_label_set_text(prop_vhf_lbl, "VHF: Loading...");
    lv_obj_set_style_text_font(prop_vhf_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(prop_vhf_lbl, lv_color_hex(0x888888), 0);

    prop_updated_lbl = lv_label_create(footer);
    lv_label_set_text(prop_updated_lbl, "Loading...");
    lv_obj_set_style_text_font(prop_updated_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(prop_updated_lbl, lv_color_hex(0x888888), 0);
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

    tabview = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 44);
    lv_obj_set_style_text_font(lv_obj_get_child(tabview, 0), &lv_font_montserrat_14, 0);

    tab_overview = lv_tabview_add_tab(tabview, "Overview");
    tab_power = lv_tabview_add_tab(tabview, "Power");
    tab_antennas = lv_tabview_add_tab(tabview, "Antennas");
    tab_rotator = lv_tabview_add_tab(tabview, "Rotator");
    tab_propagation = lv_tabview_add_tab(tabview, "Prop");

    create_overview_tab(tab_overview);
    create_power_tab(tab_power);
    create_antennas_tab(tab_antennas);
    create_rotator_tab(tab_rotator);
    create_propagation_tab(tab_propagation);
}

// ============================================================
// UI Update
// ============================================================

static void update_power_tab()
{
    // Update relay buttons
    for (int i = 0; i < 6; i++)
    {
        if (!power_relay_btns[i])
            continue;

        lv_label_set_text(power_relay_labels[i], relayLabels[i].c_str());

        if (remoteSleeping || !relayDataReady)
        {
            lv_obj_set_style_bg_color(power_relay_btns[i], lv_color_hex(0x333333), 0);
            lv_obj_set_style_border_color(power_relay_btns[i], lv_color_hex(0x666666), 0);
        }
        else if (relayStates[i])
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

    // Update battery cards
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
            lv_label_set_text(soc_lbl, buf);
            lv_color_t col = remoteBms[i].soc > 50 ? lv_color_hex(0x4caf50) : remoteBms[i].soc > 20 ? lv_color_hex(0xff9800)
                                                                                                    : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(soc_lbl, col, 0);

            snprintf(buf, sizeof(buf), "%.2fV", remoteBms[i].voltage);
            lv_label_set_text(v_lbl, buf);

            // Update bar
            lv_obj_t *bar = (i == 0) ? bar_bat1_soc : bar_bat2_soc;
            if (bar)
            {
                lv_bar_set_value(bar, (int)remoteBms[i].soc, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(bar, col, LV_PART_INDICATOR);
            }
        }
        else
        {
            lv_label_set_text(soc_lbl, "--%%");
            lv_obj_set_style_text_color(soc_lbl, lv_color_hex(0x888888), 0);
            lv_label_set_text(v_lbl, "");
            lv_obj_t *bar = (i == 0) ? bar_bat1_soc : bar_bat2_soc;
            if (bar)
            {
                lv_bar_set_value(bar, 0, LV_ANIM_OFF);
                lv_obj_set_style_bg_color(bar, lv_color_hex(0x888888), LV_PART_INDICATOR);
            }
        }
    }

    // Update MPPT
    if (lbl_mppt_power)
    {
        if (remoteMPPTValid)
        {
            snprintf(buf, sizeof(buf), "%.1fW", remoteMPPTPower);
            lv_label_set_text(lbl_mppt_power, buf);
            lv_color_t col = remoteMPPTPower > 10 ? lv_color_hex(0x4caf50) : remoteMPPTPower > 0 ? lv_color_hex(0xff9800)
                                                                                                 : lv_color_hex(0x888888);
            lv_obj_set_style_text_color(lbl_mppt_power, col, 0);
        }
        else
        {
            lv_label_set_text(lbl_mppt_power, "--W");
            lv_obj_set_style_text_color(lbl_mppt_power, lv_color_hex(0x888888), 0);
        }
    }
    if (lbl_mppt_state)
    {
        lv_label_set_text(lbl_mppt_state, remoteMPPTStateName.c_str());
    }

    // Update LoRa signal card
    if (lbl_signal_rssi)
    {
        if (hasLoRaRx)
        {
            snprintf(buf, sizeof(buf), "%.0f dBm", lastLoRaRssi);
            lv_label_set_text(lbl_signal_rssi, buf);
            // Color based on signal strength: green > -80, orange > -100, red below
            lv_color_t col = lastLoRaRssi > -80 ? lv_color_hex(0x4caf50) : lastLoRaRssi > -100 ? lv_color_hex(0xff9800)
                                                                                               : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(lbl_signal_rssi, col, 0);
        }
        else
        {
            lv_label_set_text(lbl_signal_rssi, "-- dBm");
            lv_obj_set_style_text_color(lbl_signal_rssi, lv_color_hex(0x888888), 0);
        }
    }
    if (lbl_signal)
    {
        if (hasLoRaRx)
        {
            snprintf(buf, sizeof(buf), "SNR: %.1f dB", lastLoRaSnr);
        }
        else
        {
            snprintf(buf, sizeof(buf), "SNR: --");
        }
        lv_label_set_text(lbl_signal, buf);
    }
    if (bar_signal)
    {
        if (hasLoRaRx)
        {
            // Map RSSI -130..-30 to 0..100
            int pct = constrain((int)((lastLoRaRssi + 130) * 100 / 100), 0, 100);
            lv_bar_set_value(bar_signal, pct, LV_ANIM_OFF);
            lv_color_t col = pct > 60 ? lv_color_hex(0x4caf50) : pct > 30 ? lv_color_hex(0xff9800)
                                                                          : lv_color_hex(0xf44336);
            lv_obj_set_style_bg_color(bar_signal, col, LV_PART_INDICATOR);
        }
        else
        {
            lv_bar_set_value(bar_signal, 0, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar_signal, lv_color_hex(0x888888), LV_PART_INDICATOR);
        }
    }

    // Status message
    if (lbl_power_status)
    {
        if (remoteSleeping)
            lv_label_set_text(lbl_power_status, "Remote device sleeping");
        else if (!relayDataReady)
            lv_label_set_text(lbl_power_status, "Waiting for relay data...");
        else
            lv_label_set_text(lbl_power_status, lastStatusMessage.length() > 0 ? lastStatusMessage.c_str() : "Connected");
    }
}

static void update_propagation_tab()
{
    char buf[64];

    // Solar indices
    if (solarData.valid)
    {
        if (prop_sfi_val)
        {
            snprintf(buf, sizeof(buf), "%d", solarData.sfi);
            lv_label_set_text(prop_sfi_val, buf);
            lv_color_t c = solarData.sfi >= 150 ? lv_color_hex(0x4caf50) :
                           solarData.sfi >= 100 ? lv_color_hex(0x8bc34a) :
                           solarData.sfi >= 70  ? lv_color_hex(0xff9800) : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(prop_sfi_val, c, 0);
        }
        if (prop_k_val)
        {
            snprintf(buf, sizeof(buf), "%d", solarData.kIndex);
            lv_label_set_text(prop_k_val, buf);
            lv_color_t c = solarData.kIndex <= 1 ? lv_color_hex(0x4caf50) :
                           solarData.kIndex <= 3 ? lv_color_hex(0xff9800) : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(prop_k_val, c, 0);
        }
        if (prop_a_val)
        {
            snprintf(buf, sizeof(buf), "%d", solarData.aIndex);
            lv_label_set_text(prop_a_val, buf);
            lv_color_t c = solarData.aIndex <= 7 ? lv_color_hex(0x4caf50) :
                           solarData.aIndex <= 20 ? lv_color_hex(0xff9800) : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(prop_a_val, c, 0);
        }
        if (prop_ssn_val)
        {
            snprintf(buf, sizeof(buf), "%d", solarData.ssn);
            lv_label_set_text(prop_ssn_val, buf);
            lv_obj_set_style_text_color(prop_ssn_val, lv_color_hex(0xcccccc), 0);
        }
    }

    // Band cards
    for (int i = 0; i < PROP_NUM_BANDS; i++)
    {
        if (!prop_band_cards[i]) continue;

        // HF condition text + card coloring based on condition
        if (solarData.valid && propBands[i].hfGroupIndex >= 0)
        {
            int g = propBands[i].hfGroupIndex;
            if (strlen(solarData.hfCondDay[g]) > 0)
            {
                snprintf(buf, sizeof(buf), "%s/%s", solarData.hfCondDay[g], solarData.hfCondNight[g]);
                lv_label_set_text(prop_band_cond_lbl[i], buf);
                bool dayGood = (strcmp(solarData.hfCondDay[g], "Good") == 0);
                bool dayFair = (strcmp(solarData.hfCondDay[g], "Fair") == 0);
                bool nightGood = (strcmp(solarData.hfCondNight[g], "Good") == 0);
                bool nightFair = (strcmp(solarData.hfCondNight[g], "Fair") == 0);
                // Text color based on day condition
                lv_color_t cc = dayGood ? lv_color_hex(0x4caf50) :
                                dayFair ? lv_color_hex(0xff9800) : lv_color_hex(0xf44336);
                lv_obj_set_style_text_color(prop_band_cond_lbl[i], cc, 0);
                // Card background/border based on best of day/night
                bool anyGood = dayGood || nightGood;
                bool anyFair = dayFair || nightFair;
                lv_color_t bg, br;
                if (anyGood) {
                    bg = lv_color_hex(0x1a2e1a); br = lv_color_hex(0x4caf50);  // Green
                } else if (anyFair) {
                    bg = lv_color_hex(0x2e2a1a); br = lv_color_hex(0xff9800);  // Orange
                } else {
                    bg = lv_color_hex(0x2e1a1a); br = lv_color_hex(0xf44336);  // Red
                }
                lv_obj_set_style_bg_color(prop_band_cards[i], bg, 0);
                lv_obj_set_style_border_color(prop_band_cards[i], br, 0);
            }
        }
        // 160m (i=0): use 80m-40m group (index 0)
        else if (i == 0 && solarData.valid && strlen(solarData.hfCondDay[0]) > 0)
        {
            snprintf(buf, sizeof(buf), "%s/%s", solarData.hfCondDay[0], solarData.hfCondNight[0]);
            lv_label_set_text(prop_band_cond_lbl[i], buf);
            bool good = (strcmp(solarData.hfCondDay[0], "Good") == 0);
            bool fair = (strcmp(solarData.hfCondDay[0], "Fair") == 0);
            lv_color_t cc = good ? lv_color_hex(0x4caf50) : fair ? lv_color_hex(0xff9800) : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(prop_band_cond_lbl[i], cc, 0);
            lv_color_t bg = good ? lv_color_hex(0x1a2e1a) : fair ? lv_color_hex(0x2e2a1a) : lv_color_hex(0x2e1a1a);
            lv_color_t brd = good ? lv_color_hex(0x4caf50) : fair ? lv_color_hex(0xff9800) : lv_color_hex(0xf44336);
            lv_obj_set_style_bg_color(prop_band_cards[i], bg, 0);
            lv_obj_set_style_border_color(prop_band_cards[i], brd, 0);
        }
        // 60m (i=2): use 80m-40m group (index 0)
        else if (i == 2 && solarData.valid && strlen(solarData.hfCondDay[0]) > 0)
        {
            snprintf(buf, sizeof(buf), "%s/%s", solarData.hfCondDay[0], solarData.hfCondNight[0]);
            lv_label_set_text(prop_band_cond_lbl[i], buf);
            bool good = (strcmp(solarData.hfCondDay[0], "Good") == 0);
            bool fair = (strcmp(solarData.hfCondDay[0], "Fair") == 0);
            lv_color_t cc = good ? lv_color_hex(0x4caf50) : fair ? lv_color_hex(0xff9800) : lv_color_hex(0xf44336);
            lv_obj_set_style_text_color(prop_band_cond_lbl[i], cc, 0);
            lv_color_t bg = good ? lv_color_hex(0x1a2e1a) : fair ? lv_color_hex(0x2e2a1a) : lv_color_hex(0x2e1a1a);
            lv_color_t brd = good ? lv_color_hex(0x4caf50) : fair ? lv_color_hex(0xff9800) : lv_color_hex(0xf44336);
            lv_obj_set_style_bg_color(prop_band_cards[i], bg, 0);
            lv_obj_set_style_border_color(prop_band_cards[i], brd, 0);
        }
        // 6m (i=10): E-Skip
        else if (i == 10 && solarData.valid && strlen(solarData.vhfESkipEU) > 0)
        {
            lv_label_set_text(prop_band_cond_lbl[i], solarData.vhfESkipEU);
            bool open = (strstr(solarData.vhfESkipEU, "Closed") == nullptr);
            lv_obj_set_style_text_color(prop_band_cond_lbl[i],
                open ? lv_color_hex(0x4caf50) : lv_color_hex(0x888888), 0);
            if (open) {
                lv_obj_set_style_bg_color(prop_band_cards[i], lv_color_hex(0x1a2e1a), 0);
                lv_obj_set_style_border_color(prop_band_cards[i], lv_color_hex(0x4caf50), 0);
            }
        }
        // 2m (i=11) and 70cm (i=12): Aurora
        else if ((i == 11 || i == 12) && solarData.valid && strlen(solarData.vhfAurora) > 0)
        {
            lv_label_set_text(prop_band_cond_lbl[i], solarData.vhfAurora);
            bool active = (strstr(solarData.vhfAurora, "Active") != nullptr ||
                          strstr(solarData.vhfAurora, "Aurora") != nullptr);
            lv_obj_set_style_text_color(prop_band_cond_lbl[i],
                active ? lv_color_hex(0x4caf50) : lv_color_hex(0x888888), 0);
            if (active) {
                lv_obj_set_style_bg_color(prop_band_cards[i], lv_color_hex(0x1a2e1a), 0);
                lv_obj_set_style_border_color(prop_band_cards[i], lv_color_hex(0x4caf50), 0);
            }
        }

    }

    // VHF status
    if (prop_vhf_lbl && solarData.valid)
    {
        snprintf(buf, sizeof(buf), "Es: %s  Au: %s  %s  %s",
                 strlen(solarData.vhfESkipEU) > 0 ? solarData.vhfESkipEU : "--",
                 strlen(solarData.vhfAurora) > 0 ? solarData.vhfAurora : "--",
                 solarData.geoMag, solarData.signalNoise);
        lv_label_set_text(prop_vhf_lbl, buf);
    }

    // Update time
    if (prop_updated_lbl && solarData.valid)
    {
        unsigned long age = (millis() - solarData.lastUpdate) / 1000;
        if (age < 60)
            snprintf(buf, sizeof(buf), "Updated %lus ago", age);
        else
            snprintf(buf, sizeof(buf), "Updated %lum ago", age / 60);
        lv_label_set_text(prop_updated_lbl, buf);
    }
}

static void update_ui()
{
    unsigned long secs = millis() / 1000;
    char buf[512];

    // Uptime
    snprintf(buf, sizeof(buf), "Uptime: %luh %lum %lus", secs / 3600, (secs % 3600) / 60, secs % 60);
    if (lbl_uptime)
        lv_label_set_text(lbl_uptime, buf);

    if (lbl_build)
    {
        snprintf(buf, sizeof(buf), "Build: %s %s", buildDate, buildTime);
        lv_label_set_text(lbl_build, buf);
    }

    if (lbl_overview_hw)
    {
        snprintf(buf, sizeof(buf), "Heap: %lu KB   PSRAM: %lu KB   Chip: ESP32-S3",
                 ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
        lv_label_set_text(lbl_overview_hw, buf);
    }

    // WiFi
    if (lbl_wifi)
    {
        if (WiFi.status() == WL_CONNECTED)
        {
            snprintf(buf, sizeof(buf), "WiFi: %s  IP: %s  RSSI: %d",
                     WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
        }
        else
        {
            snprintf(buf, sizeof(buf), "WiFi: not connected");
        }
        lv_label_set_text(lbl_wifi, buf);
    }

    // Overview peer rows
    if (lbl_peers)
    {
        int count = peerDiscovery.peerCount();
        const DiscoveredPeer *peers = peerDiscovery.peers();
        snprintf(buf, sizeof(buf), LV_SYMBOL_WIFI "  Peers: %d discovered", count);
        lv_label_set_text(lbl_peers, buf);

        for (int i = 0; i < MAX_PEER_ROWS; i++)
        {
            peerRowToIndex[i] = -1;
            if (peer_row_objs[i])
                lv_obj_add_flag(peer_row_objs[i], LV_OBJ_FLAG_HIDDEN);
        }

        int row = 0;
        for (int i = 0; i < count && row < MAX_PEER_ROWS; i++)
        {
            const DiscoveredPeer &p = peers[i];
            if (!peer_row_objs[row])
                continue;

            peerRowToIndex[row] = i;
            lv_obj_clear_flag(peer_row_objs[row], LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(peer_name_labels[row], p.name);
            lv_label_set_text(peer_ip_labels[row], strlen(p.ip) > 0 ? p.ip : "-");
            lv_label_set_text(peer_site_labels[row], p.site);

            unsigned long age = millis() - p.lastSeen;
            if (p.reachable && age < 30000)
            {
                lv_label_set_text(peer_status_labels[row], "Online");
            }
            else if (age < 120000)
            {
                snprintf(buf, sizeof(buf), "%lus ago", age / 1000);
                lv_label_set_text(peer_status_labels[row], buf);
            }
            else
            {
                lv_label_set_text(peer_status_labels[row], "Stale");
            }

            row++;
        }
    }

    // Rotator tab update
    bool rotatorOnline = rotatorBearing >= 0 && (millis() - rotatorLastUpdate < 30000);
    if (lbl_rotator_bearing)
    {
        if (rotatorOnline)
        {
            snprintf(buf, sizeof(buf), "%.0f\xC2\xB0", rotatorBearing);
            lv_obj_set_style_text_color(lbl_rotator_bearing, lv_color_hex(0x4caf50), 0);
        }
        else
        {
            snprintf(buf, sizeof(buf), "---\xC2\xB0");
            lv_obj_set_style_text_color(lbl_rotator_bearing, lv_color_hex(0x888888), 0);
        }
        lv_label_set_text(lbl_rotator_bearing, buf);
    }
    if (lbl_rotator_target)
    {
        if (rotatorMoving && rotatorTargetBearing >= 0)
        {
            snprintf(buf, sizeof(buf), LV_SYMBOL_RIGHT " Target: %.0f\xC2\xB0", rotatorTargetBearing);
            lv_label_set_text(lbl_rotator_target, buf);
        }
        else
        {
            lv_label_set_text(lbl_rotator_target, "");
        }
    }
    if (lbl_rotator_status)
    {
        if (!rotatorOnline)
            lv_label_set_text(lbl_rotator_status, "Waiting for rotator...");
        else if (!rotatorEnabled)
            lv_label_set_text(lbl_rotator_status, "Rotator disabled");
        else if (rotatorMoving)
            lv_label_set_text(lbl_rotator_status, "Rotating...");
        else if (!rotatorCalibrated)
            lv_label_set_text(lbl_rotator_status, "Not calibrated");
        else
            lv_label_set_text(lbl_rotator_status, "Ready");
    }
    // Update enable button label to reflect current state
    if (btn_rotator_enable)
    {
        lv_obj_t *lbl = lv_obj_get_child(btn_rotator_enable, 0);
        if (lbl)
            lv_label_set_text(lbl, rotatorEnabled ? LV_SYMBOL_POWER " Disable" : LV_SYMBOL_POWER " Enable");
    }
    // Redraw azimuthal map only when the displayed state changes
    bool mapOnline = rotatorOnline;
    if (mapDirty ||
        currentZoom != lastMapZoom ||
        mapOnline != lastMapOnline ||
        rotatorMoving != lastMapMoving ||
        fabsf(rotatorBearing - lastMapBearing) >= 0.5f ||
        fabsf(rotatorTargetBearing - lastMapTargetBearing) >= 0.5f)
    {
        drawAzimuthalMap();
        mapDirty = false;
        lastMapZoom = currentZoom;
        lastMapOnline = mapOnline;
        lastMapMoving = rotatorMoving;
        lastMapBearing = rotatorBearing;
        lastMapTargetBearing = rotatorTargetBearing;
    }

    // Antenna tab update
    if (antennaDataReady && (millis() - antennaLastUpdate < 30000))
    {
        for (int i = 0; i < MAX_ANTENNAS; i++)
        {
            if (i < antennaCount && antenna_btns[i])
            {
                int groupRaw = antennas[i].group;
                int group = groupRaw;
                if (groupRaw >= 1 && groupRaw <= 3)
                    group = groupRaw - 1;
                if (group < 0 || group > 2)
                    group = 0;
                if (antenna_group_rows[group] && lv_obj_get_parent(antenna_btns[i]) != antenna_group_rows[group])
                    lv_obj_set_parent(antenna_btns[i], antenna_group_rows[group]);

                lv_obj_clear_flag(antenna_btns[i], LV_OBJ_FLAG_HIDDEN);
                snprintf(buf, sizeof(buf), "%s", antennas[i].name);
                lv_label_set_text(antenna_labels[i], buf);
                antennaBtnIds[i] = antennas[i].id;

                if (antennas[i].active)
                {
                    lv_obj_set_style_bg_color(antenna_btns[i], lv_color_hex(0x1b3a1b), 0);
                    lv_obj_set_style_border_color(antenna_btns[i], lv_color_hex(0x4caf50), 0);
                }
                else
                {
                    lv_color_t gcol = lv_color_hex(0x2b3541);
                    if (group == 0) gcol = lv_color_hex(0x1565C0);
                    if (group == 1) gcol = lv_color_hex(0x6A1B9A);
                    if (group == 2) gcol = lv_color_hex(0x2E7D32);
                    lv_obj_set_style_bg_color(antenna_btns[i], lv_color_hex(0x1a2128), 0);
                    lv_obj_set_style_border_color(antenna_btns[i], gcol, 0);
                }
            }
            else if (antenna_btns[i])
            {
                lv_obj_add_flag(antenna_btns[i], LV_OBJ_FLAG_HIDDEN);
                antennaBtnIds[i] = -1;
            }
        }
        if (lbl_antenna_status)
            lv_label_set_text(lbl_antenna_status, "Connected");
    }
    else if (lbl_antenna_status)
    {
        lv_label_set_text(lbl_antenna_status, "Waiting for antenna controller...");
    }

    // Power tab
    update_power_tab();

    // Propagation tab
    update_propagation_tab();
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
    lv_indev_drv_register(&indev_drv);

    debugLog("[LVGL] Initialized with PSRAM buffers");

    // Allocate azimuthal map buffer in PSRAM
    map_buf = (lv_color_t *)heap_caps_malloc(MAP_SIZE * MAP_SIZE * sizeof(lv_color_t), MALLOC_CAP_SPIRAM);
    if (!map_buf)
    {
        debugLog("[MAP] PSRAM alloc failed, using internal RAM");
        map_buf = (lv_color_t *)malloc(MAP_SIZE * MAP_SIZE * sizeof(lv_color_t));
    }

    // Build UI
    create_ui();
    debugLog("[UI] Tab view created");

    // Start WiFi
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
                           debugLog("[OTA] Update started");
                       });
    ArduinoOTA.onEnd([]()
                     {
                         debugLog("[OTA] Update finished");
                         otaInProgress = false;
                     });
    ArduinoOTA.onError([](ota_error_t err)
                       {
                           debugLogf("[OTA] Error %u", (unsigned)err);
                           otaInProgress = false;
                       });
    ArduinoOTA.setHostname(deviceName.c_str());
    ArduinoOTA.setPassword("otapass");
    ArduinoOTA.begin();
    debugLog("[OTA] Ready");

    // Web server
    setupWebServer();

    // Peer discovery
    peerDiscovery.begin(deviceName.c_str(), 80, "touch-controller", "thelimes");

    debugLogf("[SETUP] Free heap: %lu KB, PSRAM: %lu KB",
              ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
    debugLog("=== Setup complete ===");
}

void loop()
{
    // LVGL task handler
    lv_timer_handler();

    // Web server
    server.handleClient();

    // WiFi management
    WiFiManager::loop();

    // OTA
    ArduinoOTA.handle();

    // Peer discovery
    peerDiscovery.loop();

    bool uiIdle = (millis() - lastTouchActivity) > 120;
    if (uiIdle && !otaInProgress)
    {
        // Avoid running potentially blocking network operations while actively touching.
        pollAllPeers();
        pollPropagationProxy();
        processHttpCommandQueue();
    }

    // Logger maintenance
    DebugLogger::periodicFlush();

    // Periodic UI update
    if (millis() - lastUiUpdate > UI_UPDATE_INTERVAL)
    {
        update_ui();
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
