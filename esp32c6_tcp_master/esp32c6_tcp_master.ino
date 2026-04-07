/*
 * ===========================================================
 *  ESP32-C6 -- Modbus TCP Master
 *  Reads temperature & humidity from ESP32-S3 TCP Slave
 * ===========================================================
 *
 * Architecture:
 *   [ESP32-C6 TCP Master] --WiFi/TCP--> [ESP32-S3 TCP Slave :502]
 *
 * This device:
 *   1. Connects to the same WiFi network as the ESP32-S3
 *   2. Resolves the slave by mDNS hostname (no static IP needed)
 *   3. Polls the ESP32-S3 TCP slave every 3 seconds
 *   4. Reads holding registers 0..3 via FC03
 *   5. Prints temperature, humidity, status, and poll count
 *
 * ESP32-S3 Slave Holding Register Map (FC03, 0-based):
 *   HREG 0: Temperature (raw x 0.1 C)
 *   HREG 1: Humidity    (raw x 0.1 %RH)
 *   HREG 2: Status      (0=OK, 1=timeout, 2=CRC, 3=exception)
 *   HREG 3: Poll count
 *
 * Library: "modbus-esp8266" by emelianov (install via Library Manager)
 *
 * Board Settings:
 *   Board:       "ESP32C6 Dev Module"
 *   USB CDC:     "Enabled"
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ModbusIP_ESP8266.h>

#include "secrets.h"   /* Provides WIFI_SSID, WIFI_PASS, SLAVE_HOSTNAME */

/* --- Modbus TCP Parameters --- */
#define SLAVE_PORT      502
#define SLAVE_ID        1       /* Unit ID (usually 1 for simple slaves) */
#define HREG_START      0       /* First holding register to read */
#define HREG_COUNT      4       /* Read 4 registers: temp, humi, status, count */

/* --- Timing --- */
#define POLL_INTERVAL_MS         3000
#define RESPONSE_TIMEOUT_MS      4000  /* Hard cap on a single transaction */
#define WIFI_RECONNECT_INTERVAL  5000
#define MDNS_REFRESH_INTERVAL    60000

/* --- Objects --- */
ModbusIP mb;
IPAddress slaveIP;  /* Resolved at runtime via mDNS */

/* --- State --- */
uint16_t result[HREG_COUNT] = {0};
uint16_t transID            = 0;
bool     waitingResp        = false;
unsigned long requestSentAt = 0;
unsigned long lastPoll      = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastMdnsRefresh = 0;
uint32_t okCount            = 0;
uint32_t errCount           = 0;

/* --- Resolve slave by mDNS --- */
bool resolveSlave() {
    Serial.printf("Resolving %s.local via mDNS...\n", SLAVE_HOSTNAME);
    IPAddress ip = MDNS.queryHost(SLAVE_HOSTNAME);
    if (ip == IPAddress(0, 0, 0, 0)) {
        Serial.println("[ERR] mDNS lookup failed");
        return false;
    }
    slaveIP = ip;
    Serial.printf("Slave resolved: %s -> %s\n",
                  SLAVE_HOSTNAME, slaveIP.toString().c_str());
    return true;
}

/* --- WiFi bring-up (re-usable on reconnect) --- */
void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
        if (!MDNS.begin("esp32c6-modbus-master")) {
            Serial.println("[WARN] mDNS start failed");
        }
        resolveSlave();
    } else {
        Serial.println("\n[WARN] WiFi connect failed; will retry in loop()");
    }
}

/* --- Setup --- */
void setup() {
    Serial.begin(115200);
    while (!Serial) delay(10);

    Serial.println();
    Serial.println("===========================================");
    Serial.println(" ESP32-C6 -- Modbus TCP Master");
    Serial.println(" Reads from ESP32-S3 TCP Slave (:502)");
    Serial.println("===========================================");

    connectWiFi();
    mb.client();
}

/* --- Main Loop --- */
void loop() {
    /* Service Modbus stack */
    mb.task();

    /* WiFi watchdog */
    if (millis() - lastWifiCheck >= WIFI_RECONNECT_INTERVAL) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Disconnected, reconnecting...");
            WiFi.disconnect();
            waitingResp = false;
            slaveIP = IPAddress(0, 0, 0, 0);
            connectWiFi();
            return;
        }
    }

    /* Periodically refresh the mDNS resolution in case the slave's IP changed */
    if (millis() - lastMdnsRefresh >= MDNS_REFRESH_INTERVAL) {
        lastMdnsRefresh = millis();
        resolveSlave();
    }

    /* Check if previous transaction completed */
    if (waitingResp && !mb.isTransaction(transID)) {
        waitingResp = false;

        int16_t  tempRaw   = (int16_t)result[0];
        uint16_t humiRaw   = result[1];
        uint16_t status    = result[2];
        uint16_t pollCount = result[3];

        if (status == 0) {
            okCount++;
            Serial.println("+--------------------------------------+");
            Serial.printf("|  Temperature: %7.1f C               |\n", tempRaw / 10.0);
            Serial.printf("|  Humidity:    %7.1f %%RH             |\n", humiRaw / 10.0);
            Serial.printf("|  Slave polls: %-5u  Status: OK      |\n", pollCount);
            Serial.printf("|  [Master OK: %lu  ERR: %lu]\n", okCount, errCount);
            Serial.println("+--------------------------------------+\n");
        } else {
            errCount++;
            Serial.printf("[WARN] Slave reports sensor error (status=%u)\n\n", status);
        }
    }

    /* Hard timeout on a stuck transaction (slave gone offline mid-request) */
    if (waitingResp && (millis() - requestSentAt >= RESPONSE_TIMEOUT_MS)) {
        Serial.println("[ERR] Transaction timed out, dropping connection");
        errCount++;
        waitingResp = false;
        if (mb.isConnected(slaveIP)) {
            mb.disconnect(slaveIP);
        }
    }

    /* Send new request at interval */
    if (!waitingResp && (millis() - lastPoll >= POLL_INTERVAL_MS)) {
        lastPoll = millis();

        if (slaveIP == IPAddress(0, 0, 0, 0)) {
            if (!resolveSlave()) {
                errCount++;
                return;
            }
        }

        if (!mb.isConnected(slaveIP)) {
            Serial.printf("Connecting to slave %s:%d...\n",
                          slaveIP.toString().c_str(), SLAVE_PORT);
            mb.connect(slaveIP, SLAVE_PORT);

            /* Pump the stack until the TCP handshake completes (or we give up) */
            uint32_t handshakeStart = millis();
            while (!mb.isConnected(slaveIP) && millis() - handshakeStart < 2000) {
                mb.task();
                delay(10);
            }
            if (!mb.isConnected(slaveIP)) {
                Serial.println("[ERR] TCP connect failed");
                errCount++;
                return;
            }
        }

        /*
         * readHreg(IP, start_reg, result_array, count)
         * Sends FC03 (Read Holding Registers) to the slave.
         * Returns a transaction ID to track completion.
         */
        transID = mb.readHreg(slaveIP, HREG_START, result, HREG_COUNT);

        if (transID) {
            waitingResp = true;
            requestSentAt = millis();
        } else {
            errCount++;
            Serial.println("[ERR] Failed to send Modbus request\n");
        }
    }
}
