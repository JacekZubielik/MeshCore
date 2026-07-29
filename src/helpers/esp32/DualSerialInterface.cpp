#include "DualSerialInterface.h"
#include <string.h>

size_t DualSerialInterface::checkRecvFrame(uint8_t dest[]) {
  // Both sub-interfaces must be polled every call, active or not - BLE's
  // checkRecvFrame() also drives its advertising/disconnect state machine,
  // so it can't be skipped just because USB is currently active (or vice versa).
  uint8_t usb_buf[MAX_FRAME_SIZE];
  size_t usb_len = usb.checkRecvFrame(usb_buf);

  uint8_t ble_buf[MAX_FRAME_SIZE];
  size_t ble_len = ble.checkRecvFrame(ble_buf);

  if (active != NULL && !active->isConnected()) {
    active = NULL;   // lost transport - let whichever speaks next take over
  }

  if (usb_len > 0) {
    active = &usb;
    memcpy(dest, usb_buf, usb_len);
    return usb_len;
  }
  if (ble_len > 0) {
    active = &ble;
    memcpy(dest, ble_buf, ble_len);
    return ble_len;
  }
  return 0;
}
