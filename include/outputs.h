#pragma once
#include <stddef.h>
#include <stdint.h>

struct OutputConfig {
  uint8_t pin;
  bool activeLow;
  uint32_t maxOnMs;  // 0 = kDefaultMaxOnMs
};

enum class OutputError : uint8_t { None, BadPin, NotPwm, BadValue, Tripped };

// Owns the configured output pins. Every "on" carries a deadline and update() switches the pin
// off when it runs out, so nothing stays on unless the app keeps renewing it.
//
// maxOnMs caps how long a pin stays on in one go. When a "set" or "pwm" lease is cut short by it,
// the pin trips: further "on" commands are refused until it gets an explicit off or a stop. That
// way an app renewing a hold forever still can't keep it on past maxOnMs.
class OutputBank {
 public:
  static constexpr size_t kMaxOutputs = 24;
  static constexpr uint32_t kDefaultMaxOnMs = 10000;

  using ChangeCallback = void (*)(uint8_t pin, bool on, float duty);
  using TripCallback = void (*)(uint8_t pin);

  void setCallbacks(ChangeCallback onChange, TripCallback onTrip);

  // Replaces the configured outputs and drives them all to their off level. Pins dropped from the
  // list are switched off and released. Changes nothing and returns BadPin if the list is too
  // long or has the same pin twice.
  OutputError configure(const OutputConfig* configs, size_t count);

  // on: on until now + ms, or later if a deadline is already further out. off: off now.
  OutputError set(uint8_t pin, bool on, uint32_t ms);
  // `count` pulses of onMs, with offMs between them. Replaces whatever the pin was doing.
  OutputError pulse(uint8_t pin, uint32_t onMs, uint32_t offMs, uint32_t count);
  // duty 0..1 until now + ms. duty 0 switches off.
  OutputError pwm(uint8_t pin, float duty, uint32_t ms);
  // Every pin off, every pulse train cancelled, trips cleared.
  void stop();
  // Call from loop(): ends deadlines and steps pulse trains.
  void update();

  size_t size() const { return count_; }
  const OutputConfig& config(size_t index) const { return states_[index].cfg; }
  bool isOn(uint8_t pin) const;
  float duty(uint8_t pin) const;
  bool isTripped(uint8_t pin) const;

 private:
  struct State {
    OutputConfig cfg;
    bool on;
    float duty;
    bool pwmAttached;
    bool tripped;
    // The current deadline was clamped to onSince + maxOnMs, so reaching it trips the pin.
    bool capped;
    uint32_t onSince;
    uint32_t deadline;
    bool train;
    uint32_t trainLeft;  // pulses still to start after the current one
    uint32_t trainOnMs;
    uint32_t trainOffMs;
    uint32_t nextPulseAt;
  };

  State* find(uint8_t pin);
  const State* find(uint8_t pin) const;
  void turnOn(State& st, float duty, uint32_t ms, bool canTrip);
  void switchOff(State& st);
  void drive(State& st, bool on, float duty);

  State states_[kMaxOutputs] = {};
  size_t count_ = 0;
  ChangeCallback onChange_ = nullptr;
  TripCallback onTrip_ = nullptr;
};
