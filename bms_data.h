#ifndef BMS_DATA_H
#define BMS_DATA_H

#include <Arduino.h>

struct BmsData {
  bool isConnected = false;
  unsigned long lastUpdate = 0; // NEU: Zeitstempel für den Watchdog
  String modelName = "Unbekannt";
  float designCapacity = 50.0; // Standard für US2000
  int cellCount = 15;
  float cellVoltages[16] = {0.0};
  float tempMin = 0.0;
  float tempMax = 0.0;
  float bmsMosfetTemp = 0.0;
  float totalVoltage = 0.0;
  float totalCurrent = 0.0;
  float soc = 0.0;
  float soh = 100.0;
  float hardwareCcLimit = 25.0;
  float hardwareDcLimit = 25.0;
  float bmsCcLimit = 25.0;
  float bmsDcLimit = 25.0;
};

struct RackTotal {
  int activeBatteries = 0;
  float totalVoltage = 0.0;
  float totalCurrent = 0.0;
  float averageSoc = 0.0;
  float averageSoh = 100.0;
  float maxCellVoltage = 0.0;
  float minCellVoltage = 5.0;
  float tempMax = 0.0;
  float tempMin = 0.0;
  float bmsMosfetTempMax = 0.0;
  float rackHardwareCcLimitSum = 0.0;
  float rackHardwareDcLimitSum = 0.0;
  float rackBmsCcLimitSum = 0.0;
  float rackBmsDcLimitSum = 0.0;
};

struct Settings {
  float maxVoltageLimit = 52.2;
  int maxCurrentPercent = 100;
  int packCount = 2; // NEU: Anzahl der angeschlossenen Akkus
};

#ifdef MAIN_PROGRAM
  SemaphoreHandle_t bmsMutex = NULL;
  BmsData bmsRack[16];
  RackTotal totalRackData;
  Settings userSettings;
#else
  extern SemaphoreHandle_t bmsMutex;
  extern BmsData bmsRack[16];
  extern RackTotal totalRackData;
  extern Settings userSettings;
#endif

extern bool cellProtectionActive;
extern bool chargeBlockedByTarget;

#endif