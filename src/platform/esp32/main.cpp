// The ESP32 side: Wi-Fi setup, mDNS, the WebSocket, NVS, the BOOT button, the status LED and
// serial commands. The protocol and the outputs live in src/core, shared with the tests.

#include <Arduino.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_arduino_version.h>
#include <mdns.h>
#include <soc/soc_caps.h>

#include "config.h"
#include "hal.h"
#include "outputs.h"
#include "pins.h"
#include "session.h"
#include "settings.h"
#include "util.h"

// ---------------------------------------------------------------------------------------------
// hal

namespace hal {

uint32_t millis() {
  return ::millis();
}

void pinOutput(uint8_t pin) {
  pinMode(pin, OUTPUT);
}

void pinRelease(uint8_t pin) {
  pinMode(pin, INPUT);
}

void pinWrite(uint8_t pin, bool high) {
  digitalWrite(pin, high ? HIGH : LOW);
}

constexpr uint32_t kPwmMax = (1u << PWM_BITS) - 1;

#if ESP_ARDUINO_VERSION_MAJOR >= 3

bool pwmAttach(uint8_t pin) {
  return ledcAttach(pin, PWM_FREQ_HZ, PWM_BITS);
}

void pwmDetach(uint8_t pin) {
  ledcDetach(pin);
}

void pwmWrite(uint8_t pin, float duty) {
  ledcWrite(pin, static_cast<uint32_t>(duty * kPwmMax + 0.5f));
}

#else

// Arduino-ESP32 2.x addresses LEDC by channel, so keep a pin -> channel map.
int8_t pwmChannel[SOC_GPIO_PIN_COUNT];
uint32_t pwmChannelsUsed = 0;

bool pwmAttach(uint8_t pin) {
  for (uint8_t ch = 0; ch < SOC_LEDC_CHANNEL_NUM; ch++) {
    if (pwmChannelsUsed & (1u << ch)) continue;
    ledcSetup(ch, PWM_FREQ_HZ, PWM_BITS);
    ledcAttachPin(pin, ch);
    pwmChannelsUsed |= 1u << ch;
    pwmChannel[pin] = ch;
    return true;
  }
  return false;
}

void pwmDetach(uint8_t pin) {
  ledcDetachPin(pin);
  pwmChannelsUsed &= ~(1u << pwmChannel[pin]);
}

void pwmWrite(uint8_t pin, float duty) {
  ledcWrite(pwmChannel[pin], static_cast<uint32_t>(duty * kPwmMax + 0.5f));
}

#endif

void randomBytes(uint8_t* out, size_t len) {
  // Hardware RNG. Truly random once the radio is on, which it is by the time anyone pairs.
  for (size_t i = 0; i < len; i += 4) {
    uint32_t r = esp_random();
    for (size_t j = 0; j < 4 && i + j < len; j++) out[i + j] = static_cast<uint8_t>(r >> (8 * j));
  }
}

void log(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
}

}  // namespace hal

// ---------------------------------------------------------------------------------------------

namespace {

Preferences prefs;
Settings settings;
OutputBank outputs;
ModuleInfo module;
const BoardProfile* board = nullptr;
OutputPin allowedPins[kMaxOutputPins];
size_t allowedCount = 0;

char deviceId[7];
char hostname[24];
char apName[40];
DeviceInfo device;
Session* session = nullptr;

AsyncWebServer server(WS_PORT);
AsyncWebSocket ws(WS_PATH);

WiFiManager wm;
WiFiManagerParameter nameParam("name", "Board name (shown in ToolKitOSC)", "", sizeof(Settings::name) - 1);
char boardHelp[400];
WiFiManagerParameter boardHelpParam(boardHelp);
WiFiManagerParameter boardParam("board", "Board profile", "", sizeof(Settings::board) - 1);

uint32_t restartAt = 0;
bool mdnsStarted = false;

// Socket callbacks run in the AsyncTCP task. They only copy frames into this queue; the loop does
// the rest, so the session and the outputs are only ever touched from one task.
struct SocketEvent {
  enum Type : uint8_t { Text, Closed } type;
  uint32_t client;
  uint16_t len;
  char text[MAX_FRAME + 1];
};
QueueHandle_t socketEvents;

// ---------------------------------------------------------------------------------------------
// Settings

void updateMdnsTxt() {
  if (!mdnsStarted) return;
  mdns_service_txt_item_set("_" MDNS_SERVICE, "_tcp", "paired", settings.token[0] ? "1" : "0");
  mdns_service_txt_item_set("_" MDNS_SERVICE, "_tcp", "name", settings.name);
}

void saveSettings() {
  prefs.putBytes("settings", &settings, sizeof(settings));
  updateMdnsTxt();
}

void loadSettings() {
  Settings stored;
  if (prefs.getBytesLength("settings") == sizeof(Settings) &&
      prefs.getBytes("settings", &stored, sizeof(stored)) == sizeof(stored) && stored.version == Settings::kVersion) {
    settings = stored;
  }

  settings.name[sizeof(settings.name) - 1] = '\0';
  settings.token[sizeof(settings.token) - 1] = '\0';
  settings.board[sizeof(settings.board) - 1] = '\0';
  if (settings.outputCount > OutputBank::kMaxOutputs) settings.outputCount = 0;

  if (!settings.name[0]) snprintf(settings.name, sizeof(settings.name), "Board %s", deviceId);
#ifdef TKOSC_DEFAULT_BOARD
  if (!settings.board[0]) copyString(settings.board, sizeof(settings.board), TKOSC_DEFAULT_BOARD);
#endif
}

void factoryReset() {
  hal::log("[reset] factory reset: forgetting the pairing, Wi-Fi, name and outputs\n");
  outputs.stop();
  prefs.clear();
  wm.resetSettings();
  delay(300);
  ESP.restart();
}

// Each boot bumps a counter that goes back to 0 once the board has run for
// POWER_CYCLE_WINDOW_MS. Reaching POWER_CYCLE_RESET_COUNT means someone is power-cycling it on
// purpose.
void countPowerCycle() {
  uint8_t cycles = prefs.getUChar("cycles", 0) + 1;
  if (cycles >= POWER_CYCLE_RESET_COUNT) {
    hal::log("[reset] powered on %u times in a row\n", (unsigned)cycles);
    factoryReset();
  }
  prefs.putUChar("cycles", cycles);
  if (cycles > 1) {
    hal::log("[reset] quick power cycle %u of %u\n", (unsigned)cycles, (unsigned)POWER_CYCLE_RESET_COUNT);
  }
}

void clearPowerCycles() {
  static bool cleared = false;
  if (cleared || millis() < POWER_CYCLE_WINDOW_MS) return;
  prefs.putUChar("cycles", 0);
  cleared = true;
}

// ---------------------------------------------------------------------------------------------
// Detection

ModuleInfo detectModule() {
  ModuleInfo m{};
#if CONFIG_IDF_TARGET_ESP32
  m.chip = Chip::Esp32;
#elif CONFIG_IDF_TARGET_ESP32S2
  m.chip = Chip::Esp32S2;
#elif CONFIG_IDF_TARGET_ESP32S3
  m.chip = Chip::Esp32S3;
#elif CONFIG_IDF_TARGET_ESP32C3
  m.chip = Chip::Esp32C3;
#else
#error "This chip isn't supported yet: add its pin table to src/core/pins.cpp"
#endif
  m.psram = psramFound();
  m.picoPackage = strstr(ESP.getChipModel(), "PICO") != nullptr;
  return m;
}

void makeDeviceId() {
  uint64_t mac = ESP.getEfuseMac();
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = static_cast<uint8_t>(mac >> (8 * i));
  snprintf(deviceId, sizeof(deviceId), "%02x%02x%02x", b[3], b[4], b[5]);
  snprintf(hostname, sizeof(hostname), HOSTNAME_PREFIX "%s", deviceId);
  snprintf(apName, sizeof(apName), SETUP_AP_PREFIX "%s", deviceId);
}

void resolvePins() {
  board = findBoardProfile(settings.board, module.chip);
  allowedCount = resolveOutputPins(module, board, allowedPins, kMaxOutputPins);

  // Drop saved outputs the current profile no longer allows.
  uint8_t kept = 0;
  for (uint8_t i = 0; i < settings.outputCount; i++) {
    for (size_t j = 0; j < allowedCount; j++) {
      if (allowedPins[j].pin == settings.outputs[i].pin) {
        settings.outputs[kept++] = settings.outputs[i];
        break;
      }
    }
  }
  settings.outputCount = kept;
}

void printPins() {
  hal::log("[pins] %u output pin(s):", (unsigned)allowedCount);
  for (size_t i = 0; i < allowedCount; i++) {
    if (allowedPins[i].label) {
      hal::log(" %u(%s)", allowedPins[i].pin, allowedPins[i].label);
    } else {
      hal::log(" %u", allowedPins[i].pin);
    }
  }
  hal::log("\n");
}

void printStatus() {
  hal::log("\nToolKitOSC board %s, firmware %s, protocol %d\n", deviceId, TKOSC_FW_VERSION, TKOSC_PROTO);
  hal::log("  name     %s\n", settings.name);
  hal::log("  chip     %s (%s)%s%s\n", chipName(module.chip), ESP.getChipModel(), module.psram ? ", PSRAM" : "",
           module.picoPackage ? ", PICO package" : "");
  hal::log("  board    %s\n", device.board);
  hal::log("  paired   %s\n", session && session->paired() ? "yes" : "no");
  hal::log("  app      %s\n", session && session->connected() ? "connected" : "not connected");
  if (WiFi.status() == WL_CONNECTED) {
    hal::log("  network  %s, ws://%s:%u%s (%s.local)\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
             WS_PORT, WS_PATH, hostname);
  } else {
    hal::log("  network  not connected%s\n", wm.getConfigPortalActive() ? ", setup access point open" : "");
  }
  printPins();
}

// ---------------------------------------------------------------------------------------------
// Network

void sendText(uint32_t client, const char* json) {
  ws.text(client, json);
}

void closeSocket(uint32_t client, uint16_t code) {
  ws.close(client, code);
}

void onSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, AwsEventType type, void* arg, uint8_t* data,
                   size_t len) {
  static SocketEvent ev;  // only touched from the AsyncTCP task

  switch (type) {
    case WS_EVT_CONNECT:
      if (server->count() > MAX_SOCKETS) {
        client->close(1013);
        return;
      }
      hal::log("[ws] #%u connected from %s\n", (unsigned)client->id(), client->remoteIP().toString().c_str());
      return;

    case WS_EVT_DISCONNECT:
      ev.type = SocketEvent::Closed;
      ev.client = client->id();
      ev.len = 0;
      xQueueSend(socketEvents, &ev, pdMS_TO_TICKS(100));
      return;

    case WS_EVT_DATA: {
      auto* info = static_cast<AwsFrameInfo*>(arg);
      if (!info->final || info->index != 0 || info->len != len || info->opcode != WS_TEXT || len > MAX_FRAME) {
        hal::log("[ws] #%u sent a fragmented, binary or oversized frame, ignored\n", (unsigned)client->id());
        return;
      }
      ev.type = SocketEvent::Text;
      ev.client = client->id();
      ev.len = static_cast<uint16_t>(len);
      memcpy(ev.text, data, len);
      ev.text[len] = '\0';
      if (xQueueSend(socketEvents, &ev, pdMS_TO_TICKS(100)) != pdTRUE) hal::log("[ws] queue full, frame dropped\n");
      return;
    }

    default:
      return;
  }
}

void drainSocketEvents() {
  static SocketEvent ev;
  while (xQueueReceive(socketEvents, &ev, 0) == pdTRUE) {
    if (ev.type == SocketEvent::Closed) {
      session->handleDisconnect(ev.client);
    } else {
      session->handleText(ev.client, ev.text, ev.len);
    }
  }
}

void startMdns() {
  if (!MDNS.begin(hostname)) {
    hal::log("[mdns] failed to start\n");
    return;
  }
  char proto[8];
  snprintf(proto, sizeof(proto), "%d", TKOSC_PROTO);

  // Plain const char* on purpose: mixing literals and char buffers makes ESPmDNS's overloads ambiguous.
  auto txt = [](const char* key, const char* value) { MDNS.addServiceTxt(MDNS_SERVICE, "tcp", key, value); };

  MDNS.addService(MDNS_SERVICE, "tcp", WS_PORT);
  txt("id", deviceId);
  txt("name", settings.name);
  txt("proto", proto);
  txt("fw", TKOSC_FW_VERSION);
  txt("paired", settings.token[0] ? "1" : "0");
  mdnsStarted = true;
  hal::log("[mdns] announcing _%s._tcp on %s.local:%u\n", MDNS_SERVICE, hostname, WS_PORT);
}

void onSaveParams() {
  const char* name = nameParam.getValue();
  if (name[0]) copyString(settings.name, sizeof(settings.name), name);

  const char* id = boardParam.getValue();
  if (strcmp(id, "generic") == 0 || findBoardProfile(id, module.chip)) {
    copyString(settings.board, sizeof(settings.board), id);
  } else if (id[0]) {
    hal::log("[setup] unknown board profile \"%s\", kept \"%s\"\n", id, settings.board);
  }

  saveSettings();
  hal::log("[setup] saved, restarting\n");
  restartAt = millis() + 1500;
}

void buildBoardHelp() {
  const BoardProfile* profiles[8];
  size_t n = boardProfilesFor(module.chip, profiles, 8);

  size_t o = snprintf(boardHelp, sizeof(boardHelp),
                      "<p>Board profile: limits the outputs to the pins your board breaks out. One of: "
                      "<code>generic</code>");
  for (size_t i = 0; i < n && o < sizeof(boardHelp); i++) {
    o += snprintf(boardHelp + o, sizeof(boardHelp) - o, ", <code>%s</code> (%s)", profiles[i]->id, profiles[i]->name);
  }
  if (o < sizeof(boardHelp)) snprintf(boardHelp + o, sizeof(boardHelp) - o, ".</p>");
}

void startWifi() {
  WiFi.mode(WIFI_STA);
  wm.setHostname(hostname);
  wm.setTitle("ToolKitOSC board");
  wm.setConfigPortalBlocking(false);
  wm.setSaveParamsCallback(onSaveParams);

  nameParam.setValue(settings.name, sizeof(settings.name) - 1);
  boardParam.setValue(settings.board, sizeof(settings.board) - 1);
  buildBoardHelp();
  wm.addParameter(&nameParam);
  wm.addParameter(&boardHelpParam);
  wm.addParameter(&boardParam);

  if (wm.autoConnect(apName)) {
    hal::log("[wifi] connected\n");
  } else {
    hal::log("[wifi] no saved network: join the \"%s\" Wi-Fi to set it up\n", apName);
  }
}

void handleWifi() {
  static bool wasConnected = false;
  static uint32_t lostSince = 0;
  static bool portalForLoss = false;

  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !wasConnected) {
    hal::log("[wifi] %s, IP %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    if (!mdnsStarted) startMdns();
    if (portalForLoss) {
      wm.stopConfigPortal();
      portalForLoss = false;
    }
  } else if (!connected && wasConnected) {
    hal::log("[wifi] connection lost\n");
    lostSince = millis();
  }

  if (!connected && lostSince && !portalForLoss && !wm.getConfigPortalActive() &&
      millis() - lostSince >= WIFI_LOST_PORTAL_MS) {
    hal::log("[wifi] still offline, opening the \"%s\" setup access point (pairing is kept)\n", apName);
    wm.startConfigPortal(apName);
    portalForLoss = true;
  }

  wasConnected = connected;
}

// ---------------------------------------------------------------------------------------------
// BOOT button and status LED

struct {
  uint8_t pin;
  bool down;
  uint32_t since;
  uint8_t stage;  // 0 none, 1 unpair reached, 2 reset reached (for the log and the LED)
} button;

void handleButton() {
  bool down = digitalRead(button.pin) == LOW;
  uint32_t now = millis();

  if (down && !button.down) {
    button.down = true;
    button.since = now;
    button.stage = 0;
    return;
  }

  if (down) {
    uint32_t held = now - button.since;
    if (held >= HOLD_RESET_MS && button.stage < 2) {
      button.stage = 2;
      hal::log("[button] let go now to factory reset\n");
    } else if (held >= HOLD_UNPAIR_MS && button.stage < 1) {
      button.stage = 1;
      hal::log("[button] let go now to unpair, or keep holding to factory reset\n");
    }
    return;
  }

  if (!button.down) return;
  button.down = false;
  uint32_t held = now - button.since;

  if (held < 30) return;  // bounce
  if (button.stage == 2) {
    factoryReset();
  } else if (button.stage == 1) {
    session->unpair();
  } else if (!session->confirmPairing()) {
    hal::log("[button] nothing is waiting for a press (hold 3 s to unpair, 10 s to factory reset)\n");
  }
}

void setLed(uint8_t r, uint8_t g, uint8_t b) {
  static uint32_t last = 0xffffffff;
  uint32_t color = (uint32_t(r) << 16) | (uint32_t(g) << 8) | b;
  if (color == last || !board) return;
  last = color;

  for (const StatusLed& led : board->leds) {
    if (led.pin < 0) continue;
    if (led.rgb) {
      neopixelWrite(led.pin, r, g, b);
    } else {
      digitalWrite(led.pin, (color != 0) != led.activeLow ? HIGH : LOW);
    }
  }
}

void updateLed() {
  uint32_t now = millis();
  bool blinkFast = (now / 150) % 2;
  bool blinkSlow = (now / 500) % 2;

  if (button.down && button.stage == 2) {
    setLed(40, 0, 0);
  } else if (button.down && button.stage == 1) {
    setLed(blinkSlow ? 40 : 0, blinkSlow ? 20 : 0, 0);
  } else if (session->pairingActive()) {
    setLed(0, 0, blinkFast ? 40 : 0);
  } else if (session->connected()) {
    setLed(0, 12, 0);
  } else if (WiFi.status() != WL_CONNECTED) {
    setLed(blinkSlow ? 20 : 0, blinkSlow ? 12 : 0, 0);
  } else {
    setLed(0, 0, 0);
  }
}

void initLeds() {
  if (!board) return;
  for (const StatusLed& led : board->leds) {
    if (led.pin < 0 || led.rgb) continue;
    pinMode(led.pin, OUTPUT);
    digitalWrite(led.pin, led.activeLow ? HIGH : LOW);
  }
}

// ---------------------------------------------------------------------------------------------
// Serial commands

void printHelp() {
  hal::log("Commands: status, pins, boards, name <text>, board <id>, unpair, reset, restart, help\n");
}

void runCommand(char* line) {
  char* arg = strchr(line, ' ');
  if (arg) {
    *arg++ = '\0';
    while (*arg == ' ') arg++;
  }

  if (strcmp(line, "status") == 0) {
    printStatus();
  } else if (strcmp(line, "pins") == 0) {
    printPins();
  } else if (strcmp(line, "boards") == 0) {
    hal::log("%s\n", boardHelp);
  } else if (strcmp(line, "name") == 0 && arg && *arg) {
    copyString(settings.name, sizeof(settings.name), arg);
    saveSettings();
    hal::log("[setup] name is now \"%s\"\n", settings.name);
  } else if (strcmp(line, "board") == 0 && arg && *arg) {
    if (strcmp(arg, "generic") != 0 && !findBoardProfile(arg, module.chip)) {
      hal::log("[setup] unknown board profile for this chip, see \"boards\"\n");
      return;
    }
    copyString(settings.board, sizeof(settings.board), arg);
    saveSettings();
    hal::log("[setup] board profile saved, restarting\n");
    restartAt = millis() + 500;
  } else if (strcmp(line, "unpair") == 0) {
    session->unpair();
  } else if (strcmp(line, "reset") == 0) {
    factoryReset();
  } else if (strcmp(line, "restart") == 0) {
    ESP.restart();
  } else if (line[0]) {
    printHelp();
  }
}

void handleSerial() {
  static char line[96];
  static size_t len = 0;

  while (Serial.available()) {
    char c = static_cast<char>(Serial.read());
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = '\0';
      runCommand(line);
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = c;
    }
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(500);  // lets a USB serial monitor attach before the first logs

  module = detectModule();
  makeDeviceId();

  prefs.begin("tkosc", false);
  countPowerCycle();
  loadSettings();
  resolvePins();

  // Outputs off before anything else, Wi-Fi included.
  outputs.setCallbacks([](uint8_t pin, bool on, float duty) { session->reportOutput(pin, on, duty); },
                       [](uint8_t pin) { session->reportTrip(pin); });
  outputs.configure(settings.outputs, settings.outputCount);

  device = {deviceId, chipName(module.chip), board ? board->name : "Generic"};
  session = new Session(settings, outputs, device, allowedPins, allowedCount);
  session->setTransport(sendText, closeSocket);
  session->setPersist(saveSettings);

  button.pin = bootButtonPin(module.chip);
  pinMode(button.pin, INPUT_PULLUP);
  initLeds();

  printStatus();
  printHelp();

  socketEvents = xQueueCreate(6, sizeof(SocketEvent));
  startWifi();
  ws.onEvent(onSocketEvent);
  server.addHandler(&ws);
  server.begin();
}

void loop() {
  static uint32_t lastCleanup = 0;

  wm.process();
  handleWifi();
  drainSocketEvents();
  session->update();
  outputs.update();
  handleButton();
  handleSerial();
  updateLed();
  clearPowerCycles();

  if (millis() - lastCleanup >= 1000) {
    ws.cleanupClients(MAX_SOCKETS);
    lastCleanup = millis();
  }

  if (restartAt && static_cast<int32_t>(millis() - restartAt) >= 0) ESP.restart();

  delay(1);
}
