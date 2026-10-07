#include <unity.h>

#include "../fake_hal.h"
#include "pins.h"

namespace {

OutputPin pins[kMaxOutputPins];

bool has(size_t count, uint8_t pin) {
  for (size_t i = 0; i < count; i++) {
    if (pins[i].pin == pin) return true;
  }
  return false;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_esp32_generic() {
  size_t n = resolveOutputPins({Chip::Esp32, false, false}, nullptr, pins, kMaxOutputPins);
  TEST_ASSERT_EQUAL(14u, n);
  TEST_ASSERT_TRUE(has(n, 16));
  TEST_ASSERT_FALSE(has(n, 0));
  TEST_ASSERT_FALSE(has(n, 12));
  TEST_ASSERT_FALSE(has(n, 34));
}

void test_esp32_psram_drops_16_17() {
  size_t n = resolveOutputPins({Chip::Esp32, true, false}, nullptr, pins, kMaxOutputPins);
  TEST_ASSERT_FALSE(has(n, 16));
  TEST_ASSERT_FALSE(has(n, 17));
  TEST_ASSERT_TRUE(has(n, 18));
}

void test_esp32_pico_drops_16_17() {
  size_t n = resolveOutputPins({Chip::Esp32, false, true}, nullptr, pins, kMaxOutputPins);
  TEST_ASSERT_FALSE(has(n, 16));
}

void test_s3_never_offers_33_to_37() {
  size_t n = resolveOutputPins({Chip::Esp32S3, false, false}, nullptr, pins, kMaxOutputPins);
  for (uint8_t pin = 33; pin <= 37; pin++) TEST_ASSERT_FALSE(has(n, pin));
  TEST_ASSERT_FALSE(has(n, 19));
  TEST_ASSERT_FALSE(has(n, 0));
}

void test_profile_needs_matching_chip() {
  TEST_ASSERT_NULL(findBoardProfile("xiao-esp32c3", Chip::Esp32S3));
  TEST_ASSERT_NOT_NULL(findBoardProfile("xiao-esp32c3", Chip::Esp32C3));
  TEST_ASSERT_NULL(findBoardProfile("generic", Chip::Esp32C3));
  TEST_ASSERT_NULL(findBoardProfile("", Chip::Esp32C3));
}

void test_xiao_c3_labels() {
  const BoardProfile* board = findBoardProfile("xiao-esp32c3", Chip::Esp32C3);
  size_t n = resolveOutputPins({Chip::Esp32C3, false, false}, board, pins, kMaxOutputPins);
  TEST_ASSERT_EQUAL(6u, n);
  TEST_ASSERT_EQUAL(3, pins[0].pin);
  TEST_ASSERT_EQUAL_STRING("D1", pins[0].label);
  TEST_ASSERT_FALSE(has(n, 9));
}

void test_profiles_only_use_pins_their_chip_allows() {
  const BoardProfile* profiles[16];
  Chip chips[] = {Chip::Esp32, Chip::Esp32S2, Chip::Esp32S3, Chip::Esp32C3};
  for (Chip chip : chips) {
    size_t count = boardProfilesFor(chip, profiles, 16);
    for (size_t i = 0; i < count; i++) {
      size_t n = resolveOutputPins({chip, false, false}, profiles[i], pins, kMaxOutputPins);
      TEST_ASSERT_EQUAL_MESSAGE(profiles[i]->pinCount, n, profiles[i]->id);
    }
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_esp32_generic);
  RUN_TEST(test_esp32_psram_drops_16_17);
  RUN_TEST(test_esp32_pico_drops_16_17);
  RUN_TEST(test_s3_never_offers_33_to_37);
  RUN_TEST(test_profile_needs_matching_chip);
  RUN_TEST(test_xiao_c3_labels);
  RUN_TEST(test_profiles_only_use_pins_their_chip_allows);
  return UNITY_END();
}
