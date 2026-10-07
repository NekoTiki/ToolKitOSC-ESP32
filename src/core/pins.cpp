#include "pins.h"

#include <string.h>

namespace {

template <size_t N>
constexpr size_t countOf(const uint8_t (&)[N]) {
  return N;
}

// Safe outputs per chip, before the module layer. Check against the datasheet when editing.

// ESP32. Not offered: 0, 2, 5, 12, 15 (strapping), 6-11 (flash), 1, 3 (serial), 34-39 (input only).
const uint8_t kEsp32[] = {4, 13, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33};

// ESP32-S2. Not offered: 0, 45, 46 (strapping), 22-32 (flash / not bonded), 19, 20 (USB),
// 43, 44 (serial).
const uint8_t kEsp32S2[] = {1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16,
                            17, 18, 21, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42};

// ESP32-S3. Not offered: 0, 3, 45, 46 (strapping), 22-32 (flash / not bonded), 19, 20 (USB),
// 43, 44 (serial), and 33-37: octal flash or PSRAM use them, and the firmware can't see octal
// PSRAM it wasn't built for, so it can't tell when they are free.
const uint8_t kEsp32S3[] = {1,  2,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                            16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48};

// ESP32-C3. Not offered: 2, 8, 9 (strapping), 11-17 (flash), 18, 19 (USB), 20, 21 (serial).
const uint8_t kEsp32C3[] = {0, 1, 3, 4, 5, 6, 7, 10};

const OutputPin kS3DevKitC1[] = {
    {1, true, nullptr},  {2, true, nullptr},  {4, true, nullptr},  {5, true, nullptr},
    {6, true, nullptr},  {7, true, nullptr},  {8, true, nullptr},  {9, true, nullptr},
    {10, true, nullptr}, {11, true, nullptr}, {12, true, nullptr}, {13, true, nullptr},
    {14, true, nullptr}, {15, true, nullptr}, {16, true, nullptr}, {17, true, nullptr},
    {18, true, nullptr}, {21, true, nullptr}, {39, true, nullptr}, {40, true, nullptr},
    {41, true, nullptr}, {42, true, nullptr}, {47, true, nullptr},
};

const OutputPin kLolinS3[] = {
    {1, true, nullptr},  {2, true, nullptr},  {4, true, nullptr},  {5, true, nullptr},
    {6, true, nullptr},  {7, true, nullptr},  {8, true, nullptr},  {9, true, nullptr},
    {10, true, nullptr}, {11, true, nullptr}, {12, true, nullptr}, {13, true, nullptr},
    {14, true, nullptr}, {15, true, nullptr}, {16, true, nullptr}, {17, true, nullptr},
    {18, true, nullptr}, {21, true, nullptr}, {39, true, nullptr}, {40, true, nullptr},
    {41, true, nullptr}, {42, true, nullptr}, {47, true, nullptr}, {48, true, nullptr},
};

// D2 (GPIO 3) is a strapping pin and D6/D7 are the serial port, so they're left out.
const OutputPin kXiaoS3[] = {
    {1, true, "D0"}, {2, true, "D1"}, {4, true, "D3"}, {5, true, "D4"},
    {6, true, "D5"}, {7, true, "D8"}, {8, true, "D9"}, {9, true, "D10"},
};

// D0 (GPIO 2), D8 and D9 are strapping pins and D6/D7 are the serial port, so they're left out.
const OutputPin kXiaoC3[] = {
    {3, true, "D1"}, {4, true, "D2"}, {5, true, "D3"}, {6, true, "D4"}, {7, true, "D5"}, {10, true, "D10"},
};

const BoardProfile kProfiles[] = {
    // The RGB LED is on GPIO 48 (v1.0) or 38 (v1.1). Both are left out and both are driven.
    {"esp32-s3-devkitc-1", "ESP32-S3-DevKitC-1", Chip::Esp32S3, kS3DevKitC1,
     sizeof(kS3DevKitC1) / sizeof(kS3DevKitC1[0]), {{48, true, false}, {38, true, false}}},
    {"lolin-s3", "WEMOS LOLIN S3", Chip::Esp32S3, kLolinS3, sizeof(kLolinS3) / sizeof(kLolinS3[0]),
     {{38, true, false}, {-1, false, false}}},
    {"xiao-esp32s3", "XIAO ESP32-S3", Chip::Esp32S3, kXiaoS3, sizeof(kXiaoS3) / sizeof(kXiaoS3[0]),
     {{21, false, true}, {-1, false, false}}},
    {"xiao-esp32c3", "XIAO ESP32-C3", Chip::Esp32C3, kXiaoC3, sizeof(kXiaoC3) / sizeof(kXiaoC3[0]),
     {{-1, false, false}, {-1, false, false}}},
};

void chipPins(Chip chip, const uint8_t** pins, size_t* count) {
  switch (chip) {
    case Chip::Esp32:
      *pins = kEsp32;
      *count = countOf(kEsp32);
      return;
    case Chip::Esp32S2:
      *pins = kEsp32S2;
      *count = countOf(kEsp32S2);
      return;
    case Chip::Esp32S3:
      *pins = kEsp32S3;
      *count = countOf(kEsp32S3);
      return;
    case Chip::Esp32C3:
      *pins = kEsp32C3;
      *count = countOf(kEsp32C3);
      return;
  }
  *pins = nullptr;
  *count = 0;
}

bool usedByModule(const ModuleInfo& module, uint8_t pin) {
  if (module.chip == Chip::Esp32 && (module.psram || module.picoPackage)) return pin == 16 || pin == 17;
  return false;
}

bool chipAllows(const ModuleInfo& module, uint8_t pin) {
  const uint8_t* pins;
  size_t count;
  chipPins(module.chip, &pins, &count);
  for (size_t i = 0; i < count; i++) {
    if (pins[i] == pin) return !usedByModule(module, pin);
  }
  return false;
}

}  // namespace

const char* chipName(Chip chip) {
  switch (chip) {
    case Chip::Esp32:
      return "ESP32";
    case Chip::Esp32S2:
      return "ESP32-S2";
    case Chip::Esp32S3:
      return "ESP32-S3";
    case Chip::Esp32C3:
      return "ESP32-C3";
  }
  return "ESP32";
}

uint8_t bootButtonPin(Chip chip) {
  return chip == Chip::Esp32C3 ? 9 : 0;
}

size_t boardProfilesFor(Chip chip, const BoardProfile** out, size_t max) {
  size_t n = 0;
  for (const BoardProfile& profile : kProfiles) {
    if (profile.chip == chip && n < max) out[n++] = &profile;
  }
  return n;
}

const BoardProfile* findBoardProfile(const char* id, Chip chip) {
  if (!id || !id[0]) return nullptr;
  for (const BoardProfile& profile : kProfiles) {
    if (profile.chip == chip && strcmp(profile.id, id) == 0) return &profile;
  }
  return nullptr;
}

size_t resolveOutputPins(const ModuleInfo& module, const BoardProfile* board, OutputPin* out, size_t max) {
  size_t n = 0;

  if (board) {
    for (size_t i = 0; i < board->pinCount && n < max; i++) {
      if (chipAllows(module, board->pins[i].pin)) out[n++] = board->pins[i];
    }
    return n;
  }

  const uint8_t* pins;
  size_t count;
  chipPins(module.chip, &pins, &count);
  for (size_t i = 0; i < count && n < max; i++) {
    if (!usedByModule(module, pins[i])) out[n++] = {pins[i], true, nullptr};
  }
  return n;
}
