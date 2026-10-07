#pragma once
// Firmware-wide constants. The pins each chip and board may use are in src/core/pins.cpp.

#include <stddef.h>
#include <stdint.h>

#define TKOSC_FW_VERSION "0.1.0"
// Sent in the mDNS TXT record and checked against "hello". Bump it with any change to PROTOCOL.md.
constexpr int TKOSC_PROTO = 1;

constexpr uint16_t WS_PORT = 8787;
#define WS_PATH "/tkosc"
// Advertised as _tkosc._tcp on host tkosc-<id>.local.
#define MDNS_SERVICE "tkosc"
#define HOSTNAME_PREFIX "tkosc-"
#define SETUP_AP_PREFIX "ToolKitOSC-Setup-"

// Wi-Fi lost for this long reopens the setup access point. The pairing token is kept, so nobody
// can take the board over by jamming its Wi-Fi.
constexpr uint32_t WIFI_LOST_PORTAL_MS = 3 * 60 * 1000;

// BOOT button, held while the board is running. A short press confirms pairing.
constexpr uint32_t HOLD_UNPAIR_MS = 3000;
constexpr uint32_t HOLD_RESET_MS = 10000;

// Power-cycling this many times in a row, each time within POWER_CYCLE_WINDOW_MS of boot, does a
// factory reset. For boards in a case where the button is out of reach.
constexpr uint8_t POWER_CYCLE_RESET_COUNT = 5;
constexpr uint32_t POWER_CYCLE_WINDOW_MS = 3000;

// Most sockets open at once. Extra ones are closed straight away.
constexpr size_t MAX_SOCKETS = 3;
// Longest text frame accepted. Every message in PROTOCOL.md fits in far less.
constexpr size_t MAX_FRAME = 1024;

// PWM for "pwm" commands.
constexpr uint32_t PWM_FREQ_HZ = 5000;
constexpr uint8_t PWM_BITS = 10;
