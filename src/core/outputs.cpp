#include "outputs.h"

#include "hal.h"

namespace {

// Wrap-safe millis() comparisons, good for deadlines up to ~24 days out.
bool reached(uint32_t now, uint32_t at) {
  return static_cast<int32_t>(now - at) >= 0;
}

uint32_t later(uint32_t a, uint32_t b) {
  return static_cast<int32_t>(a - b) >= 0 ? a : b;
}

}  // namespace

void OutputBank::setCallbacks(ChangeCallback onChange, TripCallback onTrip) {
  onChange_ = onChange;
  onTrip_ = onTrip;
}

OutputError OutputBank::configure(const OutputConfig* configs, size_t count) {
  if (count > kMaxOutputs) return OutputError::BadPin;
  for (size_t i = 0; i < count; i++) {
    for (size_t j = 0; j < i; j++) {
      if (configs[i].pin == configs[j].pin) return OutputError::BadPin;
    }
  }

  for (size_t i = 0; i < count_; i++) {
    State& st = states_[i];
    switchOff(st);
    if (st.pwmAttached) hal::pwmDetach(st.cfg.pin);

    bool kept = false;
    for (size_t j = 0; j < count; j++) kept = kept || configs[j].pin == st.cfg.pin;
    if (!kept) hal::pinRelease(st.cfg.pin);
  }

  count_ = count;
  for (size_t i = 0; i < count_; i++) {
    State& st = states_[i];
    st = State{};
    st.cfg = configs[i];
    if (st.cfg.maxOnMs == 0) st.cfg.maxOnMs = kDefaultMaxOnMs;

    // Set the off level before enabling the output so active-low boards don't click on boot.
    hal::pinWrite(st.cfg.pin, st.cfg.activeLow);
    hal::pinOutput(st.cfg.pin);
    hal::pinWrite(st.cfg.pin, st.cfg.activeLow);
  }
  return OutputError::None;
}

OutputError OutputBank::set(uint8_t pin, bool on, uint32_t ms) {
  State* st = find(pin);
  if (!st) return OutputError::BadPin;

  if (!on) {
    switchOff(*st);
    st->tripped = false;
    return OutputError::None;
  }

  if (ms == 0) return OutputError::BadValue;
  if (st->tripped) return OutputError::Tripped;

  if (st->train) switchOff(*st);
  turnOn(*st, 1.0f, ms, true);
  return OutputError::None;
}

OutputError OutputBank::pulse(uint8_t pin, uint32_t onMs, uint32_t offMs, uint32_t count) {
  State* st = find(pin);
  if (!st) return OutputError::BadPin;
  if (onMs == 0 || count == 0) return OutputError::BadValue;
  if (st->tripped) return OutputError::Tripped;

  // A train always starts from its first pulse, so a running one is cut first.
  switchOff(*st);
  st->train = true;
  st->trainLeft = count - 1;
  st->trainOnMs = onMs;
  st->trainOffMs = offMs;
  // Pulses longer than maxOnMs are shortened, not tripped: the train still has to finish.
  turnOn(*st, 1.0f, onMs, false);
  return OutputError::None;
}

OutputError OutputBank::pwm(uint8_t pin, float duty, uint32_t ms) {
  State* st = find(pin);
  if (!st) return OutputError::BadPin;
  if (!(duty >= 0.0f && duty <= 1.0f) || ms == 0) return OutputError::BadValue;

  if (duty == 0.0f) {
    switchOff(*st);
    return OutputError::None;
  }

  if (st->tripped) return OutputError::Tripped;
  if (!st->pwmAttached) {
    if (!hal::pwmAttach(pin)) return OutputError::NotPwm;
    st->pwmAttached = true;
  }

  if (st->train) switchOff(*st);
  turnOn(*st, duty, ms, true);
  return OutputError::None;
}

void OutputBank::stop() {
  for (size_t i = 0; i < count_; i++) {
    switchOff(states_[i]);
    states_[i].tripped = false;
  }
}

void OutputBank::update() {
  uint32_t now = hal::millis();

  for (size_t i = 0; i < count_; i++) {
    State& st = states_[i];

    if (st.train) {
      if (st.on && reached(now, st.deadline)) {
        drive(st, false, 0.0f);
        if (st.trainLeft == 0) {
          st.train = false;
        } else {
          st.nextPulseAt = now + st.trainOffMs;
        }
      } else if (!st.on && reached(now, st.nextPulseAt)) {
        st.trainLeft--;
        turnOn(st, 1.0f, st.trainOnMs, false);
      }
      continue;
    }

    if (st.on && reached(now, st.deadline)) {
      bool trip = st.capped;
      switchOff(st);
      if (trip) {
        st.tripped = true;
        hal::log("[out] GPIO %u reached its max on-time (%u ms), off until the app turns it off\n",
                 (unsigned)st.cfg.pin, (unsigned)st.cfg.maxOnMs);
        if (onTrip_) onTrip_(st.cfg.pin);
      }
    }
  }
}

bool OutputBank::isOn(uint8_t pin) const {
  const State* st = find(pin);
  return st && st->on;
}

float OutputBank::duty(uint8_t pin) const {
  const State* st = find(pin);
  return st ? st->duty : 0.0f;
}

bool OutputBank::isTripped(uint8_t pin) const {
  const State* st = find(pin);
  return st && st->tripped;
}

OutputBank::State* OutputBank::find(uint8_t pin) {
  for (size_t i = 0; i < count_; i++) {
    if (states_[i].cfg.pin == pin) return &states_[i];
  }
  return nullptr;
}

const OutputBank::State* OutputBank::find(uint8_t pin) const {
  for (size_t i = 0; i < count_; i++) {
    if (states_[i].cfg.pin == pin) return &states_[i];
  }
  return nullptr;
}

void OutputBank::turnOn(State& st, float duty, uint32_t ms, bool canTrip) {
  uint32_t now = hal::millis();
  if (!st.on) st.onSince = now;

  // An "on" only ever moves the deadline later, so a short pulse can't cut a longer one short.
  uint32_t until = now + ms;
  if (st.on) until = later(st.deadline, until);

  uint32_t cap = st.onSince + st.cfg.maxOnMs;
  st.capped = false;
  if (reached(until, cap)) {
    until = cap;
    st.capped = canTrip;
  }

  st.deadline = until;
  drive(st, true, duty);
}

void OutputBank::switchOff(State& st) {
  st.train = false;
  st.capped = false;
  drive(st, false, 0.0f);
}

void OutputBank::drive(State& st, bool on, float duty) {
  float level = on ? duty : 0.0f;

  if (st.pwmAttached) {
    hal::pwmWrite(st.cfg.pin, st.cfg.activeLow ? 1.0f - level : level);
  } else {
    hal::pinWrite(st.cfg.pin, on != st.cfg.activeLow);
  }

  bool changed = st.on != on || st.duty != level;
  st.on = on;
  st.duty = level;
  if (changed && onChange_) onChange_(st.cfg.pin, on, level);
}
