# ESP32 Wake-on-LAN via MQTT + W5500 — Complete Setup Guide

Remote wake for a Windows 11 PC from anywhere, with no router port forwarding.
The ESP32 connects to WiFi for MQTT (outbound only), and sends the magic packet
through a W5500 Lite Ethernet module wired directly to the PC's motherboard NIC.

```
Phone (anywhere) ──publish──▶ MQTT cloud broker
                                    │
                     ESP32 ◀──WiFi──┘ (subscribed, outbound only)
                       │
                       └──W5500 / RJ45 cable──▶ PC Ethernet NIC ──▶ PC wakes
```

Security: TLS on the broker connection (port 8883) + TOTP token that rotates
every 30 seconds (replay-attack proof).

---

## 1. Hardware Requirements

| Item | Notes |
|---|---|
| ESP32 dev board | Any standard ESP32-WROOM / DevKit |
| W5500 Lite Ethernet module | 3.3V ONLY — no onboard regulator |
| Jumper wires | Female-to-male recommended (W5500 has pin headers) |
| USB power adapter | 5V, 1A+ recommended (W5500 adds ~130mA to ESP32 draw) |
| Ethernet cable | W5500 → PC motherboard Ethernet port |

**PC requirements:**
- Wired Ethernet NIC (built into motherboard) — WoL over WiFi (WoWLAN) is
  unreliable and only works from S3 sleep, hence the wired approach
- BIOS: enable "Wake on LAN" / "Power on by PCI-E" / "Resume by LAN"
- Device Manager → Ethernet adapter → Power Management: allow wake +
  magic packet only; Advanced tab: Wake on Magic Packet = Enabled
- Fast Startup DISABLED (Control Panel → Power Options → Choose what the
  power buttons do → uncheck "Turn on fast startup")
- Verify armed: `powercfg /devicequery wake_armed` must list the Ethernet adapter
- To force true S3 sleep remotely: `rundll32.exe powrprof.dll,SetSuspendState 0,1,0`
- Disable mouse/keyboard wake in Device Manager if the PC wakes too easily

**Note:** The PC gets internet via WiFi as normal after waking. The Ethernet
link to the W5500 has no gateway — Windows ignores it for routing. The
"No internet" triangle on that adapter is expected and harmless.

---

## 2. Wiring — W5500 Lite to ESP32

W5500 Lite pin labels are abbreviated. Full mapping:

| W5500 Lite | Meaning | ESP32 pin |
|---|---|---|
| V | VCC 3.3V | **3V3** (never VIN/5V — will damage the chip) |
| G | GND | GND |
| MO | MOSI | GPIO 23 |
| MI | MISO | GPIO 19 |
| SCK | SPI clock | GPIO 18 |
| CS | Chip select | GPIO 5 |
| RST | Reset | GPIO 26 (optional but recommended for 24/7 use) |
| NT | Interrupt | Not connected |
| NC | Not connected | Not connected |

Duplicate G/V pins are the same net internally — use either.
Keep SPI wires under 15cm. If `EthernetNoHardware` appears intermittently,
shorten the wires. MOSI/MISO swapped is the most common wiring mistake.

Ethernet cable runs from the W5500 RJ45 jack directly to the PC's
motherboard Ethernet port. No router needed in this segment. The PC's NIC
stays link-up on standby power when the PC is off — that's what receives
the magic packet.

---

## 3. Arduino IDE Setup

1. Install Arduino IDE: https://www.arduino.cc/en/software
2. Add ESP32 board support:
   - File → Preferences → Additional Board Manager URLs:
     `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
   - Tools → Board → Boards Manager → search "esp32" → install (by Espressif)
3. Select board: Tools → Board → ESP32 Dev Module
4. Install libraries (Sketch → Include Library → Manage Libraries):

| Library | Author | Purpose |
|---|---|---|
| PubSubClient | Nick O'Leary | MQTT client |
| ArduinoJson | Benoit Blanchon | JSON payload parsing |
| TOTP library | Luca Dentella | Time-based one-time passwords |
| NTPClient | Fabrice Weinberg | Clock sync (required for TOTP) |
| Ethernet | Arduino (built-in) | W5500 driver |

---

## 4. Secrets and Credentials Checklist

You need to prepare THREE things before flashing:

### 4a. TOTP shared secret
1. Generate a random base32 secret at: https://it-tools.tech/otp-generator
   Example format: `JBSWY3DPEHPK3PXP` — **generate your own, do not reuse examples**
2. Convert base32 → hex bytes at: https://cryptii.com/pipes/base32-to-hex
3. Put the hex bytes into `TOTP_SECRET[]` in the sketch
4. Keep the base32 string — you need it for the phone (step 5)

### 4b. MQTT broker account
Free tier of HiveMQ Cloud (https://www.hivemq.com/mqtt-cloud-broker/) or EMQX:
- Note the broker hostname (e.g. `xxxx.s1.eu.hivemq.cloud`)
- TLS port: 8883
- Create a username + password for the ESP32
- Pick a topic name, e.g. `home/esp32/wol` (treat the topic name as
  semi-secret too)

### 4c. PC Ethernet MAC address
On the PC, run in PowerShell:
```powershell
getmac /v /fo list
```
Use the physical address of the **Ethernet** adapter (NOT WiFi, not virtual
adapters). Convert `A1-B2-C3-D4-E5-F6` into the sketch as:
```cpp
uint8_t PC_MAC[6] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };
```

---

## 5. Phone Authenticator Setup

Any RFC 6238 app works: Google Authenticator, Aegis (Android), Raivo (iOS).

1. Add account → Enter setup key manually
2. Account name: `ESP32 WoL` (anything)
3. Key: your base32 secret from step 4a
4. Type: Time-based
5. Save — it now shows a 6-digit code refreshing every 30 seconds

That 6-digit code is the `token` field in the wake command. Both phone and
ESP32 compute `HMAC-SHA1(secret, floor(unix_time / 30))` independently —
same secret + same time = same code. Captured tokens die within ~60s
(the ESP32 accepts the current and previous 30s window for clock drift).

---

## 6. Sending the Wake Command

Publish this JSON to your topic (token = current code from the authenticator):

```json
{ "token": "482910", "cmd": "wake" }
```

From a laptop with mosquitto clients:
```bash
mosquitto_pub -h your-broker.hivemq.cloud -p 8883 \
  -u your_mqtt_user -P your_mqtt_password \
  -t "home/esp32/wol" \
  -m '{"token":"482910","cmd":"wake"}'
```

Or from a phone MQTT app (MQTT Dashboard, IoT MQTT Panel) — configure the
broker with TLS on port 8883 and publish to the same topic.

The ESP32 publishes a confirmation back to the same topic:
```json
{ "status": "sent", "msg": "Magic packet sent via Ethernet — PC should wake shortly" }
```

---

## 7. The Sketch (final version)

Includes all fixes discovered during debugging:
- TOTP static buffer bug: `getCode()` returns a pointer to a shared internal
  buffer, so both results are copied into separate arrays with `strncpy`
- Guard against the ESP32 reacting to its own confirmation messages
- Guard against payloads with no token
- WiFi handles MQTT; W5500 handles the magic packet

```cpp
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <TOTP.h>
#include <NTPClient.h>

// ============================================================
//  CONFIG — fill in every value below
// ============================================================

const char* WIFI_SSID      = "YourWiFiName";
const char* WIFI_PASSWORD  = "YourWiFiPassword";

const char* MQTT_HOST      = "your-broker.hivemq.cloud";
const int   MQTT_PORT      = 8883;
const char* MQTT_USER      = "your_mqtt_user";
const char* MQTT_PASS      = "your_mqtt_password";
const char* MQTT_CLIENT_ID = "esp32-wol-01";
const char* MQTT_TOPIC     = "home/esp32/wol";

// TOTP secret — hex bytes converted from your base32 secret (section 4a)
uint8_t TOTP_SECRET[] = { 0x48, 0x65, 0x6C, 0x6C, 0x6F, 0x21, 0xDE, 0xAD, 0xBE, 0xEF };

// W5500 identity — any locally administered MAC, static IP on its own segment
byte      W5500_MAC[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress W5500_IP(192, 168, 1, 200);
IPAddress W5500_DNS(192, 168, 1, 1);
IPAddress W5500_GW(192, 168, 1, 1);
IPAddress W5500_SUBNET(255, 255, 255, 0);
const int W5500_CS = 5;

// Target PC — ETHERNET adapter MAC (not WiFi), from getmac (section 4c)
uint8_t PC_MAC[6] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };

const int WOL_PORT = 9;

// ============================================================
//  INTERNALS
// ============================================================

WiFiClientSecure tlsClient;
PubSubClient     mqtt(tlsClient);
WiFiUDP          ntpUDP;
NTPClient        timeClient(ntpUDP, "pool.ntp.org", 0, 60000);
TOTP             totp(TOTP_SECRET, sizeof(TOTP_SECRET));
EthernetUDP      wolUDP;

void sendMagicPacket() {
    uint8_t packet[102];
    for (int i = 0; i < 6; i++) packet[i] = 0xFF;
    for (int rep = 0; rep < 16; rep++)
        for (int b = 0; b < 6; b++)
            packet[6 + rep * 6 + b] = PC_MAC[b];

    wolUDP.beginPacket(IPAddress(255, 255, 255, 255), WOL_PORT);
    wolUDP.write(packet, sizeof(packet));
    wolUDP.endPacket();

    Serial.println("[WoL] Magic packet sent via W5500 Ethernet!");
    Serial.printf("[WoL] Target MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        PC_MAC[0], PC_MAC[1], PC_MAC[2], PC_MAC[3], PC_MAC[4], PC_MAC[5]);
}

void publishConfirmation() {
    StaticJsonDocument<128> doc;
    doc["status"] = "sent";
    doc["msg"]    = "Magic packet sent via Ethernet — PC should wake shortly";
    char buffer[128];
    serializeJson(doc, buffer);
    mqtt.publish(MQTT_TOPIC, buffer);
    Serial.println("[MQTT] Confirmation published");
}

void onMessage(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] Message received on: %s\n", topic);

    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, payload, length)) {
        Serial.println("[MQTT] Bad JSON — ignoring");
        return;
    }

    // Guard 1 — ignore our own confirmation messages
    if (doc.containsKey("status")) {
        Serial.println("[MQTT] Ignoring own confirmation");
        return;
    }

    const char* receivedToken = doc["token"] | "";
    const char* cmd           = doc["cmd"]   | "";

    // Guard 2 — ignore anything with no token
    if (strlen(receivedToken) == 0) {
        Serial.println("[MQTT] No token present — ignoring");
        return;
    }

    timeClient.update();
    long currentTime = timeClient.getEpochTime();

    // TOTP getCode() reuses one internal buffer — copy each result out
    char expectedToken[7];
    char prevToken[7];
    strncpy(expectedToken, totp.getCode(currentTime),      6);
    expectedToken[6] = '\0';
    strncpy(prevToken,     totp.getCode(currentTime - 30), 6);
    prevToken[6] = '\0';

    Serial.printf("[TOTP] Expected: %s | Prev: %s | Received: %s\n",
                  expectedToken, prevToken, receivedToken);

    if (strcmp(receivedToken, expectedToken) != 0 &&
        strcmp(receivedToken, prevToken)     != 0) {
        Serial.println("[TOTP] Token mismatch — rejected");
        return;
    }

    if (strcmp(cmd, "wake") == 0) {
        Serial.println("[TOTP] Token valid — sending WoL");
        sendMagicPacket();
        publishConfirmation();
    }
}

void connectWiFi() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
}

void initEthernet() {
    Ethernet.init(W5500_CS);
    Ethernet.begin(W5500_MAC, W5500_IP, W5500_DNS, W5500_GW, W5500_SUBNET);
    delay(1000);

    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("[W5500] Hardware not found — check SPI wiring!");
    } else if (Ethernet.linkStatus() == LinkOFF) {
        Serial.println("[W5500] Cable not connected!");
    } else {
        Serial.printf("[W5500] Ready! IP: %s\n", Ethernet.localIP().toString().c_str());
        wolUDP.begin(WOL_PORT);
    }
}

void connectMQTT() {
    tlsClient.setInsecure(); // for full verification: tlsClient.setCACert(ROOT_CA)
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setCallback(onMessage);

    while (!mqtt.connected()) {
        Serial.printf("[MQTT] Connecting to %s ...\n", MQTT_HOST);
        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS)) {
            Serial.println("[MQTT] Connected!");
            mqtt.subscribe(MQTT_TOPIC);
            Serial.printf("[MQTT] Subscribed to: %s\n", MQTT_TOPIC);
        } else {
            Serial.printf("[MQTT] Failed (state=%d) — retry in 5s\n", mqtt.state());
            delay(5000);
        }
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n[Boot] ESP32 WoL starting...");

    connectWiFi();
    initEthernet();

    timeClient.begin();
    timeClient.update();
    Serial.printf("[NTP] Time synced: %lu\n", timeClient.getEpochTime());

    connectMQTT();
    Serial.println("[Boot] Ready — WiFi handles MQTT, W5500 handles WoL");
}

void loop() {
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    if (!mqtt.connected()) connectMQTT();
    timeClient.update();
    mqtt.loop();
}
```

---

## 8. First-Flash Verification Order

1. **Wiring test** — flash the minimal detection sketch below first.
   Expect `W5500 detected!` and `Cable link UP` (with cable plugged into PC).

```cpp
#include <SPI.h>
#include <Ethernet.h>
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
void setup() {
    Serial.begin(115200);
    delay(1000);
    Ethernet.init(5);
    Ethernet.begin(mac, IPAddress(192,168,1,200));
    delay(1000);
    if (Ethernet.hardwareStatus() == EthernetNoHardware)
        Serial.println("W5500 NOT detected — check SPI wiring");
    else {
        Serial.println("W5500 detected!");
        Serial.println(Ethernet.linkStatus() == LinkON ? "Cable link UP" : "Cable link DOWN");
    }
}
void loop() {}
```

2. **Flash the full sketch** — serial monitor should show WiFi, W5500, NTP,
   and MQTT all connecting.
3. **Sleep the PC** (S3): `rundll32.exe powrprof.dll,SetSuspendState 0,1,0`
4. **Publish the wake command** with the current authenticator code.
5. Serial monitor should show `Token valid — sending WoL` and the PC wakes.

## 9. Troubleshooting Quick Reference

| Symptom | Likely cause |
|---|---|
| `W5500 NOT detected` | MOSI/MISO swapped, wrong CS pin, wires too long |
| `Cable link DOWN` | Cable unplugged, or PC fully unpowered at the wall |
| `Token mismatch` but tokens look equal | TOTP buffer bug — verify the strncpy fix is in place |
| Token mismatch after own confirmation | Normal if guards missing — verify Guard 1 & 2 |
| Packet sent but PC doesn't wake | Wrong MAC (WiFi vs Ethernet), BIOS WoL off, Fast Startup on |
| PC wakes randomly | Mouse/keyboard armed — `powercfg /devicequery wake_armed` |
| Random ESP32 resets | Underpowered USB adapter — use 1A+ |
