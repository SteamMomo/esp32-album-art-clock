// Minimal Arduino/ESP32 stand-in so the firmware compiles and runs on a PC.
// Only what main.cpp and the libraries it uses actually need.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <cmath>
#include <string>
#include <algorithm>
#include <type_traits>
#include <memory>
#include <new>
#include <functional>
#include <initializer_list>
#include <sys/time.h>
#include <time.h>
#include <ctime>

typedef uint8_t byte;
typedef bool boolean;

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105

#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2

#define PROGMEM
#define pgm_read_byte(addr) (*(const uint8_t*)(addr))
#define pgm_read_word(addr) (*(const uint16_t*)(addr))
// TFT_eSPI only uses dword reads to fetch pointers, so read pointer-sized.
#define pgm_read_dword(addr) (*(const uintptr_t*)(addr))
#define pgm_read_ptr(addr) (*(void* const*)(addr))

#define constrain(amt, low, high) \
  ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))

template <class T, class U>
constexpr typename std::common_type<T, U>::type min(T a, U b) {
  return b < a ? b : a;
}
template <class T, class U>
constexpr typename std::common_type<T, U>::type max(T a, U b) {
  return a < b ? b : a;
}
using std::isnan;
using std::isinf;

// ---- time and randomness (controlled by the simulator) --------------------
namespace sim {
extern uint32_t nowMs;
void onPinWrite(uint8_t pin, uint8_t value);
}
inline uint32_t millis() { return sim::nowMs; }
inline uint32_t micros() { return sim::nowMs * 1000u; }
inline void delay(uint32_t ms) { sim::nowMs += ms; }
inline void delayMicroseconds(uint32_t) {}
inline void yield() {}
inline void pinMode(uint8_t, uint8_t) {}
inline void digitalWrite(uint8_t pin, uint8_t value) {
  sim::onPinWrite(pin, value);
}
inline int digitalRead(uint8_t) { return HIGH; }
inline uint32_t digitalPinToBitMask(uint8_t) { return 0; }
inline volatile uint32_t* portOutputRegister(uint32_t) { static volatile uint32_t r; return &r; }
inline uint32_t digitalPinToPort(uint8_t) { return 0; }

long random(long howBig);
long random(long howSmall, long howBig);
void randomSeed(unsigned long seed);
uint32_t esp_random();

time_t sim_time(time_t* out);
int sim_gettimeofday(struct timeval* tv, void* tz);
struct tm* sim_localtime_r(const time_t* t, struct tm* out);
inline struct tm* sim_localtime_r(const long* t, struct tm* out) {
  const time_t wide = *t;
  return sim_localtime_r(&wide, out);
}
#define time(x) sim_time(x)
#define gettimeofday(a, b) sim_gettimeofday(a, b)
#define localtime_r(a, b) sim_localtime_r(a, b)
inline void configTzTime(const char*, const char*, const char* = nullptr,
                         const char* = nullptr) {}

// ---- LEDC backlight PWM ---------------------------------------------------
inline void ledcSetup(uint8_t, uint32_t, uint8_t) {}
inline void ledcAttachPin(uint8_t, uint8_t) {}
inline void ledcWrite(uint8_t, uint32_t) {}

// ---- FreeRTOS -------------------------------------------------------------
typedef void* SemaphoreHandle_t;
typedef void* TaskHandle_t;
typedef int BaseType_t;
typedef uint32_t TickType_t;
#define portMAX_DELAY 0xFFFFFFFFu
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(ms) (ms)
inline SemaphoreHandle_t xSemaphoreCreateMutex() {
  static int token;
  return &token;
}
inline BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t) { return pdTRUE; }
inline BaseType_t xSemaphoreGive(SemaphoreHandle_t) { return pdTRUE; }
inline void vTaskDelay(TickType_t) {}
inline BaseType_t xTaskCreatePinnedToCore(void (*)(void*), const char*,
                                          uint32_t, void*, int, TaskHandle_t*,
                                          int) {
  return pdPASS;  // the network task never runs in the simulator
}

struct EspClass {
  uint32_t getFreeHeap() { return 200000; }
  uint32_t getMaxAllocHeap() { return 110000; }
};
extern EspClass ESP;

// ---- String ---------------------------------------------------------------
class String {
 public:
  String() {}
  String(const char* s) { if (s) value_ = s; }
  String(const std::string& s) : value_(s) {}
  String(char c) : value_(1, c) {}
  String(int v) : value_(std::to_string(v)) {}
  String(unsigned int v) : value_(std::to_string(v)) {}
  String(long v) : value_(std::to_string(v)) {}
  String(unsigned long v) : value_(std::to_string(v)) {}
  String(long long v) : value_(std::to_string(v)) {}
  String(unsigned long long v) : value_(std::to_string(v)) {}
  String(float v, int decimals = 2) { format(v, decimals); }
  String(double v, int decimals = 2) { format(v, decimals); }

  String& operator=(const char* s) {
    value_ = s ? s : "";
    return *this;
  }
  const char* c_str() const { return value_.c_str(); }
  unsigned int length() const { return value_.size(); }
  bool isEmpty() const { return value_.empty(); }
  bool reserve(unsigned int n) { value_.reserve(n); return true; }
  bool concat(const char* s) { if (s) value_ += s; return true; }
  bool concat(const String& s) { value_ += s.value_; return true; }
  bool concat(char c) { value_ += c; return true; }
  String& operator+=(const String& s) { value_ += s.value_; return *this; }
  String& operator+=(const char* s) { if (s) value_ += s; return *this; }
  String& operator+=(char c) { value_ += c; return *this; }
  char operator[](unsigned int i) const { return i < value_.size() ? value_[i] : 0; }
  char& operator[](unsigned int i) { return value_[i]; }
  char charAt(unsigned int i) const { return (*this)[i]; }
  bool operator==(const String& o) const { return value_ == o.value_; }
  bool operator==(const char* o) const { return value_ == (o ? o : ""); }
  bool operator!=(const String& o) const { return value_ != o.value_; }
  bool operator!=(const char* o) const { return !(*this == o); }
  bool operator<(const String& o) const { return value_ < o.value_; }
  bool equals(const String& o) const { return value_ == o.value_; }
  bool startsWith(const String& p) const { return value_.rfind(p.value_, 0) == 0; }
  bool endsWith(const String& p) const {
    return value_.size() >= p.value_.size() &&
           value_.compare(value_.size() - p.value_.size(), p.value_.size(), p.value_) == 0;
  }
  int indexOf(char c, unsigned int from = 0) const {
    const size_t i = value_.find(c, from);
    return i == std::string::npos ? -1 : static_cast<int>(i);
  }
  int indexOf(const String& s, unsigned int from = 0) const {
    const size_t i = value_.find(s.value_, from);
    return i == std::string::npos ? -1 : static_cast<int>(i);
  }
  int lastIndexOf(char c) const {
    const size_t i = value_.rfind(c);
    return i == std::string::npos ? -1 : static_cast<int>(i);
  }
  String substring(unsigned int from) const {
    return from >= value_.size() ? String() : String(value_.substr(from));
  }
  String substring(unsigned int from, unsigned int to) const {
    if (from > to) std::swap(from, to);
    if (from >= value_.size()) return String();
    return String(value_.substr(from, to - from));
  }
  void remove(unsigned int index) { if (index < value_.size()) value_.erase(index); }
  void remove(unsigned int index, unsigned int count) {
    if (index < value_.size()) value_.erase(index, count);
  }
  void replace(const String& from, const String& to) {
    if (from.value_.empty()) return;
    size_t pos = 0;
    while ((pos = value_.find(from.value_, pos)) != std::string::npos) {
      value_.replace(pos, from.value_.size(), to.value_);
      pos += to.value_.size();
    }
  }
  void trim() {
    const size_t a = value_.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { value_.clear(); return; }
    const size_t b = value_.find_last_not_of(" \t\r\n");
    value_ = value_.substr(a, b - a + 1);
  }
  void toUpperCase() { for (char& c : value_) c = toupper(c); }
  void toLowerCase() { for (char& c : value_) c = tolower(c); }
  void toCharArray(char* buf, unsigned int size) const {
    if (!size) return;
    strncpy(buf, value_.c_str(), size - 1);
    buf[size - 1] = 0;
  }
  long toInt() const { return atol(value_.c_str()); }
  float toFloat() const { return atof(value_.c_str()); }

  friend String operator+(const String& a, const String& b) {
    return String(a.value_ + b.value_);
  }
  friend String operator+(const String& a, const char* b) {
    return String(a.value_ + (b ? b : ""));
  }
  friend String operator+(const char* a, const String& b) {
    return String(std::string(a ? a : "") + b.value_);
  }
  friend String operator+(const String& a, char b) { return String(a.value_ + b); }

 private:
  void format(double v, int decimals) {
    char buffer[48];
    snprintf(buffer, sizeof(buffer), "%.*f", decimals, v);
    value_ = buffer;
  }
  std::string value_;
};

// ---- Print / Stream ---------------------------------------------------------
class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t* data, size_t size) {
    size_t n = 0;
    while (size--) n += write(*data++);
    return n;
  }
  size_t write(const char* s) { return write(reinterpret_cast<const uint8_t*>(s), strlen(s)); }
  size_t print(const char* s) { return write(s); }
  size_t print(const String& s) { return write(s.c_str()); }
  size_t print(char c) { return write(static_cast<uint8_t>(c)); }
  size_t print(int v) { return print(String(v)); }
  size_t print(unsigned v) { return print(String(v)); }
  size_t print(long v) { return print(String(v)); }
  size_t print(unsigned long v) { return print(String(v)); }
  size_t print(double v, int d = 2) { return print(String(v, d)); }
  size_t println() { return write("\n"); }
  template <class T> size_t println(const T& v) { return print(v) + println(); }
  size_t printf(const char* format, ...) {
    char buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return write(buffer);
  }
};

class Printable {
 public:
  virtual ~Printable() {}
  virtual size_t printTo(Print& p) const = 0;
};

class Stream : public Print {
 public:
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  size_t write(uint8_t) override { return 1; }
  using Print::write;
  size_t readBytes(char* buffer, size_t length) {
    size_t n = 0;
    while (n < length) {
      const int c = read();
      if (c < 0) break;
      buffer[n++] = static_cast<char>(c);
    }
    return n;
  }
  size_t readBytes(uint8_t* buffer, size_t length) {
    return readBytes(reinterpret_cast<char*>(buffer), length);
  }
  void setTimeout(unsigned long) {}
};

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long) {}
  size_t write(uint8_t c) override { fputc(c, stdout); return 1; }
  using Print::write;
};
extern HardwareSerial Serial;
