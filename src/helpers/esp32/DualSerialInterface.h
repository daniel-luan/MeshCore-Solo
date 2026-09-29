#pragma once

#include "../BaseSerialInterface.h"
#include "../ArduinoSerialInterface.h"
#include "SerialBLEInterface.h"

// ESP32 counterpart of helpers/nrf52/DualSerialInterface.h: wraps BLE + USB
// serial so one build serves both companion transports. BLE takes priority when
// connected, USB is available as a fallback.
// enable()/disable() control BLE only — USB is always on.
// BLE state machine is only pumped when BLE is enabled; USB is not read while
// BLE is connected.
class DualSerialInterface : public BaseSerialInterface {
  SerialBLEInterface _ble;
  ArduinoSerialInterface _usb;
  uint8_t _ble_buf[MAX_FRAME_SIZE];
  uint8_t _usb_buf[MAX_FRAME_SIZE];
  bool _ble_enabled;
  bool _ble_was_connected;

public:
  DualSerialInterface() : _ble_enabled(false), _ble_was_connected(false) {}

  void begin(const char* ble_prefix, char* node_name, uint32_t pin_code, Stream& usb_stream) {
    _ble.begin(ble_prefix, node_name, pin_code);
    _usb.begin(usb_stream);
    _usb.enable();  // USB is always on
  }

  void enable() override  { _ble.enable(); _ble_enabled = true; }
  void disable() override { _ble.disable(); _ble_enabled = false; }
  bool isEnabled() const override { return _ble_enabled; }

  // Always true — USB is always available as fallback, so the mesh can send.
  bool isConnected() const override { return true; }
  // True only when a BLE companion app is paired and connected.
  bool isBLEConnected() const override { return _ble_enabled && _ble.isConnected(); }
#if defined(ARDUINO_USB_MODE) && defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT == 1
  // Both native USB modes expose a connection-aware Serial bool operator:
  // USBCDC when ARDUINO_USB_MODE=0 and HWCDC (Heltec V4) when it is 1.
  bool isClientConnected() const override { return isBLEConnected() || (bool)Serial; }
#else
  // BLE only, unlike the nRF52 version, which also counts a USB host holding the
  // CDC port open. These boards reach USB through a UART bridge rather than
  // native CDC, so there is no DTR to read: (bool)Serial is a plain "is the
  // peripheral initialised", i.e. always true once begin() has run, and
  // ArduinoSerialInterface::isConnected() likewise hardcodes true. Reporting
  // that as a connected client would permanently convince the UI an app is
  // watching the device and stop it waking the display for new messages.
  bool isClientConnected() const override { return isBLEConnected(); }
#endif

  bool isWriteBusy() const override {
    if (isBLEConnected()) return _ble.isWriteBusy();
#if defined(ARDUINO_USB_MODE) && defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT == 1
    // A native USB port without an open host cannot accept a frame.
    if (!(bool)Serial) return true;
#endif
    return _usb.isWriteBusy();
  }

  size_t writeFrame(const uint8_t src[], size_t len) override {
    if (isBLEConnected()) return _ble.writeFrame(src, len);
#if defined(ARDUINO_USB_MODE) && defined(ARDUINO_USB_CDC_ON_BOOT) && ARDUINO_USB_CDC_ON_BOOT == 1
    // Otherwise streamWriteUntil() waits 300 ms for every frame sent while
    // the device is being used standalone, freezing the UI for that time.
    if (!(bool)Serial) return 0;
#endif
    return _usb.writeFrame(src, len);
  }

  size_t checkRecvFrame(uint8_t dest[]) override {
    if (_ble_enabled) {
      size_t ble_len = _ble.checkRecvFrame(_ble_buf);
      bool ble_now = _ble.isConnected();

      if (ble_now) {
        _ble_was_connected = true;
        if (ble_len > 0) { memcpy(dest, _ble_buf, ble_len); return ble_len; }
        return 0;  // BLE active — don't read USB to keep its state machine clean
      }

      if (_ble_was_connected) {
        // BLE just disconnected — reset USB state machine so stale partial frames don't block it
        _ble_was_connected = false;
        _usb.enable();
        return 0;
      }
    }

    size_t usb_len = _usb.checkRecvFrame(_usb_buf);
    if (usb_len > 0) { memcpy(dest, _usb_buf, usb_len); return usb_len; }
    return 0;
  }
};
