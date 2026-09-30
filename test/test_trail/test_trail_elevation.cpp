#include <gtest/gtest.h>
#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// Flash strings are ordinary strings in this host-only test.
#ifndef F
#define F(text) text
#endif
#include "../../examples/companion_radio/Trail.h"

struct MemoryFile {
  std::vector<uint8_t> bytes;
  size_t pos = 0;
  size_t write(const uint8_t* data, size_t size) {
    bytes.insert(bytes.end(), data, data + size);
    return size;
  }
  int read(uint8_t* data, size_t size) {
    size = std::min(size, bytes.size() - pos);
    memcpy(data, bytes.data() + pos, size);
    pos += size;
    return (int)size;
  }
};

struct TextOutput {
  std::string text;
  size_t print(const char* value) { text += value; return strlen(value); }
  size_t write(const uint8_t* value, size_t size) { text.append((const char*)value, size); return size; }
};
struct NoWaypoints {
  struct Point { int32_t lat_1e6, lon_1e6; uint32_t ts; char label[1]; };
  int count() const { return 0; }
  Point at(int) const { return {}; }
};

class TrailElevation : public ::testing::Test {
protected:
  TrailStore trail;
  int sample = 0;
  void SetUp() override { g_mock_millis = 1000; trail.setActive(true); }
  void feed(int32_t altitude_mm, bool moving = true) {
    ++sample;
    g_mock_millis += 1000;
    trail.addPoint(moving ? sample * 100 : 0, 0, 1700000000 + sample, 5, altitude_mm);
  }
  void plateau(int metres, int count = 20) { while (count--) feed(metres * 1000); }
};

TEST_F(TrailElevation, FlatNoiseAndIsolatedSpikeDoNotBecomeClimbing) {
  for (int i = 0; i < 90; ++i) feed(i == 40 ? 800000 : 100000 + ((i % 3) - 1) * 2000);
  EXPECT_EQ(0u, trail.ascentMeters());
  EXPECT_EQ(0u, trail.descentMeters());
  EXPECT_NEAR(100, trail.currentAltitudeMeters(), 2);
  EXPECT_LE(trail.maxAltitudeMeters(), 102);
}

TEST_F(TrailElevation, ClimbAndDescentSurviveHorizontalSimplification) {
  plateau(100);
  for (int m = 101; m <= 150; ++m) feed(m * 1000);
  plateau(150);
  for (int m = 149; m >= 100; --m) feed(m * 1000);
  plateau(100);
  EXPECT_GE(trail.ascentMeters(), 45u);
  EXPECT_LE(trail.ascentMeters(), 50u);
  EXPECT_GE(trail.descentMeters(), 45u);
  EXPECT_LE(trail.descentMeters(), 50u);
  EXPECT_EQ(100, trail.minAltitudeMeters());
  EXPECT_EQ(150, trail.maxAltitudeMeters());
  int peak = 0;
  for (int i = 0; i < trail.profileCount(); ++i)
    peak = std::max(peak, (int)trail.profileAt(i).altitude_m);
  EXPECT_GE(peak, 147);  // Keep the crest on an otherwise straight route.
}

TEST_F(TrailElevation, StationaryHeightDriftDoesNotAddClimb) {
  for (int i = 0; i < 70; ++i) feed((100 + i) * 1000, false);
  EXPECT_EQ(0u, trail.ascentMeters());
  EXPECT_EQ(0u, trail.descentMeters());
}

TEST_F(TrailElevation, MissingFixAndPauseDoNotCountHeightAcrossTheGap) {
  plateau(100);
  feed(TrailStore::UNKNOWN_ALTITUDE_MM);
  plateau(500);
  EXPECT_EQ(0u, trail.ascentMeters());
  trail.setPaused(true);
  g_mock_millis += 60000;
  trail.setPaused(false);
  plateau(-20);
  EXPECT_EQ(0u, trail.descentMeters());
  EXPECT_EQ(-20, trail.minAltitudeMeters());
}

TEST_F(TrailElevation, SaveLoadPreservesElevationTotalsAndExportsMetres) {
  plateau(100);
  for (int m = 101; m <= 150; ++m) feed(m * 1000);
  plateau(150);
  MemoryFile file;
  ASSERT_TRUE(trail.writeTo(file));
  TrailStore restored;
  ASSERT_TRUE(restored.readFrom(file));
  EXPECT_EQ(trail.ascentMeters(), restored.ascentMeters());
  EXPECT_EQ(trail.descentMeters(), restored.descentMeters());
  EXPECT_EQ(trail.minAltitudeMeters(), restored.minAltitudeMeters());
  EXPECT_EQ(trail.maxAltitudeMeters(), restored.maxAltitudeMeters());
  EXPECT_EQ(trail.elapsedSeconds(), restored.elapsedSeconds());
  EXPECT_EQ(trail.count(), restored.count());
  EXPECT_EQ(150, restored.currentAltitudeMeters());
  NoWaypoints waypoints;
  TextOutput live, saved;
  restored.exportGpx(live, waypoints);
  EXPECT_NE(std::string::npos, live.text.find("<ele>150</ele>"));
  file.pos = 0;
  ASSERT_GT(TrailStore::exportGpxFromFile(file, saved, waypoints), 0u);
  EXPECT_EQ(live.text, saved.text);
}

TEST_F(TrailElevation, LegacyPaddingIsNeverTreatedAsAltitude) {
  MemoryFile file;
  ASSERT_TRUE(persist::writeHeader(file, TrailStore::SAVE_MAGIC, 1, 1));
  uint32_t accumulated = 60000;
  file.write((uint8_t*)&accumulated, sizeof(accumulated));
  TrailPoint legacy{100, 200, 1700000000, TRAIL_FLAG_SEG_START, 12345};
  file.write((uint8_t*)&legacy, sizeof(legacy));
  TrailStore restored;
  ASSERT_TRUE(restored.readFrom(file));
  EXPECT_EQ(1, restored.count());
  EXPECT_EQ(60u, restored.elapsedSeconds());
  EXPECT_EQ(TrailStore::UNKNOWN_ALTITUDE, restored.at(0).altitude_m);
  EXPECT_FALSE(restored.hasElevation());
  NoWaypoints waypoints;
  TextOutput output;
  file.pos = 0;
  ASSERT_GT(TrailStore::exportGpxFromFile(file, output, waypoints), 0u);
  EXPECT_EQ(std::string::npos, output.text.find("<ele>"));
}

TEST_F(TrailElevation, TruncatedSnapshotIsRejected) {
  plateau(100);
  MemoryFile file;
  ASSERT_TRUE(trail.writeTo(file));
  file.bytes.pop_back();
  TrailStore restored;
  EXPECT_FALSE(restored.readFrom(file));
  EXPECT_TRUE(restored.empty());
  EXPECT_FALSE(restored.hasElevation());
}

TEST_F(TrailElevation, SpeedKeepsFractionalPrecision) {
  trail.addPoint(0, 0, 1700000000, 5);
  g_mock_millis += 10000;
  trail.addPoint(100, 0, 1700000010, 5);
  EXPECT_NEAR(3.96f, trail.avgSpeedKmh(), 0.01f);  // 11 retained metres / 10 seconds.
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
