#pragma once
// hal for the tests on the PC: a clock the test moves by hand and pins that just remember their
// level. Include it from exactly one file per test folder.

#include <stdarg.h>
#include <stdio.h>

#include <map>

#include "hal.h"

namespace fake {

uint32_t now = 1000;
std::map<uint8_t, bool> level;
std::map<uint8_t, float> pwm;
int pwmChannelsLeft = 8;
bool verbose = false;

void reset() {
  now = 1000;
  level.clear();
  pwm.clear();
  pwmChannelsLeft = 8;
}

}  // namespace fake

namespace hal {

uint32_t millis() {
  return fake::now;
}

void pinOutput(uint8_t) {}

void pinRelease(uint8_t pin) {
  fake::level.erase(pin);
}

void pinWrite(uint8_t pin, bool high) {
  fake::level[pin] = high;
}

bool pwmAttach(uint8_t) {
  if (fake::pwmChannelsLeft == 0) return false;
  fake::pwmChannelsLeft--;
  return true;
}

void pwmDetach(uint8_t pin) {
  fake::pwm.erase(pin);
  fake::pwmChannelsLeft++;
}

void pwmWrite(uint8_t pin, float duty) {
  fake::pwm[pin] = duty;
}

void randomBytes(uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; i++) out[i] = static_cast<uint8_t>(i * 7 + 1);
}

void log(const char* fmt, ...) {
  if (!fake::verbose) return;
  va_list args;
  va_start(args, fmt);
  vprintf(fmt, args);
  va_end(args);
}

}  // namespace hal
