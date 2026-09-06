#ifndef BMS_DATA_H
#define BMS_DATA_H

#include <Arduino.h>

struct BmsData {
  bool isConnected = false;
  unsigned long lastUpdate = 0; 
  String modelName = "Unbekannt";
  float designCapacity = 50.0; 
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

  // NEU: Charge/Discharge-Steuerbits aus 0x92 Status-Byte
  bool chargeEnable           = true;
  bool dischargeEnable        = true;
  bool chargeImmediatelySOC05 = false;
  bool chargeImmediatelySOC09 = false;
  bool chargeFullRequest      = false;

  // NEU: Alarme aus 0x44 (pro Pack)
  bool almCellVoltageLow      = false;
  bool almCellVoltageHigh     = false;
  bool almTemperatureLow      = false;
  bool almTemperatureHigh     = false;
  bool almChargeCurrent       = false;
  bool almModuleVoltage       = false;
  bool almDischargeCurrent    = false;
  bool alarmActive            = false; 
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

  // NEU: Rack-weite Aggregation für CAN-Ausgabe
  bool rackChargeEnable    = true;   // AND über alle Packs
  bool rackDischargeEnable = true;   // AND über alle Packs
  bool rackAlarmActive     = false;  // OR über alle Packs
};

struct Settings {
  float maxVoltageLimit = 52.2;
  int maxCurrentPercent = 100;
  int packCount = 2; 
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
