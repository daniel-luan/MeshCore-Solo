#pragma once
#include "DataStore.h"
#include "RouteFormat.h"

#include <helpers/DeviceTiming.h>

class RouteFilesystem : public routes::Files {
  DataStore *_store = nullptr;
  FILESYSTEM *_fs = nullptr;
  File _reader, _writer;
  int8_t _readSlot = -1, _writeSlot = -1, _cfgSlot = -1;
  uint32_t _cfgGeneration = 0;
  static const char *path(uint8_t slot) { return slot ? "/route_b" : "/route_a"; }

public:
  explicit RouteFilesystem(DataStore &store)
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
      : _reader(*(store.getSecondaryFS() ? store.getSecondaryFS() : store.getPrimaryFS())),
        _writer(*(store.getSecondaryFS() ? store.getSecondaryFS() : store.getPrimaryFS()))
#endif
  {
    begin(store);
  }
  void begin(DataStore &store) {
    _store = &store;
    _fs = store.getSecondaryFS() ? store.getSecondaryFS() : store.getPrimaryFS();
  }
  uint32_t size(uint8_t slot) override {
    ScopedDeviceTiming timing(DeviceTiming::ROUTE_IO);
    File f = _store->openRead(_fs, path(slot));
    if (!f) return 0;
    uint32_t n = f.size();
    f.close();
    return n;
  }
  bool read(uint8_t slot, uint32_t offset, uint8_t *dest, size_t n) override {
    ScopedDeviceTiming timing(DeviceTiming::ROUTE_IO);
    if (_readSlot != slot || !_reader) {
      if (_reader) _reader.close();
      _reader = _store->openRead(_fs, path(slot));
      _readSlot = slot;
    }
    if (!_reader || !_reader.seek(offset)) return false;
#if defined(NRF52_PLATFORM)
    // LittleFS can bypass its cache for large reads. After copying an unaligned
    // file prefix, that path passes an unaligned destination to QSPI EasyDMA,
    // which rejects it. Route labels make preview/index offsets byte-aligned.
    // Keep each read below read_size so DMA always uses LittleFS's aligned cache.
    const auto *config = _fs->_getFS()->cfg;
    if (!config || config->read_size < 2) return false;
    const size_t limit = config->read_size > 128 ? 128 : config->read_size - 1;
    while (n) {
      const size_t chunk = n < limit ? n : limit;
      if (_reader.read(dest, chunk) != (int)chunk) return false;
      dest += chunk;
      n -= chunk;
    }
    return true;
#else
    return _reader.read(dest, n) == (int)n;
#endif
  }
  bool create(uint8_t slot) override {
    ScopedDeviceTiming timing(DeviceTiming::ROUTE_IO);
    cancel();
    if (_reader) {
      _reader.close();
      _readSlot = -1;
    }
    _writeSlot = slot;
    _writer = _store->openWrite(_fs, path(slot));
    return (bool)_writer;
  }
  bool append(const uint8_t *data, size_t n) override {
    ScopedDeviceTiming timing(DeviceTiming::ROUTE_IO);
    if (_writeSlot < 0) return false;
    if (!_writer) {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
      _writer = _fs->open(path(_writeSlot), FILE_O_WRITE);
#else
      _writer = _fs->open(path(_writeSlot), "a");
#endif
    }
    return _writer && _writer.write(data, n) == n;
  }
  bool finish() override {
    ScopedDeviceTiming timing(DeviceTiming::ROUTE_IO);
    if (!_writer) return false;
    _writer.flush();
    _writer.close();
    if (_reader) {
      _reader.close();
      _readSlot = -1;
    }
    return true;
  }
  void cancel() override {
    ScopedDeviceTiming timing(DeviceTiming::ROUTE_IO);
    if (_writer) _writer.close();
    _writeSlot = -1;
  }
  bool erase(uint8_t slot) override {
    ScopedDeviceTiming timing(DeviceTiming::ROUTE_IO);
    if (_reader) {
      _reader.close();
      _readSlot = -1;
    }
    return !_fs->exists(path(slot)) || _fs->remove(path(slot));
  }
  void loadSettings(uint32_t &metres, uint32_t &seconds) {
    metres = 50;
    seconds = 15;
    for (uint8_t slot = 0; slot < 2; ++slot) {
      File f = _store->openRead(_fs, slot ? "/route_cfg_b" : "/route_cfg_a");
      if (!f) continue;
      uint8_t b[20];
      bool ok = f.size() == 20 && f.read(b, 20) == 20;
      f.close();
      if (!ok || routes::u32(b) != 0x47464352 || ~routes::crc(0xffffffff, b, 16) != routes::u32(b + 16))
        continue;
      uint32_t generation = routes::u32(b + 4), m = routes::u32(b + 8), s = routes::u32(b + 12);
      if (m < 25 || m > 5000 || s < 5 || s > 600) continue;
      if (_cfgSlot < 0 || (int32_t)(generation - _cfgGeneration) > 0) {
        _cfgSlot = slot;
        _cfgGeneration = generation;
        metres = m;
        seconds = s;
      }
    }
  }
  bool saveSettings(uint32_t metres, uint32_t seconds) {
    if (metres < 25 || metres > 5000 || seconds < 5 || seconds > 600) return false;
    uint8_t b[20];
    routes::put32(b, 0x47464352);
    routes::put32(b + 4, _cfgGeneration + 1);
    routes::put32(b + 8, metres);
    routes::put32(b + 12, seconds);
    routes::put32(b + 16, ~routes::crc(0xffffffff, b, 16));
    uint8_t slot = _cfgSlot == 0 ? 1 : 0;
    const char *p = slot ? "/route_cfg_b" : "/route_cfg_a";
    File f = _store->openWrite(_fs, p);
    if (!f) return false;
    bool ok = f.write(b, 20) == 20;
    f.flush();
    f.close();
    uint8_t verify[20];
    f = _store->openRead(_fs, p);
    ok = ok && f && f.size() == 20 && f.read(verify, 20) == 20 && !memcmp(b, verify, 20);
    if (f) f.close();
    if (ok) {
      _cfgSlot = slot;
      ++_cfgGeneration;
    }
    return ok;
  }
  bool capacity(routes::Capacity &c) override {
    ScopedDeviceTiming timing(DeviceTiming::ROUTE_IO);
    uint32_t block = 0;
    if (!_store->getStorageBytes(_fs, c.total, c.used, block)) return false;
#if defined(ESP32)
    c.reserve = c.total / 4 + 65536;
#else
    c.reserve = 65536 + 2 * block;
#endif
    return true;
  }
};
