#include "session.h"

#include <string.h>

#include "config.h"
#include "hal.h"
#include "util.h"

namespace {

constexpr size_t kTokenBytes = 32;

// Fits "welcome" with every output labelled, the largest message the board sends.
char outBuffer[2048];

bool reached(uint32_t now, uint32_t at) {
  return static_cast<int32_t>(now - at) >= 0;
}

bool isNumber(JsonVariantConst v) {
  return v.is<int>() || v.is<unsigned>() || v.is<float>();
}

bool isCount(JsonVariantConst v) {
  return v.is<uint32_t>();
}

const char* errorCode(OutputError error) {
  switch (error) {
    case OutputError::BadPin:
      return "bad-pin";
    case OutputError::NotPwm:
      return "not-pwm";
    case OutputError::BadValue:
      return "bad-msg";
    case OutputError::Tripped:
      return "max-on";
    case OutputError::None:
      break;
  }
  return nullptr;
}

}  // namespace

Session::Session(Settings& settings, OutputBank& outputs, const DeviceInfo& device, const OutputPin* allowed,
                 size_t allowedCount)
    : settings_(settings), outputs_(outputs), device_(device), allowed_(allowed), allowedCount_(allowedCount) {}

void Session::setTransport(SendFn send, CloseFn close) {
  send_ = send;
  close_ = close;
}

void Session::setPersist(PersistFn persist) {
  persist_ = persist;
}

void Session::handleText(uint32_t client, const char* data, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, data, len) != DeserializationError::Ok || !doc["t"].is<const char*>()) {
    sendError(client, "bad-msg", nullptr);
    return;
  }

  JsonObjectConst msg = doc.as<JsonObjectConst>();
  const char* type = msg["t"];

  if (strcmp(type, "pair") == 0) return startPairing(client);
  if (strcmp(type, "hello") == 0) return hello(client, msg);

  // Everything else needs an authenticated session.
  if (client != current_) {
    close(client, kCloseUnauthorized);
    return;
  }

  lastSeen_ = hal::millis();
  handleCommand(client, type, msg);
}

void Session::handleDisconnect(uint32_t client) {
  if (client == pairingClient_) {
    hal::log("[pair] app left before BOOT was pressed\n");
    pairingClient_ = kNoClient;
  }

  if (client == current_) {
    hal::log("[app] disconnected, everything off\n");
    current_ = kNoClient;
    outputs_.stop();
  }
}

bool Session::confirmPairing() {
  if (!pairingActive()) return false;

  uint8_t raw[kTokenBytes];
  hal::randomBytes(raw, sizeof(raw));
  base64Encode(raw, sizeof(raw), settings_.token, sizeof(settings_.token));
  persist();

  // The old token is gone, so whoever was using it is out.
  if (current_ != kNoClient) {
    outputs_.stop();
    close(current_, kCloseUnauthorized);
    current_ = kNoClient;
  }

  JsonDocument doc;
  doc["t"] = "paired";
  doc["token"] = settings_.token;
  serializeJson(doc, outBuffer, sizeof(outBuffer));
  send(pairingClient_, outBuffer);

  hal::log("[pair] paired\n");
  pairingClient_ = kNoClient;
  return true;
}

void Session::unpair() {
  settings_.token[0] = '\0';
  persist();
  pairingClient_ = kNoClient;

  if (current_ != kNoClient) {
    outputs_.stop();
    close(current_, kCloseUnpaired);
    current_ = kNoClient;
  }
  hal::log("[pair] unpaired, ready to be added again\n");
}

void Session::update() {
  uint32_t now = hal::millis();

  if (pairingActive() && reached(now, pairingUntil_)) {
    hal::log("[pair] nobody pressed BOOT, pairing cancelled\n");
    sendError(pairingClient_, "pair-timeout", "pair");
    pairingClient_ = kNoClient;
  }

  if (current_ != kNoClient && reached(now, lastSeen_ + kDeadManMs)) {
    hal::log("[app] silent for %u ms, everything off\n", (unsigned)kDeadManMs);
    outputs_.stop();
    close(current_, kCloseTimeout);
    current_ = kNoClient;
  }
}

void Session::reportOutput(uint8_t pin, bool on, float duty) {
  if (current_ == kNoClient) return;

  JsonDocument doc;
  doc["t"] = "out";
  doc["pin"] = pin;
  doc["on"] = on;
  doc["duty"] = duty;
  serializeJson(doc, outBuffer, sizeof(outBuffer));
  send(current_, outBuffer);
}

void Session::reportTrip(uint8_t pin) {
  if (current_ != kNoClient) sendError(current_, "max-on", nullptr, pin);
}

void Session::startPairing(uint32_t client) {
  pairingClient_ = client;
  pairingUntil_ = hal::millis() + kPairWindowMs;
  hal::log("[pair] press the BOOT button within %u s to pair\n", (unsigned)(kPairWindowMs / 1000));

  JsonDocument doc;
  doc["t"] = "pairing";
  doc["timeoutMs"] = kPairWindowMs;
  serializeJson(doc, outBuffer, sizeof(outBuffer));
  send(client, outBuffer);
}

void Session::hello(uint32_t client, JsonObjectConst msg) {
  if (msg["proto"] != TKOSC_PROTO) {
    sendError(client, "bad-proto", "hello");
    close(client, kCloseBadProto);
    return;
  }

  const char* token = msg["token"] | "";
  if (!paired() || !secureEquals(token, settings_.token)) {
    close(client, kCloseUnauthorized);
    return;
  }

  // One app at a time: the newer session wins.
  if (current_ != kNoClient && current_ != client) close(current_, kCloseReplaced);

  current_ = client;
  lastSeen_ = hal::millis();
  hal::log("[app] connected\n");
  sendWelcome(client);
}

void Session::handleCommand(uint32_t client, const char* type, JsonObjectConst msg) {
  if (strcmp(type, "ping") == 0) {
    send(client, "{\"t\":\"pong\"}");
    return;
  }

  if (strcmp(type, "stop") == 0) {
    outputs_.stop();
    return;
  }

  if (strcmp(type, "config") == 0) return configure(client, msg);

  if (strcmp(type, "unpair") == 0) {
    settings_.token[0] = '\0';
    persist();
    outputs_.stop();
    close(client, kCloseNormal);
    current_ = kNoClient;
    hal::log("[pair] unpaired by the app\n");
    return;
  }

  JsonVariantConst pinValue = msg["pin"];
  if (!pinValue.is<uint8_t>()) {
    sendError(client, "bad-msg", type);
    return;
  }
  uint8_t pin = pinValue.as<uint8_t>();

  if (strcmp(type, "set") == 0) {
    if (!msg["on"].is<bool>()) return sendError(client, "bad-msg", type, pin);
    bool on = msg["on"];
    if (on && !isCount(msg["ms"])) return sendError(client, "bad-msg", type, pin);
    sendResult(client, outputs_.set(pin, on, msg["ms"] | 0u), type, pin);
  } else if (strcmp(type, "pulse") == 0) {
    if (!isCount(msg["onMs"])) return sendError(client, "bad-msg", type, pin);
    uint32_t offMs = msg["offMs"] | 0u;
    uint32_t count = msg["count"] | 1u;
    sendResult(client, outputs_.pulse(pin, msg["onMs"], offMs, count), type, pin);
  } else if (strcmp(type, "pwm") == 0) {
    if (!isNumber(msg["duty"]) || !isCount(msg["ms"])) return sendError(client, "bad-msg", type, pin);
    sendResult(client, outputs_.pwm(pin, msg["duty"].as<float>(), msg["ms"]), type, pin);
  } else {
    sendError(client, "bad-msg", type);
  }
}

void Session::configure(uint32_t client, JsonObjectConst msg) {
  JsonArrayConst list = msg["outputs"];
  if (list.isNull() || list.size() > OutputBank::kMaxOutputs) return sendError(client, "bad-msg", "config");

  OutputConfig configs[OutputBank::kMaxOutputs];
  size_t count = 0;
  for (JsonObjectConst item : list) {
    if (!item["pin"].is<uint8_t>()) return sendError(client, "bad-msg", "config");

    uint8_t pin = item["pin"];
    if (!allowed(pin)) return sendError(client, "bad-pin", "config", pin);

    configs[count++] = {pin, item["activeLow"] | false, item["maxOnMs"] | OutputBank::kDefaultMaxOnMs};
  }

  if (outputs_.configure(configs, count) != OutputError::None) return sendError(client, "bad-pin", "config");

  settings_.outputCount = static_cast<uint8_t>(count);
  memcpy(settings_.outputs, configs, count * sizeof(OutputConfig));
  persist();
  hal::log("[out] %u output(s) configured\n", (unsigned)count);
}

void Session::sendWelcome(uint32_t client) {
  JsonDocument doc;
  doc["t"] = "welcome";
  doc["proto"] = TKOSC_PROTO;
  doc["id"] = device_.id;
  doc["fw"] = TKOSC_FW_VERSION;
  doc["name"] = settings_.name;
  doc["chip"] = device_.chip;
  doc["board"] = device_.board;

  JsonArray outputs = doc["outputs"].to<JsonArray>();
  for (size_t i = 0; i < allowedCount_; i++) {
    JsonObject item = outputs.add<JsonObject>();
    item["pin"] = allowed_[i].pin;
    item["pwm"] = allowed_[i].pwm;
    if (allowed_[i].label) item["label"] = allowed_[i].label;
  }

  serializeJson(doc, outBuffer, sizeof(outBuffer));
  send(client, outBuffer);
}

void Session::sendError(uint32_t client, const char* code, const char* ref, int pin) {
  JsonDocument doc;
  doc["t"] = "err";
  doc["code"] = code;
  if (ref) doc["ref"] = ref;
  if (pin >= 0) doc["pin"] = pin;
  serializeJson(doc, outBuffer, sizeof(outBuffer));
  send(client, outBuffer);
}

void Session::sendResult(uint32_t client, OutputError error, const char* ref, int pin) {
  const char* code = errorCode(error);
  if (code) sendError(client, code, ref, pin);
}

void Session::send(uint32_t client, const char* json) {
  if (send_ && client != kNoClient) send_(client, json);
}

void Session::close(uint32_t client, uint16_t code) {
  if (close_ && client != kNoClient) close_(client, code);
}

void Session::persist() {
  if (persist_) persist_();
}

bool Session::allowed(uint8_t pin) const {
  for (size_t i = 0; i < allowedCount_; i++) {
    if (allowed_[i].pin == pin) return true;
  }
  return false;
}
