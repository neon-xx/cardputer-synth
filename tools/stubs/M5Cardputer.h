// Minimal API stubs for a host-side syntax check of the sketch. Not functional.
#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>
using std::min; using std::max;
struct String {
  std::string s;
  String() {}
  String(const char* c) : s(c ? c : "") {}
  String(const std::string& x) : s(x) {}
  String(int v) : s(std::to_string(v)) {}
  const char* c_str() const { return s.c_str(); }
  String substring(int a) const { return String(s.substr(a)); }
  int lastIndexOf(char c) const { auto p = s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  void replace(const char* a, const char* b) { size_t p; while ((p = s.find(a)) != std::string::npos) s.replace(p, strlen(a), b); }
  void toLowerCase() { for (auto& c : s) c = tolower(c); }
  bool endsWith(const char* e) const { size_t n = strlen(e); return s.size() >= n && s.compare(s.size() - n, n, e) == 0; }
  bool startsWith(const char* e) const { return s.rfind(e, 0) == 0; }
  bool operator<(const String& o) const { return s < o.s; }
  friend String operator+(const String& a, const String& b) { return String(a.s + b.s); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b.s); }
  friend String operator+(const String& a, int b) { return String(a.s + std::to_string(b)); }
  friend String operator+(const String& a, const char* b) { return String(a.s + b); }
};
uint32_t millis(); uint32_t micros(); void delay(uint32_t);
// FreeRTOS
typedef void* QueueHandle_t; typedef int BaseType_t;
#define pdTRUE 1
struct portMUX_TYPE { int x; };
#define portMUX_INITIALIZER_UNLOCKED {0}
void portENTER_CRITICAL(portMUX_TYPE*); void portEXIT_CRITICAL(portMUX_TYPE*);
QueueHandle_t xQueueCreate(int, int); BaseType_t xQueueSend(QueueHandle_t, const void*, int);
BaseType_t xQueueReceive(QueueHandle_t, void*, int); void vTaskDelay(int);
BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void*, int);
#define SERIAL_8N1 0
struct HardwareSerial { void setRxBufferSize(int); void begin(int, int, int, int); int available(); int read(); };
extern HardwareSerial Serial1;
// M5
namespace fonts { struct F {}; extern F Font0, Font2; }
enum { TFT_BLACK = 0, TFT_WHITE = 0xFFFF, TFT_GREEN = 0x07E0 };
struct LGFX_Device { void setRotation(int); void fillScreen(int); };
struct M5Canvas {
  M5Canvas(LGFX_Device*) {}
  void setColorDepth(int); void createSprite(int, int); void fillScreen(int); void fillRect(int,int,int,int,int);
  void setFont(const fonts::F*); void setTextSize(float); void setTextColor(int); void setTextColor(int,int);
  void drawString(const char*,int,int); void drawString(const String&,int,int);
  void drawRightString(const char*,int,int); void drawRightString(const String&,int,int);
  void drawCentreString(const String&,int,int); void fillCircle(int,int,int,int); void fillRoundRect(int,int,int,int,int,int);
  void drawRect(int,int,int,int,int); void pushSprite(int,int);
};
struct speaker_config_t { int sample_rate, dma_buf_len, dma_buf_count, task_priority, task_pinned_core; };
struct Speaker_Class {
  void end(); bool begin(); speaker_config_t config(); void config(const speaker_config_t&);
  void setVolume(uint8_t); size_t isPlaying(uint8_t);
  bool playRaw(const int16_t*, size_t, uint32_t, bool, uint32_t, int, bool);
};
struct config_t { bool internal_mic; };
struct M5Unified { Speaker_Class Speaker; config_t config(); void update(); };
extern M5Unified M5;
struct Keyboard_Class {
  struct KeysState { bool tab, fn, shift, ctrl, opt, alt, del, enter, space; std::vector<char> word; };
  KeysState& keysState(); bool isChange();
};
struct M5Cardputer_Class { LGFX_Device Display; Keyboard_Class Keyboard; void begin(config_t, bool); void update(); };
extern M5Cardputer_Class M5Cardputer;
