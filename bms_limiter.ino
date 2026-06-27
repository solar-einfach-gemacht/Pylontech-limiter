#include "bms_data.h"

float calculatedCVL = 52.2; 
float calculatedCCL = 50.0;
float calculatedDCL = 50.0; 

bool cellProtectionActive = false;
bool chargeBlockedByTarget = false;

void processHysteresisProtection() {
  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    int activePacks = totalRackData.activeBatteries;
    float maxCellV = totalRackData.maxCellVoltage;
    float sysVolt = totalRackData.totalVoltage;
    float sysSoc = totalRackData.averageSoc;

    // =========================================================================
    // 0. ABSOLUTER FAIL-SAFE (Kabelbruch / Totalausfall)
    // =========================================================================
    if (activePacks == 0) {
      calculatedCCL = 0.0; // Harte Notbremse: Kein Ladestrom!
      calculatedDCL = 0.0; // Harte Notbremse: Kein Entladestrom!
      calculatedCVL = 52.2;
      xSemaphoreGive(bmsMutex);
      return;
    }
    
    // Zielspannung vom Webschieber holen
    float targetVoltage = userSettings.maxVoltageLimit;
    calculatedCVL = targetVoltage;

    // Die echten Limits holen (aus Auto-Detect)
    float baseHardwareCcLimit = totalRackData.rackHardwareCcLimitSum;
    float baseHardwareDcLimit = totalRackData.rackHardwareDcLimitSum;

    // Echte Live-Grenzwerte aus dem BMS (CID92) holen
    float liveBmsCcLimit = totalRackData.rackBmsCcLimitSum;
    float liveBmsDcLimit = totalRackData.rackBmsDcLimitSum;
    
    // Falls 0x92 in der ersten Millisekunde noch leer ist, greifen wir auf Hardware-Limits zurück
    if (liveBmsCcLimit <= 0) liveBmsCcLimit = baseHardwareCcLimit;
    if (liveBmsDcLimit <= 0) liveBmsDcLimit = baseHardwareDcLimit;

    // =========================================================================
    // 1. BACKUP-ZELLSCHUTZ (Hysterese auf Zellebene)
    // =========================================================================
    if (maxCellV >= 3.55) {
      cellProtectionActive = true;
    } else if (maxCellV <= 3.47) {
      cellProtectionActive = false;
    }

    // =========================================================================
    // 2. ZIELSPANNUNGS-BLOCKADE (Amnesie-sicher)
    // =========================================================================
    // Wenn die Zielspannung erreicht wird ODER der SOC voll ist, sperren wir.
    if (sysVolt >= targetVoltage || sysSoc >= 98.0) {
      chargeBlockedByTarget = true;
    }
    
    // ERST WENN der SOC gefallen ist UND die Spannung sich beruhigt hat,
    // geben wir das Laden wieder frei!
    if (sysSoc < 96.0 && sysVolt < (targetVoltage - 0.20)) {
      chargeBlockedByTarget = false;
    }

    // =========================================================================
    // 3. DYNAMISCHE TABELLE (Kurve wandert automatisch mit) - Gesamtspannung
    // =========================================================================
    float distanceToTarget = targetVoltage - sysVolt;
    float pct = 1.0; 

    if (distanceToTarget <= 0.00)      pct = 0.00;
    else if (distanceToTarget <= 0.05) pct = 0.03; 
    else if (distanceToTarget <= 0.10) pct = 0.05;
    else if (distanceToTarget <= 0.20) pct = 0.10; 
    else if (distanceToTarget <= 0.40) pct = 0.30;
    else if (distanceToTarget <= 0.60) pct = 0.60; 
    else                               pct = 1.00;

    // Berechne den Wunsch-Ladestrom aus unserer Abstands-Kurve
    float meineLogikCCL = baseHardwareCcLimit * pct;

    // Schieber-Deckel von der Webseite einbeziehen
    float schieberFaktor = userSettings.maxCurrentPercent / 100.0;
    float globalerSchieberDeckel = baseHardwareCcLimit * schieberFaktor;
    
    meineLogikCCL = min(meineLogikCCL, globalerSchieberDeckel);

    // =========================================================================
    // 3.5 NEUER BAUSTEIN: Lineare Top-Balancing Drosselung (Einzelzelle)
    // =========================================================================
    if (maxCellV >= 3.48 && maxCellV < 3.55) {
        
        // Wir berechnen den Abstand zur roten Linie (3,55 V).
        float abstandZurNotbremse = 3.55 - maxCellV; 
        
        // Multiplikator (Faktor) zwischen 0.0 (bei 3.55) und 1.0 (bei 3.48)
        float faktor = abstandZurNotbremse / 0.07; 
        
        // Errechne das Limit: (2 Ampere * Anzahl Akkus) * Faktor
        float balancingLimit = (activePacks * 2.0) * faktor; 
        
        // Überschreibe den Strom nur, wenn unsere weiche Bremse strenger ist
        if (balancingLimit < meineLogikCCL) {
            meineLogikCCL = balancingLimit;
        }
    }

    // (Die eiserne Notbremse von >= 3.55V sparen wir uns hier, da sie in deinem 
    // "Schritt 1" und "Schritt 4" (cellProtectionActive) bereits perfekt 
    // mit einer Hysterese abgesichert ist!)

    // =========================================================================
    // 4. FINALE ERMITTLUNG (BMS + Hysterese)
    // =========================================================================
    if (cellProtectionActive || chargeBlockedByTarget) {
      calculatedCCL = 0.0; // Blockade aktiv -> Knallhart 0A!
    } else {
      // Der absolut sicherste Wert gewinnt: Pylontech-Live-Wert oder unsere Kurve
      calculatedCCL = min(meineLogikCCL, liveBmsCcLimit);
    }

    // Entladestrom folgt strikt dem BMS-Befehl (0x92) oder dem sicheren Hardware-Limit
    calculatedDCL = min(baseHardwareDcLimit, liveBmsDcLimit); 
    
    xSemaphoreGive(bmsMutex);
  }
}