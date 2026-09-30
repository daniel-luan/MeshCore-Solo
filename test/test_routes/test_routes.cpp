#include "../../examples/companion_radio/RouteProtocol.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <vector>
using namespace routes;
struct MemoryFiles : Files {
  std::vector<uint8_t> slots[2];
  int writing = -1;
  bool failWrite = false, failCapacity = false;
  uint32_t total = 2 * 1024 * 1024;
  uint32_t size(uint8_t s) override { return slots[s].size(); }
  bool read(uint8_t s, uint32_t o, uint8_t *d, size_t n) override {
    if ((uint64_t)o + n > slots[s].size()) return false;
    memcpy(d, slots[s].data() + o, n);
    return true;
  }
  bool create(uint8_t s) override {
    slots[s].clear();
    writing = s;
    return !failWrite;
  }
  bool append(const uint8_t *d, size_t n) override {
    if (failWrite || writing < 0) return false;
    slots[writing].insert(slots[writing].end(), d, d + n);
    return true;
  }
  bool finish() override { return !failWrite; }
  void cancel() override { writing = -1; }
  bool erase(uint8_t s) override {
    slots[s].clear();
    return true;
  }
  bool capacity(Capacity &c) override {
    if (failCapacity) return false;
    c.total = total;
    c.used = slots[0].size() + slots[1].size();
    c.reserve = 65536;
    return true;
  }
};
std::vector<uint8_t> fixture(const char *name) {
  const char *dir = getenv("ROUTE_FIXTURE_DIR");
  if (!dir) dir = "test/test_routes/fixtures";
  std::ifstream f(std::string(dir) + "/" + name + ".bin", std::ios::binary);
  return { std::istreambuf_iterator<char>(f), {} };
}
void drain(RouteStore &r) {
  for (int i = 0; i < 20000 && r.working(); i++)
    r.step(1000 + i);
  ASSERT_FALSE(r.working());
}
void upload(RouteStore &r, const std::vector<uint8_t> &data, uint16_t request = 1) {
  ASSERT_FALSE(data.empty());
  ASSERT_EQ(OK, r.startUpload(data.size(), ~crc(0xffffffff, data.data(), data.size()), request, 100));
  for (uint32_t pos = 0; pos < data.size();) {
    size_t n = data.size() - pos;
    if (n > CHUNK_SIZE) n = CHUNK_SIZE;
    ASSERT_EQ(OK, r.data(r.session(), pos, data.data() + pos, n, 1000));
    pos += n;
  }
  ASSERT_EQ(OK, r.commit(r.session(), 2000));
  drain(r);
}
class RoutesTest : public ::testing::Test {
protected:
  MemoryFiles files;
  RouteStore route;
  void SetUp() override {
    route.begin(files);
    drain(route);
  }
};
TEST_F(RoutesTest, CrossLanguageUploadBootAndCheckpoint) {
  upload(route, fixture("straight"));
  ASSERT_EQ(READY, route.state());
  EXPECT_EQ(140u, route.header().points);
  char name[64];
  ASSERT_TRUE(route.name(name, sizeof(name)));
  EXPECT_STREQ("Water 水 route", name);
  Checkpoint cp;
  ASSERT_TRUE(route.checkpoint(0, cp));
  EXPECT_GT(cp.lateral, 1000u);
  RouteStore reboot;
  reboot.begin(files);
  drain(reboot);
  EXPECT_EQ(READY, reboot.state());
  EXPECT_EQ(route.header().bytes, reboot.header().bytes);
}
TEST_F(RoutesTest, SupportsMoreThan32KBWithFixedRam) {
  auto data = fixture("large");
  ASSERT_GT(data.size(), 32768u);
  upload(route, data);
  EXPECT_EQ(READY, route.state());
  EXPECT_EQ(5000u, route.header().points);
  EXPECT_LT(sizeof(RouteStore), 4096u);
}
TEST_F(RoutesTest, DuplicateChunkDoesNotAppendAndChecksOffsets) {
  auto d = fixture("straight");
  ASSERT_EQ(OK, route.startUpload(d.size(), ~crc(0xffffffff, d.data(), d.size()), 7, 100));
  auto token = route.session();
  ASSERT_EQ(OK, route.data(token, 0, d.data(), 144, 200));
  ASSERT_EQ(OK, route.data(token, 0, d.data(), 144, 300));
  EXPECT_EQ(144u, route.received());
  EXPECT_EQ(144u, files.slots[0].size());
  EXPECT_EQ(BAD_OFFSET, route.data(token, 145, d.data() + 144, 144, 400));
  EXPECT_EQ(BAD_SESSION, route.data(token + 1, 144, d.data() + 144, 144, 400));
}
TEST_F(RoutesTest, InterruptedReplacementKeepsActiveRoute) {
  upload(route, fixture("straight"));
  auto replacement = fixture("segments");
  ASSERT_EQ(OK, route.startUpload(replacement.size(), 0, 9, 100));
  ASSERT_EQ(OK, route.data(route.session(), 0, replacement.data(), 144, 200));
  RouteStore reboot;
  reboot.begin(files);
  drain(reboot);
  EXPECT_EQ(140u, reboot.header().points);
}
TEST_F(RoutesTest, CorruptNewGenerationFallsBack) {
  upload(route, fixture("straight"));
  upload(route, fixture("segments"), 2);
  ASSERT_EQ(6u, route.header().points);
  files.slots[1][110] ^= 1;
  RouteStore reboot;
  reboot.begin(files);
  drain(reboot);
  EXPECT_EQ(140u, reboot.header().points);
}
TEST_F(RoutesTest, BadIndexAndShortWriteNeverActivate) {
  upload(route, fixture("straight"));
  auto d = fixture("segments");
  uint32_t index = u32(d.data() + 44);
  d[index + 17] = 17;
  upload(route, d, 3);
  EXPECT_EQ(FAILED, route.state());
  EXPECT_EQ(140u, route.header().points);
  drain(route);
  files.failWrite = true;
  EXPECT_EQ(IO_ERROR, route.startUpload(d.size(), 0, 4, 200));
  EXPECT_EQ(140u, route.header().points);
}
TEST_F(RoutesTest, CapacityFailsClosedAndPreservesReserve) {
  Capacity c;
  ASSERT_TRUE(route.capacity(c));
  EXPECT_EQ(files.total - 65536 - FOOTER_SIZE, c.maximum);
  files.failCapacity = true;
  EXPECT_EQ(IO_ERROR, route.startUpload(1000, 0, 1, 100));
  files.failCapacity = false;
  EXPECT_EQ(NO_SPACE, route.startUpload(files.total, 0, 1, 100));
}
TEST_F(RoutesTest, AbortTimeoutAndBleRejection) {
  auto d = fixture("straight");
  ASSERT_EQ(OK, route.startUpload(d.size(), 0, 1, 100));
  route.step(60101);
  EXPECT_EQ(EMPTY, route.state());
  uint8_t in[11] = { COMMAND, 'S', 'R', PROTOCOL, CAPABILITIES }, out[176];
  EXPECT_EQ(17u, handle(route, true, in, sizeof(in), out, 0));
  EXPECT_EQ(USB_ONLY, out[11]);
}
void navigate(RouteStore &r, int32_t lat, int32_t lon, uint32_t now, bool good = true) {
  r.fix(good, good, lat, lon, now);
  for (int i = 0; i < 400; i++)
    r.navigationStep(now);
}
TEST_F(RoutesTest, ForwardReverseRemainingDistanceAndNoStaleAlerts) {
  upload(route, fixture("straight"));
  ASSERT_TRUE(route.start(false));
  for (int i = 0; i < 20; i++)
    route.navigationStep(1000);
  navigate(route, 3000, 0, 2000);
  ASSERT_TRUE(route.matched());
  auto forward = route.remainingCm();
  EXPECT_GT(forward, 100000u);
  route.stop();
  ASSERT_TRUE(route.start(true));
  for (int i = 0; i < 20; i++)
    route.navigationStep(1000);
  navigate(route, 3000, 0, 2000);
  ASSERT_TRUE(route.matched());
  EXPECT_LT(route.remainingCm(), forward);
  navigate(route, 3000, 2000, 4000);
  navigate(route, 3000, 2000, 20000);
  EXPECT_TRUE(route.takeAlert());
  navigate(route, 3000, 2000, 22000, false);
  EXPECT_FALSE(route.takeAlert());
  navigate(route, 3000, 2000, 50000, false);
  EXPECT_FALSE(route.takeAlert());
}
TEST_F(RoutesTest, FollowingPreventsReplacementAndSegmentsDoNotBridge) {
  upload(route, fixture("segments"));
  route.start(false);
  EXPECT_EQ(BUSY, route.startUpload(1000, 0, 1, 100));
  for (int i = 0; i < 20; i++)
    route.navigationStep(1000);
  navigate(route, 2000, 0, 2000);
  EXPECT_TRUE(route.arrived());
  ASSERT_TRUE(route.nextSegment());
  for (int i = 0; i < 20; i++)
    route.navigationStep(3000);
  navigate(route, 20000, 0, 4000);
  EXPECT_TRUE(route.matched());
  EXPECT_EQ(1u, route.segment());
}
TEST_F(RoutesTest, AmbiguousCrossingRequiresRejoin) {
  upload(route, fixture("crossing"));
  route.start(false);
  for (int i = 0; i < 20; i++)
    route.navigationStep(0);
  navigate(route, 0, 0, 1000);
  EXPECT_TRUE(route.ambiguous());
  EXPECT_FALSE(route.takeAlert());
}
TEST(RouteGeometry, AntimeridianProjection) {
  Point a{ 1000000, 179999000, 0, 0, 0 }, b{ 1000000, -179999000, 0, 0, 0 };
  auto q = project(1000000, 180000000, a, b);
  EXPECT_LT(q.lateral, 1);
  EXPECT_NEAR(.5, q.fraction, .01);
}
TEST_F(RoutesTest, PowerLossAtEveryVerificationAndActivationStep) {
  upload(route, fixture("straight"));
  const auto old = files;
  const auto d = fixture("segments");
  ASSERT_EQ(OK, route.startUpload(d.size(), ~crc(0xffffffff, d.data(), d.size()), 2, 100));
  for (uint32_t pos = 0; pos < d.size();) {
    size_t n = std::min<size_t>(CHUNK_SIZE, d.size() - pos);
    ASSERT_EQ(OK, route.data(route.session(), pos, d.data() + pos, n, 1000));
    pos += n;
  }
  ASSERT_EQ(OK, route.commit(route.session(), 2000));
  for (int tick = 0; tick < 1000; ++tick) {
    MemoryFiles crash = files;
    RouteStore reboot;
    reboot.begin(crash);
    drain(reboot);
    ASSERT_TRUE(reboot.hasRoute());
    ASSERT_TRUE(reboot.header().points == 140 || reboot.header().points == 6);
    if (!route.working()) break;
    route.step(2000 + tick);
  }
  ASSERT_EQ(READY, route.state());
  for (size_t size = 0; size < FOOTER_SIZE; ++size) {
    MemoryFiles crash = files;
    crash.slots[1].resize(d.size() + size);
    RouteStore reboot;
    reboot.begin(crash);
    drain(reboot);
    EXPECT_EQ(140u, reboot.header().points);
  }
}
TEST_F(RoutesTest, DistantEndpointDoesNotCountAsArrival) {
  upload(route, fixture("straight"));
  route.start(false);
  for (int i = 0; i < 20; ++i)
    route.navigationStep(0);
  navigate(route, 20000, 0, 1000);
  EXPECT_FALSE(route.arrived());
  navigate(route, 20000, 0, 17000);
  EXPECT_TRUE(route.takeAlert());
}
TEST_F(RoutesTest, StaleFixResetsPendingExcursionAndPoorFixSuspendsProgress) {
  upload(route, fixture("straight"));
  route.start(false);
  for (int i = 0; i < 20; ++i)
    route.navigationStep(0);
  navigate(route, 3000, 0, 1000);
  navigate(route, 3000, 2000, 2000);
  navigate(route, 3000, 2000, 10000, false);
  navigate(route, 3000, 2000, 12000);
  EXPECT_FALSE(route.takeAlert());
  navigate(route, 3000, 2000, 26000);
  EXPECT_FALSE(route.takeAlert());
  const auto progress = route.progressCm();
  route.fix(true, false, 5000, 0, 27000);
  route.navigationStep(27000);
  EXPECT_EQ(progress, route.progressCm());
  EXPECT_TRUE(route.poorFix());
  EXPECT_FALSE(route.goodFix());
  navigate(route, 3000, 2000, 44000);
  EXPECT_FALSE(route.takeAlert());
}
TEST_F(RoutesTest, ChecksumMismatchAndCapacityFailureDuringUploadRetainActive) {
  upload(route, fixture("straight"));
  auto d = fixture("segments");
  ASSERT_EQ(OK, route.startUpload(d.size(), 0, 2, 100));
  files.failCapacity = true;
  EXPECT_EQ(IO_ERROR, route.data(route.session(), 0, d.data(), 144, 200));
  EXPECT_EQ(140u, route.header().points);
  files.failCapacity = false;
  drain(route);
  ASSERT_EQ(OK, route.startUpload(d.size(), 0, 3, 100));
  for (uint32_t pos = 0; pos < d.size();) {
    auto n = std::min<size_t>(144, d.size() - pos);
    ASSERT_EQ(OK, route.data(route.session(), pos, d.data() + pos, n, 1000));
    pos += n;
  }
  ASSERT_EQ(OK, route.commit(route.session(), 2000));
  drain(route);
  EXPECT_EQ(FAILED, route.state());
  EXPECT_EQ(140u, route.header().points);
}
TEST_F(RoutesTest, ReverseSegmentGapAndDelete) {
  upload(route, fixture("segments"));
  route.start(true);
  for (int i = 0; i < 20; ++i)
    route.navigationStep(0);
  navigate(route, 20000, 0, 1000);
  ASSERT_TRUE(route.arrived());
  ASSERT_TRUE(route.nextSegment());
  for (int i = 0; i < 20; ++i)
    route.navigationStep(2000);
  navigate(route, 2000, 0, 3000);
  EXPECT_EQ(0u, route.segment());
  EXPECT_TRUE(route.matched());
  EXPECT_EQ(BUSY, route.erase());
  route.stop();
  EXPECT_EQ(OK, route.erase());
  RouteStore reboot;
  reboot.begin(files);
  drain(reboot);
  EXPECT_FALSE(reboot.hasRoute());
}
TEST_F(RoutesTest, CrossingContinuityAndExplicitRejoin) {
  upload(route, fixture("crossing"));
  route.start(false);
  for (int i = 0; i < 20; i++)
    route.navigationStep(0);
  navigate(route, -800, -800, 1000);
  ASSERT_TRUE(route.matched());
  navigate(route, -50, -50, 2000);
  const auto before = route.progressCm();
  navigate(route, 0, 0, 3000);
  ASSERT_TRUE(route.matched());
  EXPECT_FALSE(route.ambiguous());
  EXPECT_LT(route.progressCm() - before, 3000u);
  route.rejoin();
  for (int i = 0; i < 400; i++)
    route.navigationStep(3000);
  EXPECT_TRUE(route.ambiguous());
  navigate(route, 800, 800, 5000);
  route.rejoin();
  for (int i = 0; i < 400; i++)
    route.navigationStep(5000);
  EXPECT_TRUE(route.matched());
}
TEST_F(RoutesTest, SwitchbackDoesNotJumpToParallelReturnLeg) {
  upload(route, fixture("switchbacks"));
  route.start(false);
  for (int i = 0; i < 20; i++)
    route.navigationStep(0);
  navigate(route, 400, 0, 1000);
  ASSERT_TRUE(route.matched());
  navigate(route, 500, 60, 2000);
  EXPECT_TRUE(route.matched());
  EXPECT_LT(route.progressCm(), 15000u);
  navigate(route, 900, 0, 3000);
  navigate(route, 1000, 100, 4000);
  navigate(route, 800, 100, 5000);
  EXPECT_TRUE(route.matched());
  EXPECT_GT(route.progressCm(), 12000u);
  EXPECT_FALSE(route.arrived());
}
TEST_F(RoutesTest, InconsistentPreviewHeaderAndCheckpointNeverActivate) {
  upload(route, fixture("straight"));
  auto d = fixture("segments");
  const auto preview = u32(d.data() + 40);
  put32(d.data() + preview + POINT_SIZE + 8, u32(d.data() + preview + 2 * POINT_SIZE + 8) + 1);
  upload(route, d, 2);
  EXPECT_EQ(FAILED, route.state());
  EXPECT_EQ(140u, route.header().points);
  d = fixture("segments");
  put32(d.data() + 72, u32(d.data() + 72) + 1);
  put32(d.data() + 88, ~crc(0xffffffff, d.data(), 88));
  upload(route, d, 3);
  EXPECT_EQ(FAILED, route.state());
  EXPECT_EQ(140u, route.header().points);
  d = fixture("segments");
  const auto cp = u32(d.data() + 32);
  put32(d.data() + cp + 16, 2);
  upload(route, d, 4);
  EXPECT_EQ(FAILED, route.state());
  EXPECT_EQ(140u, route.header().points);
}
TEST_F(RoutesTest, CancellationAfterFooterBeforeActivationDoesNotResurrectUpload) {
  upload(route, fixture("straight"));
  auto d = fixture("segments");
  ASSERT_EQ(OK, route.startUpload(d.size(), ~crc(0xffffffff, d.data(), d.size()), 2, 100));
  for (uint32_t pos = 0; pos < d.size();) {
    auto n = std::min<size_t>(144, d.size() - pos);
    ASSERT_EQ(OK, route.data(route.session(), pos, d.data() + pos, n, 1000));
    pos += n;
  }
  ASSERT_EQ(OK, route.commit(route.session(), 2000));
  for (int tick = 0; tick < 1000 && files.slots[1].size() == d.size(); ++tick)
    route.step(2000 + tick);
  ASSERT_EQ(d.size() + FOOTER_SIZE, files.slots[1].size());
  ASSERT_EQ(140u, route.header().points);
  ASSERT_EQ(OK, route.abort(route.session()));
  RouteStore reboot;
  reboot.begin(files);
  drain(reboot);
  EXPECT_EQ(140u, reboot.header().points);
}
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
