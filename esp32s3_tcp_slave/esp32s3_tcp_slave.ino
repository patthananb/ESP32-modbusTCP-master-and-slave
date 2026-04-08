/*
 * ===========================================================
 *  Waveshare ESP32-S3-Relay-6CH
 *  Modbus RTU Master (XY-MD02) + Modbus TCP Slave
 *  + Web UI for configurable holding-register map
 * ===========================================================
 *
 * Architecture:
 *   [XY-MD02 @0x01] --RS485--> [ESP32-S3] <--WiFi/TCP-- [Any TCP Master]
 *                    FC04        RTU Master               TCP Slave :502
 *                                   |
 *                                   +-- HTTP :80  (config web UI)
 *
 * The register map is no longer hard-coded. On boot the device loads a
 * JSON map from NVS (Preferences) describing which holding registers
 * exist, what data source feeds them, what numeric type, and what scale
 * factor. Users edit the map in a browser at:
 *
 *     http://esp32s3-modbus.local/
 *
 * Saving in the UI persists the new map and soft-reboots the device.
 *
 * Data sources currently exposed (see SRC_NAMES):
 *   temp_c       float, degrees C from XY-MD02
 *   humidity     float, %RH from XY-MD02
 *   status       float, last RTU read status (0=OK,1=timeout,2=CRC,3=exc)
 *   poll_count   float, RTU poll counter
 *   uptime_s     float, seconds since boot
 *   wifi_rssi    float, dBm
 *   free_heap    float, bytes
 *
 * Numeric types:
 *   uint16, int16   -- 1 register
 *   uint32, int32   -- 2 consecutive registers, big-endian word order
 *   float32         -- 2 consecutive registers, IEEE 754, big-endian words
 *
 * SECURITY: Modbus TCP and the config web UI both have NO authentication.
 * Keep this device on a trusted LAN/VLAN; do not expose to the public
 * internet.
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <ModbusIP_ESP8266.h>
#include <HardwareSerial.h>

#include "secrets.h"
#include "regmap.h"
#include "web_ui.h"

/* --- mDNS hostname --- */
#define SLAVE_HOSTNAME "esp32s3-modbus"

/* --- RS485 (Waveshare ESP32-S3-Relay-6CH) --- */
#define RS485_UART_NUM  1
#define RS485_TX        17
#define RS485_RX        18
#define RS485_BAUD      9600
// #define DIR_PIN 4

/* --- XY-MD02 RTU --- */
#define XYMD02_ADDR     0x01
#define XYMD02_FC       0x04
#define XYMD02_REG      0x0001
#define XYMD02_QTY      0x0002
#define RTU_RESP_LEN    (3 + 2 * XYMD02_QTY + 2)

/* --- Timing --- */
#define POLL_INTERVAL_MS         2000
#define WIFI_RECONNECT_INTERVAL  5000

/* --- Source / type name tables (enums + struct live in regmap.h) --- */
static const char* const SRC_NAMES[SRC_COUNT] = {
    "temp_c", "humidity", "status", "poll_count",
    "uptime_s", "wifi_rssi", "free_heap"
};
static const char* const DT_NAMES[DT_COUNT] = {
    "uint16", "int16", "uint32", "int32", "float32"
};
static int dtypeRegSize(uint8_t t) {
    return (t == DT_UINT32 || t == DT_INT32 || t == DT_FLOAT32) ? 2 : 1;
}

static RegEntry regTable[MAX_REGS];
static size_t   regCount = 0;

/* --- Live source values --- */
struct LiveSources {
    float temp_c    = 0;
    float humidity  = 0;
    float status    = 0;
    float poll_cnt  = 0;
};
static LiveSources live;

/* --- Objects --- */
HardwareSerial RS485(RS485_UART_NUM);
ModbusIP        mb;
AsyncWebServer  web(80);
Preferences     prefs;

/* ============================================================
 *  RTU CRC + read helper (unchanged)
 * ============================================================ */
static uint16_t modbusCRC16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) { crc >>= 1; crc ^= 0xA001; }
            else         { crc >>= 1; }
        }
    }
    return crc;
}

static int readXYMD02(int16_t *temp, uint16_t *humi) {
    uint8_t req[8] = {
        XYMD02_ADDR, XYMD02_FC,
        (uint8_t)(XYMD02_REG >> 8), (uint8_t)(XYMD02_REG & 0xFF),
        (uint8_t)(XYMD02_QTY >> 8), (uint8_t)(XYMD02_QTY & 0xFF),
        0, 0
    };
    uint16_t crc = modbusCRC16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    while (RS485.available()) RS485.read();

    #ifdef DIR_PIN
    digitalWrite(DIR_PIN, HIGH);
    #endif
    RS485.write(req, 8);
    RS485.flush();
    #ifdef DIR_PIN
    digitalWrite(DIR_PIN, LOW);
    #endif

    uint8_t resp[RTU_RESP_LEN] = {0};
    size_t rx = RS485.readBytes(resp, RTU_RESP_LEN);
    if (rx == 0)            return 1;
    if (rx < RTU_RESP_LEN)  return 1;
    if (resp[1] & 0x80)     return 3;

    uint16_t calcCRC = modbusCRC16(resp, rx - 2);
    uint16_t recvCRC = (uint16_t)resp[rx - 2] | ((uint16_t)resp[rx - 1] << 8);
    if (calcCRC != recvCRC) return 2;

    *temp = (int16_t)(((uint16_t)resp[3] << 8) | resp[4]);
    *humi = ((uint16_t)resp[5] << 8) | resp[6];
    return 0;
}

/* ============================================================
 *  Source value lookup
 * ============================================================ */
static float getSourceValue(uint8_t src) {
    switch (src) {
        case SRC_TEMP_C:     return live.temp_c;
        case SRC_HUMIDITY:   return live.humidity;
        case SRC_STATUS:     return live.status;
        case SRC_POLL_COUNT: return live.poll_cnt;
        case SRC_UPTIME_S:   return millis() / 1000.0f;
        case SRC_WIFI_RSSI:  return (float)WiFi.RSSI();
        case SRC_FREE_HEAP:  return (float)ESP.getFreeHeap();
    }
    return 0;
}

/* ============================================================
 *  Pack a source value into 1..2 holding registers
 * ============================================================ */
static void writeRegValue(const RegEntry& e) {
    float v = getSourceValue(e.source) * e.scale;
    switch (e.type) {
        case DT_UINT16:
            mb.Hreg(e.address, (uint16_t)v);
            break;
        case DT_INT16:
            mb.Hreg(e.address, (uint16_t)(int16_t)v);
            break;
        case DT_UINT32: {
            uint32_t u = (uint32_t)v;
            mb.Hreg(e.address,     (uint16_t)(u >> 16));
            mb.Hreg(e.address + 1, (uint16_t)(u & 0xFFFF));
            break;
        }
        case DT_INT32: {
            uint32_t u = (uint32_t)(int32_t)v;
            mb.Hreg(e.address,     (uint16_t)(u >> 16));
            mb.Hreg(e.address + 1, (uint16_t)(u & 0xFFFF));
            break;
        }
        case DT_FLOAT32: {
            uint32_t u;
            memcpy(&u, &v, 4);
            mb.Hreg(e.address,     (uint16_t)(u >> 16));
            mb.Hreg(e.address + 1, (uint16_t)(u & 0xFFFF));
            break;
        }
    }
}

static void refreshAllRegs() {
    for (size_t i = 0; i < regCount; i++) writeRegValue(regTable[i]);
}

/* ============================================================
 *  Default register table (matches the original hard-coded map)
 * ============================================================ */
static void loadDefaults() {
    regCount = 0;
    auto add = [](uint16_t addr, uint8_t src, uint8_t type, float scale,
                  const char* desc) {
        RegEntry& e = regTable[regCount++];
        e.address = addr;
        e.source  = src;
        e.type    = type;
        e.scale   = scale;
        strncpy(e.description, desc, DESC_LEN - 1);
        e.description[DESC_LEN - 1] = 0;
    };
    add(0, SRC_TEMP_C,     DT_INT16,  10, "Temperature x0.1 C");
    add(1, SRC_HUMIDITY,   DT_UINT16, 10, "Humidity x0.1 %RH");
    add(2, SRC_STATUS,     DT_UINT16,  1, "Last RTU read status");
    add(3, SRC_POLL_COUNT, DT_UINT16,  1, "RTU poll counter");
}

/* ============================================================
 *  NVS: load + save register table as JSON
 * ============================================================ */
static bool loadConfig() {
    prefs.begin("regmap", true);
    String json = prefs.getString("json", "");
    prefs.end();
    if (json.length() == 0) return false;

    JsonDocument doc;
    if (deserializeJson(doc, json)) return false;

    JsonArray arr = doc["registers"].as<JsonArray>();
    if (arr.isNull()) return false;

    regCount = 0;
    for (JsonObject o : arr) {
        if (regCount >= MAX_REGS) break;
        RegEntry& e = regTable[regCount++];
        e.address = o["address"] | 0;
        e.source  = o["source"]  | 0;
        e.type    = o["type"]    | 0;
        e.scale   = o["scale"]   | 1.0f;
        const char* d = o["description"] | "";
        strncpy(e.description, d, DESC_LEN - 1);
        e.description[DESC_LEN - 1] = 0;
        if (e.source >= SRC_COUNT) e.source = 0;
        if (e.type   >= DT_COUNT)  e.type   = 0;
    }
    return regCount > 0;
}

static bool saveConfig() {
    JsonDocument doc;
    JsonArray arr = doc["registers"].to<JsonArray>();
    for (size_t i = 0; i < regCount; i++) {
        JsonObject o = arr.add<JsonObject>();
        o["address"]     = regTable[i].address;
        o["source"]      = regTable[i].source;
        o["type"]        = regTable[i].type;
        o["scale"]       = regTable[i].scale;
        o["description"] = regTable[i].description;
    }
    String s;
    serializeJson(doc, s);
    prefs.begin("regmap", false);
    bool ok = prefs.putString("json", s) > 0;
    prefs.end();
    return ok;
}

/* ============================================================
 *  Apply current table to the Modbus stack
 *  (called once at boot after the table is loaded)
 * ============================================================ */
static void applyTableToModbus() {
    for (size_t i = 0; i < regCount; i++) {
        const RegEntry& e = regTable[i];
        int n = dtypeRegSize(e.type);
        for (int k = 0; k < n; k++) {
            mb.addHreg(e.address + k, 0);
        }
    }
}

/* ============================================================
 *  Web server
 * ============================================================ */
static void buildConfigJson(String& out) {
    JsonDocument doc;
    JsonArray sources = doc["sources"].to<JsonArray>();
    for (int i = 0; i < SRC_COUNT; i++) sources.add(SRC_NAMES[i]);
    JsonArray types = doc["types"].to<JsonArray>();
    for (int i = 0; i < DT_COUNT; i++) types.add(DT_NAMES[i]);

    JsonArray regs = doc["registers"].to<JsonArray>();
    for (size_t i = 0; i < regCount; i++) {
        JsonObject o = regs.add<JsonObject>();
        o["address"]     = regTable[i].address;
        o["source"]      = regTable[i].source;
        o["type"]        = regTable[i].type;
        o["scale"]       = regTable[i].scale;
        o["description"] = regTable[i].description;
    }
    serializeJson(doc, out);
}

static void buildLiveJson(String& out) {
    JsonDocument doc;
    JsonObject regs = doc["registers"].to<JsonObject>();
    for (size_t i = 0; i < regCount; i++) {
        const RegEntry& e = regTable[i];
        // Reconstruct displayable value (signed-aware) for the first reg
        uint16_t raw = mb.Hreg(e.address);
        long val;
        if (e.type == DT_INT16) val = (int16_t)raw;
        else                    val = raw;
        regs[String(e.address)] = val;
    }
    serializeJson(doc, out);
}

static volatile bool rebootPending = false;

static void setupWebServer() {
    web.on("/", HTTP_GET, [](AsyncWebServerRequest* req) {
        req->send_P(200, "text/html", INDEX_HTML);
    });

    web.on("/api/config", HTTP_GET, [](AsyncWebServerRequest* req) {
        String out;
        buildConfigJson(out);
        req->send(200, "application/json", out);
    });

    web.on("/api/live", HTTP_GET, [](AsyncWebServerRequest* req) {
        String out;
        buildLiveJson(out);
        req->send(200, "application/json", out);
    });

    /* POST /api/config -- replace register table */
    web.on("/api/config", HTTP_POST,
        [](AsyncWebServerRequest* req) {},
        nullptr,
        [](AsyncWebServerRequest* req, uint8_t* data, size_t len,
           size_t index, size_t total) {
            static String body;
            if (index == 0) body = "";
            body.concat((const char*)data, len);
            if (index + len != total) return;

            JsonDocument doc;
            if (deserializeJson(doc, body)) {
                req->send(400, "text/plain", "invalid JSON");
                return;
            }
            JsonArray arr = doc["registers"].as<JsonArray>();
            if (arr.isNull()) {
                req->send(400, "text/plain", "missing 'registers' array");
                return;
            }
            if (arr.size() > MAX_REGS) {
                req->send(400, "text/plain", "too many registers");
                return;
            }

            // Validate
            for (JsonObject o : arr) {
                int src = o["source"] | -1;
                int t   = o["type"]   | -1;
                int a   = o["address"]| -1;
                if (src < 0 || src >= SRC_COUNT) {
                    req->send(400, "text/plain", "bad source");
                    return;
                }
                if (t < 0 || t >= DT_COUNT) {
                    req->send(400, "text/plain", "bad type");
                    return;
                }
                if (a < 0 || a > 0xFFFF) {
                    req->send(400, "text/plain", "bad address");
                    return;
                }
            }

            // Commit to in-memory table
            regCount = 0;
            for (JsonObject o : arr) {
                RegEntry& e = regTable[regCount++];
                e.address = o["address"] | 0;
                e.source  = o["source"]  | 0;
                e.type    = o["type"]    | 0;
                e.scale   = o["scale"]   | 1.0f;
                const char* d = o["description"] | "";
                strncpy(e.description, d, DESC_LEN - 1);
                e.description[DESC_LEN - 1] = 0;
            }
            if (!saveConfig()) {
                req->send(500, "text/plain", "save failed");
                return;
            }
            req->send(200, "application/json", "{\"ok\":true}");
            rebootPending = true;
        });

    web.on("/api/config/reset", HTTP_POST, [](AsyncWebServerRequest* req) {
        loadDefaults();
        saveConfig();
        req->send(200, "application/json", "{\"ok\":true}");
        rebootPending = true;
    });

    web.onNotFound([](AsyncWebServerRequest* req) {
        req->send(404, "text/plain", "not found");
    });

    web.begin();
}

/* ============================================================
 *  TCP client connect callback (logs only)
 * ============================================================ */
static bool cbConn(IPAddress ip) {
    Serial.printf("[TCP] Client connected: %s\n", ip.toString().c_str());
    return true;
}

/* ============================================================
 *  WiFi + mDNS bring-up
 * ============================================================ */
static void connectWiFi() {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(SLAVE_HOSTNAME);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("Connecting to WiFi '%s'", WIFI_SSID);
    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
        delay(500);
        Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\nWiFi connected! IP: %s\n",
                      WiFi.localIP().toString().c_str());
        if (MDNS.begin(SLAVE_HOSTNAME)) {
            MDNS.addService("modbus", "tcp", 502);
            MDNS.addService("http",   "tcp", 80);
            Serial.printf("mDNS: %s.local\n", SLAVE_HOSTNAME);
        } else {
            Serial.println("[WARN] mDNS start failed");
        }
    } else {
        Serial.println("\n[WARN] WiFi connect failed; will retry in loop()");
    }
}

/* ============================================================
 *  Setup
 * ============================================================ */
void setup() {
    Serial.begin(115200);
    //while (!Serial) delay(10);

    Serial.println();
    Serial.println("===========================================");
    Serial.println(" ESP32-S3 Modbus RTU->TCP Gateway");
    Serial.println(" RTU Master + TCP Slave + Config Web UI");
    Serial.println("===========================================");

    /* RS485 */
    RS485.begin(RS485_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);
    RS485.setTimeout(30);
    #ifdef DIR_PIN
    pinMode(DIR_PIN, OUTPUT);
    digitalWrite(DIR_PIN, LOW);
    #endif
    Serial.printf("RS485: UART%d TX=GPIO%d RX=GPIO%d %d baud\n",
                  RS485_UART_NUM, RS485_TX, RS485_RX, RS485_BAUD);

    /* Load register map (or seed defaults) */
    if (!loadConfig()) {
        Serial.println("No saved config; loading defaults");
        loadDefaults();
        saveConfig();
    } else {
        Serial.printf("Loaded %u register entries from NVS\n",
                      (unsigned)regCount);
    }

    connectWiFi();

    /* Modbus TCP slave */
    mb.server();
    mb.onConnect(cbConn);
    applyTableToModbus();
    Serial.println("TCP Slave listening on port 502");

    /* Web server */
    setupWebServer();
    Serial.println("Web UI listening on port 80");
    Serial.printf("Open: http://%s.local/\n\n", SLAVE_HOSTNAME);
}

/* ============================================================
 *  Main loop
 * ============================================================ */
unsigned long lastPoll = 0;
unsigned long lastWifiCheck = 0;
unsigned long lastRefresh = 0;

void loop() {
    mb.task();

    if (rebootPending) {
        delay(200);
        Serial.println("Rebooting to apply new register map...");
        ESP.restart();
    }

    /* WiFi watchdog */
    if (millis() - lastWifiCheck >= WIFI_RECONNECT_INTERVAL) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Disconnected, reconnecting...");
            WiFi.disconnect();
            connectWiFi();
        }
    }

    /* Poll XY-MD02 */
    if (millis() - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = millis();
        int16_t  tempRaw = 0;
        uint16_t humiRaw = 0;
        int status = readXYMD02(&tempRaw, &humiRaw);
        if (status == 0) {
            live.poll_cnt += 1;
            live.temp_c   = tempRaw / 10.0f;
            live.humidity = humiRaw / 10.0f;
            live.status   = 0;
            Serial.printf("XY-MD02: %.1fC  %.1f%%RH  [OK #%.0f]\n",
                          live.temp_c, live.humidity, live.poll_cnt);
        } else {
            live.status = status;
            Serial.printf("XY-MD02: ERROR (status=%d)\n", status);
        }
        refreshAllRegs();
    }

    /* Refresh dynamic sources (uptime, RSSI, heap) every second
       so masters polling those see fresh values between RTU reads */
    if (millis() - lastRefresh >= 1000) {
        lastRefresh = millis();
        refreshAllRegs();
    }
}
