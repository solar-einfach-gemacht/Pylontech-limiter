
# 🔋 Pylontech CAN-Bus Limiter (Beta)

**Smarter CAN-Bus Limiter für Pylontech-Akkus. Verhindert das 53,2V-Überspannungsproblem an Wechselrichtern wie Deye, Growatt, Solis etc. durch dynamische Ladespannungs- und Stromregelung. ESP32-basiert. Inklusive linearem Top-Balancing und Einzelzell-Notbremse.**

---

## ⚠️ Wichtiger Hinweis & Haftungsausschluss (BETA)
**Dieses Projekt befindet sich in der offenen Beta-Phase.**
Die Software greift aktiv in die Steuerung von Hochstrom-Akkusystemen ein. Ein Fehler kann zu schweren Schäden an der Hardware, Brandgefahr oder Verlust der Garantie führen. 
* **Nutzung auf eigene Gefahr!** Ich übernehme keinerlei Haftung für Schäden an Wechselrichtern, Akkus, Eigentum oder Personen.
* **Testumgebung:** Das System wurde bisher mit einem Setup aus 3 Pylontech-Akkus (US2000C / US3000C) getestet. Code-Unterstützung bis 16 Akkus ist implementiert, aber ein großer Feldtest steht noch aus.
* **Bitte unter Aufsicht testen!** Wer sich an der Beta beteiligt, sollte sein System in der ersten Zeit genau im Auge behalten (idealerweise über das integrierte Web-Dashboard).

---

## 🛑 Das Problem: Der "Blindflug"
Wechselrichter wie Deye oder Growatt übernehmen blind die vom Pylontech-BMS geforderten Ladespannungen (oft 53,2V). Dies führt in der Praxis regelmäßig zu starken Überspannungen auf Zellebene, Alarmen und im schlimmsten Fall zu aufgeblähten Akkus. 
Weicht man auf die Einstellung "User-defined" (ohne CAN-Kommunikation) aus, verliert man alle wertvollen Sicherheitslogiken des Pylontech-BMS – ein gefährlicher Blindflug.

## 💡 Die Lösung: Der Limiter
Dieses ESP32-Gateway wird zwischen die Pylontech-Akkus und den Wechselrichter geschaltet. Es liest die echten Pylontech-Daten aus, berechnet extrem sichere, sanfte Ladekurven und sendet diese modifiziert an den Wechselrichter (Victron CAN-Protokoll). **Die volle Sicherheitskontrolle des Original-BMS bleibt dabei jederzeit erhalten!**

---

## ✨ Die 4-Stufen Ladelogik

1. **Begrenzung der Gesamtspannung:** Die Ladespannung (CVL) ist per Web-Oberfläche frei wählbar (z.B. auf schonende 52,0V oder 52,2V). Der Ladestrom (CCL) kann von 0-100% gedrosselt werden.
2. **Vorausschauendes Laden (0,9V Hysterese):** Der Limiter bremst rechtzeitig ab. Ab 0,9V unterhalb der Zielspannung wird der Ladestrom pro 0,1V Annäherung automatisch um ca. 11% reduziert.
3. **Lineares Top-Balancing (ab 3,48V):** Sobald die allererste Einzelzelle 3,48V berührt, wird der Ladestrom auf sanfte 2A pro Akku limitiert. Steigt die Zelle weiter Richtung 3,55V, wird der Strom butterweich und stufenlos weiter heruntergeregelt.
4. **Die eiserne Notbremse (bei 3,55V):** Erreicht eine Zelle den kritischen Wert von 3,55V, sendet der Limiter knallhart einen Ladestopp (0 Ampere), bis sich die Zelle auf 3,47V beruhigt hat.

*Zusätzlich vergleicht der Limiter seine errechneten Werte immer mit den Anforderungen des Original-BMS und wählt konsequent den strengeren (kleineren) Wert.*

---

## 🛠️ Hardware & Setup
* **Mikrocontroller:** ESP32
* **Kommunikation:** CAN-Transceiver (Richtung Wechselrichter) & RS485/RS232 (Richtung Pylontech)
* **Setup:** Nach dem Flashen spannt der ESP32 ein eigenes WLAN auf (WiFiManager). Nach Eingabe der Heimnetz-Daten ist das Dashboard über den Browser erreichbar.
* **Schnittstellen:** Integriertes Web-Dashboard + MQTT-Anbindung für Smart Home Systeme.

---

## 🤝 Beta-Tester gesucht!
Du hast einen Deye, Growatt oder ähnlichen Wechselrichter und Pylontech-Akkus? Du bist genervt vom High-Voltage-Alarm? 
Lade den Code herunter, teste ihn (bitte unter Beobachtung) und gib Feedback in den **Issues**! Jeder Erfahrungsbericht hilft, den Code zur finalen Dauerlösung für die Community zu machen.

## ⚙️ Für Entwickler & Tester (Quick Setup)
Das Projekt richtet sich aktuell an erfahrene Nutzer. Eine Schritt-für-Schritt-Anleitung für Anfänger folgt nach der Beta-Phase.
* **Plattform:** ESP32 (Kompiliert mit Arduino IDE)
* **Benötigte Bibliotheken:** `WiFiManager`, `PubSubClient` (Rest sind Standard-ESP32-Libs)
* **Hardware-Pins (RS485/Serial2):** RX = 18, TX = 17, DE/RE = 21
* **CAN-Bus:** Standard TWAI (Pins 15 & 16)
