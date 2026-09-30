#pragma once
#include "RouteFormat.h"

#include <Arduino.h>

namespace routes {
class RouteStore {
  Files *_files = nullptr;
  int8_t _active = -1, _slot = 0;
  Header _header{}, _verify{};
  State _state = CHECKING;
  Error _error = OK;
  bool _boot = true, _following = false, _reverse = false;
  uint8_t _phase = 0, _bootSlot = 0;
  uint32_t _generation = 0, _candidateGeneration = 0, _expectedCrc = 0, _crc = 0xffffffff;
  uint32_t _position = 0, _cursor = 0, _child = 0, _segments = 0, _lastDistance = 0, _lastSegmentStart = 0;
  uint32_t _session = 0, _received = 0, _uploadBytes = 0, _uploadCrc = 0, _lastActivity = 0;
  uint32_t _lastChunkOffset = UINT32_MAX, _lastChunkCrc = 0;
  uint16_t _lastChunkBytes = 0;
  uint16_t _beginRequest = 0;
  uint8_t _scratch[512]{};
  uint8_t _page[PAGE_SIZE]{};
  uint32_t _pageIndex = UINT32_MAX;
  uint8_t _pageEntry = 0;
  uint32_t _cacheStart = UINT32_MAX, _cacheCount = 0;
  Point _points[32]{};
  Point _preview[64]{};
  uint32_t _previewLoaded = 0;
  // Flash index DFS. Depth <= 32 is far above the depth of any 32-bit file.
  struct Frame {
    uint32_t page;
    uint8_t next;
  };
  Frame _stack[32]{};
  uint8_t _depth = 0;
  bool _segmentReady = false, _globalSearch = false, _search = false, _global = false, _ambiguous = false,
       _matched = false, _arrived = false, _off = false;
  uint32_t _segment = 0, _segmentStart = 0, _segmentEnd = 0, _matchIndex = 0, _progress = 0;
  uint32_t _scanStart = 0, _scanEnd = 0, _scanCursor = 0, _nextFix = 0, _searchStarted = 0, _outsideSince = 0,
           _offDistance = 50, _offDelay = 15;
  int32_t _lat = 0, _lon = 0, _targetLat = 0, _targetLon = 0;
  float _best = 1e30f, _second = 1e30f, _lateral = 0;
  uint32_t _bestIndex = 0, _bestProgress = 0, _secondProgress = 0, _jumpCm = 5000;
  int32_t _bestLat = 0, _bestLon = 0;
  bool _fixGood = false, _poor = false, _alert = false;

  bool read(uint32_t offset, uint8_t *out, size_t n) {
    return _active >= 0 && _files->read(_active, offset, out, n);
  }
  bool pointAt(uint32_t index, Point &p) {
    if (index >= _header.points) return false;
    if (_cacheStart == UINT32_MAX || index < _cacheStart || index >= _cacheStart + _cacheCount) {
      // 32 records per cache fill: a maximum 512-byte filesystem read.
      uint32_t start = (index / 32) * 32, count = _header.points - start;
      if (count > 32) count = 32;
      if (!read(_header.pointOffset + start * POINT_SIZE, _scratch, count * POINT_SIZE)) return false;
      for (uint32_t i = 0; i < count; ++i)
        _points[i] = point(_scratch + i * POINT_SIZE);
      _cacheStart = start;
      _cacheCount = count;
    }
    p = _points[index - _cacheStart];
    return true;
  }
  void failed(Error error) {
    _files->cancel();
    _error = error;
    _phase = 0;
    if (_boot) {
      ++_bootSlot;
      if (_bootSlot < 2) {
        _slot = _bootSlot;
        _phase = 1;
      } else {
        _boot = false;
        _phase = _active >= 0 ? 11 : 0;
        _previewLoaded = 0;
        _state = _active >= 0 ? CHECKING : EMPTY;
      }
    } else {
      _state = FAILED;
      if (_active >= 0) {
        _phase = 11;
        _previewLoaded = 0;
      }
    }
  }
  void verified() {
    if (_boot) {
      if (_active < 0 || (int32_t)(_candidateGeneration - _generation) > 0) {
        _active = _slot;
        _header = _verify;
        _generation = _candidateGeneration;
      }
      ++_bootSlot;
      if (_bootSlot < 2) {
        _slot = _bootSlot;
        _phase = 1;
      } else {
        _boot = false;
        _phase = _active >= 0 ? 11 : 0;
        _previewLoaded = 0;
        _state = _active >= 0 ? CHECKING : EMPTY;
        _error = OK;
      }
    } else
      _phase = 10; // completion footer is a separate filesystem operation
  }
  bool validPoint(const Point &p, uint32_t i, uint32_t &segments, uint32_t &distance) {
    if (!validCoordinate(p.lat, p.lon) || (p.flags & ~SEG_START) || p.distance > _verify.distance ||
        p.distance < distance)
      return false;
    if (i == 0 && (!(p.flags & SEG_START) || p.distance != 0)) return false;
    if (p.flags & SEG_START) {
      if (i && (p.distance != distance || i - _lastSegmentStart < 2)) return false;
      ++segments;
      _lastSegmentStart = i;
    }
    distance = p.distance;
    return true;
  }
  void startSearch(bool global) {
    _pageIndex = UINT32_MAX;
    _search = true;
    _global = global;
    _globalSearch = global;
    _best = _second = 1e30f;
    _ambiguous = false;
    if (global) {
      _depth = 1;
      _stack[0] = { _header.root, 0 };
    } else {
      _depth = 0;
      _scanStart = _matchIndex > 32 ? _matchIndex - 32 : _segmentStart;
      if (_scanStart < _segmentStart) _scanStart = _segmentStart;
      _scanEnd = _matchIndex + 65;
      if (_scanEnd > _segmentEnd) _scanEnd = _segmentEnd;
      _scanCursor = _scanStart;
    }
  }
  void consider(uint32_t index, const Point &a, const Point &b) {
    if (b.flags & SEG_START || index < _segmentStart || index >= _segmentEnd) return;
    Projection p = project(_lat, _lon, a, b);
    uint32_t along = a.distance + (uint32_t)lroundf((b.distance - a.distance) * p.fraction);
    // Local following permits 20m GPS wobble but not a jump backwards along a loop.
    if (!_globalSearch && _matched &&
        ((!_reverse && along + 2000 < _progress) || (_reverse && along > _progress + 2000)))
      return;
    if (!_globalSearch && _matched && llabs((int64_t)along - _progress) > _jumpCm) return;
    const bool continuity = !_globalSearch && _matched && fabsf(p.lateral - _best) <= 3;
    if (continuity ? llabs((int64_t)along - _progress) < llabs((int64_t)_bestProgress - _progress)
                   : p.lateral < _best) {
      if (_best < 1e29f && llabs((int64_t)along - _bestProgress) > 10000) {
        _second = _best;
        _secondProgress = _bestProgress;
      }
      _best = p.lateral;
      _bestIndex = index;
      _bestProgress = along;
      _bestLat = p.lat;
      _bestLon = p.lon;
    } else if (llabs((int64_t)along - _bestProgress) > 10000 && p.lateral < _second) {
      _second = p.lateral;
      _secondProgress = along;
    }
  }
  void finishSearch(uint32_t now) {
    _search = false;
    if (_best >= 1e29f || (_globalSearch && _second < _best + 10)) {
      _ambiguous = true;
      _matched = false;
      _outsideSince = 0;
      return;
    }
    _matched = true;
    _matchIndex = _bestIndex;
    _progress = _bestProgress;
    _lateral = _best;
    _targetLat = _bestLat;
    _targetLon = _bestLon;
    Point start, end;
    if (!pointAt(_segmentStart, start) || !pointAt(_segmentEnd, end)) {
      _ambiguous = true;
      _matched = false;
      return;
    }
    _arrived =
        _lateral <= 15 && (_reverse ? _progress <= start.distance + 1500 : _progress + 1500 >= end.distance);
    if (_arrived) {
      _outsideSince = 0;
      return;
    }
    if (_lateral < _offDistance / 2.0f) {
      _outsideSince = 0;
      _off = false;
    } else if (_lateral > _offDistance) {
      if (!_outsideSince) _outsideSince = now ? now : 1;
      if (!_off && (uint32_t)(now - _outsideSince) >= _offDelay * 1000) {
        _off = true;
        _alert = true;
      }
    } else
      _outsideSince = 0;
    // When on-route, guide ahead along the path, not to the projected point behind you.
    Point ahead;
    uint32_t index = _reverse ? _bestIndex : _bestIndex + 1;
    if (_lateral < _offDistance && pointAt(index, ahead)) {
      _targetLat = ahead.lat;
      _targetLon = ahead.lon;
    }
  }

public:
  void begin(Files &files) {
    _files = &files;
    _boot = true;
    _bootSlot = 0;
    _slot = 0;
    _phase = 1;
    _state = CHECKING;
  }
  State state() const { return _state; }
  Error error() const { return _error; }
  bool hasRoute() const { return _active >= 0; }
  bool following() const { return _following; }
  bool reverse() const { return _reverse; }
  const Header &header() const { return _header; }
  uint32_t session() const { return _session; }
  uint32_t received() const { return _received; }
  uint32_t transferBytes() const { return _uploadBytes; }
  uint32_t verifyProgress() const {
    return _verify.bytes ? (uint32_t)((uint64_t)_position * 100 / _verify.bytes) : 0;
  }
  bool working() const { return _phase != 0 || _state == RECEIVING; }
  bool capacity(Capacity &c) {
    if (!_files || !_files->capacity(c)) return false;
    uint8_t inactive = _active == 0 ? 1 : 0;
    c.reclaimable = _files->size(inactive);
    uint64_t budget = c.total > c.reserve ? c.total - c.reserve : 0;
    if (c.used < c.reclaimable || c.used > c.total) return false;
    uint64_t used = c.used - c.reclaimable;
    c.maximum = budget > used + FOOTER_SIZE ? (uint32_t)(budget - used - FOOTER_SIZE) : 0;
    return true;
  }
  Error startUpload(uint32_t bytes, uint32_t checksum, uint16_t request, uint32_t now) {
    if (_state == RECEIVING && request == _beginRequest && bytes == _uploadBytes && checksum == _uploadCrc) {
      _lastActivity = now;
      return OK;
    }
    if (_following || _phase || _state == RECEIVING) return BUSY;
    Capacity c;
    if (!capacity(c)) return IO_ERROR;
    if (bytes < HEADER_SIZE || bytes > c.maximum) return NO_SPACE;
    _slot = _active == 0 ? 1 : 0;
    if (!_files->create(_slot)) return IO_ERROR;
    _uploadBytes = bytes;
    _uploadCrc = checksum;
    _received = 0;
    _beginRequest = request;
    _session = now + 1 + _generation;
    if (!_session) _session = 1;
    _lastActivity = now;
    _lastChunkOffset = UINT32_MAX;
    _state = RECEIVING;
    _error = OK;
    return OK;
  }
  Error data(uint32_t token, uint32_t offset, const uint8_t *bytes, size_t n, uint32_t now) {
    if (token != _session || !token) return BAD_SESSION;
    if (_state != RECEIVING) return BUSY;
    if (!n || n > CHUNK_SIZE || (uint64_t)offset + n > _uploadBytes) return BAD_REQUEST;
    uint32_t hash = ~crc(0xffffffff, bytes, n);
    if (offset == _lastChunkOffset && n == _lastChunkBytes && hash == _lastChunkCrc) {
      _lastActivity = now;
      return OK;
    }
    if (offset != _received) return BAD_OFFSET;
    Capacity capacityNow;
    if (!capacity(capacityNow)) {
      failed(IO_ERROR);
      return IO_ERROR;
    }
    if (_uploadBytes > capacityNow.maximum) {
      failed(NO_SPACE);
      return NO_SPACE;
    }
    if (!_files->append(bytes, n)) {
      failed(IO_ERROR);
      return IO_ERROR;
    }
    _lastChunkOffset = offset;
    _lastChunkBytes = n;
    _lastChunkCrc = hash;
    _received += n;
    _lastActivity = now;
    return OK;
  }
  Error commit(uint32_t token, uint32_t now) {
    if (token != _session || !token) return BAD_SESSION;
    _lastActivity = now;
    if (_state == VERIFYING || (_state == READY && _received == _uploadBytes)) return OK;
    if (_state != RECEIVING || _received != _uploadBytes) return BAD_OFFSET;
    if (!_files->finish()) {
      failed(IO_ERROR);
      return IO_ERROR;
    }
    _boot = false;
    _phase = 1;
    _state = VERIFYING;
    return OK;
  }
  Error abort(uint32_t token) {
    if (token != _session || !token) return BAD_SESSION;
    if (_state != RECEIVING && _state != VERIFYING) return BAD_REQUEST;
    // Once activated, finish filling its preview rather than leaving a partial cache.
    if (_state == VERIFYING && _active == _slot) return BUSY;
    _files->cancel();
    if (!_files->erase(_slot)) {
      failed(IO_ERROR);
      return IO_ERROR;
    }
    _phase = 0;
    _state = _active >= 0 ? READY : EMPTY;
    _error = OK;
    return OK;
  }
  Error erase() {
    if (_following || _phase || _state == RECEIVING) return BUSY;
    // Remove the fallback first, so a crash halfway through cannot resurrect it.
    uint8_t inactive = _active == 0 ? 1 : 0;
    if (!_files->erase(inactive) || (_active >= 0 && !_files->erase(_active))) return IO_ERROR;
    if (_active < 0 && !_files->erase(1)) return IO_ERROR;
    _active = -1;
    _state = EMPTY;
    _header = {};
    _cacheStart = UINT32_MAX;
    return OK;
  }
  // Exactly one verification page/chunk per tick; no whole-file checksum or scan.
  void step(uint32_t now) {
    if (_state == RECEIVING && (uint32_t)(now - _lastActivity) > 60000) {
      abort(_session);
      _error = BAD_SESSION;
    }
    if (!_phase) return;
    if (_phase == 1) {
      if (_files->size(_slot) < HEADER_SIZE || !_files->read(_slot, 0, _scratch, HEADER_SIZE) ||
          !_verify.decode(_scratch)) {
        failed(BAD_FILE);
        return;
      }
      _position = 0;
      _crc = 0xffffffff;
      _cursor = 0;
      _segments = 0;
      _lastDistance = 0;
      _lastSegmentStart = 0;
      _phase = _boot ? 2 : 3;
      if (!_boot) {
        if (_verify.bytes != _uploadBytes) {
          failed(BAD_FILE);
          return;
        }
        _expectedCrc = _uploadCrc;
        _candidateGeneration = _generation + 1;
      }
    } else if (_phase == 2) {
      if (_files->size(_slot) != _verify.bytes + FOOTER_SIZE ||
          !_files->read(_slot, _verify.bytes, _scratch, FOOTER_SIZE) || u32(_scratch) != COMMIT_MAGIC ||
          u32(_scratch + 4) != _verify.bytes || ~crc(0xffffffff, _scratch, 16) != u32(_scratch + 16)) {
        failed(BAD_FILE);
        return;
      }
      _candidateGeneration = u32(_scratch + 8);
      _expectedCrc = u32(_scratch + 12);
      _phase = 3;
    } else if (_phase == 3) {
      size_t n = _verify.bytes - _position;
      if (n > 512) n = 512;
      if (!_files->read(_slot, _position, _scratch, n)) {
        failed(IO_ERROR);
        return;
      }
      _crc = crc(_crc, _scratch, n);
      _position += n;
      if (_position == _verify.bytes) {
        if (~_crc != _expectedCrc) {
          failed(BAD_FILE);
          return;
        }
        _phase = 4;
        _cursor = 0;
      }
    } else if (_phase == 4) {
      uint32_t n = _verify.points - _cursor;
      if (n > 32) n = 32;
      if (!_files->read(_slot, _verify.pointOffset + _cursor * POINT_SIZE, _scratch, n * POINT_SIZE)) {
        failed(IO_ERROR);
        return;
      }
      for (uint32_t i = 0; i < n; ++i)
        if (!validPoint(point(_scratch + i * POINT_SIZE), _cursor + i, _segments, _lastDistance)) {
          failed(BAD_FILE);
          return;
        }
      _cursor += n;
      if (_cursor == _verify.points) {
        if (_segments != _verify.segments || _lastDistance != _verify.distance ||
            _verify.points - _lastSegmentStart < 2) {
          failed(BAD_FILE);
          return;
        }
        _phase = 5;
        _cursor = 0;
      }
    } else if (_phase == 5) {
      if (_cursor == _verify.checkpoints) {
        _phase = 6;
        _cursor = 0;
        _lastDistance = 0;
        return;
      }
      if (!_files->read(_slot, _verify.checkpointOffset + _cursor * CHECKPOINT_SIZE, _scratch,
                        CHECKPOINT_SIZE)) {
        failed(IO_ERROR);
        return;
      }
      if (!validCoordinate((int32_t)u32(_scratch), (int32_t)u32(_scratch + 4)) ||
          u32(_scratch + 8) > _verify.distance || u32(_scratch + 12) >= _verify.segments ||
          u32(_scratch + 16) >= _verify.points - 1 ||
          (uint64_t)u32(_scratch + 24) + u16(_scratch + 28) > _verify.labelBytes || !u16(_scratch + 28) ||
          u16(_scratch + 30)) {
        failed(BAD_FILE);
        return;
      }
      _child = u32(_scratch + 16);
      _lastDistance = u32(_scratch + 8);
      _phase = 14;
    } else if (_phase == 14) {
      if (!_files->read(_slot, _verify.pointOffset + _child * POINT_SIZE, _scratch, 2 * POINT_SIZE)) {
        failed(IO_ERROR);
        return;
      }
      Point a = point(_scratch), b = point(_scratch + POINT_SIZE);
      if ((b.flags & SEG_START) || _lastDistance < a.distance || _lastDistance > b.distance) {
        failed(BAD_FILE);
        return;
      }
      ++_cursor;
      _phase = 5;
    } else if (_phase == 6) {
      uint32_t n = _verify.previews - _cursor;
      if (n > 32) n = 32;
      if (!_files->read(_slot, _verify.previewOffset + _cursor * POINT_SIZE, _scratch, n * POINT_SIZE)) {
        failed(IO_ERROR);
        return;
      }
      for (uint32_t i = 0; i < n; ++i) {
        Point p = point(_scratch + i * POINT_SIZE);
        if (!validCoordinate(p.lat, p.lon) || p.distance > _verify.distance || p.distance < _lastDistance ||
            ((_cursor + i) == 0 && (!(p.flags & SEG_START) || p.distance)) ||
            (p.flags & ~(SEG_START | PROFILE_GAP))) {
          failed(BAD_FILE);
          return;
        }
        _lastDistance = p.distance;
      }
      _cursor += n;
      if (_cursor == _verify.previews) {
        if (_lastDistance != _verify.distance) {
          failed(BAD_FILE);
          return;
        }
        _phase = 7;
        _cursor = 0;
      }
    } else if (_phase == 7) {
      if (_cursor == _verify.pages) {
        verified();
        return;
      }
      if (!_files->read(_slot, _verify.indexOffset + _cursor * PAGE_SIZE, _page, PAGE_SIZE)) {
        failed(IO_ERROR);
        return;
      }
      Bounds b = bounds(_page);
      uint8_t kind = _page[16], count = _page[17];
      if (!b.valid() || kind > 1 || !count || count > 16 || u16(_page + 18) || u32(_page + 28) ||
          u32(_page + 20) >= _verify.points || u32(_page + 24) >= _verify.points ||
          u32(_page + 20) > u32(_page + 24)) {
        failed(BAD_FILE);
        return;
      }
      if (_cursor == _verify.root && (u32(_page + 20) != 0 || u32(_page + 24) != _verify.points - 1 ||
                                      !b.contains(_verify.bbox) || !_verify.bbox.contains(b))) {
        failed(BAD_FILE);
        return;
      }
      if (kind == 1 && (count != 1 || u32(_page + 48) != u32(_page + 20) || !u32(_page + 52) ||
                        u32(_page + 52) > 64 || u32(_page + 24) != u32(_page + 20) + u32(_page + 52) - 1)) {
        failed(BAD_FILE);
        return;
      }
      _pageEntry = 0;
      _child = u32(_page + 20);
      _phase = 8;
    } else if (_phase == 8) {
      uint8_t count = _page[17];
      if (_pageEntry == count) {
        if (_child != u32(_page + 24) + 1) {
          failed(BAD_FILE);
          return;
        }
        ++_cursor;
        _phase = 7;
        return;
      }
      const uint8_t *entry = _page + 32 + _pageEntry * 24;
      Bounds eb = bounds(entry);
      if (!eb.valid() || !bounds(_page).contains(eb)) {
        failed(BAD_FILE);
        return;
      }
      uint32_t ref = u32(entry + 16), extent = u32(entry + 20);
      if (_page[16] == 0) {
        if (ref >= _cursor || extent ||
            !_files->read(_slot, _verify.indexOffset + ref * PAGE_SIZE, _scratch, 32)) {
          failed(BAD_FILE);
          return;
        }
        Bounds cb = bounds(_scratch);
        if (!eb.contains(cb) || !cb.contains(eb) || u32(_scratch + 20) != _child) {
          failed(BAD_FILE);
          return;
        }
        _child = u32(_scratch + 24) + 1;
        ++_pageEntry;
      } else {
        // Validate leaf bounds against all points and the outgoing edge's endpoint.
        uint32_t end = ref + extent;
        if (end < _verify.points) ++end;
        if (_child >= end) {
          _child = ref + extent;
          ++_pageEntry;
          return;
        }
        uint32_t n = end - _child;
        if (n > 32) n = 32;
        if (!_files->read(_slot, _verify.pointOffset + _child * POINT_SIZE, _scratch, n * POINT_SIZE)) {
          failed(IO_ERROR);
          return;
        }
        for (uint32_t i = 0; i < n; ++i) {
          Point p = point(_scratch + i * POINT_SIZE);
          Bounds pb{ p.lat, p.lon, p.lat, p.lon };
          if (!eb.contains(pb)) {
            failed(BAD_FILE);
            return;
          }
        }
        _child += n;
      }
    } else if (_phase == 10) {
      put32(_scratch, COMMIT_MAGIC);
      put32(_scratch + 4, _verify.bytes);
      put32(_scratch + 8, _candidateGeneration);
      put32(_scratch + 12, _expectedCrc);
      put32(_scratch + 16, ~crc(0xffffffff, _scratch, 16));
      if (!_files->append(_scratch, FOOTER_SIZE) || !_files->finish()) {
        failed(IO_ERROR);
        return;
      }
      _phase = 13;
    } else if (_phase == 13) {
      // Flush has no error result in Arduino File. Read back the complete footer
      // before acknowledging activation, keeping the previous slot on failure.
      if (_files->size(_slot) != _verify.bytes + FOOTER_SIZE ||
          !_files->read(_slot, _verify.bytes, _scratch, FOOTER_SIZE) || u32(_scratch) != COMMIT_MAGIC ||
          u32(_scratch + 4) != _verify.bytes || u32(_scratch + 8) != _candidateGeneration ||
          u32(_scratch + 12) != _expectedCrc || ~crc(0xffffffff, _scratch, 16) != u32(_scratch + 16)) {
        failed(IO_ERROR);
        return;
      }
      _active = _slot;
      _header = _verify;
      _generation = _candidateGeneration;
      _phase = 0;
      _state = VERIFYING;
      _error = OK;
      _cacheStart = UINT32_MAX;
      _previewLoaded = 0;
      _phase = 11;
    } else if (_phase == 11) {
      uint32_t n = _header.previews - _previewLoaded;
      if (n > 32) n = 32;
      if (!read(_header.previewOffset + _previewLoaded * POINT_SIZE, _scratch, n * POINT_SIZE)) {
        _active = -1;
        _phase = 0;
        _state = FAILED;
        _error = IO_ERROR;
        return;
      }
      for (uint32_t i = 0; i < n; ++i)
        _preview[_previewLoaded + i] = point(_scratch + i * POINT_SIZE);
      _previewLoaded += n;
      if (_previewLoaded == _header.previews) {
        _phase = 0;
        if (_state != FAILED) _state = READY;
      }
    }
  }
  bool label(uint32_t offset, uint16_t bytes, char *out, size_t capacity) {
    if (!capacity) return false;
    out[0] = 0;
    if ((uint64_t)offset + bytes > _header.labelBytes) return false;
    size_t n = bytes;
    if (n >= capacity) n = capacity - 1;
    if (!read(_header.labelOffset + offset, (uint8_t *)out, n)) return false;
    // Remove an incomplete final UTF-8 sequence after truncation.
    if (n < bytes) {
      while (n && ((uint8_t)out[n - 1] & 0xc0) == 0x80)
        --n;
      if (n && ((uint8_t)out[n - 1] & 0x80)) --n;
    }
    out[n] = 0;
    return true;
  }
  bool name(char *out, size_t capacity) {
    return label(_header.nameOffset, _header.nameBytes, out, capacity);
  }
  bool preview(uint32_t i, Point &p) {
    if (i >= _previewLoaded) return false;
    p = _preview[i];
    return true;
  }
  bool checkpoint(uint32_t i, Checkpoint &c) {
    if (i >= _header.checkpoints ||
        !read(_header.checkpointOffset + i * CHECKPOINT_SIZE, _scratch, CHECKPOINT_SIZE))
      return false;
    c = { (int32_t)u32(_scratch), (int32_t)u32(_scratch + 4), u32(_scratch + 8),  u32(_scratch + 12),
          u32(_scratch + 16),     u32(_scratch + 20),         u32(_scratch + 24), u16(_scratch + 28) };
    return true;
  }
  void stop() {
    _alert = false;
    _following = false;
    _search = false;
    _fixGood = false;
    _outsideSince = 0;
    _off = false;
  }
  bool start(bool reverse) {
    if (!hasRoute() || working()) return false;
    _alert = false;
    _fixGood = false;
    _following = true;
    _reverse = reverse;
    _matched = false;
    _ambiguous = false;
    _arrived = false;
    _off = false;
    _outsideSince = 0;
    _segmentReady = false;
    _segment = reverse ? _header.segments - 1 : 0;
    _segmentStart = 0;
    _segmentEnd = _header.points - 1;
    // Segment boundary discovery runs incrementally before matching.
    _scanCursor = 0;
    _scanStart = 0;
    _scanEnd = 0;
    _depth = 0;
    _global = false;
    _search = false;
    _nextFix = 0;
    return true;
  }
  bool nextSegment() {
    if (!_following || !_arrived) return false;
    if ((!_reverse && _segment + 1 >= _header.segments) || (_reverse && _segment == 0)) return false;
    _segmentReady = false;
    _segment += _reverse ? -1 : 1;
    _matched = false;
    _arrived = false;
    _scanCursor = 0;
    _scanStart = 0;
    _scanEnd = 0;
    _nextFix = 0;
    return true;
  }
  void rejoin() {
    if (_following && _fixGood && _segmentReady) {
      _searchStarted = _nextFix - 1000;
      _outsideSince = 0;
      _arrived = false;
      startSearch(true);
    }
  }
  void settings(uint32_t metres, uint32_t seconds) {
    _offDistance = metres;
    _offDelay = seconds;
    _outsideSince = 0;
  }
  uint32_t offDistance() const { return _offDistance; }
  uint32_t offDelay() const { return _offDelay; }
  bool locating() const { return _following && (!_segmentReady || _search); }
  bool ambiguous() const { return _ambiguous; }
  bool arrived() const { return _arrived; }
  bool goodFix() const { return _fixGood; }
  bool poorFix() const { return _poor; }
  bool matched() const { return _matched; }
  uint32_t segment() const { return _segment; }
  uint32_t progressCm() const { return _progress; }
  float lateralMetres() const { return _lateral; }
  uint32_t remainingCm() const { return _reverse ? _progress : _header.distance - _progress; }
  void target(int32_t &lat, int32_t &lon) const {
    lat = _targetLat;
    lon = _targetLon;
  }
  bool takeAlert() {
    bool alert = _alert;
    _alert = false;
    return alert;
  }
  void fix(bool valid, bool quality, int32_t lat, int32_t lon, uint32_t now) {
    _poor = valid && !quality;
    _fixGood = valid && quality;
    if (!_fixGood) {
      _outsideSince = 0;
      _alert = false;
      _search = false;
      return;
    }
    if (_search && (uint32_t)(now - _searchStarted) > 5000) {
      _search = false;
      _outsideSince = 0;
      _nextFix = 0;
    }
    if (!_following || _arrived || !_segmentReady || _search || (int32_t)(now - _nextFix) < 0) return;
    _nextFix = now + 1000;
    Point previous{ _lat, _lon, 0, NO_ALT, 0 };
    const double movement = project(lat, lon, previous, previous).lateral;
    _jumpCm = (uint32_t)fmin(5000.0 + movement * 200, UINT32_MAX);
    _lat = lat;
    _lon = lon;
    if (_ambiguous) return;
    _searchStarted = now;
    startSearch(!_matched);
  }
  void navigationStep(uint32_t now) {
    if (!_following) return;
    if (!_segmentReady) {
      uint32_t end = _scanCursor + 32;
      if (end > _header.points) end = _header.points;
      for (; _scanCursor < end; ++_scanCursor) {
        Point p;
        if (!pointAt(_scanCursor, p)) {
          stop();
          return;
        }
        if (p.flags & SEG_START) {
          if (_scanStart == _segment) _segmentStart = _scanCursor;
          if (_scanStart == _segment + 1) {
            _segmentEnd = _scanCursor - 1;
            _segmentReady = true;
            return;
          }
          ++_scanStart;
        }
      }
      if (_scanCursor == _header.points) {
        _segmentReady = true;
        _segmentEnd = _header.points - 1;
      }
      return;
    }
    if (!_search || !_fixGood) return;
    if (_global) {
      if (!_depth) {
        finishSearch(now);
        return;
      }
      Frame &f = _stack[_depth - 1];
      if (_pageIndex != f.page && !read(_header.indexOffset + f.page * PAGE_SIZE, _page, PAGE_SIZE)) {
        stop();
        return;
      }
      _pageIndex = f.page;
      if (f.next >= _page[17]) {
        --_depth;
        return;
      }
      const uint8_t *e = _page + 32 + f.next * 24;
      ++f.next;
      if (boundDistance(_lat, _lon, bounds(e)) > _best + 10) return;
      uint32_t ref = u32(e + 16);
      if (_page[16] == 0) {
        if (_depth >= 32) {
          stop();
          return;
        }
        _stack[_depth++] = { ref, 0 };
      } else {
        _scanStart = ref;
        _scanCursor = ref;
        _scanEnd = ref + u32(e + 20);
        if (_scanStart < _segmentStart) _scanCursor = _segmentStart;
        if (_scanEnd > _segmentEnd) _scanEnd = _segmentEnd;
        _global = false; // restore index walk after this leaf
      }
    } else {
      uint32_t end = _scanCursor + 16;
      if (end > _scanEnd) end = _scanEnd;
      for (; _scanCursor < end; ++_scanCursor) {
        Point a, b;
        if (!pointAt(_scanCursor, a) || !pointAt(_scanCursor + 1, b)) {
          stop();
          return;
        }
        consider(_scanCursor, a, b);
      }
      if (_scanCursor >= _scanEnd) {
        if (_depth)
          _global = true;
        else
          finishSearch(now);
      }
    }
  }
};
static_assert(sizeof(RouteStore) < 4096, "Route cache/state must stay under 4KB regardless of file size");
} // namespace routes
