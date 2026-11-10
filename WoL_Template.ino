#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <Ethernet.h>
#include <EthernetUdp.h>
#include <utility/w5100.h>   // direct W5500 PHY register access (link recovery)
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
const int W5500_CS  = 5;
const int W5500_RST = 26;   // W5500 RST pin (section 2). Set to -1 if not wired.

// Target PC — ETHERNET adapter MAC (not WiFi), from getmac (section 4c)
uint8_t PC_MAC[6] = { 0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6 };

const int WOL_PORT = 9;

// Link handling
const unsigned long ETH_POLL_MS        = 1000;   // how often the link is checked
const unsigned long LINK_RECOVERY_MS   = 10000;  // link down this long -> restart PHY
const unsigned long LINK_WAIT_MS       = 4000;   // wait this long for link on a wake command
const int           WOL_REPEAT         = 3;      // magic packets per wake command

// ============================================================
//  INTERNALS
// ============================================================

WiFiClientSecure tlsClient;
PubSubClient     mqtt(tlsClient);
WiFiUDP          ntpUDP;
NTPClient        timeClient(ntpUDP, "pool.ntp.org", 0, 60000);
TOTP             totp(TOTP_SECRET, sizeof(TOTP_SECRET));
EthernetUDP      wolUDP;

bool               ethReady      = false;     // W5500 found and UDP socket open
EthernetLinkStatus lastLink      = Unknown;
unsigned long      linkDownSince = 0;
unsigned long      lastEthPoll   = 0;
unsigned long      lastEthInit   = 0;
bool               phyForced10M  = false;

// W5500 PHYCFGR bits
const uint8_t PHY_RST       = 0x80;  // 0 = PHY in reset
const uint8_t PHY_OPMD      = 0x40;  // 1 = mode set by software (bits below)
const uint8_t PHY_MODE_AUTO = 0x38;  // all capable, auto-negotiation
const uint8_t PHY_MODE_10HD = 0x00;  // forced 10BASE-T half duplex
const uint8_t PHY_DPX       = 0x04;  // 1 = full duplex
const uint8_t PHY_SPD       = 0x02;  // 1 = 100 Mbps

uint8_t readPhyCfg() {
    SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
    uint8_t cfg = W5100.getPHYCFGR_W5500();
    SPI.endTransaction();
    return cfg;
}

void writePhyCfg(uint8_t cfg) {
    SPI.beginTransaction(SPI_ETHERNET_SETTINGS);
    W5100.setPHYCFGR_W5500(cfg);
    SPI.endTransaction();
}

// Set the PHY operating mode and reset the PHY so the link is negotiated again.
void restartPhy(uint8_t modeBits) {
    if (Ethernet.hardwareStatus() != EthernetW5500) return;
    uint8_t cfg = PHY_OPMD | modeBits;
    writePhyCfg(cfg | PHY_RST);
    writePhyCfg(cfg);              // RST low = reset
    delay(2);
    writePhyCfg(cfg | PHY_RST);
}

void printLinkUp() {
    uint8_t cfg = readPhyCfg();
    Serial.printf("[W5500] Link UP (%s Mbps, %s duplex)\n",
                  (cfg & PHY_SPD) ? "100" : "10",
                  (cfg & PHY_DPX) ? "full" : "half");
}

// Returns true when the W5500 answers and the WoL socket is open.
// The link does NOT have to be up — it is watched by serviceEthernet().
bool initEthernet() {
    ethReady    = false;
    lastEthInit = millis();

    if (W5500_RST >= 0) {
        pinMode(W5500_RST, OUTPUT);
        digitalWrite(W5500_RST, LOW);
        delay(5);
        digitalWrite(W5500_RST, HIGH);
        delay(200);
    }

    Ethernet.init(W5500_CS);
    Ethernet.begin(W5500_MAC, W5500_IP, W5500_DNS, W5500_GW, W5500_SUBNET);

    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("[W5500] Hardware not found — check SPI wiring! Will retry.");
        return false;
    }

    // Open the socket no matter what the link is doing. A UDP socket does not
    // need a link to be opened, and the PC may only link up later.
    wolUDP.stop();
    if (!wolUDP.begin(WOL_PORT)) {
        Serial.println("[W5500] Could not open UDP socket! Will retry.");
        return false;
    }

    phyForced10M  = false;
    lastLink      = Unknown;
    linkDownSince = millis();
    ethReady      = true;
    Serial.printf("[W5500] Ready! IP: %s — waiting for link\n",
                  Ethernet.localIP().toString().c_str());
    return true;
}

// Call often from loop(). Tracks the link, recovers a stuck PHY and
// re-initialises the W5500 if it lost its configuration (brown-out, reset).
void serviceEthernet() {
    unsigned long now = millis();
    if (now - lastEthPoll < ETH_POLL_MS) return;
    lastEthPoll = now;

    if (!ethReady) {
        if (now - lastEthInit >= LINK_RECOVERY_MS) initEthernet();
        return;
    }

    // The chip forgets its IP when it resets — use that to detect it.
    if (!(Ethernet.localIP() == W5500_IP)) {
        Serial.println("[W5500] Chip lost its config (reset?) — re-initialising");
        initEthernet();
        return;
    }

    EthernetLinkStatus link = Ethernet.linkStatus();
    if (link != lastLink) {
        lastLink      = link;
        linkDownSince = now;
        if (link == LinkON) printLinkUp();
        else                Serial.println("[W5500] Link DOWN");
    }

    // Link stayed down: restart the PHY, alternating between normal
    // auto-negotiation and forced 10 Mbps. A powered-off PC keeps its NIC in a
    // low-power 10 Mbps mode that does not always negotiate cleanly.
    if (link != LinkON && now - linkDownSince >= LINK_RECOVERY_MS) {
        phyForced10M = !phyForced10M;
        Serial.printf("[W5500] Still no link — restarting PHY (%s)\n",
                      phyForced10M ? "forced 10M half" : "auto-negotiation");
        restartPhy(phyForced10M ? PHY_MODE_10HD : PHY_MODE_AUTO);
        linkDownSince = now;
    }
}

bool waitForLink(unsigned long timeoutMs) {
    unsigned long start = millis();
    while (Ethernet.linkStatus() != LinkON) {
        if (millis() - start >= timeoutMs) return false;
        delay(100);
    }
    return true;
}

// Returns nullptr on success, otherwise a short reason for the failure.
const char* sendMagicPacket() {
    if (!ethReady && !initEthernet()) return "W5500 not responding";

    // Never send without a link: the packet cannot reach the PC and the
    // W5500 may never report the send as finished.
    if (!waitForLink(LINK_WAIT_MS)) {
        Serial.println("[WoL] No Ethernet link to the PC — packet NOT sent");
        return "No Ethernet link to the PC";
    }

    uint8_t packet[102];
    for (int i = 0; i < 6; i++) packet[i] = 0xFF;
    for (int rep = 0; rep < 16; rep++)
        for (int b = 0; b < 6; b++)
            packet[6 + rep * 6 + b] = PC_MAC[b];

    int sent = 0;
    for (int n = 0; n < WOL_REPEAT; n++) {
        if (wolUDP.beginPacket(IPAddress(255, 255, 255, 255), WOL_PORT) &&
            wolUDP.write(packet, sizeof(packet)) == sizeof(packet) &&
            wolUDP.endPacket()) {
            sent++;
        }
        delay(100);
    }

    if (sent == 0) {
        Serial.println("[WoL] W5500 refused the packet — re-initialising");
        initEthernet();
        return "W5500 send failed";
    }

    Serial.printf("[WoL] Magic packet sent via W5500 Ethernet (%d/%d)\n", sent, WOL_REPEAT);
    Serial.printf("[WoL] Target MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        PC_MAC[0], PC_MAC[1], PC_MAC[2], PC_MAC[3], PC_MAC[4], PC_MAC[5]);
    return nullptr;
}

void publishConfirmation(const char* error) {
    StaticJsonDocument<192> doc;
    if (error) {
        doc["status"] = "failed";
        doc["msg"]    = error;
    } else {
        doc["status"] = "sent";
        doc["msg"]    = "Magic packet sent via Ethernet — PC should wake shortly";
    }
    char buffer[192];
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
        publishConfirmation(sendMagicPacket());
    }
}

void connectWiFi() {
    Serial.printf("[WiFi] Connecting to %s", WIFI_SSID);
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\n[WiFi] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
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
    serviceEthernet();
    timeClient.update();
    mqtt.loop();
}