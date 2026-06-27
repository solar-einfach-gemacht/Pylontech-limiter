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
    // HIER IST DER LEBENSRETTENDE FIX: = {0} löscht den Stack-Müll!
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
    
    // FIX: Die sichere Embedded-Rundung (+ 0.5) ohne math.h!
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
        // FRAME 4: PYLONTECH ALARM STATUS (0x359)
        // =========================================================================
        msg.identifier = 0x359;
        msg.data_length_code = 7;
        msg.data[0] = 0x00; 
        msg.data[1] = 0x00;
        msg.data[2] = 0x00;
        msg.data[3] = 0x00; 
        
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
        msg.data[0] = 0xC0;
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