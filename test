#include <Arduino.h>

#define RX_PIN      18
#define TX_PIN      17
#define DE_RE_PIN   21

uint16_t calcChecksum(const String& frame) {
    uint32_t sum = 0;
    for (int i = 0; i < (int)frame.length(); i++) sum += (uint8_t)frame[i];
    sum = ~sum; sum %= 0x10000;
    sum += 1;
    return (uint16_t)sum;
}

uint16_t calcLenid(int infoLen) {
    if (infoLen == 0) return 0;
    uint16_t lenid = infoLen;
    uint16_t lsum = (lenid & 0xF) + ((lenid >> 4) & 0xF) + ((lenid >> 8) & 0xF);
    uint16_t lmod = lsum % 16;
    uint16_t linv = (0b1111 - lmod + 1) & 0xF;
    return (linv << 12) | lenid;
}

String buildFrame(uint8_t address, uint8_t cid2, const String& info = "") {
    uint8_t cid1 = 0x46;
    uint16_t lenid = calcLenid(info.length());
    String frame = ""; char buf[32];
    snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%04X", 0x20, address, cid1, cid2, lenid);
    frame += buf; frame += info;
    uint16_t chk = calcChecksum(frame);
    snprintf(buf, sizeof(buf), "%04X", chk);
    return String("~") + frame + buf + "\r";
}

void sendCommandAndPrint(uint8_t cid2) {
    String cmd = buildFrame(0x02, cid2, "02"); 
    
    while(Serial2.available()) { Serial2.read(); }
    
    digitalWrite(DE_RE_PIN, HIGH);
    delay(2);
    Serial2.print(cmd);
    Serial2.flush();
    digitalWrite(DE_RE_PIN, LOW);
    
    Serial.printf("\n--- Sende Kommando 0x%02X ---\n", cid2);
    
    unsigned long start = millis();
    String res = "";
    while(millis() - start < 500) {
        while(Serial2.available()) {
            res += (char)Serial2.read();
        }
        delay(1);
    }
    
    if (res.length() > 0) {
        Serial.print("Antwort: "); 
        Serial.println(res);
    } else {
        Serial.println("Antwort: [KEINE ANTWORT / TIMEOUT / KOMMANDO UNBEKANNT]");
    }
}

void setup() {
    Serial.begin(115200);
    pinMode(DE_RE_PIN, OUTPUT);
    digitalWrite(DE_RE_PIN, LOW);
    Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
    
    delay(3000);
    Serial.println("\n========================================");
    Serial.println("STARTE PYLONTECH 0x6X PROTOKOLL SCAN...");
    Serial.println("========================================");
}

void loop() {
    // Referenz (damit wir sehen, dass die Kommunikation generell steht)
    sendCommandAndPrint(0x42); 
    delay(1000);
    
    // Die neuen, heißen Kandidaten
    sendCommandAndPrint(0x61); // Get System Analog Data
    delay(1000);
    sendCommandAndPrint(0x62); // Get System Alarm Info
    delay(1000);
    sendCommandAndPrint(0x63); // Get System Charge Dischargement Info
    delay(1000);
    
    Serial.println("\nScan-Durchlauf beendet. Warte 10 Sekunden...");
    delay(10000);
}
