#include <unity.h>

#include <string>
#include <vector>

#include "../fake_hal.h"
#include "session.h"

namespace {

struct Sent {
  uint32_t client;
  std::string json;
};

std::vector<Sent> sent;
std::vector<std::pair<uint32_t, uint16_t>> closed;
int persists = 0;

Settings settings;
OutputBank outputs;
const OutputPin allowed[] = {{4, true, "D4"}, {25, true, nullptr}};
const DeviceInfo device = {"a1b2c3", "ESP32", "Generic"};
Session* session = nullptr;

void text(uint32_t client, const std::string& json) {
  session->handleText(client, json.data(), json.size());
}

JsonDocument last(uint32_t client) {
  JsonDocument doc;
  for (auto it = sent.rbegin(); it != sent.rend(); ++it) {
    if (it->client == client) {
      deserializeJson(doc, it->json);
      break;
    }
  }
  return doc;
}

uint16_t closeCode(uint32_t client) {
  for (auto& c : closed) {
    if (c.first == client) return c.second;
  }
  return 0;
}

std::string pairAndConnect(uint32_t client) {
  text(client, R"({"t":"pair"})");
  session->confirmPairing();
  std::string token = last(client)["token"].as<std::string>();
  text(client, R"({"t":"hello","proto":1,"token":")" + token + R"("})");
  return token;
}

}  // namespace

void setUp() {
  fake::reset();
  sent.clear();
  closed.clear();
  persists = 0;
  settings = Settings();
  outputs = OutputBank();
  outputs.setCallbacks([](uint8_t pin, bool on, float duty) { session->reportOutput(pin, on, duty); },
                       [](uint8_t pin) { session->reportTrip(pin); });

  delete session;
  session = new Session(settings, outputs, device, allowed, 2);
  session->setTransport([](uint32_t client, const char* json) { sent.push_back({client, json}); },
                        [](uint32_t client, uint16_t code) { closed.push_back({client, code}); });
  session->setPersist([] { persists++; });
}

void tearDown() {}

void test_commands_before_hello_are_refused() {
  text(1, R"({"t":"set","pin":4,"on":true,"ms":500})");
  TEST_ASSERT_EQUAL(Session::kCloseUnauthorized, closeCode(1));
}

void test_bad_json() {
  text(1, "not json");
  TEST_ASSERT_EQUAL_STRING("bad-msg", last(1)["code"]);
}

void test_pairing_flow() {
  text(1, R"({"t":"pair"})");
  TEST_ASSERT_EQUAL_STRING("pairing", last(1)["t"]);
  TEST_ASSERT_EQUAL(30000, last(1)["timeoutMs"].as<int>());
  TEST_ASSERT_TRUE(session->pairingActive());

  TEST_ASSERT_TRUE(session->confirmPairing());
  JsonDocument paired = last(1);
  TEST_ASSERT_EQUAL_STRING("paired", paired["t"]);
  TEST_ASSERT_EQUAL(44u, strlen(paired["token"]));
  TEST_ASSERT_EQUAL_STRING(settings.token, paired["token"]);
  TEST_ASSERT_EQUAL(1, persists);

  text(1, std::string(R"({"t":"hello","proto":1,"token":")") + settings.token + R"("})");
  JsonDocument welcome = last(1);
  TEST_ASSERT_EQUAL_STRING("welcome", welcome["t"]);
  TEST_ASSERT_EQUAL_STRING("a1b2c3", welcome["id"]);
  TEST_ASSERT_EQUAL(2u, welcome["outputs"].size());
  TEST_ASSERT_EQUAL_STRING("D4", welcome["outputs"][0]["label"]);
  TEST_ASSERT_TRUE(welcome["outputs"][1]["label"].isNull());
  TEST_ASSERT_TRUE(session->connected());
}

void test_button_without_pairing_does_nothing() {
  TEST_ASSERT_FALSE(session->confirmPairing());
  TEST_ASSERT_FALSE(session->paired());
}

void test_pairing_times_out() {
  text(1, R"({"t":"pair"})");
  fake::now += Session::kPairWindowMs;
  session->update();
  TEST_ASSERT_FALSE(session->pairingActive());
  TEST_ASSERT_EQUAL_STRING("pair-timeout", last(1)["code"]);
  TEST_ASSERT_FALSE(session->confirmPairing());
}

void test_wrong_token() {
  pairAndConnect(1);
  text(2, R"({"t":"hello","proto":1,"token":"nope"})");
  TEST_ASSERT_EQUAL(Session::kCloseUnauthorized, closeCode(2));
}

void test_wrong_proto() {
  std::string token = pairAndConnect(1);
  text(2, R"({"t":"hello","proto":2,"token":")" + token + R"("})");
  TEST_ASSERT_EQUAL(Session::kCloseBadProto, closeCode(2));
}

void test_newer_session_replaces_older() {
  std::string token = pairAndConnect(1);
  text(2, R"({"t":"hello","proto":1,"token":")" + token + R"("})");
  TEST_ASSERT_EQUAL(Session::kCloseReplaced, closeCode(1));
  // The replaced socket can't send commands any more.
  text(1, R"({"t":"ping"})");
  TEST_ASSERT_EQUAL(1u, closed.back().first);
  TEST_ASSERT_EQUAL(Session::kCloseUnauthorized, closed.back().second);
}

void test_config_and_set() {
  pairAndConnect(1);
  text(1, R"({"t":"config","outputs":[{"pin":4,"maxOnMs":5000},{"pin":25,"activeLow":true}]})");
  TEST_ASSERT_EQUAL(2u, outputs.size());
  TEST_ASSERT_EQUAL(2, settings.outputCount);
  TEST_ASSERT_TRUE(settings.outputs[1].activeLow);

  text(1, R"({"t":"set","pin":4,"on":true,"ms":2000})");
  TEST_ASSERT_TRUE(outputs.isOn(4));
  JsonDocument out = last(1);
  TEST_ASSERT_EQUAL_STRING("out", out["t"]);
  TEST_ASSERT_EQUAL(4, out["pin"].as<int>());
  TEST_ASSERT_TRUE(out["on"].as<bool>());
}

void test_config_rejects_pins_not_offered() {
  pairAndConnect(1);
  text(1, R"({"t":"config","outputs":[{"pin":0}]})");
  TEST_ASSERT_EQUAL_STRING("bad-pin", last(1)["code"]);
  TEST_ASSERT_EQUAL(0u, outputs.size());
}

void test_set_on_unconfigured_pin() {
  pairAndConnect(1);
  text(1, R"({"t":"set","pin":4,"on":true,"ms":500})");
  JsonDocument err = last(1);
  TEST_ASSERT_EQUAL_STRING("bad-pin", err["code"]);
  TEST_ASSERT_EQUAL_STRING("set", err["ref"]);
}

void test_set_on_without_ms() {
  pairAndConnect(1);
  text(1, R"({"t":"config","outputs":[{"pin":4}]})");
  text(1, R"({"t":"set","pin":4,"on":true})");
  TEST_ASSERT_EQUAL_STRING("bad-msg", last(1)["code"]);
  TEST_ASSERT_FALSE(outputs.isOn(4));
}

void test_dead_man() {
  pairAndConnect(1);
  text(1, R"({"t":"config","outputs":[{"pin":4}]})");
  text(1, R"({"t":"set","pin":4,"on":true,"ms":5000})");

  fake::now += 2000;
  text(1, R"({"t":"ping"})");
  TEST_ASSERT_EQUAL_STRING("pong", last(1)["t"]);
  fake::now += 2000;
  session->update();
  TEST_ASSERT_TRUE(outputs.isOn(4));

  fake::now += 1500;
  session->update();
  TEST_ASSERT_FALSE(outputs.isOn(4));
  TEST_ASSERT_EQUAL(Session::kCloseTimeout, closeCode(1));
  TEST_ASSERT_FALSE(session->connected());
}

void test_disconnect_stops_outputs() {
  pairAndConnect(1);
  text(1, R"({"t":"config","outputs":[{"pin":4}]})");
  text(1, R"({"t":"set","pin":4,"on":true,"ms":5000})");
  session->handleDisconnect(1);
  TEST_ASSERT_FALSE(outputs.isOn(4));
}

void test_unpair_from_app() {
  pairAndConnect(1);
  text(1, R"({"t":"unpair"})");
  TEST_ASSERT_FALSE(session->paired());
  TEST_ASSERT_EQUAL(Session::kCloseNormal, closeCode(1));
}

void test_repairing_drops_old_session() {
  pairAndConnect(1);
  text(2, R"({"t":"pair"})");
  session->confirmPairing();
  TEST_ASSERT_EQUAL(Session::kCloseUnauthorized, closeCode(1));
  TEST_ASSERT_FALSE(session->connected());
}

void test_trip_reported() {
  pairAndConnect(1);
  text(1, R"({"t":"config","outputs":[{"pin":4,"maxOnMs":1000}]})");
  text(1, R"({"t":"set","pin":4,"on":true,"ms":5000})");
  fake::now += 1000;
  outputs.update();
  JsonDocument err = last(1);
  TEST_ASSERT_EQUAL_STRING("max-on", err["code"]);
  TEST_ASSERT_EQUAL(4, err["pin"].as<int>());
}

// The command log only prints: what the board does and answers is the same either way.
void test_command_log_changes_nothing() {
  fake::verbose = true;
  pairAndConnect(1);
  text(1, R"({"t":"config","outputs":[{"pin":4}]})");

  for (bool on : {true, false}) {
    session->setCommandLog(on);
    TEST_ASSERT_EQUAL(on, session->commandLog());

    text(1, R"({"t":"set","pin":25,"on":true,"ms":500})");
    TEST_ASSERT_EQUAL_STRING("bad-pin", last(1)["code"]);

    text(1, R"({"t":"pulse","pin":4,"onMs":200,"offMs":100,"count":2})");
    TEST_ASSERT_TRUE(outputs.isOn(4));
    text(1, R"({"t":"stop"})");
    TEST_ASSERT_FALSE(outputs.isOn(4));
  }
  fake::verbose = false;
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_commands_before_hello_are_refused);
  RUN_TEST(test_bad_json);
  RUN_TEST(test_pairing_flow);
  RUN_TEST(test_button_without_pairing_does_nothing);
  RUN_TEST(test_pairing_times_out);
  RUN_TEST(test_wrong_token);
  RUN_TEST(test_wrong_proto);
  RUN_TEST(test_newer_session_replaces_older);
  RUN_TEST(test_config_and_set);
  RUN_TEST(test_config_rejects_pins_not_offered);
  RUN_TEST(test_set_on_unconfigured_pin);
  RUN_TEST(test_set_on_without_ms);
  RUN_TEST(test_dead_man);
  RUN_TEST(test_disconnect_stops_outputs);
  RUN_TEST(test_unpair_from_app);
  RUN_TEST(test_repairing_drops_old_session);
  RUN_TEST(test_trip_reported);
  RUN_TEST(test_command_log_changes_nothing);
  return UNITY_END();
}
