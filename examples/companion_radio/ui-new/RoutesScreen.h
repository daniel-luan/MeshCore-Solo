#pragma once
#include "../RouteStore.h"
#include "GfxUtils.h"
#include "PopupMenu.h"

class RoutesScreen : public UIScreen {
  UITask *_task;
  uint8_t _view = 0, _confirm = 0;
  uint32_t _checkpoint = 0;
  int _scroll = 0;
  PopupMenu _menu;
  char _distanceLabel[32], _delayLabel[32];
  enum Action { VIEW, FORWARD, REVERSE, STOP, REJOIN, NEXT, OFF_DISTANCE, OFF_DELAY, DELETE };
  routes::RouteStore &store() { return the_mesh.routeStore(); }
  void menus() {
    _confirm = 0;
    _menu.begin("Route actions");
    _menu.addItem("View route (no GPS)");
    _menu.addItem("Start forward");
    _menu.addItem("Start reverse");
    _menu.addItem("Stop navigation");
    _menu.addItem("Rejoin route");
    _menu.addItem("Next segment");
    snprintf(_distanceLabel, sizeof(_distanceLabel), "Off-route: %lum", (unsigned long)store().offDistance());
    snprintf(_delayLabel, sizeof(_delayLabel), "Alert delay: %lus", (unsigned long)store().offDelay());
    _menu.addValueItem(_distanceLabel);
    _menu.addValueItem(_delayLabel);
    _menu.addItem("Delete route");
  }
  void start(bool reverse) {
    if (!_task->hasGPS()) {
      _task->showAlert("GPS required", 1500);
      return;
    }
    if (!_task->getGPSState()) {
      _confirm = reverse ? 3 : 2;
      _menu.beginConfirm("GPS is off", "Enable GPS & start");
      return;
    }
    _task->showAlert(store().start(reverse) ? "Navigation started" : "Route not ready", 1500);
    _view = 1;
  }
  void row(DisplayDriver &d, int i, const char *value) {
    d.setCursor(2, d.listStart() + i * d.lineStep());
    d.print(value);
  }
  void dist(char *out, size_t n, float metres) { geo::fmtDist(out, n, metres / 1000, _task->useImperial()); }
  void overview(DisplayDriver &d) {
    const int visible = d.listVisible();
    char value[64], name[64]{};
    if (!store().name(name, sizeof(name))) snprintf(name, sizeof(name), "Route name unavailable");
    for (int i = 0; i < visible && i + _scroll < 6; i++) {
      switch (i + _scroll) {
      case 0:
        snprintf(value, sizeof(value), "%s", name);
        break;
      case 1: {
        char length[16];
        dist(length, sizeof(length), store().header().distance / 100.0f);
        snprintf(value, sizeof(value), "Distance: %s", length);
        break;
      }
      case 2:
        snprintf(value, sizeof(value), "Points: %lu", (unsigned long)store().header().points);
        break;
      case 3:
        snprintf(value, sizeof(value), "Segments: %lu", (unsigned long)store().header().segments);
        break;
      case 4:
        snprintf(value, sizeof(value), "Checkpoints: %lu", (unsigned long)store().header().checkpoints);
        break;
      default:
        snprintf(value, sizeof(value), "%s",
                 store().following() ? "Navigation active" : "Enter: view route");
        break;
      }
      d.drawTextEllipsized(2, d.listStart() + i * d.lineStep(), d.width() - 4, value);
    }
  }
  void navigate(DisplayDriver &d) {
    auto &r = store();
    if (!r.following()) {
      row(d, 0, "Navigation stopped");
      row(d, 1, "Hold Enter to start");
      return;
    }
    const char *state = !r.goodFix()    ? (r.poorFix() ? "Poor fix" : "No GPS fix")
                        : r.ambiguous() ? "Rejoin needed"
                        : r.locating()  ? "Locating route"
                        : r.arrived()   ? "Segment end"
                                        : "Following route";
    row(d, 0, state);
    if (!r.matched()) return;
    char value[40], length[16];
    dist(length, sizeof(length), r.remainingCm() / 100.0f);
    snprintf(value, sizeof(value), "Remaining: %s", length);
    row(d, 1, value);
    dist(length, sizeof(length), r.lateralMetres());
    snprintf(value, sizeof(value), "Off route: %s", length);
    row(d, 2, value);
    int32_t lat, lon, tl, tn;
    r.target(tl, tn);
    if (_task->currentLocation(lat, lon) && r.goodFix()) {
      int bearing = geo::bearingDeg(lat, lon, tl, tn), heading;
      snprintf(value, sizeof(value), "Target: %03d %s", bearing, geo::bearingCardinal(bearing));
      row(d, 3, value);
      if (d.listVisible() > 4) {
        if (_task->currentCourse(heading))
          snprintf(value, sizeof(value), "Last course: %03d", heading);
        else
          snprintf(value, sizeof(value), "Course: move to update");
        row(d, 4, value);
      }
    }
  }
  void map(DisplayDriver &d) {
    auto &r = store();
    routes::Point first;
    if (!r.preview(0, first)) return;
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    float coslat = cosf(first.lat * 1e-6f * (float)M_PI / 180);
    auto xy = [&](int32_t lat, int32_t lon, float &x, float &y) {
      x = routes::wrapRadians((lon - (double)first.lon) * M_PI / 180 / 1e6) * coslat;
      y = (lat - (double)first.lat) * M_PI / 180 / 1e6;
    };
    for (uint32_t i = 0; i < r.header().previews; i++) {
      routes::Point p;
      if (!r.preview(i, p)) return;
      float x, y;
      xy(p.lat, p.lon, x, y);
      if (x < minX) minX = x;
      if (x > maxX) maxX = x;
      if (y < minY) minY = y;
      if (y > maxY) maxY = y;
    }
    const int top = d.listStart(), bottom = d.height() - d.lineStep() - 2;
    float sx = (d.width() - 8) / fmaxf(maxX - minX, 1e-6f),
          sy = (bottom - top - 4) / fmaxf(maxY - minY, 1e-6f), scale = fminf(sx, sy);
    auto project = [&](int32_t lat, int32_t lon, int &x, int &y) {
      float a, b;
      xy(lat, lon, a, b);
      x = 4 + (d.width() - 8 - (maxX - minX) * scale) / 2 + (a - minX) * scale;
      y = bottom - 2 - (bottom - top - 4 - (maxY - minY) * scale) / 2 - (b - minY) * scale;
      if (x < 2) x = 2;
      if (x > d.width() - 3) x = d.width() - 3;
      if (y < top) y = top;
      if (y > bottom) y = bottom;
    };
    int px = 0, py = 0;
    for (uint32_t i = 0; i < r.header().previews; i++) {
      routes::Point p;
      r.preview(i, p);
      int x, y;
      project(p.lat, p.lon, x, y);
      if (i && !(p.flags & routes::SEG_START)) gfx::drawLine(d, px, py, x, y);
      px = x;
      py = y;
    }
    TrailStore &trail = _task->trail();
    for (int i = 0; i < trail.count(); i++) {
      const TrailPoint &p = trail.at(i);
      int x, y;
      project(p.lat_1e6, p.lon_1e6, x, y);
      if (i && !(p.flags & TRAIL_FLAG_SEG_START)) {
        int dx = x - px, dy = y - py, steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
        for (int k = 0; k <= steps; k += 3)
          d.fillRect(px + (steps ? dx * k / steps : 0), py + (steps ? dy * k / steps : 0), 1, 1);
      }
      px = x;
      py = y;
    }
    int32_t lat, lon;
    if (r.following() && r.goodFix() && _task->getGPSState() && _task->currentLocation(lat, lon)) {
      int x, y;
      project(lat, lon, x, y);
      d.drawRect(x - 2, y - 2, 5, 5);
    }
    d.setCursor(2, bottom + 2);
    d.print("Route line / Trail dots");
  }
  void elevation(DisplayDriver &d) {
    auto &r = store();
    int low = INT16_MAX, high = INT16_MIN, known = 0;
    for (uint32_t i = 0; i < r.header().previews; i++) {
      routes::Point p;
      if (!r.preview(i, p)) return;
      if (p.altitude != routes::NO_ALT) {
        if (p.altitude < low) low = p.altitude;
        if (p.altitude > high) high = p.altitude;
        known++;
      }
    }
    if (known < 2 || !r.header().distance) {
      row(d, 0, "No route elevation");
      return;
    }
    if (high - low < 10) {
      low -= 5;
      high += 5;
    }
    char value[40];
    float units = _task->useImperial() ? 3.28084f : 1;
    snprintf(value, sizeof(value), "%ld .. %ld %s", (long)lroundf(low * units), (long)lroundf(high * units),
             units > 1 ? "ft" : "m");
    row(d, 0, value);
    int top = d.listStart() + d.lineStep(), bottom = d.height() - d.lineStep() - 3, px = 0, py = 0;
    bool connected = false;
    gfx::drawLine(d, 3, bottom, d.width() - 4, bottom);
    for (uint32_t i = 0; i < r.header().previews; i++) {
      routes::Point p;
      r.preview(i, p);
      if (p.altitude == routes::NO_ALT) {
        connected = false;
        continue;
      }
      int x = 3 + (uint64_t)p.distance * (d.width() - 7) / r.header().distance,
          y = bottom - (p.altitude - low) * (bottom - top) / (high - low);
      if (connected && !(p.flags & (routes::SEG_START | routes::PROFILE_GAP))) gfx::drawLine(d, px, py, x, y);
      px = x;
      py = y;
      connected = true;
    }
    if (r.following() && r.matched()) {
      int x = 3 + (uint64_t)r.progressCm() * (d.width() - 7) / r.header().distance;
      gfx::drawLine(d, x, top, x, bottom);
    }
    char length[16];
    dist(length, sizeof(length), r.header().distance / 100.0f);
    d.setCursor(3, bottom + 2);
    d.print("0");
    d.drawTextRightAlign(d.width() - 3, bottom + 2, length);
  }
  void checkpoints(DisplayDriver &d) {
    auto &r = store();
    if (!r.header().checkpoints) {
      row(d, 0, "No checkpoints");
      return;
    }
    if (_checkpoint >= r.header().checkpoints) _checkpoint = 0;
    routes::Checkpoint cp;
    char name[64], value[48], length[16];
    if (!r.checkpoint(_checkpoint, cp) || !r.label(cp.labelOffset, cp.labelBytes, name, sizeof(name))) {
      row(d, 0, "Storage read failed");
      return;
    }
    snprintf(value, sizeof(value), "%lu/%lu", (unsigned long)_checkpoint + 1,
             (unsigned long)r.header().checkpoints);
    row(d, 0, value);
    d.drawTextEllipsized(2, d.listStart() + d.lineStep(), d.width() - 4, name);
    if (r.following() && r.matched()) {
      int64_t ahead =
          r.reverse() ? (int64_t)r.progressCm() - cp.distance : (int64_t)cp.distance - r.progressCm();
      dist(length, sizeof(length), llabs(ahead) / 100.0f);
      snprintf(value, sizeof(value), "%s: %s", ahead >= 0 ? "Along route" : "Passed", length);
    } else {
      dist(length, sizeof(length), cp.distance / 100.0f);
      snprintf(value, sizeof(value), "From start: %s", length);
    }
    row(d, 2, value);
    dist(length, sizeof(length), cp.lateral / 100.0f);
    snprintf(value, sizeof(value), "Offset: %s", length);
    row(d, 3, value);
  }

public:
  explicit RoutesScreen(UITask *task) : _task(task) {}
  void onShow() override {
    _view = 0;
    _scroll = 0;
    _checkpoint = 0;
    _menu.active = false;
  }
  int render(DisplayDriver &d) override {
    d.setTextSize(1);
    d.setColor(DisplayDriver::LIGHT);
    const char *names[] = { "ROUTE", "NAVIGATE", "ROUTE VIEW", "ELEVATION", "CHECKPOINTS" };
    char title[32];
    snprintf(title, sizeof(title), "%s %u/5", names[_view], _view + 1);
    d.drawCenteredHeader(title);
    auto &r = store();
    if (r.state() == routes::CHECKING)
      row(d, 0, "Checking route");
    else if (r.working())
      row(d, 0, r.state() == routes::RECEIVING ? "Importing route" : "Verifying route");
    else if (r.state() == routes::FAILED && !r.hasRoute()) {
      row(d, 0, "Route import failed");
      row(d, 1, r.errorMessage());
      row(d, 2, "Check browser log");
    } else if (!r.hasRoute()) {
      row(d, 0, "No saved route");
      row(d, 1, "Import over USB");
    } else
      switch (_view) {
      case 0:
        overview(d);
        break;
      case 1:
        navigate(d);
        break;
      case 2:
        map(d);
        break;
      case 3:
        elevation(d);
        break;
      case 4:
        checkpoints(d);
        break;
      }
    if (_menu.active) _menu.render(d);
    return 1000;
  }
  bool handleInput(char key) override {
    auto &r = store();
    if (_menu.active) {
      if (!_confirm && (keyIsPrev(key) || keyIsNext(key)) &&
          (_menu.selectedIndex() == OFF_DISTANCE || _menu.selectedIndex() == OFF_DELAY)) {
        int dir = keyIsNext(key) ? 1 : -1;
        uint32_t m = r.offDistance(), s = r.offDelay();
        if (_menu.selectedIndex() == OFF_DISTANCE) {
          int value = (int)m + dir * 25;
          if (value < 25) value = 200;
          if (value > 200) value = 25;
          m = value;
        } else {
          int value = (int)s + dir * 5;
          if (value < 5) value = 60;
          if (value > 60) value = 5;
          s = value;
        }
        if (!the_mesh.saveRouteSettings(m, s)) _task->showAlert("Route settings failed", 2000);
        int selected = _menu.selectedIndex();
        menus();
        _menu.setSelected(selected);
        return true;
      }
      auto result = _menu.handleInput(key);
      if (result == PopupMenu::CANCELLED) {
        _confirm = 0;
        return true;
      }
      if (result == PopupMenu::VALUE_NEXT) return handleInput(KEY_RIGHT);
      if (result != PopupMenu::SELECTED) return true;
      int selected = _menu.selectedIndex();
      uint8_t confirm = _confirm;
      _confirm = 0;
      if (confirm) {
        if (selected != 0) return true;
        if (confirm == 1)
          _task->showAlert(r.erase() == routes::OK ? "Route deleted" : "Delete failed", 1500);
        else {
          _task->applyGpsState(true);
          start(confirm == 3);
        }
        return true;
      }
      switch (selected) {
      case VIEW:
        if (!r.hasRoute() || r.working()) {
          _task->showAlert("Route not ready", 1500);
          break;
        }
        _view = 2;
        _scroll = 0;
        break;
      case FORWARD:
        start(false);
        break;
      case REVERSE:
        start(true);
        break;
      case STOP:
        r.stop();
        _task->showAlert("Navigation stopped", 1000);
        break;
      case REJOIN:
        r.rejoin();
        break;
      case NEXT:
        if (!r.nextSegment()) _task->showAlert("No next segment", 1200);
        break;
      case DELETE:
        _confirm = 1;
        _menu.beginConfirm("Delete saved route?", "Delete route");
        break;
      }
      return true;
    }
    if (key == KEY_CANCEL) {
      _task->gotoToolsScreen();
      return true;
    }
    if (key == KEY_CONTEXT_MENU) {
      menus();
      return true;
    }
    if (keyIsPrev(key)) {
      _view = (_view + 4) % 5;
      _scroll = 0;
      return true;
    }
    if (keyIsNext(key)) {
      _view = (_view + 1) % 5;
      _scroll = 0;
      return true;
    }
    if (key == KEY_UP || key == KEY_DOWN) {
      int dir = key == KEY_UP ? -1 : 1;
      if (_view == 0) {
        _scroll += dir;
        if (_scroll < 0) _scroll = 0;
        if (_scroll > 5) _scroll = 5;
      }
      if (_view == 4 && r.header().checkpoints) {
        _checkpoint = dir > 0 ? (_checkpoint + 1) % r.header().checkpoints
                              : (_checkpoint ? _checkpoint - 1 : r.header().checkpoints - 1);
      }
      return true;
    }
    if (key == KEY_ENTER) {
      if (_view == 0 && r.hasRoute() && !r.working()) {
        _view = 2;
        _scroll = 0;
        return true;
      }
      _task->showAlert("Hold Enter for menu", 1000);
      return true;
    }
    return false;
  }
};
