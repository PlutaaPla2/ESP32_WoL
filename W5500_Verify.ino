#include <SPI.h>
#include <Ethernet.h>
// Wiring + link test. Leave it running and watch the link while you plug the
// cable, shut the PC down, sleep it, etc. The link must stay (or come back) UP
// with the PC off — otherwise the magic packet can never reach it.
byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
EthernetLinkStatus last = Unknown;
bool first = true;
void setup() {
    Serial.begin(115200);
    delay(1000);
    Ethernet.init(5);
    Ethernet.begin(mac, IPAddress(192,168,1,200));
    if (Ethernet.hardwareStatus() == EthernetNoHardware)
        Serial.println("W5500 NOT detected — check SPI wiring");
    else
        Serial.println("W5500 detected!");
}
void loop() {
    if (Ethernet.hardwareStatus() == EthernetNoHardware) return;
    EthernetLinkStatus now = Ethernet.linkStatus();
    if (first || now != last) {
        Serial.printf("[%lus] %s\n", millis() / 1000,
                      now == LinkON ? "Cable link UP" : "Cable link DOWN");
        last = now;
        first = false;
    }
    delay(200);
}
