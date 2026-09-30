#pragma once
// Shared wire/disk contract. Explicit byte encoding avoids ABI/padding dependencies.

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace routes {
static constexpr uint32_t MAGIC = 0x3152434d;        // MCR1
static constexpr uint32_t COMMIT_MAGIC = 0x544d4352; // RCMT
static constexpr uint16_t VERSION = 1;
static constexpr uint32_t HEADER_SIZE = 96, POINT_SIZE = 16, CHECKPOINT_SIZE = 32;
static constexpr uint32_t PAGE_SIZE = 416, FOOTER_SIZE = 20;
static constexpr uint16_t SEG_START = 1, PROFILE_GAP = 2; // PROFILE_GAP is allowed only in previews.
static constexpr int16_t NO_ALT = INT16_MIN;
static constexpr uint8_t COMMAND = 0xf0, RESPONSE = 0x7f, PROTOCOL = 1;
static constexpr uint16_t CHUNK_SIZE = 144;
enum Op : uint8_t { CAPABILITIES = 1, INFO, BEGIN, DATA, STATUS, COMMIT, ABORT, DELETE_ROUTE };
enum Error : uint8_t {
  OK = 0,
  UNSUPPORTED,
  BUSY,
  BAD_REQUEST,
  NO_SPACE,
  IO_ERROR,
  BAD_FILE,
  BAD_SESSION,
  BAD_OFFSET,
  USB_ONLY
};
enum State : uint8_t { CHECKING = 0, READY, EMPTY, RECEIVING, VERIFYING, FAILED };
inline uint16_t u16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
inline uint32_t u32(const uint8_t *p) {
  return u16(p) | ((uint32_t)u16(p + 2) << 16);
}
inline void put16(uint8_t *p, uint16_t v) {
  p[0] = v;
  p[1] = v >> 8;
}
inline void put32(uint8_t *p, uint32_t v) {
  put16(p, v);
  put16(p + 2, v >> 16);
}
inline uint32_t crc(uint32_t value, const uint8_t *data, size_t n) {
  while (n--) {
    value ^= *data++;
    for (int i = 0; i < 8; ++i)
      value = (value >> 1) ^ ((value & 1) ? 0xedb88320UL : 0);
  }
  return value;
}
struct Point {
  int32_t lat, lon;
  uint32_t distance;
  int16_t altitude;
  uint16_t flags;
};
inline Point point(const uint8_t *p) {
  return { (int32_t)u32(p), (int32_t)u32(p + 4), u32(p + 8), (int16_t)u16(p + 12), u16(p + 14) };
}
inline bool validCoordinate(int32_t lat, int32_t lon) {
  return lat >= -90000000 && lat <= 90000000 && lon >= -180000000 && lon <= 180000000;
}
struct Bounds {
  int32_t minLat, minLon, maxLat, maxLon;
  bool valid() const {
    return validCoordinate(minLat, minLon) && validCoordinate(maxLat, maxLon) && minLat <= maxLat &&
           minLon <= maxLon;
  }
  bool contains(const Bounds &b) const {
    return minLat <= b.minLat && minLon <= b.minLon && maxLat >= b.maxLat && maxLon >= b.maxLon;
  }
};
inline Bounds bounds(const uint8_t *p) {
  return { (int32_t)u32(p), (int32_t)u32(p + 4), (int32_t)u32(p + 8), (int32_t)u32(p + 12) };
}
struct Header {
  uint32_t bytes = 0, points = 0, checkpoints = 0, previews = 0, pages = 0;
  uint32_t pointOffset = 0, checkpointOffset = 0, labelOffset = 0, previewOffset = 0, indexOffset = 0;
  uint32_t root = 0, distance = 0, segments = 0, labelBytes = 0, nameOffset = 0;
  uint16_t nameBytes = 0;
  Bounds bbox{};
  bool decode(const uint8_t *p) {
    if (u32(p) != MAGIC || u16(p + 4) != VERSION || u16(p + 6) != HEADER_SIZE || u32(p + 92) != 0 ||
        ~crc(0xffffffff, p, 88) != u32(p + 88) || u16(p + 70) != 0)
      return false;
    bytes = u32(p + 8);
    points = u32(p + 12);
    checkpoints = u32(p + 16);
    previews = u32(p + 20);
    pages = u32(p + 24);
    pointOffset = u32(p + 28);
    checkpointOffset = u32(p + 32);
    labelOffset = u32(p + 36);
    previewOffset = u32(p + 40);
    indexOffset = u32(p + 44);
    root = u32(p + 48);
    distance = u32(p + 52);
    segments = u32(p + 56);
    labelBytes = u32(p + 60);
    nameOffset = u32(p + 64);
    nameBytes = u16(p + 68);
    bbox = bounds(p + 72);
    uint64_t cp = (uint64_t)HEADER_SIZE + points * uint64_t(POINT_SIZE);
    uint64_t labels = cp + checkpoints * uint64_t(CHECKPOINT_SIZE), preview = labels + labelBytes;
    uint64_t index = preview + previews * uint64_t(POINT_SIZE), end = index + pages * uint64_t(PAGE_SIZE);
    return points >= 2 && segments > 0 && segments <= points && previews >= 2 && previews <= 64 &&
           pages > 0 && root == pages - 1 && pointOffset == HEADER_SIZE && checkpointOffset == cp &&
           labelOffset == labels && previewOffset == preview && indexOffset == index && bytes == end &&
           end <= UINT32_MAX - FOOTER_SIZE && nameBytes > 0 &&
           (uint64_t)nameOffset + nameBytes <= labelBytes && bbox.valid();
  }
};
struct Capacity {
  uint32_t total = 0, used = 0, reserve = 0, reclaimable = 0, maximum = 0;
};
// Storage adapter owns actual filesystem handles. Core is usable with native fake filesystems.
class Files {
public:
  virtual ~Files() {}
  virtual uint32_t size(uint8_t slot) = 0;
  virtual bool read(uint8_t slot, uint32_t offset, uint8_t *dest, size_t n) = 0;
  virtual bool create(uint8_t slot) = 0;
  virtual bool append(const uint8_t *data, size_t n) = 0;
  virtual bool finish() = 0;
  virtual void cancel() = 0;
  virtual bool erase(uint8_t slot) = 0;
  virtual bool capacity(Capacity &c) = 0;
};
struct Checkpoint {
  int32_t lat, lon;
  uint32_t distance, segment, pointIndex, lateral, labelOffset;
  uint16_t labelBytes;
};
inline double wrapRadians(double lon) {
  while (lon > M_PI)
    lon -= 2 * M_PI;
  while (lon < -M_PI)
    lon += 2 * M_PI;
  return lon;
}
struct Projection {
  float lateral, fraction;
  int32_t lat, lon;
};
inline Projection project(int32_t lat, int32_t lon, const Point &a, const Point &b) {
  const double r = M_PI / 180.0 / 1e6, earth = 6371000.0;
  double coslat = cos(lat * r), ax = wrapRadians((a.lon - (double)lon) * r) * coslat * earth,
         ay = (a.lat - (double)lat) * r * earth;
  double bx = wrapRadians((b.lon - (double)lon) * r) * coslat * earth, by = (b.lat - (double)lat) * r * earth;
  double dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy;
  double f = len > 0 ? -(ax * dx + ay * dy) / len : 0;
  if (f < 0) f = 0;
  if (f > 1) f = 1;
  double targetlon = a.lon + wrapRadians((b.lon - (double)a.lon) * r) / r * f;
  if (targetlon > 180000000) targetlon -= 360000000;
  if (targetlon < -180000000) targetlon += 360000000;
  return { (float)hypot(ax + f * dx, ay + f * dy), (float)f,
           (int32_t)lround(a.lat + (b.lat - (double)a.lat) * f), (int32_t)lround(targetlon) };
}
// Conservative spherical lower bound; longitude intentionally ignored near the poles.
inline float boundDistance(int32_t lat, int32_t lon, const Bounds &b) {
  double r = M_PI / 180.0 / 1e6, la = lat * r;
  double low = b.minLat * r, high = b.maxLat * r;
  double delta = 0;
  if (lon < b.minLon)
    delta = fmin((b.minLon - (double)lon) * r, (lon + 360000000.0 - b.maxLon) * r);
  else if (lon > b.maxLon)
    delta = fmin((lon - (double)b.maxLon) * r, (b.minLon + 360000000.0 - lon) * r);
  if (delta < 0) delta = 0;
  double optimum = atan2(sin(la), cos(la) * cos(delta));
  if (optimum < low) optimum = low;
  if (optimum > high) optimum = high;
  double dot = sin(la) * sin(optimum) + cos(la) * cos(optimum) * cos(delta);
  if (dot > 1) dot = 1;
  if (dot < -1) dot = -1;
  return (float)(6371000.0 * acos(dot));
}
} // namespace routes
