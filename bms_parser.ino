#include "bms_data.h"

// Ein Alarm gilt erst, wenn ihn dasselbe Pack in so vielen Antworten HINTEREINANDER meldet.
#ifndef ALARM_CONFIRM_COUNT
#define ALARM_CONFIRM_COUNT 3
#endif

long fhemSubstrHex(const String& res, int start, int len) {
  if (start < 0 || start + len > (int)res.length()) return 0;
  String sub = res.substring(start, start + len);
  return strtol(sub.c_str(), NULL, 16);
}

// Max. 1 Log-Zeile pro Sekunde, damit ein dauerhaft schlechter Pack die Konsole nicht flutet
static void logReject(const char* cmd, int bmsIdx, const char* why) {
  static unsigned long lastLog = 0;
  if (millis() - lastLog < 1000) return;
  lastLog = millis();
  Serial.printf("[BMS] Pack %d %s: Frame verworfen (%s)\n", bmsIdx + 1, cmd, why);
}

// =========================================================================
// PROTOKOLL-CHECK nach Pylontech RS485 V3.3
// Frame: ~ VER(2) ADR(2) CID1(2) RTN(2) LENGTH(4) INFO CHKSUM(4) \r
//        0  1-2   3-4    5-6     7-8     9-12     13..   ..
// LENGTH = LCHKSUM(1 Zeichen) + LENID(3 Zeichen). LENID zählt bereits die
// ASCII-ZEICHEN der INFO (nicht Bytes) -> NICHT mit 2 multiplizieren!
// Schneidet Müll vor '~' / nach '\r' ab; res enthält danach nur den Frame.
// =========================================================================
bool validatePylonFrame(String& res, uint8_t expectedAdr, int& dataStart, int& infoChars, const char*& why) {
  int s = res.lastIndexOf('~');
  if (s < 0) { why = "kein Startzeichen"; return false; }
  int e = res.indexOf('\r', s);
  if (e < 0) { why = "kein Endzeichen (Timeout/abgeschnitten)"; return false; }
  res = res.substring(s, e);

  int len = res.length();
  if (len < 18) { why = "zu kurz"; return false; }
  for (int i = 1; i < len; i++) {
    char c = res[i];
    if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) { why = "ungueltiges Zeichen"; return false; }
  }

  if (fhemSubstrHex(res, 3, 2) != expectedAdr) { why = "falsche Pack-Adresse"; return false; }
  if (fhemSubstrHex(res, 5, 2) != 0x46)        { why = "CID1 != 46"; return false; }
  if (fhemSubstrHex(res, 7, 2) != 0x00)        { why = "RTN != 00 (Fehlercode vom Akku)"; return false; }

  long lenField = fhemSubstrHex(res, 9, 4);
  infoChars = (int)(lenField & 0x0FFF);
  if (calcLenid(infoChars) != (uint16_t)lenField) { why = "LENGTH-Pruefsumme falsch"; return false; }
  if (len != 13 + infoChars + 4)                  { why = "Laenge passt nicht zum LENID"; return false; }

  String body = res.substring(1, len - 4);
  if (calcChecksum(body) != (uint16_t)fhemSubstrHex(res, len - 4, 4)) { why = "Checksumme falsch"; return false; }

  dataStart = 13;
  return true;
}

// 0. MANUFACTURER INFO (CID51) - INFO = Name(10) + Version(2) + Hersteller(20), ohne Command-Byte
bool parseManufacturerInfo(uint8_t adr, String res) {
  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  int dataStart, infoChars; const char* why = "";
  if (!validatePylonFrame(res, adr, dataStart, infoChars, why)) { logReject("0x51", bmsIdx, why); return false; }
  if (infoChars < 60 || infoChars > 80) return false;

  String deviceName = "";
  for (int i = 0; i < 20; i += 2) {
    char c = (char)fhemSubstrHex(res, dataStart + i, 2);
    if (c >= 32 && c <= 126) deviceName += c;
  }
  deviceName.trim();
  if (deviceName.length() < 3) return false;

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
// INFO = DATAFLAG(1) CMD(1) M(1) Zellen(2*M) N(1) Temp(2*N) Strom(2) Spannung(2) Rest(2)
//        UserDef(1) Total(2) Zyklen(2) [+ Rest2(3) Total2(3) bei >65Ah]
bool parseAnalogData(uint8_t adr, String res) {
  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  int dataStart, infoChars; const char* why = "";
  if (!validatePylonFrame(res, adr, dataStart, infoChars, why)) { logReject("0x42", bmsIdx, why); return false; }

  int pos = dataStart + 4;                          // DATAFLAG(2) + Command(2) überspringen
  long cellCount = fhemSubstrHex(res, pos, 2); pos += 2;
  if (cellCount < 10 || cellCount > 16) { logReject("0x42", bmsIdx, "Zellanzahl unplausibel"); return false; }

  long tempCount = fhemSubstrHex(res, pos + cellCount * 4, 2);
  if (tempCount < 1 || tempCount > 8) { logReject("0x42", bmsIdx, "Temperaturanzahl unplausibel"); return false; }

  // Erwartete Mindestlänge aus den Zählfeldern -> verhindert z.B. eine kurze 0x44-Antwort als Messwerte
  int need = 2 * (15 + 2 * (int)cellCount + 2 * (int)tempCount);
  if (infoChars < need) { logReject("0x42", bmsIdx, "Laenge passt nicht zu Messwerten"); return false; }

  // ---- alles erst in lokale Variablen lesen ----
  float tempCells[16] = {0};
  for (int i = 0; i < cellCount; i++) {
    float v = fhemSubstrHex(res, pos, 4) / 1000.0; pos += 4;
    if (v < 2.0 || v > 4.5) {
      logReject("0x42", bmsIdx, "Zellspannung unplausibel");
      return false;
    }
    tempCells[i] = v;
  }
  pos += 2; // Temperaturanzahl

  float maxT = -100.0, minT = 100.0, mosfetT = 25.0;
  int validTemps = 0;
  for (int i = 0; i < tempCount; i++) {
    long tRaw = fhemSubstrHex(res, pos, 4); pos += 4;
    if (tRaw & 0x8000) tRaw = tRaw - 0x10000;
    float tVal = (tRaw - 2731) / 10.0;
    if (tVal < -50.0 || tVal > 150.0) continue;      // defekten/ungenutzten Sensor ignorieren
    validTemps++;
    if (i == 0) {
      mosfetT = tVal;
    } else {
      if (tVal > maxT) maxT = tVal;
      if (tVal < minT) minT = tVal;
    }
  }
  if (validTemps == 0) { logReject("0x42", bmsIdx, "keine plausible Temperatur"); return false; }
  if (maxT < -90.0) { maxT = mosfetT; minT = mosfetT; }

  short sCurrent = (short)fhemSubstrHex(res, pos, 4); pos += 4;
  float tCurrent = sCurrent / 10.0;

  float tVoltage = fhemSubstrHex(res, pos, 4) / 1000.0; pos += 4;
  if (tVoltage < cellCount * 2.0 || tVoltage > cellCount * 4.5) { logReject("0x42", bmsIdx, "Gesamtspannung unplausibel"); return false; }

  long remainCapRaw = fhemSubstrHex(res, pos, 4); pos += 4;
  pos += 2; // User-defined count
  long totalCapRaw = fhemSubstrHex(res, pos, 4); pos += 4;
  pos += 4; // Zyklenzahl

  if (totalCapRaw == 65535 && (pos + 12 <= dataStart + infoChars)) {   // >65Ah: US3000/US5000
    remainCapRaw = fhemSubstrHex(res, pos, 6); pos += 6;
    totalCapRaw  = fhemSubstrHex(res, pos, 6); pos += 6;
  }

  // ---- Frame ist sauber -> unter Mutex ins Live-System ----
  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    bmsRack[bmsIdx].isConnected = true;
    bmsRack[bmsIdx].lastUpdate = millis();

    bmsRack[bmsIdx].cellCount = cellCount;
    for (int i = 0; i < cellCount; i++) bmsRack[bmsIdx].cellVoltages[i] = tempCells[i];

    bmsRack[bmsIdx].bmsMosfetTemp = mosfetT;
    bmsRack[bmsIdx].tempMin = minT;
    bmsRack[bmsIdx].tempMax = maxT;
    bmsRack[bmsIdx].totalCurrent = tCurrent;
    bmsRack[bmsIdx].totalVoltage = tVoltage;

    if (totalCapRaw > 0) {
      float reportedAh = totalCapRaw / 1000.0;

      // Kapazitätsklasse -> Limits. Den echten Modellnamen (0x51) NICHT überschreiben.
      if (reportedAh > 10.0 && reportedAh <= 56.0) {
        bmsRack[bmsIdx].designCapacity = 50.0;
        bmsRack[bmsIdx].hardwareCcLimit = 25.0; bmsRack[bmsIdx].hardwareDcLimit = 25.0;
        if (bmsRack[bmsIdx].bmsCcLimit == 25.0) { bmsRack[bmsIdx].bmsCcLimit = 25.0; bmsRack[bmsIdx].bmsDcLimit = 25.0; }
      }
      else if (reportedAh > 56.0 && reportedAh <= 85.0) {
        bmsRack[bmsIdx].designCapacity = 74.0;
        bmsRack[bmsIdx].hardwareCcLimit = 37.0; bmsRack[bmsIdx].hardwareDcLimit = 37.0;
        if (bmsRack[bmsIdx].bmsCcLimit == 25.0) { bmsRack[bmsIdx].bmsCcLimit = 37.0; bmsRack[bmsIdx].bmsDcLimit = 37.0; }
      }
      else if (reportedAh > 85.0) {
        bmsRack[bmsIdx].designCapacity = 100.0;
        bmsRack[bmsIdx].hardwareCcLimit = 80.0; bmsRack[bmsIdx].hardwareDcLimit = 80.0;
        if (bmsRack[bmsIdx].bmsCcLimit == 25.0) { bmsRack[bmsIdx].bmsCcLimit = 80.0; bmsRack[bmsIdx].bmsDcLimit = 80.0; }
      }

      // Gegen SOC-Flackern: nur rechnen, wenn keine nativen 0x61-Daten vorliegen
      if (!bmsRack[bmsIdx].hasNativeSoh) {
        bmsRack[bmsIdx].soc = ((float)remainCapRaw / (float)totalCapRaw) * 100.0;
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
// INFO = Command(1) LadeSpg(2) EntladeSpg(2) Ladestrom(2) Entladestrom(2) Status(1) = 20 Zeichen
bool parseChargeManagement(uint8_t adr, String res) {
  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  int dataStart, infoChars; const char* why = "";
  if (!validatePylonFrame(res, adr, dataStart, infoChars, why)) { logReject("0x92", bmsIdx, why); return false; }
  if (infoChars < 20 || infoChars > 40) { logReject("0x92", bmsIdx, "Laenge passt nicht zu 0x92"); return false; }

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
// INFO = DATAFLAG(1) CMD(1) M(1) ZellAlarm(M) N(1) TempAlarm(N) Lade(1) ModulSpg(1) Entlade(1) Status1-5(5)
bool parseAlarmInfo(uint8_t adr, String res) {
  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  int dataStart, infoChars; const char* why = "";
  if (!validatePylonFrame(res, adr, dataStart, infoChars, why)) { logReject("0x44", bmsIdx, why); return false; }

  int pos = dataStart + 4;                          // DATAFLAG(2) + Command(2) überspringen
  long cellCount = fhemSubstrHex(res, pos, 2); pos += 2;
  if (cellCount < 1 || cellCount > 16) { logReject("0x44", bmsIdx, "Zellanzahl unplausibel"); return false; }

  long tempCount = fhemSubstrHex(res, pos + cellCount * 2, 2);
  if (tempCount < 1 || tempCount > 8) { logReject("0x44", bmsIdx, "Temperaturanzahl unplausibel"); return false; }

  // Erwartete Länge aus den Zählfeldern: nicht kürzer, und nur wenig länger (Messwert-Antworten sind viel länger)
  int need = 2 * (12 + (int)cellCount + (int)tempCount);
  if (infoChars < need || infoChars > need + 8) { logReject("0x44", bmsIdx, "Laenge passt nicht zu Alarmdaten"); return false; }

  bool almCellLow = false, almCellHigh = false, alarmActive = false;
  for (int i = 0; i < cellCount; i++) {
    long v = fhemSubstrHex(res, pos, 2); pos += 2;
    if (v == 0x01) almCellLow  = true;
    if (v == 0x02) almCellHigh = true;
    if (v != 0x00) alarmActive = true;
  }
  pos += 2; // Temperaturanzahl

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

  // ---- ENTPRELLEN: Alarm erst nach ALARM_CONFIRM_COUNT gültigen Antworten in Folge melden ----
  static uint8_t alarmStreak[16] = {0};
  if (alarmActive) {
    if (alarmStreak[bmsIdx] < 255) alarmStreak[bmsIdx]++;
    static unsigned long lastAlarmLog = 0;
    if (millis() - lastAlarmLog > 1000) {          // Rohframe zur Ursachenforschung
      lastAlarmLog = millis();
      Serial.printf("[%lu][ALARM] Pack %d (%d/%d): ", millis(), bmsIdx + 1, alarmStreak[bmsIdx], ALARM_CONFIRM_COUNT);
      Serial.println(res);
    }
  } else {
    alarmStreak[bmsIdx] = 0;
  }
  bool confirmed = (alarmStreak[bmsIdx] >= ALARM_CONFIRM_COUNT);

  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    bmsRack[bmsIdx].almCellVoltageLow   = confirmed && almCellLow;
    bmsRack[bmsIdx].almCellVoltageHigh  = confirmed && almCellHigh;
    bmsRack[bmsIdx].almTemperatureLow   = confirmed && almTempLow;
    bmsRack[bmsIdx].almTemperatureHigh  = confirmed && almTempHigh;
    bmsRack[bmsIdx].almChargeCurrent    = confirmed && almChargeCurrent;
    bmsRack[bmsIdx].almModuleVoltage    = confirmed && almModuleVoltage;
    bmsRack[bmsIdx].almDischargeCurrent = confirmed && almDischargeCurrent;
    bmsRack[bmsIdx].alarmActive         = confirmed;
    xSemaphoreGive(bmsMutex);
    return true;
  }
  return false;
}

// 4. NATIVEN SOC & SOH PARSEN (CID61) - nicht Teil der Pylontech-Spezifikation V3.3, Offsets unverändert
bool parseSystemAnalogData(uint8_t adr, String res) {
  int bmsIdx = adr - 2;
  if (bmsIdx < 0 || bmsIdx >= 16) return false;

  int dataStart, infoChars; const char* why = "";
  if (!validatePylonFrame(res, adr, dataStart, infoChars, why)) { logReject("0x61", bmsIdx, why); return false; }
  if (infoChars < 20) return false;

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
      // Watchdog: Pack ohne gültigen Frame seit 10 s gilt als offline
      if (bmsRack[i].isConnected && (millis() - bmsRack[i].lastUpdate > 10000)) {
        bmsRack[i].isConnected = false;
      }

      if (bmsRack[i].isConnected) {
        count++;
        vSum += bmsRack[i].totalVoltage; cSum += bmsRack[i].totalCurrent;
        socSum += bmsRack[i].soc;

        // sohSum nur addieren, wenn der Master (Pack 1) nicht den Rack-SOH übernimmt
        if (!(i == 0 && bmsRack[i].hasNativeSoh)) {
           sohSum += bmsRack[i].soh;
        }

        if (bmsRack[i].tempMax > tMax) tMax = bmsRack[i].tempMax;
        if (bmsRack[i].tempMin < tMin) tMin = bmsRack[i].tempMin;
        if (bmsRack[i].bmsMosfetTemp > bmsTMax) bmsTMax = bmsRack[i].bmsMosfetTemp;

        for (int c = 0; c < bmsRack[i].cellCount; c++) {
          float cv = bmsRack[i].cellVoltages[c];
          if (cv < 2.0 || cv > 4.5) continue;   // Sicherheitsnetz
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

      if (bmsRack[0].hasNativeSoh && bmsRack[0].isConnected) {
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
