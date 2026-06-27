#define MAIN_PROGRAM
#include "bms_data.h"
#include <WiFi.h>

#define RX_PIN      18
#define TX_PIN      17
#define DE_RE_PIN   21

extern float calculatedCVL;
extern float calculatedCCL;
extern float calculatedDCL;
extern void processHysteresisProtection();

extern void initCanBus();
extern void sendVictronCanFrames();
extern bool parseManufacturerInfo(uint8_t adr, String res);
extern bool parseAnalogData(uint8_t adr, String res);
extern bool parseChargeManagement(uint8_t adr, String res);
extern void calculateRackTotals();

extern void initCommunication();
extern void processCommunication();

// =========================================
// FRAME BUILDER FÜR BELIEBIGE ADRESSEN
// =========================================
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

void sendBmsCommandRaw(String cmd) {
  while(Serial2.available()) { Serial2.read(); } 
  digitalWrite(DE_RE_PIN, HIGH);
  delay(2);
  Serial2.print(cmd);
  Serial2.flush();
  digitalWrite(DE_RE_PIN, LOW);
}

String readBmsResponse() {
    String res = "";
    unsigned long start = millis();
    while(millis() - start < 120) { 
      while(Serial2.available()) { res += (char)Serial2.read(); } 
      vTaskDelay(pdMS_TO_TICKS(1)); 
    }
    return res;
}

// =========================================
// HAUPT-TASKS
// =========================================
void TaskBatteryLoop(void * pvParameters) {
  pinMode(DE_RE_PIN, OUTPUT);
  digitalWrite(DE_RE_PIN, LOW);
  Serial2.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
  unsigned long lastDiagOut = 0;

  for(;;) {
    // Sichere die Anzahl der Durchläufe (1 bis max 16)
    int maxPacks = userSettings.packCount;
    if (maxPacks < 1) maxPacks = 1;
    if (maxPacks > 16) maxPacks = 16;
    
    // Zielgerichtete Schleife NUR für die konfigurierten Akkus
    for(int i = 0; i < maxPacks; i++) {
        uint8_t adr = 2 + i; // Adressen starten bei 0x02
        char devIdHex[3];
        snprintf(devIdHex, sizeof(devIdHex), "%02X", adr);
        String infoStr = String(devIdHex);

        // 1. Modell abfragen (Einmalig)
        if (bmsRack[i].modelName == "Unbekannt") {
            sendBmsCommandRaw(buildFrame(adr, 0x51, infoStr));
            parseManufacturerInfo(adr, readBmsResponse());
            vTaskDelay(pdMS_TO_TICKS(40));
        }

        // 2. Sensordaten abfragen (Immer)
        sendBmsCommandRaw(buildFrame(adr, 0x42, infoStr));
        parseAnalogData(adr, readBmsResponse());
        vTaskDelay(pdMS_TO_TICKS(40));

        // 3. Management Info abfragen (Immer)
        sendBmsCommandRaw(buildFrame(adr, 0x92, infoStr));
        parseChargeManagement(adr, readBmsResponse());
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    // ==========================================
    // PROZESSIERUNG & CAN AN VICTRON
    // ==========================================
    calculateRackTotals();
    processHysteresisProtection();  
    sendVictronCanFrames();         
    
    // ==========================================
    // DIAGNOSE AUSGABE
    // ==========================================
    if (millis() - lastDiagOut > 4000) { 
      lastDiagOut = millis();
      if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        Serial.println(F("\n====================================================================="));
        Serial.println(F("                    RACK MASTER LIVE DETAIL-DIAGNOSE                  "));
        Serial.println(F("====================================================================="));
        Serial.printf("Packs konfiguriert: %d | Packs online: %d | System-SOC: %.1f %%\n", maxPacks, totalRackData.activeBatteries, totalRackData.averageSoc);
        Serial.printf("Spannung: %.2f V | Gesamtstrom: %.2f A\n", totalRackData.totalVoltage, totalRackData.totalCurrent);
        Serial.printf("Zell-Max: %.3f V | Zell-Min: %.3f V | Delta: %.3f V\n", totalRackData.maxCellVoltage, totalRackData.minCellVoltage, (totalRackData.maxCellVoltage - totalRackData.minCellVoltage));
        Serial.printf("Hardware-Basis    -> CCL-Max: %.1f A | DCL-Max: %.1f A\n", totalRackData.rackHardwareCcLimitSum, totalRackData.rackHardwareDcLimitSum);
        Serial.printf("Pylontech (0x92)  -> CCL-Live: %.1f A | DCL-Live: %.1f A\n", totalRackData.rackBmsCcLimitSum, totalRackData.rackBmsDcLimitSum);
        Serial.println(F("---------------------------------------------------------------------"));
        Serial.printf("CAN OUT AN VICTRON-> CVL: %.1fV | CCL (Geregelt): %.1fA | DCL: %.1fA\n", calculatedCVL, calculatedCCL, calculatedDCL);
        Serial.println(F("=================================================================\n"));
        xSemaphoreGive(bmsMutex);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(150)); 
  }
}

void TaskCommLoop(void * pvParameters) {
  initCommunication();
  for(;;) { 
    processCommunication(); 
    vTaskDelay(pdMS_TO_TICKS(5)); 
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println(F("=== SYSTEM START ==="));
  bmsMutex = xSemaphoreCreateMutex();
  if (bmsMutex == NULL) { while(1); }
  initCanBus();
  xTaskCreatePinnedToCore(TaskBatteryLoop, "Task_Battery", 16384, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskCommLoop, "Task_Comm", 8192, NULL, 3, NULL, 0);
}

void loop() { 
  vTaskDelete(NULL);
}