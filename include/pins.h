#pragma once
// Which GPIOs the board offers as outputs. Three layers, each only removing pins:
//   chip    - strapping, flash, USB, serial and input-only pins are never offered
//   module  - pins the module uses itself (PSRAM, in-package flash), detected at boot
//   board   - an optional profile, chosen on the setup page: the pins the board breaks out and
//             doesn't already use, with their silkscreen names
// A profile can't add a pin the chip or module layer removed, so a wrong profile only hides pins.

#include <stddef.h>
#include <stdint.h>

enum class Chip : uint8_t { Esp32, Esp32S2, Esp32S3, Esp32C3 };

// What the firmware detects about the module at boot.
struct ModuleInfo {
  Chip chip;
  bool psram;        // PSRAM found (ESP32 WROVER: GPIO 16/17 are taken)
  bool picoPackage;  // ESP32-PICO-*: in-package flash on GPIO 16/17
};

struct OutputPin {
  uint8_t pin;
  bool pwm;
  const char* label;  // silkscreen name, nullptr when unknown
};

struct StatusLed {
  int8_t pin;  // -1 = none
  bool rgb;    // WS2812 driven with neopixelWrite()
  bool activeLow;
};

struct BoardProfile {
  const char* id;    // saved in NVS, e.g. "lolin-s3"
  const char* name;  // sent in "welcome", e.g. "WEMOS LOLIN S3"
  Chip chip;
  const OutputPin* pins;
  size_t pinCount;
  StatusLed leds[2];
};

// Longest list resolveOutputPins() can return.
constexpr size_t kMaxOutputPins = 40;

const char* chipName(Chip chip);
uint8_t bootButtonPin(Chip chip);

// Every board profile made for `chip`. Returns how many were written to `out`.
size_t boardProfilesFor(Chip chip, const BoardProfile** out, size_t max);
// nullptr for "", "generic", an unknown id, or a profile made for another chip.
const BoardProfile* findBoardProfile(const char* id, Chip chip);

size_t resolveOutputPins(const ModuleInfo& module, const BoardProfile* board, OutputPin* out, size_t max);
