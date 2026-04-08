#pragma once
#include <stdint.h>

/*
 * Shared types for the configurable register map.
 * Lives in a header so the Arduino IDE's auto-generated function
 * prototypes (inserted at the top of the .ino) can see RegEntry.
 */

#define MAX_REGS  32
#define DESC_LEN  48

enum Source {
    SRC_TEMP_C = 0,
    SRC_HUMIDITY,
    SRC_STATUS,
    SRC_POLL_COUNT,
    SRC_UPTIME_S,
    SRC_WIFI_RSSI,
    SRC_FREE_HEAP,
    SRC_COUNT
};

enum DType {
    DT_UINT16 = 0,
    DT_INT16,
    DT_UINT32,
    DT_INT32,
    DT_FLOAT32,
    DT_COUNT
};

struct RegEntry {
    uint16_t address;
    uint8_t  source;
    uint8_t  type;
    float    scale;
    char     description[DESC_LEN];
};
