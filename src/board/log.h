#pragma once
// Logging goes to Arduino Serial = UART0 (GPIO43 TX / GPIO44 RX) through the
// on-board WCH USB-UART bridge (ARDUINO_USB_CDC_ON_BOOT=0). It is not native USB CDC.
#include <Arduino.h>

#if defined(RXNUDGE_LOG) && RXNUDGE_LOG
#define MT_LOG_BEGIN() Serial.begin(115200)
#define MT_LOGF(fmt, ...) Serial.printf("[%8lu] " fmt "\n", millis(), ##__VA_ARGS__)
#else
#define MT_LOG_BEGIN()
#define MT_LOGF(fmt, ...)
#endif
