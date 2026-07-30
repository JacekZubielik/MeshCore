#pragma once

#include <helpers/ArduinoSerialInterface.h>

// ArduinoSerialInterface::isConnected() always returns true (no generic way to
// detect a host). On boards where Serial is a HWCDC instance (native USB
// Serial/JTAG controller, ARDUINO_USB_MODE=1 + ARDUINO_USB_CDC_ON_BOOT=1 - e.g.
// the T-Beam Supreme), it DOES know whether a USB host has the port open (DTR) -
// use that instead. This file is compiled for every ESP32 companion-radio env
// (shared build_src_filter wildcard), so the override must stay conditional:
// on boards where Serial is a plain HardwareSerial (UART bridge chip) or the
// USB-OTG USBCDC class (ARDUINO_USB_MODE=0), Serial.isConnected() doesn't exist
// and this must fall back to the inherited always-true behavior.
class SerialUSBInterface : public ArduinoSerialInterface {
public:
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  bool isConnected() const override { return Serial.isConnected(); }
#endif
};
