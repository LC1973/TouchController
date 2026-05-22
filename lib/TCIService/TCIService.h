/**
 * TCI Service - Expert Electronics TCI Protocol Client for ESP32
 * 
 * Connects to Expert SDR3 or other TCI-compatible radios via WebSocket
 * Receives VFO frequency, mode, and antenna information
 */

#ifndef TCI_SERVICE_H
#define TCI_SERVICE_H

#include <Arduino.h>
#include <WebSocketsClient.h>
#include <functional>

class TCIServiceClass {
public:
    // Callback types
    using FrequencyCallback = std::function<void(double freqMHz, int vfo)>;
    using ModeCallback = std::function<void(const String& mode, int vfo)>;
    using AntennaCallback = std::function<void(const String& rxAntenna, const String& txAntenna, int receiver)>;
    using TXStateCallback = std::function<void(bool isTransmitting)>;
    using ConnectionCallback = std::function<void(bool connected)>;
    using LogCallback = std::function<void(const String& msg)>;
    
    TCIServiceClass();
    
    // Configuration
    void begin(const String& host, uint16_t port = 40001);
    void setHost(const String& host, uint16_t port = 40001);
    void disconnect();
    
    // Must be called in loop()
    void loop();
    
    // Callbacks
    void onFrequencyChange(FrequencyCallback callback) { _frequencyCallback = callback; }
    void onModeChange(ModeCallback callback) { _modeCallback = callback; }
    void onAntennaChange(AntennaCallback callback) { _antennaCallback = callback; }
    void onTXStateChange(TXStateCallback callback) { _txStateCallback = callback; }
    void onConnectionChange(ConnectionCallback callback) { _connectionCallback = callback; }
    void onLog(LogCallback callback) { _logCallback = callback; }
    
    // Current state getters
    bool isConnected() const { return _connected; }
    bool isTransmitting() const { return _isTransmitting; }
    double getFrequency(int vfo = 0) const { return vfo == 0 ? _vfoAFreq : _vfoBFreq; }
    String getMode(int vfo = 0) const { return vfo == 0 ? _vfoAMode : _vfoBMode; }
    String getRxAntenna() const { return _rxAntenna; }
    String getTxAntenna() const { return _txAntenna; }
    String getDeviceInfo() const { return _deviceInfo; }
    
    // Send commands to radio
    void sendCommand(const String& cmd);
    void queryFrequency(int vfo = 0);
    void queryMode(int vfo = 0);
    void queryAntennas(int receiver = 0);
    
private:
    WebSocketsClient _webSocket;
    String _host;
    uint16_t _port;
    bool _connected;
    bool _isTransmitting;
    unsigned long _lastReconnectAttempt;
    unsigned long _reconnectInterval;
    
    // Cached radio state
    double _vfoAFreq;
    double _vfoBFreq;
    String _vfoAMode;
    String _vfoBMode;
    String _rxAntenna;
    String _txAntenna;
    String _deviceInfo;
    String _protocolInfo;
    
    // Callbacks
    FrequencyCallback _frequencyCallback;
    ModeCallback _modeCallback;
    AntennaCallback _antennaCallback;
    TXStateCallback _txStateCallback;
    ConnectionCallback _connectionCallback;
    LogCallback _logCallback;

    void tciLog(const String& msg);
    
    // WebSocket event handler
    void onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length);
    void processMessage(const String& message);
    void sendInitCommands();
};

extern TCIServiceClass TCIService;

#endif // TCI_SERVICE_H
