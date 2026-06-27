#include "bms_data.h"

long fhemSubstrHex(String res, int start, int len) {
  if (start + len > res.length()) return 0;
  String sub = res.substring(start, start + len);
  return strtol(sub.c_str(), NULL, 16);
}

// 0. MANUFACTURER INFO (CID51)
bool parseManufacturerInfo(uint8_t adr, String res) {
  int basePos = res.indexOf("4600");
  if (res.length() < 30 || basePos == -1) return false;

  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  int dataStart = basePos + 8;
  if (dataStart + 20 > res.length()) return false;

  String deviceName = "";
  for (int i = 0; i < 20; i += 2) {
    char c = (char)fhemSubstrHex(res, dataStart + i, 2);
    if (c >= 32 && c <= 126) deviceName += c;
  }
  deviceName.trim();

  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    if (bmsRack[bmsIdx].modelName == "Unbekannt") {
      bmsRack[bmsIdx].modelName = deviceName;
    }
    xSemaphoreGive(bmsMutex);
    return true;
  }
  return false;
}

// 1. SENSORDATEN PARSEN (CID42) 
bool parseAnalogData(uint8_t adr, String res) {
  if (res.length() < 40) return false;
  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    bmsRack[bmsIdx].isConnected = true; 
    bmsRack[bmsIdx].lastUpdate = millis(); 
    
    int pos = 17; 
    long cellCount = fhemSubstrHex(res, pos, 2); pos += 2;
    if (cellCount < 10 || cellCount > 16) { xSemaphoreGive(bmsMutex); return false; }
    bmsRack[bmsIdx].cellCount = cellCount;

    for (int i = 0; i < cellCount; i++) {
      bmsRack[bmsIdx].cellVoltages[i] = fhemSubstrHex(res, pos, 4) / 1000.0; pos += 4;
    }
    
    long tempCount = fhemSubstrHex(res, pos, 2); pos += 2;
    float maxT = -100.0, minT = 100.0;

    for (int i = 0; i < tempCount; i++) {
      long tRaw = fhemSubstrHex(res, pos, 4); pos += 4;
      if (tRaw & 0x8000) tRaw = tRaw - 0x10000;
      float tVal = (tRaw - 2731) / 10.0;

      if (i == 0) {
        if (tVal > -40.0 && tVal < 120.0) bmsRack[bmsIdx].bmsMosfetTemp = tVal;
        else bmsRack[bmsIdx].bmsMosfetTemp = 25.0;
      } else {
        if (tVal > maxT) maxT = tVal;
        if (tVal < minT) minT = tVal;
      }
    }
    
    if (tempCount <= 1) { maxT = bmsRack[bmsIdx].bmsMosfetTemp; minT = bmsRack[bmsIdx].bmsMosfetTemp; }
    bmsRack[bmsIdx].tempMin = minT; bmsRack[bmsIdx].tempMax = maxT;

    short sCurrent = (short)fhemSubstrHex(res, pos, 4); pos += 4;
    bmsRack[bmsIdx].totalCurrent = sCurrent / 10.0;
    
    bmsRack[bmsIdx].totalVoltage = fhemSubstrHex(res, pos, 4) / 1000.0; pos += 4;
    long remainCapRaw = fhemSubstrHex(res, pos, 4); pos += 4;
    pos += 2; 
    long totalCapRaw = fhemSubstrHex(res, pos, 4); pos += 4;
    long cycleCount = fhemSubstrHex(res, pos, 4); pos += 4;

    // === NEU: PYLONTECH EXTENDED CAPACITY PROTOKOLL (US3000C / US5000) ===
    // Wenn die Kapazität über 65.5Ah liegt, sendet Pylontech den Platzhalter "FFFF"
    // und hängt die ECHTEN Werte als 3-Byte-Zahlen hinten an den String an!
    if (totalCapRaw == 65535 && (pos + 12 <= res.length())) {
      remainCapRaw = fhemSubstrHex(res, pos, 6); pos += 6;
      totalCapRaw = fhemSubstrHex(res, pos, 6); pos += 6;
    }
    
    if (totalCapRaw > 0) {
      float reportedAh = totalCapRaw / 1000.0;

      // Auto-Detect Notfall-Limits
      if (reportedAh > 10.0 && reportedAh <= 56.0) {
        bmsRack[bmsIdx].modelName = "US2000 Auto";
        bmsRack[bmsIdx].designCapacity = 50.0;
        bmsRack[bmsIdx].hardwareCcLimit = 25.0; bmsRack[bmsIdx].hardwareDcLimit = 25.0;
        if(bmsRack[bmsIdx].bmsCcLimit == 25.0) { bmsRack[bmsIdx].bmsCcLimit = 25.0; bmsRack[bmsIdx].bmsDcLimit = 25.0; }
      } 
      else if (reportedAh > 56.0 && reportedAh <= 85.0) {
        bmsRack[bmsIdx].modelName = "US3000 Auto";
        bmsRack[bmsIdx].designCapacity = 74.0;
        bmsRack[bmsIdx].hardwareCcLimit = 37.0; bmsRack[bmsIdx].hardwareDcLimit = 37.0;
        if(bmsRack[bmsIdx].bmsCcLimit == 25.0) { bmsRack[bmsIdx].bmsCcLimit = 37.0; bmsRack[bmsIdx].bmsDcLimit = 37.0; }
      } 
      else if (reportedAh > 85.0) {
        bmsRack[bmsIdx].modelName = "US5000 Auto";
        bmsRack[bmsIdx].designCapacity = 100.0;
        bmsRack[bmsIdx].hardwareCcLimit = 80.0; bmsRack[bmsIdx].hardwareDcLimit = 80.0;
        if(bmsRack[bmsIdx].bmsCcLimit == 25.0) { bmsRack[bmsIdx].bmsCcLimit = 80.0; bmsRack[bmsIdx].bmsDcLimit = 80.0; }
      }

      // Die ECHTE, unzensierte Berechnung!
      bmsRack[bmsIdx].soc = ((float)remainCapRaw / (float)totalCapRaw) * 100.0;
      
      float calcSoh = (reportedAh / bmsRack[bmsIdx].designCapacity) * 100.0;
      bmsRack[bmsIdx].soh = (calcSoh > 100.0) ? 100.0 : calcSoh;
    }

    xSemaphoreGive(bmsMutex);
    return true;
  }
  return false;
}

// 2. LIVE LADEGRENZWERTE PARSEN (CID92)
bool parseChargeManagement(uint8_t adr, String res) {
  int basePos = res.indexOf("4600"); 
  if (basePos == -1) return false;
  
  int dataStart = basePos + 8;
  if (dataStart + 18 > res.length()) return false; 
  int bmsIdx = adr - 2;

  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    long ccLimitRaw = fhemSubstrHex(res, dataStart + 10, 4);
    long dcLimitRaw = fhemSubstrHex(res, dataStart + 14, 4);
    
    float valCC = ccLimitRaw / 10.0;
    float valDC = 0.0;
    if (dcLimitRaw > 0) valDC = (65536 - dcLimitRaw) / 10.0;

    if (valCC >= 0 && valCC <= 200 && valDC >= 0 && valDC <= 200) {
      bmsRack[bmsIdx].bmsCcLimit = valCC; bmsRack[bmsIdx].bmsDcLimit = valDC; 
    } else {
      float normalDC = dcLimitRaw / 10.0;
      if (valCC >= 0 && valCC <= 200 && normalDC >= 0 && normalDC <= 200) {
        bmsRack[bmsIdx].bmsCcLimit = valCC; bmsRack[bmsIdx].bmsDcLimit = normalDC;
      }
    }
    xSemaphoreGive(bmsMutex);
    return true;
  }
  return false;
}

// 3. GESAMT-LOGIK BERECHNEN (RACK TOTALS)
void calculateRackTotals() {
  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    int count = 0;
    float vSum = 0.0, cSum = 0.0, socSum = 0.0, sohSum = 0.0;
    float maxV = 0.0, minV = 5.0, tMax = -100.0, tMin = 100.0, bmsTMax = -100.0;
    float hwCcSum = 0.0, hwDcSum = 0.0, bmsCcSum = 0.0, bmsDcSum = 0.0;

    for (int i = 0; i < 16; i++) {
      if (bmsRack[i].isConnected && (millis() - bmsRack[i].lastUpdate > 10000)) {
        bmsRack[i].isConnected = false;
      }

      if (bmsRack[i].isConnected) {
        count++;
        vSum += bmsRack[i].totalVoltage; cSum += bmsRack[i].totalCurrent;
        socSum += bmsRack[i].soc; sohSum += bmsRack[i].soh;
        
        if (bmsRack[i].tempMax > tMax) tMax = bmsRack[i].tempMax;
        if (bmsRack[i].tempMin < tMin) tMin = bmsRack[i].tempMin;
        if (bmsRack[i].bmsMosfetTemp > bmsTMax) bmsTMax = bmsRack[i].bmsMosfetTemp;
        
        for (int c = 0; c < bmsRack[i].cellCount; c++) {
          float cv = bmsRack[i].cellVoltages[c];
          if (cv > maxV) maxV = cv;
          if (cv < minV) minV = cv;
        }
        hwCcSum  += bmsRack[i].hardwareCcLimit; hwDcSum  += bmsRack[i].hardwareDcLimit;
        bmsCcSum += bmsRack[i].bmsCcLimit; bmsDcSum += bmsRack[i].bmsDcLimit;
      }
    }
    
    totalRackData.activeBatteries = count;
    if (count > 0) {
      totalRackData.totalVoltage = vSum / count; totalRackData.totalCurrent = cSum;
      totalRackData.averageSoc = socSum / count; totalRackData.averageSoh = sohSum / count;
      totalRackData.maxCellVoltage = maxV; totalRackData.minCellVoltage = minV;
      totalRackData.tempMax = tMax; totalRackData.tempMin = tMin;
      totalRackData.bmsMosfetTempMax = bmsTMax;
      totalRackData.rackHardwareCcLimitSum = hwCcSum; totalRackData.rackHardwareDcLimitSum = hwDcSum;
      totalRackData.rackBmsCcLimitSum = bmsCcSum; totalRackData.rackBmsDcLimitSum = bmsDcSum;
    } else {
      totalRackData.minCellVoltage = 0.0; totalRackData.rackHardwareCcLimitSum = 0.0;
      totalRackData.rackHardwareDcLimitSum = 0.0; totalRackData.rackBmsCcLimitSum = 0.0; totalRackData.rackBmsDcLimitSum = 0.0;
    }
    xSemaphoreGive(bmsMutex);
  }
}