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