#pragma once

#include <Arduino.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>
#include <time.h>
#include "Persist.h"
#include "GeoUtils.h"

// RAM-only GPS trail ring buffer.
// Storage cost: CAPACITY(512) × sizeof(TrailPoint)(16 B, padded) = 8 KB,
// always resident in .bss (UITask::_trail, and ui_task is a global object —
// see main.cpp). Because the nRF52 heap region starts just above .bss, every
// byte added here is a byte taken from the heap: at 1024 points (16 KB) free
// heap fell to ~4 KB and the input/menu path started failing its allocations
// while the periodic redraw kept running. Keep this conservative. The trail
// survives auto-off (only
// the display blanks) but is lost on reboot — user explicitly snapshots to a
// LittleFS slot before powering down to keep it.
//
// Samples are simplified in-stream as they arrive (see addPoint()): a straight
// stretch is stored as just its two endpoints, no matter how long, while a
// turn still gets a vertex roughly every min-delta of deviation — so CAPACITY
// covers a much longer route than CAPACITY × min-delta would suggest.

struct TrailPoint {
  int32_t  lat_1e6;
  int32_t  lon_1e6;
  uint32_t ts;            // epoch seconds (RTC)
  uint8_t  flags;         // bit 0 = SEG_START (don't draw a line from the previous point)
  int16_t  altitude_m;    // filtered metres; INT16_MIN means unavailable (also on v1 trails)
};
static_assert(sizeof(TrailPoint) == 16, "Altitude must fit the existing trail point padding");

static const uint8_t TRAIL_FLAG_SEG_START = 0x01;

class TrailStore {
public:
  static const int CAPACITY = 512;
  static constexpr int16_t UNKNOWN_ALTITUDE = INT16_MIN;
  static constexpr int32_t UNKNOWN_ALTITUDE_MM = INT32_MIN;
  static const int CLIMB_THRESHOLD_M = 5;
  // _count is serialised as uint16_t in the save header — fail the build loudly
  // if CAPACITY is ever grown past what that can hold, rather than truncating.
  static_assert(CAPACITY <= 0xFFFF, "TrailStore::CAPACITY must fit in the uint16_t save-header count");

  // Fixed sampling cadence — matches the sensor manager's default GPS update
  // rate (1 s). Density is controlled by the min-delta gate (settings) rather
  // than by throttling the GPS poll. The NodePrefs::trail_interval_idx field
  // is retained as reserved for backwards compatibility but no longer used.
  static const uint16_t SAMPLING_SECS = 1;

  // Min-delta (metres) gates samples too close to the previous one.
  // Default 5 m: keeps walking jitter out, dense enough for a visible trail.
  // The index is a unit-agnostic "level" (0=finest … 3=coarsest); the actual
  // gate distance and its label follow the global metric/imperial preference.
  static const uint8_t MIN_DELTA_COUNT = 4;
  static uint16_t minDeltaMeters(uint8_t idx, bool imperial) {
    static const uint16_t MET[MIN_DELTA_COUNT] = { 5, 10, 25, 100 };
    static const uint16_t IMP[MIN_DELTA_COUNT] = { 5, 9, 23, 91 };  // ≈ 15/30/75/300 ft
    if (idx >= MIN_DELTA_COUNT) idx = 0;
    return imperial ? IMP[idx] : MET[idx];
  }
  static const char* minDeltaLabel(uint8_t idx, bool imperial) {
    static const char* MET[MIN_DELTA_COUNT] = { "5 m", "10 m", "25 m", "100 m" };
    static const char* IMP[MIN_DELTA_COUNT] = { "15 ft", "30 ft", "75 ft", "300 ft" };
    if (idx >= MIN_DELTA_COUNT) idx = 0;
    return imperial ? IMP[idx] : MET[idx];
  }

  // Speed / pace display units. UNITS_KMH / UNITS_MPH show speed; UNITS_PACE_KM
  // / UNITS_PACE_MI show time per distance ("pace"). Index 0 = km/h default.
  enum Units : uint8_t {
    UNITS_KMH     = 0,
    UNITS_MPH     = 1,
    UNITS_PACE_KM = 2,
    UNITS_PACE_MI = 3,
  };
  static const uint8_t UNITS_COUNT = 4;
  static const char* unitLabel(uint8_t idx) {
    static const char* L[UNITS_COUNT] = { "km/h", "mph", "min/km", "min/mi" };
    return L[idx < UNITS_COUNT ? idx : 0];
  }
  static bool unitIsPace(uint8_t idx) { return idx == UNITS_PACE_KM || idx == UNITS_PACE_MI; }

  bool isActive() const { return _active; }
  void setActive(bool a) {
    if (a != _active) resetElevationFilter();
    if (a && !_active) {
      // off → on: start a new session timer.
      _session_start_ms = millis();
      _paused = false;
    } else if (_active && !a) {
      // on → off: bank the elapsed of this session and arm a segment break
      // so the renderer doesn't draw a straight line through the dead time.
      if (_session_start_ms != 0) {
        _accumulated_ms   += millis() - _session_start_ms;
        _session_start_ms  = 0;
      }
      flushPending();
      _pending_seg_break = true;
      _paused = false;
    }
    _active = a;
  }

  // Auto-pause: freeze the active trail without ending the session. Banks the
  // running session time (so elapsedSeconds() stops advancing) and arms a
  // segment break so the map doesn't draw a line across the idle gap; resuming
  // restarts the session timer. Distinct from setActive() — the trail stays
  // "on" (UI shows "paused", home-screen blink keeps going). No-op if inactive.
  bool isPaused() const { return _paused; }
  void setPaused(bool p) {
    if (!_active || p == _paused) return;
    resetElevationFilter();
    if (p) {
      if (_session_start_ms != 0) {
        _accumulated_ms  += millis() - _session_start_ms;
        _session_start_ms = 0;
      }
      flushPending();
      _pending_seg_break = true;
    } else {
      _session_start_ms = millis();  // resume timing from now
    }
    _paused = p;
  }

  int  count() const { return _count; }
  bool empty() const { return _count == 0; }

  // i = 0 → oldest entry, i = count()-1 → newest.
  const TrailPoint& at(int i) const { return _buf[(_head + i) % CAPACITY]; }
  const TrailPoint& first() const   { return at(0); }
  const TrailPoint& last()  const   { return at(_count - 1); }

  void clear() {
    _head = 0; _count = 0;
    _pending_seg_break = false;
    _accumulated_ms    = 0;
    _session_start_ms  = 0;
    _paused            = false;
    _has_pending       = false;
    _ascent_m = _descent_m = 0;
    _min_alt_m = _max_alt_m = UNKNOWN_ALTITUDE;
    resetElevationFilter();
  }

  // Returns true if the sample was accepted (passed the min-delta gate) —
  // whether that landed it as a new committed vertex right away, or just
  // extended the pending candidate (see below). First point of the ring and
  // the first point after a stop/start cycle get flagged TRAIL_FLAG_SEG_START
  // so the map renderer breaks the line.
  //
  // Straight-run simplification (a fixed-corridor / Reumann–Witkam pass): a
  // sample is only ever a *candidate* vertex (_pending) until a later one
  // proves the run has bent. The corridor is the straight line anchored at the
  // last committed vertex and aimed at the first sample of this run (_dir);
  // each new sample is tested against *that fixed line*. While it stays within
  // CORRIDOR_FACTOR x min_delta_m of the corridor the run is still "straight
  // enough", so drop the intermediate and extend; once a sample leaves the
  // corridor the previous in-corridor sample (_pending) was the last good
  // vertex, so commit it and open a fresh corridor from there.
  //
  // The corridor is deliberately tighter than the min-delta gate itself
  // (CORRIDOR_FACTOR < 1): the gate is the noise floor for *rejecting* samples
  // outright, while the corridor decides when a run has bent enough to need a
  // new vertex. Field data showed plenty of headroom (4 km at a 10 m gate cost
  // just 38 of the 512-point capacity), so a tighter corridor trades some of
  // that headroom for a polyline that hugs the real track more closely.
  //
  // The fixed direction is the whole point: testing the newest sample against
  // a line that re-aims at the newest sample (or testing the previous
  // candidate, which always sits right beside that moving endpoint where the
  // cross-track is near zero) lets a long gentle curve slip through one
  // sub-min_delta step at a time and collapse to a single chord that deviates
  // arbitrarily far. Anchoring the direction bounds the stored polyline to
  // ~CORRIDOR_FACTOR x min_delta_m of the real track, while a straight road
  // still costs just its two endpoints no matter how long it is.
  // How much tighter the simplification corridor is than the min-delta gate
  // (see addPoint() above). 1.0 would track the gate exactly; lowering this
  // commits more vertices per route (more fidelity, less reduction).
  static constexpr float CORRIDOR_FACTOR = 0.5f;

  bool addPoint(int32_t lat_1e6, int32_t lon_1e6, uint32_t ts, uint16_t min_delta_m,
                int32_t altitude_mm = UNKNOWN_ALTITUDE_MM) {
    int16_t altitude = filterAltitude(altitude_mm);
    const TrailPoint* ref = _has_pending ? &_pending : (_count > 0 ? &last() : nullptr);
    if (ref && !_pending_seg_break) {
      float d = haversineMeters(ref->lat_1e6, ref->lon_1e6, lat_1e6, lon_1e6);
      if (d < (float)min_delta_m) return false;
    }
    // Count climbing only on accepted movement, not while GPS wanders in place.
    recordElevation(altitude);

    if (_count == 0 || _pending_seg_break) {
      flushPending();   // shouldn't normally have one here, but never lose real distance
      commitPoint(lat_1e6, lon_1e6, ts, TRAIL_FLAG_SEG_START, altitude);
      _pending_seg_break = false;
      return true;
    }

    TrailPoint sample{ lat_1e6, lon_1e6, ts, 0, altitude };
    if (!_has_pending) {
      _pending     = sample;   // last in-corridor sample (the commit candidate)
      _dir         = sample;   // fixes the corridor direction: last() → _dir
      _has_pending = true;
      return true;
    }

    if (crossTrackMeters(last(), _dir, sample) <= (float)min_delta_m * CORRIDOR_FACTOR
        && !elevationBend(last(), _dir, sample)) {
      _pending = sample;                                            // within corridor — extend
    } else {
      commitPoint(_pending.lat_1e6, _pending.lon_1e6, _pending.ts, 0, _pending.altitude_m);
      _pending = sample;   // the exiting sample opens the next run...
      _dir     = sample;   // ...and fixes its corridor direction from the just-committed vertex
    }
    return true;
  }

  // Sum of pairwise Haversine deltas across the whole ring, skipping segment
  // boundaries (a SEG_START point isn't reached from its predecessor). The
  // uncommitted candidate (_pending) is counted too, so the live total doesn't
  // stall over a long straight run — where simplification holds the whole
  // stretch as one pending point until a bend forces a commit (see addPoint()).
  uint32_t totalDistanceMeters() const {
    float d = 0;
    for (int i = 1; i < _count; i++) {
      if (at(i).flags & TRAIL_FLAG_SEG_START) continue;
      d += haversineMeters(at(i - 1).lat_1e6, at(i - 1).lon_1e6,
                            at(i).lat_1e6,     at(i).lon_1e6);
    }
    if (_has_pending && _count > 0)
      d += haversineMeters(last().lat_1e6, last().lon_1e6,
                            _pending.lat_1e6, _pending.lon_1e6);
    return (uint32_t)d;
  }

  // Cumulative active tracking time across all start→stop sessions, in
  // seconds. Counts ticks while the trail is on, freezes while it's off.
  // millis()-based so it doesn't depend on RTC sync.
  uint32_t elapsedSeconds() const {
    uint32_t ms = _accumulated_ms;
    if (_active && _session_start_ms != 0) ms += millis() - _session_start_ms;
    return ms / 1000;
  }

  // Average speed in km/h = total distance / cumulative active time.
  float avgSpeedKmh() const {
    uint32_t es = elapsedSeconds();
    if (es == 0) return 0;
    return (float)totalDistanceMeters() / (float)es * 3.6f;
  }

  int16_t currentAltitudeMeters() const {
    return !_active && _count > 0 ? last().altitude_m : _current_alt_m;
  }
  int16_t minAltitudeMeters() const { return _min_alt_m; }
  int16_t maxAltitudeMeters() const { return _max_alt_m; }
  uint32_t ascentMeters() const { return _ascent_m; }
  uint32_t descentMeters() const { return _descent_m; }
  bool hasElevation() const { return _min_alt_m != UNKNOWN_ALTITUDE; }
  // A missing/poor fix must not become a climb across the acquisition gap.
  void breakElevationSampling() { resetElevationFilter(); }
  int profileCount() const { return _count + (_has_pending ? 1 : 0); }
  const TrailPoint& profileAt(int i) const { return i == _count ? _pending : at(i); }

  // Compute bounding box across all points. Returns false if empty.
  bool boundingBox(int32_t& min_lat, int32_t& min_lon,
                   int32_t& max_lat, int32_t& max_lon) const {
    if (_count == 0) return false;
    min_lat = max_lat = first().lat_1e6;
    min_lon = max_lon = first().lon_1e6;
    for (int i = 1; i < _count; i++) {
      const auto& p = at(i);
      if (p.lat_1e6 < min_lat) min_lat = p.lat_1e6;
      if (p.lat_1e6 > max_lat) max_lat = p.lat_1e6;
      if (p.lon_1e6 < min_lon) min_lon = p.lon_1e6;
      if (p.lon_1e6 > max_lon) max_lon = p.lon_1e6;
    }
    return true;
  }

  // Persistent snapshot — single slot at the given filesystem path.
  // Layout: 4-byte magic "TRAL", uint8 version, uint8 reserved, uint16 count,
  // uint32 accumulated_ms, then v2 ascent/descent(uint32 each), min/max altitude
  // (int16 each), followed by `count` 16-byte points. v1 has no elevation fields;
  // its point padding is explicitly discarded when loading/exporting.
  static const uint32_t SAVE_MAGIC = 0x4C415254;  // "TRAL"
  static const uint8_t  SAVE_VERSION = 2;

  // Caller supplies an opened, writable File (the FS-open call is
  // platform-specific). Returns true if the header and every point wrote
  // cleanly. The file is left open for the caller to close.
  template <typename F>
  bool writeTo(F& file) {
    flushPending();   // the snapshot should include the latest position, not lag behind it
    if (!persist::writeHeader(file, SAVE_MAGIC, SAVE_VERSION, (uint16_t)_count)) return false;
    uint32_t accum = currentAccumulatedMs();
    if (file.write((uint8_t*)&accum, sizeof(accum)) != sizeof(accum)) return false;
    if (file.write((uint8_t*)&_ascent_m, sizeof(_ascent_m)) != sizeof(_ascent_m)
        || file.write((uint8_t*)&_descent_m, sizeof(_descent_m)) != sizeof(_descent_m)
        || file.write((uint8_t*)&_min_alt_m, sizeof(_min_alt_m)) != sizeof(_min_alt_m)
        || file.write((uint8_t*)&_max_alt_m, sizeof(_max_alt_m)) != sizeof(_max_alt_m)) return false;
    for (int i = 0; i < _count; i++) {
      if (file.write((uint8_t*)&at(i), sizeof(TrailPoint)) != sizeof(TrailPoint)) return false;
    }
    return true;
  }

  template <typename F>
  bool readFrom(F& file) {
    uint16_t cnt = 0;
    uint32_t accum = 0;
    uint8_t version = 0;
    ElevationStats stats{};
    if (!readSnapshotHeader(file, version, cnt, accum, stats)) return false;
    if (_active) {
      _active = false;
      _session_start_ms = 0;
    }
    _paused = false;
    _has_pending = false;   // any candidate belonged to the session being replaced
    _head = 0;
    _count = 0;
    resetElevationFilter();
    for (int i = 0; i < cnt; i++) {
      TrailPoint p{};
      if (!readSnapshotPoint(file, version, p)) { clear(); return false; }
      _buf[_count++] = p;
    }
    _ascent_m = stats.ascent;
    _descent_m = stats.descent;
    _min_alt_m = stats.minimum;
    _max_alt_m = stats.maximum;
    if (_count > 0) _current_alt_m = last().altitude_m;
    _accumulated_ms = accum;
    _pending_seg_break = true;
    return true;
  }

  // GPX writers — shared helpers so we can dump from RAM and from flash
  // through the same formatting code.

  template <typename S>
  static size_t gpxHeader(S& out) {
    size_t n = 0;
    n += out.print(F("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"));
    n += out.print(F("<gpx version=\"1.1\" creator=\"MeshCore\" "
                      "xmlns=\"http://www.topografix.com/GPX/1/1\">\n"));
    return n;
  }

  template <typename S>
  static size_t gpxTrackOpen(S& out, const char* name) {
    size_t n = 0;
    n += out.print(F("<trk><name>"));
    n += out.print(name);
    n += out.print(F("</name>\n"));
    return n;
  }

  // Emit saved waypoints as <wpt> elements. In GPX 1.1 these must precede the
  // <trk>. Duck-typed over any store exposing count()/at(i) whose entries have
  // lat_1e6 / lon_1e6 / ts / label, so Trail.h stays decoupled from Waypoint.h.
  template <typename S, typename WP>
  static size_t gpxWaypoints(S& out, WP& store) {
    size_t n = 0;
    for (int i = 0; i < store.count(); i++) {
      const auto& w = store.at(i);
      // XML-escape the user label (&, <, > only).
      char esc[64]; int e = 0;
      for (const char* p = w.label; *p && e < (int)sizeof(esc) - 6; p++) {
        if      (*p == '&') { memcpy(esc + e, "&amp;", 5); e += 5; }
        else if (*p == '<') { memcpy(esc + e, "&lt;",  4); e += 4; }
        else if (*p == '>') { memcpy(esc + e, "&gt;",  4); e += 4; }
        else                  esc[e++] = *p;
      }
      esc[e] = '\0';
      char buf[160];
      int len = snprintf(buf, sizeof(buf),
        "<wpt lat=\"%.6f\" lon=\"%.6f\"><name>%s</name>",
        w.lat_1e6 / 1.0e6, w.lon_1e6 / 1.0e6, esc);
      if (len > 0) {
        if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;  // truncated: emit chars only, not the NUL
        n += out.write((const uint8_t*)buf, (size_t)len);
      }
      if (w.ts > 1000000000UL) {                 // append <time> when the RTC was set
        time_t t = (time_t)w.ts;
        struct tm* gt = ::gmtime(&t);
        if (gt) {
          len = snprintf(buf, sizeof(buf),
            "<time>%04d-%02d-%02dT%02d:%02d:%02dZ</time>",
            gt->tm_year + 1900, gt->tm_mon + 1, gt->tm_mday,
            gt->tm_hour, gt->tm_min, gt->tm_sec);
          if (len > 0) { if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;
                         n += out.write((const uint8_t*)buf, (size_t)len); }
        }
      }
      n += out.print(F("</wpt>\n"));
    }
    return n;
  }

  template <typename S>
  static size_t gpxFooter(S& out, bool in_segment) {
    size_t n = 0;
    if (in_segment) n += out.print(F("</trkseg>\n"));
    n += out.print(F("</trk></gpx>\n"));
    return n;
  }

  // Emit a single <trkpt>; opens a <trkseg> on a segment boundary. Updates
  // `in_segment` to track open/close pairing.
  template <typename S>
  static size_t gpxPoint(S& out, const TrailPoint& p, bool first, bool& in_segment) {
    size_t n = 0;
    bool seg_start = first || (p.flags & TRAIL_FLAG_SEG_START);
    if (seg_start) {
      if (in_segment) n += out.print(F("</trkseg>\n"));
      n += out.print(F("<trkseg>\n"));
      in_segment = true;
    }
    char buf[152];
    time_t t = (time_t)p.ts;
    struct tm* gt = ::gmtime(&t);
    if (!gt) return n;  // defensive: skip malformed timestamps
    char elevation[24] = "";
    if (p.altitude_m != UNKNOWN_ALTITUDE)
      snprintf(elevation, sizeof(elevation), "<ele>%d</ele>", (int)p.altitude_m);
    int len = snprintf(buf, sizeof(buf),
      "<trkpt lat=\"%.6f\" lon=\"%.6f\">%s<time>%04d-%02d-%02dT%02d:%02d:%02dZ</time></trkpt>\n",
      p.lat_1e6 / 1.0e6, p.lon_1e6 / 1.0e6,
      elevation,
      gt->tm_year + 1900, gt->tm_mon + 1, gt->tm_mday,
      gt->tm_hour, gt->tm_min, gt->tm_sec);
    if (len < 0) return n;
    if ((size_t)len >= sizeof(buf)) len = sizeof(buf) - 1;  // truncated: emit chars only, not the NUL
    n += out.write((const uint8_t*)buf, (size_t)len);
    return n;
  }

  // Dump the live RAM ring as GPX (with saved waypoints). Returns bytes written.
  template <typename S, typename WP>
  size_t exportGpx(S& out, WP& wpts, const char* trk_name = "MeshCore Trail") {
    size_t total = gpxHeader(out);
    total += gpxWaypoints(out, wpts);
    total += gpxTrackOpen(out, trk_name);
    bool in_segment = false;
    for (int i = 0; i < _count; i++) {
      total += gpxPoint(out, at(i), i == 0, in_segment);
    }
    if (_has_pending) total += gpxPoint(out, _pending, _count == 0, in_segment);
    total += gpxFooter(out, in_segment);
    return total;
  }

  // Stream a saved trail straight from the open file as GPX without
  // touching the live RAM ring. Returns 0 on format mismatch.
  template <typename F, typename S, typename WP>
  static size_t exportGpxFromFile(F& file, S& out, WP& wpts, const char* trk_name = "MeshCore Trail") {
    uint16_t cnt = 0;
    uint32_t accum = 0;
    uint8_t version = 0;
    ElevationStats stats{};
    if (!readSnapshotHeader(file, version, cnt, accum, stats)) return 0;

    size_t total = gpxHeader(out);
    total += gpxWaypoints(out, wpts);
    total += gpxTrackOpen(out, trk_name);
    bool in_segment = false;
    for (uint16_t i = 0; i < cnt; i++) {
      TrailPoint p{};
      if (!readSnapshotPoint(file, version, p)) return 0;
      total += gpxPoint(out, p, i == 0, in_segment);
    }
    total += gpxFooter(out, in_segment);
    return total;
  }

  uint32_t currentAccumulatedMs() const {
    uint32_t ms = _accumulated_ms;
    if (_active && _session_start_ms != 0) ms += millis() - _session_start_ms;
    return ms;
  }

  // Approximate great-circle distance in metres. Delegates to the shared
  // geo:: Haversine (km) so there is a single implementation.
  static float haversineMeters(int32_t la1, int32_t lo1, int32_t la2, int32_t lo2) {
    return geo::haversineKm(la1, lo1, la2, lo2) * 1000.0f;
  }

private:
  TrailPoint _buf[CAPACITY];
  int      _head             = 0;
  int      _count            = 0;
  bool     _active           = false;
  bool     _paused           = false;  // auto-paused (active but timer/sampling frozen)
  bool     _pending_seg_break = false;  // next addPoint flags itself SEG_START
  uint32_t _accumulated_ms   = 0;       // banked active time across previous sessions
  uint32_t _session_start_ms = 0;       // millis() of the current active session, 0 if none

  // Streaming simplification: a sample that passed the min-delta gate but
  // hasn't been committed as a real vertex yet — see addPoint(). _dir is the
  // first sample of the current run; last()→_dir fixes the corridor direction
  // every later sample of the run is tested against.
  bool       _has_pending = false;
  TrailPoint _pending;
  TrailPoint _dir;

  struct ElevationStats {
    uint32_t ascent = 0, descent = 0;
    int16_t minimum = UNKNOWN_ALTITUDE, maximum = UNKNOWN_ALTITUDE;
  };
  uint32_t _ascent_m = 0, _descent_m = 0;
  int16_t _min_alt_m = UNKNOWN_ALTITUDE, _max_alt_m = UNKNOWN_ALTITUDE;
  int16_t _current_alt_m = UNKNOWN_ALTITUDE, _elevation_anchor_m = UNKNOWN_ALTITUDE;
  int32_t _alt_window[5]{};
  uint8_t _alt_count = 0, _alt_next = 0;
  float _filtered_alt_m = 0;
  uint32_t _last_alt_sample_ms = 0;

  void resetElevationFilter() {
    _alt_count = _alt_next = 0;
    _current_alt_m = _elevation_anchor_m = UNKNOWN_ALTITUDE;
  }

  int16_t filterAltitude(int32_t mm) {
    uint32_t now = millis();
    if (mm == UNKNOWN_ALTITUDE_MM || mm < -32767000 || mm > 32767000) {
      resetElevationFilter();
      return UNKNOWN_ALTITUDE;
    }
    if (_alt_count && (uint32_t)(now - _last_alt_sample_ms) > 10000) resetElevationFilter();
    _last_alt_sample_ms = now;
    _alt_window[_alt_next] = mm;
    _alt_next = (_alt_next + 1) % 5;
    if (_alt_count < 5) ++_alt_count;
    if (_alt_count < 5) return UNKNOWN_ALTITUDE;
    // Median rejects isolated spikes; EMA smooths the remaining vertical jitter.
    int32_t sorted[5];
    memcpy(sorted, _alt_window, sizeof(sorted));
    for (int i = 1; i < 5; ++i) {
      int32_t value = sorted[i]; int j = i;
      while (j > 0 && sorted[j - 1] > value) { sorted[j] = sorted[j - 1]; --j; }
      sorted[j] = value;
    }
    float median = sorted[2] / 1000.0f;
    if (_current_alt_m == UNKNOWN_ALTITUDE) _filtered_alt_m = median;
    else _filtered_alt_m += (median - _filtered_alt_m) * 0.25f;
    _current_alt_m = (int16_t)lroundf(_filtered_alt_m);
    return _current_alt_m;
  }

  void recordElevation(int16_t altitude) {
    if (altitude == UNKNOWN_ALTITUDE) { _elevation_anchor_m = UNKNOWN_ALTITUDE; return; }
    if (_min_alt_m == UNKNOWN_ALTITUDE || altitude < _min_alt_m) _min_alt_m = altitude;
    if (_max_alt_m == UNKNOWN_ALTITUDE || altitude > _max_alt_m) _max_alt_m = altitude;
    if (_elevation_anchor_m != UNKNOWN_ALTITUDE) {
      int delta = (int)altitude - _elevation_anchor_m;
      if (abs(delta) < CLIMB_THRESHOLD_M) return;
      if (delta > 0) _ascent_m += delta;
      else _descent_m += -delta;
    }
    _elevation_anchor_m = altitude;
  }

  // Preserve crests/valleys even when a route is horizontally straight.
  static bool elevationBend(const TrailPoint& a, const TrailPoint& b, const TrailPoint& c) {
    if (a.altitude_m == UNKNOWN_ALTITUDE || b.altitude_m == UNKNOWN_ALTITUDE
        || c.altitude_m == UNKNOWN_ALTITUDE)
      return (a.altitude_m == UNKNOWN_ALTITUDE) != (b.altitude_m == UNKNOWN_ALTITUDE)
          || (b.altitude_m == UNKNOWN_ALTITUDE) != (c.altitude_m == UNKNOWN_ALTITUDE);
    float ab = haversineMeters(a.lat_1e6, a.lon_1e6, b.lat_1e6, b.lon_1e6);
    float ac = haversineMeters(a.lat_1e6, a.lon_1e6, c.lat_1e6, c.lon_1e6);
    if (ab < 0.01f) return false;
    // Fix the grade at the start of the run, like the horizontal corridor.
    // Re-aiming at each new endpoint would erase a gradual crest/valley.
    float expected = a.altitude_m + (b.altitude_m - a.altitude_m) * ac / ab;
    return fabsf(c.altitude_m - expected) >= 3.0f;
  }

  template <typename F>
  static bool readSnapshotHeader(F& file, uint8_t& version, uint16_t& count,
                                 uint32_t& accumulated, ElevationStats& stats) {
    uint32_t magic = 0;
    uint8_t reserved = 0;
    if (file.read((uint8_t*)&magic, 4) != 4 || magic != SAVE_MAGIC
        || file.read(&version, 1) != 1 || (version != 1 && version != SAVE_VERSION)
        || file.read(&reserved, 1) != 1 || file.read((uint8_t*)&count, 2) != 2
        || count > CAPACITY || file.read((uint8_t*)&accumulated, 4) != 4) return false;
    if (version == 1) return true;
    return file.read((uint8_t*)&stats.ascent, 4) == 4
        && file.read((uint8_t*)&stats.descent, 4) == 4
        && file.read((uint8_t*)&stats.minimum, 2) == 2
        && file.read((uint8_t*)&stats.maximum, 2) == 2
        && ((stats.minimum == UNKNOWN_ALTITUDE && stats.maximum == UNKNOWN_ALTITUDE)
            || (stats.minimum != UNKNOWN_ALTITUDE && stats.maximum != UNKNOWN_ALTITUDE
                && stats.minimum <= stats.maximum));
  }

  template <typename F>
  static bool readSnapshotPoint(F& file, uint8_t version, TrailPoint& point) {
    if (file.read((uint8_t*)&point, sizeof(point)) != (int)sizeof(point)) return false;
    if (version == 1) point.altitude_m = UNKNOWN_ALTITUDE;
    return true;
  }

  void commitPoint(int32_t lat_1e6, int32_t lon_1e6, uint32_t ts, uint8_t flags, int16_t altitude) {
    int pos;
    if (_count < CAPACITY) {
      pos = (_head + _count) % CAPACITY;
      _count++;
    } else {
      pos = _head;
      _head = (_head + 1) % CAPACITY;
    }
    _buf[pos].lat_1e6 = lat_1e6;
    _buf[pos].lon_1e6 = lon_1e6;
    _buf[pos].ts      = ts;
    _buf[pos].flags   = flags;
    _buf[pos].altitude_m = altitude;
  }

  // Commit the pending candidate (if any) as a real vertex. Called before a
  // segment break (stop/pause/save) so the last stretch of a straight run is
  // never silently dropped just because no bend came along to force a commit.
  void flushPending() {
    if (!_has_pending) return;
    commitPoint(_pending.lat_1e6, _pending.lon_1e6, _pending.ts, 0, _pending.altitude_m);
    _has_pending = false;
  }

  // Perpendicular ("cross-track") distance from q to the line through a→b, in
  // metres. Planar approximation (equirectangular, centred at a) — accurate
  // enough at trail scale, where consecutive points are metres to a few km
  // apart. Falls back to a straight distance to a if a and b coincide.
  static float crossTrackMeters(const TrailPoint& a, const TrailPoint& b, const TrailPoint& q) {
    static const float M_PER_DEG = 111320.0f;   // metres per degree of latitude
    float lat_rad   = (a.lat_1e6 * 1e-6f) * ((float)M_PI / 180.0f);
    float lon_scale = cosf(lat_rad);
    float bx = (b.lon_1e6 - a.lon_1e6) * 1e-6f * M_PER_DEG * lon_scale;
    float by = (b.lat_1e6 - a.lat_1e6) * 1e-6f * M_PER_DEG;
    float ab_len = sqrtf(bx * bx + by * by);
    if (ab_len < 0.01f) return haversineMeters(a.lat_1e6, a.lon_1e6, q.lat_1e6, q.lon_1e6);
    float qx = (q.lon_1e6 - a.lon_1e6) * 1e-6f * M_PER_DEG * lon_scale;
    float qy = (q.lat_1e6 - a.lat_1e6) * 1e-6f * M_PER_DEG;
    float cross = bx * qy - by * qx;
    return fabsf(cross) / ab_len;
  }
};
