/**
 * TCI Service - Expert Electronics TCI Protocol Client for ESP32
 */

#include "TCIService.h"

// Global instance
TCIServiceClass TCIService;

void TCIServiceClass::tciLog(const String& msg) {
    Serial.println(msg);
    if (_logCallback) _logCallback(msg);
}

TCIServiceClass::TCIServiceClass() 
    : _port(40001)
    , _connected(false)
    , _isTransmitting(false)
    , _lastReconnectAttempt(0)
    , _reconnectInterval(5000)
    , _vfoAFreq(14.200)
    , _vfoBFreq(7.100)
    , _vfoAMode("USB")
    , _vfoBMode("LSB")
    , _rxAntenna("")
    , _txAntenna("")
{
}

void TCIServiceClass::begin(const String& host, uint16_t port) {
    _host = host;
    _port = port;
    
    Serial.printf("[TCI] Connecting to %s:%d\n", host.c_str(), port);
    
    // Setup WebSocket client
    _webSocket.begin(host.c_str(), port, "/");
    
    // Set event handler using lambda
    _webSocket.onEvent([this](WStype_t type, uint8_t* payload, size_t length) {
        this->onWebSocketEvent(type, payload, length);
    });
    
    // Set reconnect interval
    _webSocket.setReconnectInterval(_reconnectInterval);
    
    // Enable heartbeat — pong timeout generous to avoid false disconnects
    _webSocket.enableHeartbeat(15000, 8000, 2);
}

void TCIServiceClass::setHost(const String& host, uint16_t port) {
    disconnect();
    begin(host, port);
}

void TCIServiceClass::disconnect() {
    _webSocket.disconnect();
    _connected = false;
    if (_connectionCallback) {
        _connectionCallback(false);
    }
}

void TCIServiceClass::loop() {
    _webSocket.loop();
}

void TCIServiceClass::sendCommand(const String& cmd) {
    if (_connected) {
        String cmdCopy = cmd;
        _webSocket.sendTXT(cmdCopy);
        Serial.printf("[TCI] Sent: %s\n", cmd.c_str());
    }
}

void TCIServiceClass::queryFrequency(int vfo) {
    sendCommand("vfo:0," + String(vfo) + ";");
}

void TCIServiceClass::queryMode(int vfo) {
    sendCommand("modulation:" + String(vfo) + ";");
}

void TCIServiceClass::queryAntennas(int receiver) {
    sendCommand("rx_antenna:" + String(receiver) + ";");
    sendCommand("tx_antenna:" + String(receiver) + ";");
}

void TCIServiceClass::sendInitCommands() {
    Serial.println("[TCI] Sending initialization commands...");
    
    // Standard TCI handshake — only send spec-defined commands
    // (ExpertSDR will close the connection if it receives unknown commands)
    sendCommand("ready;");
    sendCommand("protocol;");
    sendCommand("device;");
    
    // Query current state
    sendCommand("vfo:0,0;");      // VFO A frequency
    sendCommand("vfo:0,1;");      // VFO B frequency
    sendCommand("modulation:0;"); // VFO A mode
    sendCommand("modulation:1;"); // VFO B mode
    sendCommand("trx:0;");        // TX state
    sendCommand("rx_antenna:0;");
    sendCommand("tx_antenna:0;");
    
    Serial.println("[TCI] Initialization commands sent");
}

void TCIServiceClass::onWebSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("[TCI] Disconnected");
            _connected = false;
            if (_connectionCallback) {
                _connectionCallback(false);
            }
            break;
            
        case WStype_CONNECTED:
            Serial.printf("[TCI] Connected to %s\n", (char*)payload);
            _connected = true;
            if (_connectionCallback) {
                _connectionCallback(true);
            }
            sendInitCommands();
            break;
            
        case WStype_TEXT:
            {
                String message = String((char*)payload);
                message.trim();
                // Log ALL incoming messages for debugging
                tciLog("[TCI] RAW: " + message);
                processMessage(message);
            }
            break;
            
        case WStype_BIN:
            Serial.printf("[TCI] Binary message received: %u bytes\n", length);
            break;
            
        case WStype_PING:
        case WStype_PONG:
            // Heartbeat handled automatically
            break;
            
        case WStype_ERROR:
            Serial.println("[TCI] WebSocket error");
            break;
            
        default:
            break;
    }
}

void TCIServiceClass::processMessage(const String& message) {
    // Parse multiple commands separated by semicolons
    int start = 0;
    while (start < message.length()) {
        int end = message.indexOf(';', start);
        if (end < 0) end = message.length();
        
        String cmd = message.substring(start, end);
        cmd.trim();
        
        if (cmd.length() > 0) {
            // Parse VFO frequency: vfo:0,0,14200000; (receiver, vfo, freq_hz)
            // Only track receiver 0 (main receiver); receiver 1 is a sub-receiver and would overwrite.
            if (cmd.startsWith("vfo:")) {
                int firstComma = cmd.indexOf(',');
                int secondComma = cmd.indexOf(',', firstComma + 1);
                if (firstComma > 0 && secondComma > 0) {
                    int receiver = cmd.substring(4, firstComma).toInt();
                    if (receiver == 0) {
                        int vfo = cmd.substring(firstComma + 1, secondComma).toInt();
                        double freqHz = cmd.substring(secondComma + 1).toDouble();
                        double freqMHz = freqHz / 1000000.0;
                        
                        if (vfo == 0) {
                            _vfoAFreq = freqMHz;
                        } else {
                            _vfoBFreq = freqMHz;
                        }
                        
                        char buf[48];
                        snprintf(buf, sizeof(buf), "[TCI] VFO %c: %.6f MHz", vfo == 0 ? 'A' : 'B', freqMHz);
                        tciLog(buf);
                        
                        if (_frequencyCallback) {
                            _frequencyCallback(freqMHz, vfo);
                        }
                    }
                }
            }
            // dds: is a duplicate of vfo: for the centre frequency — ignore it to avoid double-updates
            else if (cmd.startsWith("dds:")) {
                // intentionally ignored; vfo: carries the same information more precisely
            }
            // Parse modulation: modulation:0,USB;
            else if (cmd.startsWith("modulation:")) {
                int comma = cmd.indexOf(',');
                if (comma > 0) {
                    int vfo = cmd.substring(11, comma).toInt();
                    String mode = cmd.substring(comma + 1);
                    
                    if (vfo == 0) {
                        _vfoAMode = mode;
                    } else {
                        _vfoBMode = mode;
                    }
                    
                    Serial.printf("[TCI] Mode VFO %c: %s\n", vfo == 0 ? 'A' : 'B', mode.c_str());
                    
                    if (_modeCallback) {
                        _modeCallback(mode, vfo);
                    }
                }
            }
            // Parse TX state: trx:0,true; or trx:0,false;
            else if (cmd.startsWith("trx:")) {
                int comma = cmd.indexOf(',');
                if (comma > 0) {
                    String state = cmd.substring(comma + 1);
                    state.toLowerCase();
                    bool transmitting = (state == "true" || state == "1" || state == "tx");
                    
                    if (transmitting != _isTransmitting) {
                        _isTransmitting = transmitting;
                        Serial.printf("[TCI] TX: %s\n", transmitting ? "TRANSMITTING" : "RECEIVING");
                        
                        if (_txStateCallback) {
                            _txStateCallback(transmitting);
                        }
                    }
                }
            }
            // Parse RX antenna: rx_antenna:0,ANT1;
            else if (cmd.startsWith("rx_antenna:")) {
                int comma = cmd.indexOf(',');
                if (comma > 0) {
                    String antenna = cmd.substring(comma + 1);
                    int receiver = cmd.substring(11, comma).toInt();
                    _rxAntenna = antenna;
                    Serial.printf("[TCI] RX Antenna receiver %d: %s\n", receiver, antenna.c_str());
                    
                    if (_antennaCallback) {
                        _antennaCallback(_rxAntenna, _txAntenna, receiver);
                    }
                } else {
                    Serial.printf("[TCI] RX Antenna - no comma found in: %s\n", cmd.c_str());
                }
            }
            // Parse TX antenna: tx_antenna:0,ANT1;
            else if (cmd.startsWith("tx_antenna:")) {
                int comma = cmd.indexOf(',');
                if (comma > 0) {
                    String antenna = cmd.substring(comma + 1);
                    int receiver = cmd.substring(11, comma).toInt();
                    _txAntenna = antenna;
                    Serial.printf("[TCI] TX Antenna receiver %d: %s\n", receiver, antenna.c_str());
                    
                    if (_antennaCallback) {
                        _antennaCallback(_rxAntenna, _txAntenna, receiver);
                    }
                } else {
                    Serial.printf("[TCI] TX Antenna - no comma found in: %s\n", cmd.c_str());
                }
            }
            // Parse device info: device:SunSDR2DX;
            else if (cmd.startsWith("device:")) {
                _deviceInfo = cmd.substring(7);
                Serial.printf("[TCI] Device: %s\n", _deviceInfo.c_str());
            }
            // Parse protocol: protocol:ExpertSDR3,2.0;
            else if (cmd.startsWith("protocol:")) {
                _protocolInfo = cmd.substring(9);
                Serial.printf("[TCI] Protocol: %s\n", _protocolInfo.c_str());
            }
        }
        
        start = end + 1;
    }
}

