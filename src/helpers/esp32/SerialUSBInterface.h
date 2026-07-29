#pragma once

#include <helpers/ArduinoSerialInterface.h>

// ArduinoSerialInterface::isConnected() always returns true (no generic way to
// detect a host). On ESP32-S3 with ARDUINO_USB_MODE=1, Serial is a HWCDC instance
// that DOES know whether a USB host has the port open (DTR) - use that instead.
class SerialUSBInterface : public ArduinoSerialInterface {
public:
  bool isConnected() const override { return Serial.isConnected(); }
};
