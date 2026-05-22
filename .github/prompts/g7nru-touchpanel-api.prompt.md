---
mode: ask
description: Ensure this ESP32 project exposes the data required by the G7NRU TouchController Overview pane (uptime, build date, mDNS registration) and can push state-change notifications to it
---

# G7NRU TouchController API Compatibility

This project must expose data that the **G7NRU TouchController** can discover and display on its Overview pane, and must notify the touch panel whenever its state changes so that updates appear within ~500 ms rather than waiting for the 5-second polling cycle.

## Requirements

### 1. mDNS peer registration via PeerDiscovery

Call `peerDiscovery.begin()` once WiFi is connected.  The service is
`_g7nru._tcp` and requires two TXT records — `role` and `site`:

```cpp
#include <PeerDiscovery.h>
#include <ESPmDNS.h>

// After MDNS.begin(hostname) and WiFi connected:
peerDiscovery.begin(
    hostname,    // mDNS hostname (e.g. "lora-gateway")
    80,          // HTTP port
    "role-name", // role TXT record — see known roles below
    "site-name"  // site TXT record (e.g. "paddock", "thelimes")
);
```

Call `peerDiscovery.loop()` from the main `loop()`.

**Known roles** (use the exact string):
| Role string             | Used by            |
|-------------------------|--------------------|
| `lora-gateway`          | LoRa gateway node  |
| `lora-remote`           | LoRa remote node   |
| `rotator-controller`    | Antenna rotator    |
| `antenna-controller`    | Relay antenna ctrl |
| `touch-controller`      | The touch panel itself |

For any other device that should appear in the Overview peer list, pick a
short lowercase kebab-case role string (e.g. `"weather-station"`).

---

### 2. `/api/status` HTTP endpoint (required for uptime & build date)

Every device **must** serve a JSON response on `GET /api/status` that
includes at minimum:

```json
{
  "uptimeMs": 123456,
  "buildDate": "May  7 2026 12:34:56",
  "wifi_rssi": -62
}
```

> **`wifi_rssi`** *(required)*: Report the current WiFi RSSI in dBm — always a
> negative integer when on WiFi, e.g. `-62`.  Omit the field **or** set it to `0`
> if the device is connected via Ethernet — the Overview pane will display
> `"Wired"` for those peers.  The touch panel colour-codes signal strength:
> green ≥ −55 dBm, amber ≥ −70 dBm, red < −70 dBm.

Minimum implementation:

```cpp
#include <ArduinoJson.h>

const char *buildDate = __DATE__;
const char *buildTime = __TIME__;

void handleStatusApi() {
    JsonDocument doc;
    doc["uptimeMs"]  = millis();
    doc["buildDate"] = String(buildDate) + " " + String(buildTime);
    doc["wifi_rssi"] = WiFi.RSSI();   // omit or set 0 for wired devices

    // Add any role-specific fields below (see extended format)

    String json;
    serializeJson(doc, json);
    server.send(200, "application/json", json);
}

// In setup():
server.on("/api/status", handleStatusApi);
```

> **Why `buildDate` can be missing**: `__DATE__` and `__TIME__` are
> substituted at compile time.  If the handler returns the string literal
> `""` or omits the key, the Overview pane shows `"-"` for that peer.
> Always concatenate both macros so the full timestamp is present.

---

### 3. Extended `/api/status` fields (role-specific, optional)

The TouchController reads these additional fields when present.  Omit any
that don't apply to this device.

#### LoRa gateway / remote (`lora-gateway`, `lora-remote`)

```json
{
  "uptimeMs": 123456,
  "buildDate": "May  7 2026 12:34:56",
  "states":  [true, false, true, false, false, true],
  "labels":  ["Label0","Label1","Label2","Label3","Label4","Label5"],
  "relayDataReady": true,
  "remoteSleeping": false,
  "dalyBms": [
    { "name":"Battery 1","connected":true,"voltage":26.1,"soc":87.0,
      "tempHigh":22.5,"cells":[3275,3276,3274,3277] },
    { "name":"Battery 2","connected":false,"voltage":0,"soc":0,
      "tempHigh":-127,"cells":[] }
  ],
  "mppt": {
    "valid":true,"power":145.2,"state":3,"stateName":"Absorption",
    "systemVoltage":24.0
  },
  "lora": { "valid":true,"rssi":-87,"snr":6.5,"age":4 },
  "temperatures": [
    { "label":"Cabinet", "celsius": 28.3 },
    { "label":"Ambient",  "celsius": 14.1 }
  ],
  "lastStatus": "All OK"
}
```

#### Rotator controller (`rotator-controller`)

Include at minimum `uptimeMs` and `buildDate`.  The rotator tab polls
`/api/rotator` separately for bearing data.

#### Antenna controller (`antenna-controller`)

Include at minimum `uptimeMs` and `buildDate`.  The antennas tab polls
`/api/antennas` separately for relay states.

---

### 4. CORS / content-type

The TouchController fetches via `HTTPClient` (not a browser), so no CORS
headers are required.  The `Content-Type` must be `application/json`.

---

### 5. Push notification on state change — `POST /api/notify` on the touch panel

**This is the most important integration point for low-latency updates.**

Whenever this device changes state (antenna switched, relay toggled, rotator
moved, etc.) it must POST a notification to the touch controller so the UI
refreshes immediately rather than waiting up to 5 seconds for the next poll.

#### Endpoint on the touch controller

```
POST http://<touch-panel-ip>/api/notify?role=<your-role>
```

The touch panel responds `{"ok":true}` and immediately triggers a fresh
`GET /api/status` poll of the notifying device.

#### Implementation pattern

Resolve the touch panel's IP from mDNS at startup (role `touch-controller`,
service `_g7nru._tcp`) and cache it.  Call `sendNotify()` after every
meaningful state change:

```cpp
#include <HTTPClient.h>
#include <ESPmDNS.h>

static String touchPanelUrl; // resolved once, updated on reconnect

// Call once after WiFi + mDNS are ready
void resolveTouchPanel() {
    int n = MDNS.queryService("g7nru", "tcp");
    for (int i = 0; i < n; i++) {
        String role = MDNS.txt(i, "role");
        if (role == "touch-controller") {
            touchPanelUrl = "http://" + MDNS.IP(i).toString()
                            + ":" + String(MDNS.port(i))
                            + "/api/notify?role=YOUR_ROLE_STRING";
            break;
        }
    }
}

// Call after any state change that should appear on the touch panel promptly
void sendNotify() {
    if (touchPanelUrl.isEmpty()) return;
    HTTPClient http;
    http.begin(touchPanelUrl);
    http.setTimeout(300); // fire-and-forget; keep timeout short
    http.POST("");
    http.end();
}
```

Replace `YOUR_ROLE_STRING` with the exact role string used in
`peerDiscovery.begin()` (e.g. `antenna-controller`, `rotator-controller`,
`lora-gateway`).

> **When to call `sendNotify()`:**
> - Antenna-controller: after any relay state change driven by TCI frequency update
> - Rotator-controller: when movement starts, stops, or target changes
> - LoRa gateway / remote: after relay states are confirmed changed
> - Any device: on WiFi reconnect (so the touch panel picks up the new IP quickly)

> **Failure handling:** The notify is best-effort.  If the touch panel is
> unreachable the call simply times out after 300 ms.  The polling safety-net
> will pick up the change within 5 seconds regardless.

---

### Checklist

- [ ] `MDNS.begin(hostname)` called before `peerDiscovery.begin()`
- [ ] `peerDiscovery.begin(hostname, port, role, site)` called on WiFi connect
- [ ] `peerDiscovery.loop()` called every iteration of `loop()`
- [ ] `server.on("/api/status", handleStatusApi)` registered
- [ ] `handleStatusApi` includes `uptimeMs` (millis) and `buildDate` (__DATE__ + " " + __TIME__)
- [ ] `handleStatusApi` includes `wifi_rssi` (WiFi.RSSI(), or omit/0 for wired)
- [ ] Role string matches one of the known values (or a new consistent kebab-case string)
- [ ] Site string matches the physical location used by the rest of the network
- [ ] `resolveTouchPanel()` called after WiFi + mDNS ready; URL cached
- [ ] `sendNotify()` called after every meaningful state change
- [ ] `sendNotify()` timeout is ≤ 300 ms (fire-and-forget)
