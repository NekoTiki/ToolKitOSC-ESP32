#pragma once
// Everything the board keeps across reboots, saved as one NVS blob. Bump kVersion when the layout
// changes: a blob with another version is ignored and the board starts from defaults.

#include "outputs.h"

struct Settings {
  static constexpr uint8_t kVersion = 1;

  uint8_t version = kVersion;
  char name[33] = "";
  // Empty while unpaired.
  char token[49] = "";
  // Board profile id. "" = not chosen yet (the build's default applies), "generic" = none.
  char board[24] = "";
  // Saved so the off level of active-low outputs is right from the first moment after boot.
  uint8_t outputCount = 0;
  OutputConfig outputs[OutputBank::kMaxOutputs] = {};
};
