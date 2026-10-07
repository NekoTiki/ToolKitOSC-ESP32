#pragma once
// The protocol in PROTOCOL.md, independent of the transport: the platform passes in each text
// frame with the id of the socket it came from, and sends or closes sockets when asked.
// Not thread-safe: call everything from the same task (the Arduino loop on the ESP32).

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

#include "outputs.h"
#include "pins.h"
#include "settings.h"

struct DeviceInfo {
  const char* id;     // "a1b2c3"
  const char* chip;   // "ESP32-S3"
  const char* board;  // board profile name, or "Generic"
};

class Session {
 public:
  static constexpr uint32_t kNoClient = 0xffffffff;
  static constexpr uint32_t kPairWindowMs = 30000;
  // No message from the app for this long: everything off and the socket is closed.
  static constexpr uint32_t kDeadManMs = 3000;

  static constexpr uint16_t kCloseNormal = 1000;
  static constexpr uint16_t kCloseUnpaired = 4000;
  static constexpr uint16_t kCloseBadProto = 4400;
  static constexpr uint16_t kCloseUnauthorized = 4401;
  static constexpr uint16_t kCloseTimeout = 4408;
  static constexpr uint16_t kCloseReplaced = 4409;

  using SendFn = void (*)(uint32_t client, const char* json);
  using CloseFn = void (*)(uint32_t client, uint16_t code);
  // Settings changed (token, outputs) and should be saved.
  using PersistFn = void (*)();

  Session(Settings& settings, OutputBank& outputs, const DeviceInfo& device, const OutputPin* allowed,
          size_t allowedCount);

  void setTransport(SendFn send, CloseFn close);
  void setPersist(PersistFn persist);

  void handleText(uint32_t client, const char* data, size_t len);
  void handleDisconnect(uint32_t client);
  // BOOT short press. Returns false when no pairing was waiting for it.
  bool confirmPairing();
  // Clears the token and drops the app (BOOT 3 s, serial "unpair", or the app's "unpair").
  void unpair();
  void update();

  // For OutputBank's callbacks.
  void reportOutput(uint8_t pin, bool on, float duty);
  void reportTrip(uint8_t pin);

  bool paired() const { return settings_.token[0] != '\0'; }
  bool connected() const { return current_ != kNoClient; }
  bool pairingActive() const { return pairingClient_ != kNoClient; }

 private:
  void startPairing(uint32_t client);
  void hello(uint32_t client, JsonObjectConst msg);
  void handleCommand(uint32_t client, const char* type, JsonObjectConst msg);
  void configure(uint32_t client, JsonObjectConst msg);
  void sendWelcome(uint32_t client);
  void sendError(uint32_t client, const char* code, const char* ref, int pin = -1);
  void sendResult(uint32_t client, OutputError error, const char* ref, int pin);
  void send(uint32_t client, const char* json);
  void close(uint32_t client, uint16_t code);
  void persist();
  bool allowed(uint8_t pin) const;

  Settings& settings_;
  OutputBank& outputs_;
  DeviceInfo device_;
  const OutputPin* allowed_;
  size_t allowedCount_;

  SendFn send_ = nullptr;
  CloseFn close_ = nullptr;
  PersistFn persist_ = nullptr;

  uint32_t current_ = kNoClient;
  uint32_t lastSeen_ = 0;
  uint32_t pairingClient_ = kNoClient;
  uint32_t pairingUntil_ = 0;
};
