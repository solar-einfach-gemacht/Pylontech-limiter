#include "driver/twai.h"
#include "bms_data.h"

extern float calculatedCVL;
extern float calculatedCCL;
extern float calculatedDCL;

void initCanBus() {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)15, (gpio_num_t)16, TWAI_MODE_NORMAL);
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS(); 
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        twai_start();
        Serial.println(F("CAN-Bus (TWAI) erfolgreich auf 500k im Pylontech-Modus gestartet."));
    } else {
        Serial.println(F("KRITISCHER FEHLER: CAN-Bus Setup failed!"));
    }
}

void sendVictronCanFrames() {
    twai_message_t msg = {0};
    msg.extd = 0; 
    msg.rtr = 0;
    
    float validTmax = totalRackData.tempMax;
    float validTmin = totalRackData.tempMin;
    if (totalRackData.activeBatteries == 0 || totalRackData.minCellVoltage < 2.0) {
        validTmax = 22.0;
        validTmin = 21.0;
    }
    
    // =========================================================================
    // FRAME 1: Systemgrenzen (0x351) -> Little-Endian
    // =========================================================================
    msg.identifier = 0x351;
    msg.data_length_code = 8;
    
    uint16_t cvl_out = (uint16_t)((calculatedCVL * 10.0) + 0.5); 
    int16_t  ccl_out = (int16_t)((calculatedCCL * 10.0) + 0.5);
    int16_t  dcl_out = (int16_t)((calculatedDCL * 10.0) + 0.5); 
    
    msg.data[0] = lowByte(cvl_out); msg.data[1] = highByte(cvl_out);
    msg.data[2] = lowByte(ccl_out); msg.data[3] = highByte(ccl_out);
    msg.data[4] = lowByte(dcl_out); msg.data[5] = highByte(dcl_out);
    msg.data[6] = 0; msg.data[7] = 0;
    twai_transmit(&msg, pdMS_TO_TICKS(5));

    if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        
        // =========================================================================
        // FRAME 2: SOC und SOH (0x355) -> Little-Endian
        // =========================================================================
        msg.identifier = 0x355;
        msg.data_length_code = 4;
        
        float currentSoc = totalRackData.averageSoc;
        if (totalRackData.activeBatteries == 0 || currentSoc <= 0.0) {
            currentSoc = 50.0;
        }
        
        uint16_t soc_can = (uint16_t)(currentSoc + 0.5);
        uint16_t soh_can = (uint16_t)(totalRackData.averageSoh + 0.5); 
        
        msg.data[0] = lowByte(soc_can); msg.data[1] = highByte(soc_can);
        msg.data[2] = lowByte(soh_can); msg.data[3] = highByte(soh_can);
        twai_transmit(&msg, pdMS_TO_TICKS(5));

        // =========================================================================
        // FRAME 3: Messwerte & Temperaturen (0x356) -> Little-Endian
        // =========================================================================
        msg.identifier = 0x356;
        msg.data_length_code = 8;
        
        float currentVolt = totalRackData.totalVoltage;
        if (totalRackData.activeBatteries == 0 || currentVolt < 40.0) {
            currentVolt = 49.8;
        }
        
        uint16_t sysV = (uint16_t)((currentVolt * 100.0) + 0.5);
        int16_t  sysC = (int16_t)((totalRackData.totalCurrent * 10.0) + 0.5); 
        int16_t  tmax_can = (int16_t)((validTmax * 10.0) + 0.5);
        int16_t  tmin_can = (int16_t)((validTmin * 10.0) + 0.5);
        
        msg.data[0] = lowByte(sysV);     msg.data[1] = highByte(sysV);
        msg.data[2] = lowByte(sysC);     msg.data[3] = highByte(sysC);
        msg.data[4] = lowByte(tmax_can); msg.data[5] = highByte(tmax_can);
        msg.data[6] = lowByte(tmin_can); msg.data[7] = highByte(tmin_can);
        twai_transmit(&msg, pdMS_TO_TICKS(5));

        // =========================================================================
        // FRAME 4: PYLONTECH ALARM STATUS (0x359) - V1.3 Protocol (Protection Bytes)
        // =========================================================================
        msg.identifier = 0x359;
        msg.data_length_code = 7;
        
        uint8_t protection1 = 0;  // Byte 0: Harte Schutzabschaltung Teil 1
        uint8_t protection2 = 0;  // Byte 1: Harte Schutzabschaltung Teil 2

        for (int i = 0; i < 16; i++) {
            if (bmsRack[i].isConnected) {
                if (bmsRack[i].almCellVoltageHigh)  protection1 |= 0x08; // Bit 3: Over Voltage
                if (bmsRack[i].almCellVoltageLow)   protection1 |= 0x10; // Bit 4: Under Voltage
                if (bmsRack[i].almTemperatureHigh)  protection1 |= 0x20; // Bit 5: Over Temperature
                if (bmsRack[i].almTemperatureLow)   protection1 |= 0x40; // Bit 6: Under Temperature
                if (bmsRack[i].almDischargeCurrent) protection1 |= 0x80; // Bit 7: Discharge Overcurrent
                
                if (bmsRack[i].almChargeCurrent)    protection2 |= 0x40; // Bit 6: Charge Overcurrent
                if (bmsRack[i].almModuleVoltage)    protection2 |= 0x08; // Bit 3: Generischer Modul-Fehler
            }
        }

        // ABSOLUTER FAIL-SAFE: Wenn gar kein Akku antwortet, Systemfehler an Wechselrichter funken
        if (totalRackData.activeBatteries == 0) {
            protection2 |= 0x80; // Bit 7: System error
        }

        msg.data[0] = protection1;
        msg.data[1] = protection2;
        msg.data[2] = 0x00;  // Alarm/Warning (leer gelassen, Protection reicht)
        msg.data[3] = 0x00;  // Alarm/Warning (leer gelassen)
        
        int packs = totalRackData.activeBatteries;
        if (packs <= 0) packs = 2; 
        msg.data[4] = (uint8_t)packs;
        msg.data[5] = 'P';
        msg.data[6] = 'N';
        twai_transmit(&msg, pdMS_TO_TICKS(5));

        // =========================================================================
        // FRAME 5: PYLONTECH WATCHDOG / CHARGE ENABLE (0x35C)
        // =========================================================================
        msg.identifier = 0x35C;
        msg.data_length_code = 2;
        
        uint8_t flags = 0x00;
        
        // 1. Basis-Erlaubnis vom ECHTEN Pylontech BMS holen
        if (totalRackData.rackChargeEnable)    flags |= 0x80; // Bit 7 (Laden erlaubt)
        if (totalRackData.rackDischargeEnable) flags |= 0x40; // Bit 6 (Entladen erlaubt)
        
        // 2. Deine eigene Limiter-Hysterese überschreibt das BMS!
        if (cellProtectionActive || chargeBlockedByTarget || totalRackData.activeBatteries == 0) {
            flags &= ~0x80; // Löscht Bit 7 (Laden verboten!)
        }
        
        msg.data[0] = flags;
        msg.data[1] = 0x00;
        twai_transmit(&msg, pdMS_TO_TICKS(5));

        // =========================================================================
        // FRAME 6: PYLON Herstellername (0x35E)
        // =========================================================================
        msg.identifier = 0x35E;
        msg.data_length_code = 8;
        uint8_t nameData[8] = {'P', 'Y', 'L', 'O', 'N', ' ', ' ', ' '};
        memcpy(msg.data, nameData, 8);
        twai_transmit(&msg, pdMS_TO_TICKS(5));
        
        xSemaphoreGive(bmsMutex);
    }
}
