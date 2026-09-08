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

void setup() {
    Serial.begin(115200);
    pinMode(DE_RE_PIN, OUTPUT);
    digitalWrite(DE_RE_PIN, LOW);
    Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
    
    delay(3000);
    Serial.println(F("\n=========================================="));
    Serial.println(F(" PYLONTECH 0x61 SLAVE-TESTER "));
    Serial.println(F("=========================================="));
}

void loop() {
    // Schleife über Master (0x02) und Slaves 1-3 (0x03, 0x04, 0x05)
    for (uint8_t addr = 0x02; addr <= 0x05; addr++) {
        char devIdHex[3];
        snprintf(devIdHex, sizeof(devIdHex), "%02X", addr);
        String infoStr = String(devIdHex);

        String cmd = buildFrame(addr, 0x61, infoStr);
        
        while(Serial2.available()) { Serial2.read(); }
        
        digitalWrite(DE_RE_PIN, HIGH);
        delay(2);
        Serial2.print(cmd);
        Serial2.flush();
        digitalWrite(DE_RE_PIN, LOW);
        
        Serial.printf("\n--- Frage 0x61 von Adresse 0x%02X ab ---\n", addr);
        
        unsigned long start = millis();
        String res = "";
        while(millis() - start < 150) {
            while(Serial2.available()) {
                res += (char)Serial2.read();
            }
            delay(1);
        }
        
        if (res.length() > 0) {
            Serial.println("Antwort: " + res);
        } else {
            Serial.println("Antwort: [TIMEOUT / KEIN AKKU UNTER DIESER ADRESSE]");
        }
        
        delay(1000); // Kurze Atempause für den Bus
    }
    
    Serial.println(F("\nScan beendet. Nächster Durchlauf in 10 Sekunden..."));
    delay(10000);
}
