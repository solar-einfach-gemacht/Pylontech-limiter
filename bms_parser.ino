#include "bms_data.h"

long fhemSubstrHex(const String& res, int start, int len) {
  if (start + len > (int)res.length()) return 0;
  String sub = res.substring(start, start + len);
  return strtol(sub.c_str(), NULL, 16);
}

// =========================================================================
// ZENTRALER SCHUTZSCHILD: Pylontech-Protokoll-Check
// =========================================================================
bool validatePylonFrame(const String& res, uint8_t expectedAdr, int& dataStart, int& infoChars) {
  // Sucht CID1=46 und RTN=00 (Normal). Blockiert Kommunikationsfehler (z.B. RTN=91) sofort.
  int basePos = res.indexOf("4600"); 
  if (basePos < 2) return false; 

  // 1. ADRESS-CHECK: Hat der richtige Akku geantwortet?
  long respAdr = fhemSubstrHex(res, basePos - 2, 2);
  if (respAdr != expectedAdr) return false;

  // 2. LÄNGEN-EXTRAKTION (LENID aus dem Header)
  long lenId = fhemSubstrHex(res, basePos + 5, 3); // LCHKSUM (1 Zeichen) überspringen, LENID (3 Zeichen) lesen
  infoChars = lenId * 2; // LENID ist in Bytes, wir brauchen Hex-Zeichen (2 pro Byte)
  dataStart = basePos + 8; // Datenblock beginnt 8 Zeichen nach "4600"

  // 3. FRAME-GRÖSSEN-CHECK: Ist der String lang genug für die versprochenen Daten?
  if (dataStart + infoChars > (int)res.length()) return false;

  return true;
}

// 0. MANUFACTURER INFO (CID51)
bool parseManufacturerInfo(uint8_t adr, String res) {
  int dataStart, infoChars;
  if (!validatePylonFrame(res, adr, dataStart, infoChars)) return false;
  
  if (infoChars < 40) return false; // Mindestens 20 Bytes für den Namen nötig

  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  String deviceName = "";
  for (int i = 0; i < 20; i += 2) {
    char c = (char)fhemSubstrHex(res, dataStart + 2 + i, 2); // +2 überspringt den Command Value
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
  int dataStart, infoChars;
  if (!validatePylonFrame(res, adr, dataStart, infoChars)) return false;
  
  // LÄNGEN-TYP-PRÜFUNG: 0x42 (Messwerte) ist extrem lang (>90 Hex-Zeichen). 
  // Das verhindert zuverlässig das fehlerhafte Einlesen einer kurzen 0x44 Alarm-Antwort!
  if (infoChars < 90) return false; 

  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    bmsRack[bmsIdx].isConnected = true; 
    bmsRack[bmsIdx].lastUpdate = millis(); 
    
    int pos = dataStart + 2; // +2 überspringt den Command Value
    long cellCount = fhemSubstrHex(res, pos, 2); pos += 2;
    if (cellCount < 10 || cellCount > 16) { xSemaphoreGive(bmsMutex); return false; }
    bmsRack[bmsIdx].cellCount = cellCount;

    for (int i = 0; i < cellCount; i++) {
      float parsedVoltage = fhemSubstrHex(res, pos, 4) / 1000.0; 
      pos += 4;
      
      // Plausibilitätsprüfung
      if (parsedVoltage < 2.0 || parsedVoltage > 4.5) {
        xSemaphoreGive(bmsMutex);
        return false; 
      }
      bmsRack[bmsIdx].cellVoltages[i] = parsedVoltage;
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

    if (totalCapRaw == 65535 && (pos + 12 <= dataStart + infoChars)) {
      remainCapRaw = fhemSubstrHex(res, pos, 6); pos += 6;
      totalCapRaw = fhemSubstrHex(res, pos, 6); pos += 6;
    }
    
    if (totalCapRaw > 0) {
      float reportedAh = totalCapRaw / 1000.0;

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

      bmsRack[bmsIdx].soc = ((float)remainCapRaw / (float)totalCapRaw) * 100.0;
      
      if (!bmsRack[bmsIdx].hasNativeSoh) {
        float calcSoh = (reportedAh / bmsRack[bmsIdx].designCapacity) * 100.0;
        bmsRack[bmsIdx].soh = (calcSoh > 100.0) ? 100.0 : calcSoh;
      }
    }

    xSemaphoreGive(bmsMutex);
    return true;
  }
  return false;
}

// 2. LIVE LADEGRENZWERTE PARSEN (CID92)
bool parseChargeManagement(uint8_t adr, String res) {
  int dataStart, infoChars;
  if (!validatePylonFrame(res, adr, dataStart, infoChars)) return false;
  
  // LÄNGEN-TYP-PRÜFUNG
  if (infoChars < 20 || infoChars > 60) return false; 

  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

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

    uint8_t statusByte = (uint8_t) fhemSubstrHex(res, dataStart + 18, 2);
    bmsRack[bmsIdx].chargeEnable           = statusByte & 0x80; 
    bmsRack[bmsIdx].dischargeEnable        = statusByte & 0x40; 
    bmsRack[bmsIdx].chargeImmediatelySOC05 = statusByte & 0x20; 
    bmsRack[bmsIdx].chargeImmediatelySOC09 = statusByte & 0x10; 
    bmsRack[bmsIdx].chargeFullRequest      = statusByte & 0x08; 

    xSemaphoreGive(bmsMutex);
    return true;
  }
  return false;
}

// 3. ALARM INFO PARSEN (CID44)
bool parseAlarmInfo(uint8_t adr, String res) {
  int dataStart, infoChars;
  if (!validatePylonFrame(res, adr, dataStart, infoChars)) return false;
  
  // LÄNGEN-TYP-PRÜFUNG: 0x44 (Alarme) ist recht kurz (~50-68 Hex-Zeichen). 
  // Das verhindert zuverlässig das fehlerhafte Einlesen einer langen 0x42 Messwert-Antwort!
  if (infoChars > 80) return false; 

  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;
  
  int pos = dataStart + 2; // +2 überspringt den Command Value                                             
  long cellCount = fhemSubstrHex(res, pos, 2); pos += 2;
  if (cellCount < 1 || cellCount > 16) return false;
  if (pos + (int)(cellCount * 2) + 2 > dataStart + infoChars) return false;
  
  bool almCellLow = false, almCellHigh = false, alarmActive = false;
  for (int i = 0; i < cellCount; i++) {
    long v = fhemSubstrHex(res, pos, 2); pos += 2;
    if (v == 0x01) almCellLow  = true;
    if (v == 0x02) almCellHigh = true;
    if (v != 0x00) alarmActive = true;
  }
  
  long tempCount = fhemSubstrHex(res, pos, 2); pos += 2;
  if (tempCount < 0 || tempCount > 8) return false;
  if (pos + (int)(tempCount * 2) + 6 > dataStart + infoChars) return false;
  
  bool almTempLow = false, almTempHigh = false;
  for (int i = 0; i < tempCount; i++) {
    long v = fhemSubstrHex(res, pos, 2); pos += 2;
    if (v == 0x01) almTempLow  = true;
    if (v == 0x02) almTempHigh = true;
    if (v != 0x00) alarmActive = true;
  }
  
  long chargeCurrentAlm    = fhemSubstrHex(res, pos, 2); pos += 2;
  long moduleVoltageAlm    = fhemSubstrHex(res, pos, 2); pos += 2;
  long dischargeCurrentAlm = fhemSubstrHex(res, pos, 2); pos += 2;
  
  bool almChargeCurrent    = (chargeCurrentAlm    != 0x00);
  bool almModuleVoltage    = (moduleVoltageAlm    != 0x00);
  bool almDischargeCurrent = (dischargeCurrentAlm != 0x00);
  
  if (almChargeCurrent || almModuleVoltage || almDischargeCurrent) alarmActive = true;

  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    bmsRack[bmsIdx].almCellVoltageLow   = almCellLow;
    bmsRack[bmsIdx].almCellVoltageHigh  = almCellHigh;
    bmsRack[bmsIdx].almTemperatureLow   = almTempLow;
    bmsRack[bmsIdx].almTemperatureHigh  = almTempHigh;
    bmsRack[bmsIdx].almChargeCurrent    = almChargeCurrent;
    bmsRack[bmsIdx].almModuleVoltage    = almModuleVoltage;
    bmsRack[bmsIdx].almDischargeCurrent = almDischargeCurrent;
    bmsRack[bmsIdx].alarmActive         = alarmActive;
    xSemaphoreGive(bmsMutex);
    return true;
  }
  return false;
}

// 4. NATIVEN SOC & SOH PARSEN (CID61)
bool parseSystemAnalogData(uint8_t adr, String res) {
  int dataStart, infoChars;
  if (!validatePylonFrame(res, adr, dataStart, infoChars)) return false;
  
  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    long socRaw = fhemSubstrHex(res, dataStart + 8, 2);
    long sohRaw = fhemSubstrHex(res, dataStart + 18, 2);
    
    if (socRaw >= 0 && socRaw <= 100) {
        bmsRack[bmsIdx].soc = (float)socRaw;
    }
    if (sohRaw > 0 && sohRaw <= 100) {
        bmsRack[bmsIdx].soh = (float)sohRaw;
        bmsRack[bmsIdx].hasNativeSoh = true; 
    }

    xSemaphoreGive(bmsMutex);
    return true;
  }
  return false;
}

// 5. GESAMT-LOGIK BERECHNEN (RACK TOTALS)
void calculateRackTotals() {
  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    int count = 0;
    float vSum = 0.0, cSum = 0.0, socSum = 0.0, sohSum = 0.0;
    float maxV = 0.0, minV = 5.0, tMax = -100.0, tMin = 100.0, bmsTMax = -100.0;
    float hwCcSum = 0.0, hwDcSum = 0.0, bmsCcSum = 0.0, bmsDcSum = 0.0;
    
    bool rcEnable = true, rdEnable = true, rAlarm = false;

    for (int i = 0; i < 16; i++) {
      if (bmsRack[i].isConnected && (millis() - bmsRack[i].lastUpdate > 10000)) {
        bmsRack[i].isConnected = false;
      }

      if (bmsRack[i].isConnected) {
        count++;
        vSum += bmsRack[i].totalVoltage; cSum += bmsRack[i].totalCurrent;
        socSum += bmsRack[i].soc; 
        
        if (i == 0 && bmsRack[i].hasNativeSoh) {
           sohSum = bmsRack[i].soh * userSettings.packCount; 
        } else {
           sohSum += bmsRack[i].soh;
        }
        
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

        if (!bmsRack[i].chargeEnable)    rcEnable = false;
        if (!bmsRack[i].dischargeEnable) rdEnable = false;
        if (bmsRack[i].alarmActive)      rAlarm   = true;
      }
    }
    
    totalRackData.activeBatteries = count;
    if (count > 0) {
      totalRackData.totalVoltage = vSum / count; totalRackData.totalCurrent = cSum;
      totalRackData.averageSoc = socSum / count; 
      
      if (bmsRack[0].hasNativeSoh) {
         totalRackData.averageSoh = bmsRack[0].soh; 
      } else {
         totalRackData.averageSoh = sohSum / count;
      }
      
      totalRackData.maxCellVoltage = maxV; totalRackData.minCellVoltage = minV;
      totalRackData.tempMax = tMax; totalRackData.tempMin = tMin;
      totalRackData.bmsMosfetTempMax = bmsTMax;
      totalRackData.rackHardwareCcLimitSum = hwCcSum; totalRackData.rackHardwareDcLimitSum = hwDcSum;
      totalRackData.rackBmsCcLimitSum = bmsCcSum; totalRackData.rackBmsDcLimitSum = bmsDcSum;
      
      totalRackData.rackChargeEnable    = rcEnable;
      totalRackData.rackDischargeEnable = rdEnable;
      totalRackData.rackAlarmActive     = rAlarm;
    } else {
      totalRackData.minCellVoltage = 0.0; totalRackData.rackHardwareCcLimitSum = 0.0;
      totalRackData.rackHardwareDcLimitSum = 0.0; totalRackData.rackBmsCcLimitSum = 0.0; totalRackData.rackBmsDcLimitSum = 0.0;
      
      totalRackData.rackChargeEnable    = false;   
      totalRackData.rackDischargeEnable = false;
      totalRackData.rackAlarmActive     = true;    
    }
    xSemaphoreGive(bmsMutex);
  }
}
