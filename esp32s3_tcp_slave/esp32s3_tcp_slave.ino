/*
 * ===========================================================
 *  Wav/Users/bean/work/Senior Project/Code/esp32MODBUSTCP/esp32s3_tcp_slave/esp32s3_tcp_slave.inoeshare ESP32-S3-Relay-6CH
 *  Modbus RTU Master (XY-MD02) + Modbus TCP Slave
 * ===========================================================
 *
 * Architecture:
 *   [XY-MD02 @0x01] --RS485--> [ESP32-S3] <--WiFi/TCP-- [Any TCP Master]
 *                    FC04        RTU Master               TCP Slave :502
 *
 * This device:
 *   1. Polls XY-MD02 every 2s via RS485 (FC04, register 0x0001, qty 2)
 *   2. Stores temp/humidity in local Modbus holding registers
 *   3. Serves them to any Modbus TCP master on port 502
 *   4. Advertises itself over mDNS as <SLAVE_HOSTNAME>.local
 *
 * TCP Slave Holding Register Map (FC03, 0-based):
 *   HREG 0: Temperature (raw x 0.1 C, signed)
 *   HREG 1: Humidity    (raw x 0.1 %RH, unsigned)
 *   HREG 2: Status      (0=OK, 1=timeout, 2=CRC err, 3=exception)
 *   HREG 3: Poll count  (rolls over at 65535)
 *
 * Library: "modbus-esp8266" by emelianov (install via Library Manager)
 *   Arduino IDE -> Sketch -> Include Library -> Manage Libraries
 *   Search: "modbus-esp8266" -> Install
 *
 * Board Settings:
 *   Board:       "ESP32S3 Dev Module"
 *   USB CDC:     "Enabled"
 *   Upload Mode: "UART0 / Hardware CDC"
 *
 * RS485 Wiring (onboard transceiver):
 *   A(+) ---- XY-MD02 A(+)
 *   B(-) ---- XY-MD02 B(-)
 *
 * SECURITY: Modbus TCP has no authentication. Do NOT expose port 502
 * to the public internet. Keep this device on a trusted LAN/VLAN.
 */

#include <WiFi.h>
#include <ESPmDNS.h>
#include <ModbusIP_ESP8266.h>   /* Modbus TCP Slave */
#include <HardwareSerial.h>

#include "secrets.h"            /* Provides WIFI_SSID and WIFI_PASS */

/* --- mDNS hostname (advertised as <name>.local) --- */
#define SLAVE_HOSTNAME "esp32s3-modbus"

/* --- RS485 Pins (Waveshare ESP32-S3-Relay-6CH) --- */
#define RS485_UART_NUM  1
#define RS485_TX        17
#define RS485_RX        18
#define RS485_BAUD      9600

/*
 * If your board needs manual DE/RE direction control,
 * uncomment below and set the correct GPIO:
 */
// #define DIR_PIN 4

/* --- XY-MD02 RTU Parameters --- */
#define XYMD02_ADDR     0x01
#define XYMD02_FC       0x04    /* FC04: Read Input Registers */
#define XYMD02_REG      0x0001  /* Start register */
#define XYMD02_QTY      0x0002  /* 2 registers: temp + humidity */

/* RTU FC04 response = addr(1) + fc(1) + bytecount(1) + data(2*qty) + crc(2) */
#define RTU_RESP_LEN    (3 + 2 * XYMD02_QTY + 2)

/* --- TCP Slave Holding Register Addresses (0-based) --- */
#define HREG_TEMP       0
#define HREG_HUMI       1
#define HREG_STATUS     2
#define HREG_POLL_CNT   3

/* --- Timing --- */
#define POLL_INTERVAL_MS         2000
#define WIFI_RECONNECT_INTERVAL  5000

/* --- Objects --- */
HardwareSerial RS485(RS485_UART_NUM);
ModbusIP mb;  /* Modbus TCP Slave */

/* --- RTU CRC-16 --- */
uint16_t modbusCRC16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc >>= 1;
                crc ^= 0xA001;
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* --- Read XY-MD02 via RTU, returns status code --- */
int readXYMD02(int16_t *temp, uint16_t *humi) {
    /* Build FC04 request */
    uint8_t req[8];
    req[0] = XYMD02_ADDR;
    req[1] = XYMD02_FC;
    req[2] = (XYMD02_REG >> 8) & 0xFF;
    req[3] = XYMD02_REG & 0xFF;
    req[4] = (XYMD02_QTY >> 8) & 0xFF;
    req[5] = XYMD02_QTY & 0xFF;
    uint16_t crc = modbusCRC16(req, 6);
    req[6] = crc & 0xFF;
    req[7] = (crc >> 8) & 0xFF;

    /* Flush RX */
    while (RS485.available()) RS485.read();

    /* Transmit */
    #ifdef DIR_PIN
    digitalWrite(DIR_PIN, HIGH);
    #endif

    RS485.write(req, 8);
    RS485.flush(); // Blocks for ~8.3ms at 9600 baud

    #ifdef DIR_PIN
    digitalWrite(DIR_PIN, LOW);
    #endif

    /* readBytes will block until RTU_RESP_LEN bytes arrive or setTimeout() expires */
    uint8_t resp[RTU_RESP_LEN] = {0};
    size_t rx = RS485.readBytes(resp, RTU_RESP_LEN);

    if (rx == 0) return 1;            /* Timeout (no data at all) */
    if (rx < RTU_RESP_LEN) return 1;  /* Short response (partial data) */

    /* Exception check */
    if (resp[1] & 0x80) return 3;

    /* CRC check */
    uint16_t calcCRC = modbusCRC16(resp, rx - 2);
    uint16_t recvCRC = (uint16_t)resp[rx - 2] | ((uint16_t)resp[rx - 1] << 8);
    if (calcCRC != recvCRC) return 2; /* CRC error */

    /* Parse data (big-endian) */
    *temp = (int16_t)(((uint16_t)resp[3] << 8) | resp[4]);
    *humi = ((uint16_t)resp[5] << 8) | resp[6];

    return 0;  /* OK */
}

/*
 * TCP client connect callback. Hook for future ACL/IP filtering;
 * currently logs the peer and accepts every connection.
 */
bool cbConn(IPAddress ip) {
    Serial.printf("[TCP] Client connected: %s\n", ip.toString().c_str());
    return true;
}

/* --- WiFi bring-up + mDNS (re-usable on reconnect) --- */
void connectWiFi() {
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
        Serial.printf("\nWiFi connected! IP: %s\n", WiFi.localIP().toString().c_str());
        if (MDNS.begin(SLAVE_HOSTNAME)) {
            MDNS.addService("modbus", "tcp", 502);
            Serial.printf("mDNS: %s.local\n", SLAVE_HOSTNAME);
        } else {
            Serial.println("[WARN] mDNS start failed");
        }
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
    Serial.println(" ESP32-S3 Modbus RTU->TCP Gateway");
    Serial.println(" RTU Master (XY-MD02) + TCP Slave (:502)");
    Serial.println("===========================================");

    /* RS485 UART */
    RS485.begin(RS485_BAUD, SERIAL_8N1, RS485_RX, RS485_TX);
    RS485.setTimeout(30);
    #ifdef DIR_PIN
    pinMode(DIR_PIN, OUTPUT);
    digitalWrite(DIR_PIN, LOW);
    #endif
    Serial.printf("RS485: UART%d TX=GPIO%d RX=GPIO%d %d baud\n",
                  RS485_UART_NUM, RS485_TX, RS485_RX, RS485_BAUD);

    connectWiFi();

    /* Modbus TCP Slave -- start server on port 502 */
    mb.server();
    mb.onConnect(cbConn);   /* Register callback ONCE */

    /* Add holding registers (FC03 readable by TCP master) */
    mb.addHreg(HREG_TEMP,     0);
    mb.addHreg(HREG_HUMI,     0);
    mb.addHreg(HREG_STATUS,   0);
    mb.addHreg(HREG_POLL_CNT, 0);

    Serial.println("TCP Slave listening on port 502");
    Serial.println("Holding Registers: HR0=Temp, HR1=Humi, HR2=Status, HR3=PollCnt\n");
}

/* --- Main Loop --- */
uint16_t pollCount = 0;
unsigned long lastPoll = 0;
unsigned long lastWifiCheck = 0;

void loop() {
    /* Service TCP slave (must be called frequently) */
    mb.task();

    /* WiFi watchdog: reconnect if dropped */
    if (millis() - lastWifiCheck >= WIFI_RECONNECT_INTERVAL) {
        lastWifiCheck = millis();
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[WiFi] Disconnected, reconnecting...");
            WiFi.disconnect();
            connectWiFi();
        }
    }

    /* Poll XY-MD02 at interval */
    if (millis() - lastPoll >= POLL_INTERVAL_MS) {
        lastPoll = millis();

        int16_t  tempRaw = 0;
        uint16_t humiRaw = 0;
        int status = readXYMD02(&tempRaw, &humiRaw);

        if (status == 0) {
            pollCount++;
            mb.Hreg(HREG_TEMP,     (uint16_t)tempRaw);
            mb.Hreg(HREG_HUMI,     humiRaw);
            mb.Hreg(HREG_STATUS,   0);
            mb.Hreg(HREG_POLL_CNT, pollCount);

            Serial.printf("XY-MD02: %.1fC  %.1f%%RH  [OK #%u]\n",
                          tempRaw / 10.0, humiRaw / 10.0, pollCount);
        } else {
            mb.Hreg(HREG_STATUS, (uint16_t)status);
            Serial.printf("XY-MD02: ERROR (status=%d)\n", status);
        }
    }
}
