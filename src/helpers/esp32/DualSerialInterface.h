#pragma once

#include "../BaseSerialInterface.h"
#include "SerialUSBInterface.h"
#include "SerialBLEInterface.h"

// Combines a USB (native CDC) and a BLE transport behind a single BaseSerialInterface,
// so the companion radio app can be reached over either without reflashing.
// Both transports are always enabled; whichever one last delivered a complete frame
// becomes "active" and receives all replies, until it disconnects.
class DualSerialInterface : public BaseSerialInterface {
  SerialUSBInterface usb;
  SerialBLEInterface ble;
  BaseSerialInterface* active;

public:
  DualSerialInterface() : active(NULL) { }

  void begin(const char* prefix, char* name, uint32_t pin_code) {
    usb.begin(Serial);
    ble.begin(prefix, name, pin_code);
  }

  void enable() override {
    usb.enable();
    ble.enable();
  }

  void disable() override {
    usb.disable();
    ble.disable();
    active = NULL;
  }

  bool isEnabled() const override { return usb.isEnabled() || ble.isEnabled(); }

  bool isConnected() const override { return active != NULL && active->isConnected(); }

  bool isWriteBusy() const override { return active == NULL || active->isWriteBusy(); }

  size_t writeFrame(const uint8_t src[], size_t len) override {
    return active != NULL ? active->writeFrame(src, len) : 0;
  }

  size_t checkRecvFrame(uint8_t dest[]) override;
};
