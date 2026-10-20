#pragma once
#include <cstdint>

namespace alerts {

// LEDC PWM on GPIO48, modelled on Seeed's sticky_buzzer.cpp (10-bit, duty 256).
// Uses LEDC timer 3 / channel 7, so it never collides with Arduino's channel 0.
bool buzzerInit();
void buzzerTone(uint32_t hz, uint32_t ms);  // blocking, default duty
// Blocking tone with an explicit 10-bit duty (volume; 512 = 50 % = loudest).
void buzzerToneDuty(uint32_t hz, uint32_t ms, uint16_t duty);
void buzzerOff();
bool buzzerCompiledIn();

}  // namespace alerts
