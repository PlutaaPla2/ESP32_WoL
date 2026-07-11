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