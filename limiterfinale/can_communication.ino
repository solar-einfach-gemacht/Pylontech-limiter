#include "driver/twai.h"
#include "bms_data.h"
#include <math.h>

extern float calculatedCVL;
extern float calculatedCCL;
extern float calculatedDCL;

// -------------------------------------------------------------------------
// Interner Zustand des CAN-Moduls
// -------------------------------------------------------------------------
static bool     canBusRunning = false;   // true, solange der TWAI-Treiber sendefähig ist
static uint32_t canTxErrorCount = 0;     // Zähler für fehlgeschlagene Sendungen
static uint32_t canLastErrorPrintMs = 0; // Rate-Limit für Fehlerausgabe

// -------------------------------------------------------------------------
// Hilfsfunktionen (nur primitive Typen, damit die Arduino-Prototypen passen)
// -------------------------------------------------------------------------

// Float -> int16 mit korrekter Rundung (auch bei negativen Werten) und Begrenzung
static int16_t canToI16(float value, float scale) {
    long r = lroundf(value * scale);
    if (r > 32767)  r = 32767;
    if (r < -32768) r = -32768;
    return (int16_t)r;
}

// Float -> uint16 mit korrekter Rundung und Begrenzung (negative Werte werden 0)
static uint16_t canToU16(float value, float scale) {
    long r = lroundf(value * scale);
    if (r > 65535) r = 65535;
    if (r < 0)     r = 0;
    return (uint16_t)r;
}

// 16 Bit Little-Endian in Datenpuffer schreiben
static void canPutLE16(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)(value & 0xFF);
    dst[1] = (uint8_t)(value >> 8);
}

// Frame senden und Fehler zählen
static void canSend(twai_message_t &msg) {
    esp_err_t res = twai_transmit(&msg, pdMS_TO_TICKS(5));
    if (res != ESP_OK) {
        canTxErrorCount++;
        uint32_t now = millis();
        if (now - canLastErrorPrintMs > 5000) {
            canLastErrorPrintMs = now;
            Serial.print(F("CAN TX Fehler, ID 0x"));
            Serial.print(msg.identifier, HEX);
            Serial.print(F(" Code "));
            Serial.print((int)res);
            Serial.print(F(" Gesamt: "));
            Serial.println(canTxErrorCount);
        }
    }
}

// Bus-Off-Recovery und RX-Queue leeren (wird bei jedem Sendezyklus aufgerufen)
static void canServiceBus() {
    uint32_t alerts = 0;
    if (twai_read_alerts(&alerts, 0) == ESP_OK) {
        if (alerts & TWAI_ALERT_BUS_OFF) {
            Serial.println(F("CAN: Bus-Off erkannt, starte Recovery..."));
            canBusRunning = false;
            twai_initiate_recovery();
        }
        if (alerts & TWAI_ALERT_BUS_RECOVERED) {
            Serial.println(F("CAN: Bus wiederhergestellt, starte Treiber."));
            if (twai_start() == ESP_OK) {
                canBusRunning = true;
            }
        }
    }

    // Empfangene Frames (z.B. Keepalive 0x305 vom Wechselrichter) verwerfen,
    // damit die RX-Queue nicht volllaeuft
    twai_message_t rx;
    while (twai_receive(&rx, 0) == ESP_OK) {
        // nichts zu tun
    }
}

// -------------------------------------------------------------------------
// Initialisierung
// -------------------------------------------------------------------------
void initCanBus() {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)15, (gpio_num_t)16, TWAI_MODE_NORMAL);
    g_config.tx_queue_len = 10;  // wir senden 6 Frames direkt hintereinander
    g_config.rx_queue_len = 10;

    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        if (twai_start() == ESP_OK) {
            canBusRunning = true;
            // Alerts fuer Bus-Off-Erkennung aktivieren
            twai_reconfigure_alerts(TWAI_ALERT_BUS_OFF | TWAI_ALERT_BUS_RECOVERED, NULL);
            Serial.println(F("CAN-Bus (TWAI) erfolgreich auf 500k im Pylontech-Modus gestartet."));
        } else {
            Serial.println(F("KRITISCHER FEHLER: twai_start() fehlgeschlagen!"));
        }
    } else {
        Serial.println(F("KRITISCHER FEHLER: CAN-Bus Setup failed!"));
    }
}

// -------------------------------------------------------------------------
// Senden aller Frames
// -------------------------------------------------------------------------
void sendVictronCanFrames() {
    // Momentaufnahme aller Daten, die wir fuer die Frames brauchen
    struct CanSnapshot {
        float   cvl, ccl, dcl;
        float   tempMax, tempMin;
        float   minCellVoltage;
        float   soc, soh;
        float   voltage, current;
        int     activeBatteries;
        bool    chargeEnable, dischargeEnable;
        bool    cellProtectionActive, chargeBlockedByTarget;
        uint8_t protection1, protection2;
    };

    static CanSnapshot lastSnap;
    static bool        haveSnap = false;
    static uint8_t     mutexFailCount = 0;

    // Bus pruefen / ggf. wiederherstellen
    canServiceBus();
    if (!canBusRunning) return;

    CanSnapshot s;

    // ---------------------------------------------------------------------
    // 1) Daten unter Mutex kopieren, Mutex sofort wieder freigeben
    // ---------------------------------------------------------------------
    if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        s.cvl = calculatedCVL;
        s.ccl = calculatedCCL;
        s.dcl = calculatedDCL;

        s.tempMax         = totalRackData.tempMax;
        s.tempMin         = totalRackData.tempMin;
        s.minCellVoltage  = totalRackData.minCellVoltage;
        s.soc             = totalRackData.averageSoc;
        s.soh             = totalRackData.averageSoh;
        s.voltage         = totalRackData.totalVoltage;
        s.current         = totalRackData.totalCurrent;
        s.activeBatteries = totalRackData.activeBatteries;
        s.chargeEnable    = totalRackData.rackChargeEnable;
        s.dischargeEnable = totalRackData.rackDischargeEnable;

        s.cellProtectionActive   = cellProtectionActive;
        s.chargeBlockedByTarget  = chargeBlockedByTarget;

        // Protection-Bytes aus allen verbundenen Racks zusammenbauen
        s.protection1 = 0;
        s.protection2 = 0;
        for (int i = 0; i < 16; i++) {
            if (!bmsRack[i].isConnected) continue;

            // Byte 0
            if (bmsRack[i].almCellVoltageHigh)  s.protection1 |= 0x02; // Bit 1: Cell/Module Over Voltage
            if (bmsRack[i].almCellVoltageLow)   s.protection1 |= 0x04; // Bit 2: Cell/Module Under Voltage
            if (bmsRack[i].almTemperatureHigh)  s.protection1 |= 0x08; // Bit 3: Cell Over Temperature
            if (bmsRack[i].almTemperatureLow)   s.protection1 |= 0x10; // Bit 4: Cell Under Temperature
            if (bmsRack[i].almDischargeCurrent) s.protection1 |= 0x80; // Bit 7: Discharge Overcurrent

            // Byte 1
            if (bmsRack[i].almChargeCurrent)    s.protection2 |= 0x01; // Bit 0: Charge Overcurrent

            // almModuleVoltage wird bewusst NICHT auf System Error gemappt,
            // um ungewollte Abschaltungen zu vermeiden.
        }

        xSemaphoreGive(bmsMutex);

        lastSnap       = s;
        haveSnap       = true;
        mutexFailCount = 0;
    } else {
        // Mutex nicht bekommen: letzte gueltige Werte weiterverwenden,
        // aber nur kurz. Bei dauerhaftem Ausfall nichts mehr senden,
        // damit der Wechselrichter den Kommunikations-Timeout ausloest.
        if (!haveSnap) return;
        if (mutexFailCount < 255) mutexFailCount++;
        if (mutexFailCount > 20) return;
        s = lastSnap;
    }

    // ---------------------------------------------------------------------
    // 2) Ersatzwerte bei Ausfall (zusammen mit System Error + Ladesperre)
    // ---------------------------------------------------------------------
    float validTmax = s.tempMax;
    float validTmin = s.tempMin;
    if (s.activeBatteries == 0 || s.minCellVoltage < 2.0f) {
        validTmax = 22.0f;
        validTmin = 21.0f;
    }

    float currentSoc = s.soc;
    if (s.activeBatteries == 0 || currentSoc <= 0.0f) {
        currentSoc = 50.0f;
    }

    float currentVolt = s.voltage;
    if (s.activeBatteries == 0 || currentVolt < 40.0f) {
        currentVolt = 49.8f;
    }

    twai_message_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.extd = 0;
    msg.rtr  = 0;

    // =========================================================================
    // FRAME 1: Systemgrenzen (0x351) -> Little-Endian
    // Bytes: CVL (0.1 V), CCL (0.1 A), DCL (0.1 A)
    // =========================================================================
    msg.identifier       = 0x351;
    msg.data_length_code = 6;   // DVL (Byte 6/7) wird nicht gesendet
    memset(msg.data, 0, 8);

    canPutLE16(&msg.data[0], canToU16(s.cvl, 10.0f));
    canPutLE16(&msg.data[2], (uint16_t)canToI16(fmaxf(s.ccl, 0.0f), 10.0f));
    canPutLE16(&msg.data[4], (uint16_t)canToI16(fmaxf(s.dcl, 0.0f), 10.0f));
    canSend(msg);

    // =========================================================================
    // FRAME 2: SOC und SOH (0x355) -> Little-Endian
    // =========================================================================
    msg.identifier       = 0x355;
    msg.data_length_code = 4;
    memset(msg.data, 0, 8);

    canPutLE16(&msg.data[0], canToU16(currentSoc, 1.0f));
    canPutLE16(&msg.data[2], canToU16(s.soh, 1.0f));
    canSend(msg);

    // =========================================================================
    // FRAME 3: Messwerte & Temperaturen (0x356) -> Little-Endian
    // Bytes: Spannung (0.01 V), Strom (0.1 A, signed), Tmax/Tmin (0.1 C, signed)
    // =========================================================================
    msg.identifier       = 0x356;
    msg.data_length_code = 8;
    memset(msg.data, 0, 8);

    canPutLE16(&msg.data[0], canToU16(currentVolt, 100.0f));
    canPutLE16(&msg.data[2], (uint16_t)canToI16(s.current, 10.0f));
    canPutLE16(&msg.data[4], (uint16_t)canToI16(validTmax, 10.0f));
    canPutLE16(&msg.data[6], (uint16_t)canToI16(validTmin, 10.0f));
    canSend(msg);

    // =========================================================================
    // FRAME 4: PYLONTECH ALARM STATUS (0x359) - V1.3 Protocol
    // =========================================================================
    msg.identifier       = 0x359;
    msg.data_length_code = 7;
    memset(msg.data, 0, 8);

    uint8_t protection1 = s.protection1;  // Byte 0: Protection Teil 1
    uint8_t protection2 = s.protection2;  // Byte 1: Protection Teil 2
    uint8_t alarm1 = 0;                   // Byte 2: Warnungen Teil 1 (aktuell ungenutzt)
    uint8_t alarm2 = 0;                   // Byte 3: Warnungen Teil 2 (aktuell ungenutzt)

    // ABSOLUTER FAIL-SAFE: Wenn gar kein Akku antwortet, System Error senden
    if (s.activeBatteries == 0) {
        protection2 |= 0x08; // Byte 1, Bit 3: System Error
    }

    int packs = s.activeBatteries;
    if (packs <= 0) packs = 2; // Fallback, damit der WR keinen Fehler wegen "0 Packs" meldet

    msg.data[0] = protection1;
    msg.data[1] = protection2;
    msg.data[2] = alarm1;
    msg.data[3] = alarm2;
    msg.data[4] = (uint8_t)packs;
    msg.data[5] = 'P'; // 0x50
    msg.data[6] = 'N'; // 0x4E
    canSend(msg);

    // =========================================================================
    // FRAME 5: PYLONTECH CHARGE / DISCHARGE ENABLE (0x35C)
    // =========================================================================
    msg.identifier       = 0x35C;
    msg.data_length_code = 2;
    memset(msg.data, 0, 8);

    uint8_t flags = 0x00;

    // 1. Basis-Erlaubnis vom echten Pylontech-BMS
    if (s.chargeEnable)    flags |= 0x80; // Bit 7: Laden erlaubt
    if (s.dischargeEnable) flags |= 0x40; // Bit 6: Entladen erlaubt

    // 2. Eigene Limiter-Hysterese ueberschreibt das BMS
    if (s.cellProtectionActive || s.chargeBlockedByTarget) {
        flags &= (uint8_t)~0x80; // Laden verboten
    }

    // 3. Kein Akku verfuegbar: Laden UND Entladen sperren
    if (s.activeBatteries == 0) {
        flags &= (uint8_t)~(0x80 | 0x40);
    }

    msg.data[0] = flags;
    msg.data[1] = 0x00;
    canSend(msg);

    // =========================================================================
    // FRAME 6: PYLON Herstellername (0x35E)
    // =========================================================================
    msg.identifier       = 0x35E;
    msg.data_length_code = 8;
    const uint8_t nameData[8] = {'P', 'Y', 'L', 'O', 'N', ' ', ' ', ' '};
    memcpy(msg.data, nameData, 8);
    canSend(msg);
}
