#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <WiFiManager.h> 
#include <ESPmDNS.h> 
#include <PubSubClient.h> 
#include "bms_data.h"

WebServer server(80);
Preferences prefs;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

extern float calculatedCVL;
extern float calculatedCCL;
extern float calculatedDCL;

char mqtt_server[40] = "192.168.178.X";
char mqtt_port[6] = "1883";
char mqtt_topic[50] = "solar/bms";
char mqtt_user[40] = "";
char mqtt_pass[40] = "";

unsigned long lastMqttSend = 0;

const char PAGE_MAIN[] PROGMEM = R"=====(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="utf-8">
  <title>BMS CAN Gateway</title>
  <style>
    body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Arial, sans-serif; background: #eef2f7; color: #333333; margin: 0; padding: 20px; text-align: center; }
    .container { max-width: 650px; margin: 0 auto; background: #ffffff; padding: 25px; border-radius: 14px; box-shadow: 0 4px 20px rgba(0,0,0,0.05); border: 1px solid #dbe2ec; }
    h1 { color: #003366; margin-bottom: 5px; font-weight: 700; }
    .status-card { background: #e1f5fe; padding: 15px; border-radius: 10px; margin: 18px 0; font-size: 20px; font-weight: bold; color: #0288d1; border: 1px solid #b3e5fc; }
    .grid { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; margin: 18px 0; }
    .card { background: #f8fafc; padding: 14px; border-radius: 8px; text-align: left; border: 1px solid #e2e8f0; }
    .card-title { font-size: 11px; color: #64748b; text-transform: uppercase; font-weight: bold; letter-spacing: 0.5px; }
    .card-value { font-size: 22px; font-weight: bold; margin-top: 5px; color: #1e293b; }
    .slider-box { background: #f0f4f8; border: 1px solid #d9e2ec; padding: 15px; border-radius: 10px; margin: 15px 0; text-align: left; }
    label { font-weight: bold; display: block; margin-bottom: 5px; color: #003366; }
    input[type=range] { width: 100%; margin: 10px 0; accent-color: #0052cc; }
    .btn { background: #0052cc; color: white; border: none; padding: 12px 20px; font-size: 16px; border-radius: 8px; cursor: pointer; width: 100%; margin-top: 10px; font-weight: bold; transition: background 0.2s; }
    .btn:hover { background: #003d99; transition: background 0.2s; }
    .btn-danger { background: #dc2626; margin-top: 25px; font-size: 14px; padding: 8px 15px; }
    .btn-danger:hover { background: #991b1b; }
    .alert { background: #ffebee; color: #c62828; border: 1px solid #ffcdd2; padding: 12px; border-radius: 8px; margin: 15px 0; font-weight: bold; }
    .mqtt-info { font-size: 11px; color: #64748b; text-align: left; margin-top: 20px; background: #f1f5f9; padding: 10px; border-radius: 6px; border: 1px solid #e2e8f0; line-height: 1.4; }
    .pack-section { background: #f8fafc; border: 1px solid #e2e8f0; padding: 15px; border-radius: 10px; margin: 18px 0; text-align: left; }
    .pack-header { font-weight: bold; color: #003366; border-bottom: 2px solid #e2e8f0; padding-bottom: 6px; margin-bottom: 12px; display: flex; justify-content: space-between; font-size: 15px; }
    .cells-grid { display: grid; grid-template-columns: repeat(4, 1fr); gap: 8px; }
    @media(max-width: 480px) { .cells-grid { grid-template-columns: repeat(2, 1fr); } }
    .cell-box { background: #ffffff; border: 1px solid #cbd5e1; padding: 8px; border-radius: 6px; font-size: 13px; text-align: center; box-shadow: 0 1px 3px rgba(0,0,0,0.02); }
    .cell-num { font-size: 10px; color: #64748b; display: block; font-weight: bold; }
    .cell-volt { font-weight: bold; color: #0f172a; }
  </style>
  <script>
    let currentMaxAmpereBase = 50.0;
    function updateSliderText(percentVal) {
      let amps = (currentMaxAmpereBase * (percentVal / 100.0)).toFixed(1);
      document.getElementById('ctxt').innerText = percentVal + " % (" + amps + " A)";
    }

    function updateData() {
      fetch('/api/data').then(res => res.json()).then(data => {
        document.getElementById('volt').innerText = data.volt + " V";
        document.getElementById('current').innerText = data.current + " A";
        document.getElementById('delta').innerText = data.delta + " V";
        document.getElementById('temp').innerText = data.tmax + " °C";
        document.getElementById('mosfet').innerText = data.tmosfet + " °C";
        document.getElementById('soc').innerText = data.soc + " %";
      
        document.getElementById('cvl').innerText = data.cvl + " V";
        document.getElementById('ccl').innerText = data.ccl + " A";
        document.getElementById('dcl').innerText = data.dcl + " A";
        
        if(data.packs && data.packs.length > 0) {
          currentMaxAmpereBase = data.hw_ccl_sum;
        }

        let currentSliderVal = document.getElementsByName('climit')[0].value;
        updateSliderText(currentSliderVal);
        
        if(data.hyst) document.getElementById('warn').style.display = 'block';
        else document.getElementById('warn').style.display = 'none';

        let packsHtml = "";
        data.packs.forEach((pack, index) => {
          packsHtml += `<div class="pack-section">`;
          packsHtml += `<div class="pack-header">`;
          packsHtml += `<span>🔋 ${pack.model} (ID: ${pack.pack_idx})</span>`;
          packsHtml += `<span style="color:#475569;">SOC: ${pack.soc}% | SOH: ${pack.soh}% | Volt: ${pack.v}V</span>`;
          packsHtml += `</div>`;
          packsHtml += `<div style="font-size:12px; color:#475569; margin-bottom:10px; font-weight: 500;">`;
          packsHtml += `🌡️ Min ${pack.tmin}°C / Max ${pack.tmax}°C &nbsp;|&nbsp; 📟 BMS: ${pack.tbms}°C`;
          packsHtml += `</div>`;
          packsHtml += `<div class="cells-grid">`;
          
          pack.cells.forEach((cellV, cIdx) => {
            packsHtml += `<div class="cell-box">`;
            packsHtml += `<span class="cell-num">Zelle ${String(cIdx+1).padStart(2, '0')}</span>`;
            packsHtml += `<span class="cell-volt">${cellV.toFixed(3)} V</span>`;
            packsHtml += `</div>`;
          });
          
          packsHtml += `</div></div>`;
        });
        document.getElementById('packs_container').innerHTML = packsHtml;
      });
    }
    setInterval(updateData, 1500);
    window.onload = updateData;

    function confirmReset() {
      if(confirm("Möchtest du wirklich alle WLAN- und MQTT-Einstellungen löschen und das Gateway neu starten?")) {
        window.location.href = "/factory_reset";
      }
    }
  </script>
</head>
<body>
  <div class="container">
    <h1>BMS CAN Gateway</h1>
    <p style="color:#64748b; margin-top:0; font-size:14px; font-weight: 500;">Pylontech Verbund & Logiksteuerung</p>
    
    <div id="warn" class="alert" style="display: none;">⚠️ ZELLSCHUTZ AKTIV: LADESTROM REDUZIERT/BLOCKIERT!</div>

    <div class="status-card">
      System-SOC: <span id="soc">-- %</span>
    </div>

    <div class="grid">
      <div class="card"><div class="card-title">Gesamtspannung</div><div class="card-value" id="volt">-- V</div></div>
      <div class="card"><div class="card-title">Aktueller Strom</div><div class="card-value" id="current">-- A</div></div>
      <div class="card"><div class="card-title">Zell-Delta</div><div class="card-value" id="delta">-- V</div></div>
      <div class="card"><div class="card-title">Max. Zell-Temp</div><div class="card-value" id="temp">-- °C</div></div>
      <div class="card" style="grid-column: span 2; background: #f1f5f9; border: 1px solid #cbd5e1;">
        <div class="card-title" style="color: #475569;">BMS Platine (MOSFET Höchstwert)</div>
        <div class="card-value" id="mosfet" style="color: #0f172a;">-- °C</div>
      </div>
    </div>

    <div id="packs_container"></div>

    <h3 style="text-align: left; margin-top: 25px; color: #003366;">Victron CAN-Ausgabe (Live)</h3>
    <div class="grid" style="grid-template-columns: 1fr 1fr 1fr;">
      <div class="card" style="background:#f0fdf4; border: 1px solid #bbf7d0;"><div class="card-title" style="color:#166534;">CVL (Spannung)</div><div class="card-value" id="cvl" style="color:#14532d;">-- V</div></div>
      <div class="card" style="background:#f0fdf4; border: 1px solid #bbf7d0;"><div class="card-title" style="color:#166534;">CCL (Ladestrom)</div><div class="card-value" id="ccl" style="color:#14532d;">-- A</div></div>
      <div class="card" style="background:#f0fdf4; border: 1px solid #bbf7d0;"><div class="card-title" style="color:#166534;">DCL (Entladen)</div><div class="card-value" id="dcl" style="color:#14532d;">-- A</div></div>
    </div>

    <form action="/save" method="POST">
      <div class="slider-box">
        <label>Maximale Ladespannung (CVL)</label>
        <input type="range" name="vlimit" min="51.8" max="52.4" step="0.1" value="%VLIMIT%" oninput="document.getElementById('vtxt').innerText = this.value + ' V'">
        <div style="text-align:right; font-weight:bold; color: #0052cc;" id="vtxt">%VLIMIT% V</div>
      </div>

      <div class="slider-box">
        <label>Ladestrom Drosselung (CCL)</label>
        <input type="range" name="climit" min="10" max="100" step="5" value="%CLIMIT%" oninput="updateSliderText(this.value)">
        <div style="text-align:right; font-weight:bold; color: #0052cc;" id="ctxt">%CLIMIT% %</div>
      </div>
      
      <button type="submit" class="btn">Einstellungen speichern</button>
    </form>

    <div class="mqtt-info">
      <strong>MQTT-Status:</strong> Broker: %MQTT_SERVER%:%MQTT_PORT% | Topic: %MQTT_TOPIC%<br>
      <span>Adresse im Heimnetz: <a href="http://limiter.local" style="color:#0052cc; font-weight: bold; text-decoration: none;">http://limiter.local</a></span>
    </div>

    <button onclick="confirmReset()" class="btn btn-danger">⚙️ WLAN & MQTT zurücksetzen (Reset)</button>
  </div>
</body>
</html>
)=====";

String buildDataJson() {
  String json = "{";
  if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(30)) == pdTRUE) {
    json += "\"soc\":" + String(totalRackData.averageSoc, 1) + ",";
    json += "\"volt\":" + String(totalRackData.totalVoltage, 2) + ",";
    json += "\"current\":" + String(totalRackData.totalCurrent, 2) + ",";
    json += "\"delta\":" + String(totalRackData.maxCellVoltage - totalRackData.minCellVoltage, 3) + ",";
    json += "\"tmax\":" + String(totalRackData.tempMax, 1) + ",";
    json += "\"tmosfet\":" + String(totalRackData.bmsMosfetTempMax, 1) + ",";
    json += "\"cvl\":" + String(calculatedCVL, 1) + ",";
    json += "\"ccl\":" + String(calculatedCCL, 1) + ",";
    json += "\"dcl\":" + String(calculatedDCL, 1) + ",";
    json += "\"hw_ccl_sum\":" + String(totalRackData.rackHardwareCcLimitSum, 1) + ",";
    json += "\"hyst\":" + String((cellProtectionActive || chargeBlockedByTarget) ? 1 : 0) + ",";
    
    json += "\"packs\":[";
    bool firstPack = true;
    for (int b = 0; b < 16; b++) {
      if (bmsRack[b].isConnected) {
        if (!firstPack) json += ",";
        firstPack = false;
        
        json += "{";
        json += "\"pack_idx\":" + String(b + 1) + ",";
        json += "\"model\":\"" + bmsRack[b].modelName + "\",";
        json += "\"soc\":" + String(bmsRack[b].soc, 1) + ",";
        json += "\"soh\":" + String(bmsRack[b].soh, 1) + ",";
        json += "\"v\":" + String(bmsRack[b].totalVoltage, 2) + ",";
        json += "\"tmin\":" + String(bmsRack[b].tempMin, 1) + ",";
        json += "\"tmax\":" + String(bmsRack[b].tempMax, 1) + ",";
        json += "\"tbms\":" + String(bmsRack[b].bmsMosfetTemp, 1) + ",";
        
        json += "\"cells\":[";
        for (int c = 0; c < bmsRack[b].cellCount; c++) {
          if (c > 0) json += ",";
          json += String(bmsRack[b].cellVoltages[c], 3);
        }
        json += "]";
        json += "}";
      }
    }
    json += "]";
    xSemaphoreGive(bmsMutex);
  }
  json += "}";
  return json;
}

void handleApiData() {
  server.send(200, "application/json", buildDataJson());
}

void handleRoot() {
  String html = String(PAGE_MAIN);
  html.replace("%VLIMIT%", String(userSettings.maxVoltageLimit, 1));
  html.replace("%CLIMIT%", String(userSettings.maxCurrentPercent));
  html.replace("%MQTT_SERVER%", String(mqtt_server));
  html.replace("%MQTT_PORT%", String(mqtt_port));
  html.replace("%MQTT_TOPIC%", String(mqtt_topic));
  server.send(200, "text/html", html);
}

void handleSave() {
  if (server.hasArg("vlimit") && server.hasArg("climit")) {
    float vlim = server.arg("vlimit").toFloat();
    int clim = server.arg("climit").toInt();
    if (vlim < 51.8) vlim = 51.8;
    if (vlim > 52.4) vlim = 52.4;
    if (xSemaphoreTake(bmsMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      userSettings.maxVoltageLimit = vlim;
      userSettings.maxCurrentPercent = clim;
      xSemaphoreGive(bmsMutex);
    }
    prefs.begin("gateway", false);
    prefs.putFloat("vlimit", vlim);
    prefs.putInt("climit", clim);
    prefs.end();
    server.sendHeader("Location", "/");
    server.send(303);
  } else {
    server.send(400, "text/plain", "Bad Request");
  }
}

void handleFactoryReset() {
  server.send(200, "text/plain", "WLAN-Daten werden geloescht. Das Gateway startet jetzt neu...");
  delay(1500);
  WiFiManager wm;
  wm.resetSettings(); 
  prefs.begin("gateway", false);
  prefs.clear(); 
  prefs.end();
  ESP.restart();
}

void reconnectMqtt() {
  if (!mqttClient.connected()) {
    Serial.print(F("MQTT: Versuche Verbindung mit Broker ["));
    Serial.print(mqtt_server);
    Serial.print(F("]... "));
    String clientId = "BmsGatewayS3-";
    clientId += String(random(0xffff), HEX);
    
    bool status = false;
    if (strlen(mqtt_user) > 0) {
      status = mqttClient.connect(clientId.c_str(), mqtt_user, mqtt_pass);
    } else {
      status = mqttClient.connect(clientId.c_str());
    }

    if (status) {
      Serial.println(F("ERFOLGREICH verbunden!"));
    } else {
      Serial.print(F("FEHLGESCHLAGEN, Code = "));
      Serial.println(mqttClient.state());
    }
  }
}

void initCommunication() {
  prefs.begin("gateway", true);
  userSettings.maxVoltageLimit = prefs.getFloat("vlimit", 52.2);
  if(userSettings.maxVoltageLimit < 51.8 || userSettings.maxVoltageLimit > 52.4) {
    userSettings.maxVoltageLimit = 52.2;
  }
  userSettings.maxCurrentPercent = prefs.getInt("climit", 100);
  
  // Lese die Akku-Anzahl aus dem Speicher (Standard: 2)
  userSettings.packCount = prefs.getInt("packs", 2);

  String s_srv = prefs.getString("mq_server", "192.168.178.X");
  String s_prt = prefs.getString("mq_port", "1883");
  String s_tpc = prefs.getString("mq_topic", "solar/bms");
  String s_usr = prefs.getString("mq_user", "");
  String s_pas = prefs.getString("mq_pass", "");
  
  strcpy(mqtt_server, s_srv.c_str());
  strcpy(mqtt_port, s_prt.c_str());
  strcpy(mqtt_topic, s_tpc.c_str());
  strcpy(mqtt_user, s_usr.c_str());
  strcpy(mqtt_pass, s_pas.c_str());
  prefs.end();

  char pack_str[4];
  snprintf(pack_str, 4, "%d", userSettings.packCount);

  WiFiManager wm;
  WiFiManagerParameter custom_packs("packs", "Anzahl Akkus (1-16)", pack_str, 3);
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Broker IP", mqtt_server, 40);
  WiFiManagerParameter custom_mqtt_port("port", "MQTT Port", mqtt_port, 6);
  WiFiManagerParameter custom_mqtt_topic("topic", "MQTT Main Topic", mqtt_topic, 50);
  WiFiManagerParameter custom_mqtt_user("user", "MQTT Username (Optional)", mqtt_user, 40);
  WiFiManagerParameter custom_mqtt_pass("pass", "MQTT Passwort (Optional)", mqtt_pass, 40, "type='password'");
  
  wm.addParameter(&custom_packs);
  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_mqtt_port);
  wm.addParameter(&custom_mqtt_topic);
  wm.addParameter(&custom_mqtt_user);
  wm.addParameter(&custom_mqtt_pass);
  
  Serial.println(F("Starte WiFiManager AutoConnect..."));
  if(!wm.autoConnect("BMS-Gateway-Setup")) {
    delay(3000);
    ESP.restart();
  }

  Serial.println(F("\nWLAN erfolgreich verbunden!"));
  if (MDNS.begin("limiter")) {
    MDNS.addService("http", "tcp", 80);
  }

  // Akku-Anzahl speichern und auf Limits prüfen
  int inputPacks = atoi(custom_packs.getValue());
  if (inputPacks < 1) inputPacks = 1;
  if (inputPacks > 16) inputPacks = 16;
  userSettings.packCount = inputPacks;

  prefs.begin("gateway", false);
  prefs.putInt("packs", userSettings.packCount);
  prefs.putString("mq_server", custom_mqtt_server.getValue());
  prefs.putString("mq_port", custom_mqtt_port.getValue());
  prefs.putString("mq_topic", custom_mqtt_topic.getValue());
  prefs.putString("mq_user", custom_mqtt_user.getValue());
  prefs.putString("mq_pass", custom_mqtt_pass.getValue());
  prefs.end();

  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(mqtt_port, custom_mqtt_port.getValue());
  strcpy(mqtt_topic, custom_mqtt_topic.getValue());
  strcpy(mqtt_user, custom_mqtt_user.getValue());
  strcpy(mqtt_pass, custom_mqtt_pass.getValue());

  int portInt = atoi(mqtt_port);
  if(portInt <= 0) portInt = 1883;
  
  mqttClient.setServer(mqtt_server, portInt);
  mqttClient.setBufferSize(1500); 

  server.on("/", handleRoot);
  server.on("/api/data", handleApiData);
  server.on("/save", handleSave);
  server.on("/factory_reset", handleFactoryReset); 
  server.begin();
}

void processCommunication() {
  server.handleClient();
  mqttClient.loop();

  if (millis() - lastMqttSend > 3000) {
    lastMqttSend = millis();
    if (WiFi.status() == WL_CONNECTED) {
      if (!mqttClient.connected()) {
        reconnectMqtt();
      }
      if (mqttClient.connected()) {
        String payload = buildDataJson();
        String fullTopic = String(mqtt_topic) + "/json";
        mqttClient.publish(fullTopic.c_str(), payload.c_str());
      }
    }
  }
}