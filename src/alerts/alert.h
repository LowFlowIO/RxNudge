#pragma once

namespace alerts {

// Output capability, decided from compile switch + self-test result in NVS.
enum class AlertMode { AudibleAndVisual, VisualOnly };

void alertInit();
AlertMode alertMode();
// Persists the self-test answer (true = user heard the buzzer).
void setBuzzerConfirmed(bool heard);
// Has the user ever answered the buzzer self-test?
bool buzzerTested();

// Visual alert: inverts the screen with FULL refreshes `flashes` times and
// restores the previous frame. Blocking.
void visualFlash(int flashes);
// Medication alert pattern: three beeps (if audible) plus one visual flash.
void raiseAlert();

}  // namespace alerts
