#pragma once
// The little the shared core needs from the hardware. Implemented by src/platform/esp32/main.cpp
// (real pins) and by test/fake_hal.h for the tests on the PC.

#include <stddef.h>
#include <stdint.h>

namespace hal {

uint32_t millis();
void pinOutput(uint8_t pin);
// Back to a plain input, for a pin that is no longer configured as an output.
void pinRelease(uint8_t pin);
void pinWrite(uint8_t pin, bool high);
// Attaches `pin` to a PWM channel. Returns false when no channel is left.
bool pwmAttach(uint8_t pin);
void pwmDetach(uint8_t pin);
// duty 0..1 as the pin should see it (the caller has already inverted it for active-low outputs).
void pwmWrite(uint8_t pin, float duty);
void randomBytes(uint8_t* out, size_t len);
void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace hal
