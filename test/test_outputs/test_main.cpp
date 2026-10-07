#include <unity.h>

#include "../fake_hal.h"
#include "outputs.h"

#define ASSERT_ERR(expected, actual) TEST_ASSERT_EQUAL_INT(static_cast<int>(expected), static_cast<int>(actual))

namespace {

OutputBank bank;
int changes = 0;
int trips = 0;

void advance(uint32_t ms) {
  // Small steps, like loop() would.
  for (uint32_t i = 0; i < ms; i += 10) {
    fake::now += 10;
    bank.update();
  }
}

void configure(uint32_t maxOnMs = 0, bool activeLow = false) {
  OutputConfig configs[] = {{4, activeLow, maxOnMs}, {25, false, 0}};
  ASSERT_ERR(OutputError::None, bank.configure(configs, 2));
}

}  // namespace

void setUp() {
  fake::reset();
  bank = OutputBank();
  changes = 0;
  trips = 0;
  bank.setCallbacks([](uint8_t, bool, float) { changes++; }, [](uint8_t) { trips++; });
}

void tearDown() {}

void test_configure_drives_off_level() {
  configure(0, true);
  TEST_ASSERT_TRUE(fake::level[4]);  // active low: off is HIGH
  TEST_ASSERT_FALSE(fake::level[25]);
}

void test_configure_rejects_duplicates() {
  OutputConfig configs[] = {{4, false, 0}, {4, false, 0}};
  ASSERT_ERR(OutputError::BadPin, bank.configure(configs, 2));
}

void test_unknown_pin() {
  configure();
  ASSERT_ERR(OutputError::BadPin, bank.set(5, true, 100));
}

void test_set_switches_off_at_deadline() {
  configure();
  ASSERT_ERR(OutputError::None, bank.set(4, true, 2000));
  TEST_ASSERT_TRUE(fake::level[4]);
  advance(1990);
  TEST_ASSERT_TRUE(bank.isOn(4));
  advance(20);
  TEST_ASSERT_FALSE(bank.isOn(4));
  TEST_ASSERT_FALSE(fake::level[4]);
  TEST_ASSERT_EQUAL(0, trips);
}

void test_shorter_on_does_not_cut_a_longer_one() {
  configure();
  bank.set(4, true, 2000);
  advance(500);
  bank.set(4, true, 100);
  advance(500);
  TEST_ASSERT_TRUE(bank.isOn(4));
}

void test_later_on_extends() {
  configure();
  bank.set(4, true, 1000);
  advance(800);
  bank.set(4, true, 1000);
  advance(800);
  TEST_ASSERT_TRUE(bank.isOn(4));
  advance(300);
  TEST_ASSERT_FALSE(bank.isOn(4));
}

void test_off_is_immediate() {
  configure();
  bank.set(4, true, 5000);
  bank.set(4, false, 0);
  TEST_ASSERT_FALSE(bank.isOn(4));
}

void test_on_needs_ms() {
  configure();
  ASSERT_ERR(OutputError::BadValue, bank.set(4, true, 0));
}

void test_renewed_lease_trips_at_max_on() {
  configure(1500);
  // A hold: 1 s lease renewed every 500 ms.
  for (int i = 0; i < 4; i++) {
    bank.set(4, true, 1000);
    advance(500);
  }
  TEST_ASSERT_FALSE(bank.isOn(4));
  TEST_ASSERT_TRUE(bank.isTripped(4));
  TEST_ASSERT_EQUAL(1, trips);

  ASSERT_ERR(OutputError::Tripped, bank.set(4, true, 1000));
  TEST_ASSERT_FALSE(bank.isOn(4));

  bank.set(4, false, 0);
  ASSERT_ERR(OutputError::None, bank.set(4, true, 1000));
  TEST_ASSERT_TRUE(bank.isOn(4));
}

void test_pulse_train() {
  configure();
  ASSERT_ERR(OutputError::None, bank.pulse(4, 200, 300, 3));

  int ons = 0;
  bool was = false;
  for (int t = 0; t < 2000; t += 10) {
    if (bank.isOn(4) && !was) ons++;
    was = bank.isOn(4);
    advance(10);
  }
  TEST_ASSERT_EQUAL(3, ons);
  TEST_ASSERT_FALSE(bank.isOn(4));
}

void test_long_pulse_is_shortened_not_tripped() {
  configure(500);
  bank.pulse(4, 2000, 0, 1);
  advance(600);
  TEST_ASSERT_FALSE(bank.isOn(4));
  TEST_ASSERT_FALSE(bank.isTripped(4));
}

void test_pwm_inverts_for_active_low() {
  configure(0, true);
  ASSERT_ERR(OutputError::None, bank.pwm(4, 0.25f, 1000));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.75f, fake::pwm[4]);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.25f, bank.duty(4));
  advance(1010);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1.0f, fake::pwm[4]);
}

void test_pwm_out_of_channels() {
  configure();
  fake::pwmChannelsLeft = 0;
  ASSERT_ERR(OutputError::NotPwm, bank.pwm(4, 0.5f, 1000));
}

void test_pwm_rejects_bad_duty() {
  configure();
  ASSERT_ERR(OutputError::BadValue, bank.pwm(4, 1.5f, 1000));
}

void test_stop() {
  configure();
  bank.set(4, true, 5000);
  bank.pulse(25, 100, 100, 10);
  bank.stop();
  advance(500);
  TEST_ASSERT_FALSE(bank.isOn(4));
  TEST_ASSERT_FALSE(bank.isOn(25));
}

void test_reconfigure_releases_dropped_pins() {
  configure();
  bank.set(25, true, 5000);
  OutputConfig configs[] = {{4, false, 0}};
  bank.configure(configs, 1);
  TEST_ASSERT_EQUAL(0u, fake::level.count(25));
  ASSERT_ERR(OutputError::BadPin, bank.set(25, true, 100));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_configure_drives_off_level);
  RUN_TEST(test_configure_rejects_duplicates);
  RUN_TEST(test_unknown_pin);
  RUN_TEST(test_set_switches_off_at_deadline);
  RUN_TEST(test_shorter_on_does_not_cut_a_longer_one);
  RUN_TEST(test_later_on_extends);
  RUN_TEST(test_off_is_immediate);
  RUN_TEST(test_on_needs_ms);
  RUN_TEST(test_renewed_lease_trips_at_max_on);
  RUN_TEST(test_pulse_train);
  RUN_TEST(test_long_pulse_is_shortened_not_tripped);
  RUN_TEST(test_pwm_inverts_for_active_low);
  RUN_TEST(test_pwm_out_of_channels);
  RUN_TEST(test_pwm_rejects_bad_duty);
  RUN_TEST(test_stop);
  RUN_TEST(test_reconfigure_releases_dropped_pins);
  return UNITY_END();
}
