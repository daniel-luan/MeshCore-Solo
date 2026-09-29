#include "SSD1306Display.h"
#include <helpers/DeviceTiming.h>
#ifdef OLED_MISC_FIXED_FONT
  #include "MiscFixedRenderer.h"
#endif

bool SSD1306Display::i2c_probe(TwoWire& wire, uint8_t addr) {
  wire.beginTransmission(addr);
  uint8_t error = wire.endTransmission();
  return (error == 0);
}

bool SSD1306Display::begin() {
  if (!_isOn) {
    if (_peripher_power) _peripher_power->claim();
    _isOn = true;
  }
  #ifdef DISPLAY_ROTATION
  display.setRotation(DISPLAY_ROTATION);
  #endif
  return display.begin(SSD1306_SWITCHCAPVCC, DISPLAY_ADDRESS, true, false) && i2c_probe(Wire, DISPLAY_ADDRESS);
}

void SSD1306Display::turnOn() {
#ifdef SSD1306_SKIP_UNCHANGED_FRAMES
  _force_redraw = true;
#endif
  if (!_isOn) {
    if (_peripher_power) _peripher_power->claim();
    _isOn = true;  // set before begin() to prevent double claim
    if (_peripher_power) begin();  // re-init display after power was cut
  }
  display.ssd1306_command(SSD1306_DISPLAYON);
}

void SSD1306Display::turnOff() {
  display.ssd1306_command(SSD1306_DISPLAYOFF);
  if (_isOn) {
    if (_peripher_power) {
#if PIN_OLED_RESET >= 0
      digitalWrite(PIN_OLED_RESET, LOW);
#endif
      _peripher_power->release();
    }
    _isOn = false;
  }
}

void SSD1306Display::clear() {
  display.clearDisplay();
  display.display();
#ifdef SSD1306_SKIP_UNCHANGED_FRAMES
  _force_redraw = true;
#endif
}

void SSD1306Display::startFrame(Color bkg) {
  display.clearDisplay();  // TODO: apply 'bkg'
  _color = SSD1306_WHITE;
  display.setTextColor(_color);
  display.setTextSize(1);
  display.cp437(true);         // Use full 256 char 'Code Page 437' font
}

void SSD1306Display::setTextSize(int sz) {
  _text_sz = sz;
  _vw_dirty = true;
  display.setTextSize(sz);
}

void SSD1306Display::setColor(Color c) {
  _color = (c != 0) ? SSD1306_WHITE : SSD1306_BLACK;
  display.setTextColor(_color);
}

void SSD1306Display::setCursor(int x, int y) {
  display.setCursor(x, y);
}

#ifdef OLED_MISC_FIXED_FONT
uint8_t SSD1306Display::glyphXAdvance(uint32_t cp) {
  return miscFixedXAdvance(cp, _text_sz);
}
#endif

void SSD1306Display::print(const char* str) {
#ifdef OLED_MISC_FIXED_FONT
  miscFixedPrint(display, str, _text_sz, _color);
#else
  display.print(str);
#endif
}

void SSD1306Display::fillRect(int x, int y, int w, int h) {
  display.fillRect(x, y, w, h, _color);
}

void SSD1306Display::drawRect(int x, int y, int w, int h) {
  display.drawRect(x, y, w, h, _color);
}

void SSD1306Display::drawXbm(int x, int y, const uint8_t* bits, int w, int h) {
  display.drawBitmap(x, y, bits, w, h, SSD1306_WHITE);
}

uint16_t SSD1306Display::getTextWidth(const char* str) {
#ifdef OLED_MISC_FIXED_FONT
  return miscFixedTextWidth(str, _text_sz);
#else
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return w;
#endif
}

void SSD1306Display::endFrame() {
#ifdef SSD1306_SKIP_UNCHANGED_FRAMES
  const uint8_t* buf = display.getBuffer();
  uint16_t n = (uint16_t)((width() * height()) / 8);
  uint32_t h = 2166136261u;
  for (uint16_t i = 0; i < n; i++) { h ^= buf[i]; h *= 16777619u; }
  if (!_force_redraw && h == _last_frame_hash) return;
  _force_redraw = false;
  _last_frame_hash = h;
#endif
#ifdef FIRMWARE_SOLO_BUILD
  ScopedDeviceTiming timing(DeviceTiming::OLED_FLUSH);
#endif
  display.display();
}
