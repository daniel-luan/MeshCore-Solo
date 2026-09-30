#pragma once

#include <Arduino.h>

// Maximum foreground durations since boot or the last on-device reset.
// micros() subtraction remains valid across its wraparound for these short calls.
class DeviceTiming {
public:
  enum Metric : uint8_t {
    MESH_LOOP,
    SENSOR_LOOP,
    UI_LOOP,
    UI_EVENTS,
    UI_POLL,
    UI_RENDER,
    UI_BACKGROUND,
    ADVERT_WRITE,
    PREFS_WRITE,
    CONTACTS_WRITE,
    CONTACTS_OPEN,
    CONTACTS_DATA,
    CONTACTS_CHUNK,
    CONTACTS_CLOSE,
    CONTACTS_REPLACE,
    USB_WRITE,
    OLED_FLUSH,
    ROUTE_IO,
    ROUTE_VERIFY,
    ROUTE_SEARCH,
    METRIC_COUNT
  };

  void record(Metric metric, uint32_t elapsed_us) {
    if (elapsed_us > _max_us[metric]) _max_us[metric] = elapsed_us;
  }

  uint32_t maxMicros(Metric metric) const { return _max_us[metric]; }

  void reset() {
    for (uint8_t i = 0; i < METRIC_COUNT; i++) _max_us[i] = 0;
  }

private:
  uint32_t _max_us[METRIC_COUNT] = {};
};

extern DeviceTiming device_timing;

class ScopedDeviceTiming {
public:
  explicit ScopedDeviceTiming(DeviceTiming::Metric metric)
    : _metric(metric), _started_us(micros()) {}
  ~ScopedDeviceTiming() { device_timing.record(_metric, micros() - _started_us); }

private:
  DeviceTiming::Metric _metric;
  uint32_t _started_us;
};
