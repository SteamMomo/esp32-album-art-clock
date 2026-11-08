// Wi-Fi, HTTP, flash and sensor stand-ins. In the simulator the network is
// always "offline", so the clock faces run exactly as they do when idle.
#pragma once
#include <Arduino.h>
#include <SPI.h>

// ---- Wi-Fi ------------------------------------------------------------------
#define WL_CONNECTED 3
#define WL_DISCONNECTED 6
#define WIFI_STA 1
struct WiFiClass {
  int status() { return WL_CONNECTED; }
  void begin(const char*, const char*) {}
  void mode(int) {}
  void setAutoReconnect(bool) {}
  void disconnect() {}
};
extern WiFiClass WiFi;

class WiFiClient : public Stream {
 public:
  bool connected() { return false; }
  void stop() {}
  void setTimeout(unsigned long) {}
};
class WiFiClientSecure : public WiFiClient {
 public:
  void setCACert(const char*) {}
  void setInsecure() {}
};

// ---- HTTP -------------------------------------------------------------------
#define HTTP_CODE_OK 200
#define HTTP_CODE_NO_CONTENT 204
#define HTTP_CODE_UNAUTHORIZED 401
#define HTTP_CODE_FORBIDDEN 403
#define HTTP_CODE_TOO_MANY_REQUESTS 429
class HTTPClient {
 public:
  bool begin(WiFiClient&, const String&) { return false; }
  bool begin(const String&) { return false; }
  void end() {}
  void setTimeout(uint16_t) {}
  void setReuse(bool) {}
  void addHeader(const String&, const String&) {}
  void collectHeaders(const char* [], size_t) {}
  String header(const char*) { return String(); }
  int GET() { return -1; }
  int POST(const String&) { return -1; }
  int POST(const char*) { return -1; }
  int getSize() { return -1; }
  String getString() { return String(); }
  WiFiClient& getStream() { return client_; }
  WiFiClient* getStreamPtr() { return &client_; }

 private:
  WiFiClient client_;
};

// ---- LittleFS ---------------------------------------------------------------
#define FILE_READ "r"
#define FILE_WRITE "w"
namespace fs {
class File {
 public:
  explicit operator bool() const { return false; }
  size_t write(const uint8_t*, size_t n) { return n; }
  size_t write(uint8_t) { return 1; }
  size_t size() { return 0; }
  int read() { return -1; }
  void close() {}
};
class FS {
 public:
  bool begin(bool = false) { return true; }
  File open(const char*, const char* = "r") { return File(); }
  File open(const String& p, const char* m = "r") { return open(p.c_str(), m); }
  bool remove(const char*) { return true; }
  bool rename(const char*, const char*) { return true; }
  bool exists(const char*) { return false; }
};
}  // namespace fs
using fs::File;
using fs::FS;
extern fs::FS LittleFS;

// ---- Preferences (NVS) --------------------------------------------------------
class Preferences {
 public:
  bool begin(const char*, bool = false) { return true; }
  void end() {}
  uint8_t getUChar(const char*, uint8_t fallback = 0) { return fallback; }
  size_t putUChar(const char*, uint8_t) { return 1; }
  String getString(const char*, const String& fallback = String()) { return fallback; }
  size_t putString(const char*, const String&) { return 1; }
  bool remove(const char*) { return true; }
  bool clear() { return true; }
};

// ---- Touch ------------------------------------------------------------------
struct TS_Point {
  int16_t x = 0, y = 0, z = 0;
};
class XPT2046_Touchscreen {
 public:
  XPT2046_Touchscreen(uint8_t, uint8_t = 255) {}
  bool begin(SPIClass&) { return true; }
  bool begin() { return true; }
  void setRotation(uint8_t) {}
  bool touched() { return false; }
  TS_Point getPoint() { return TS_Point(); }
};

// ---- JPEG decoder -----------------------------------------------------------
typedef enum { JDR_OK = 0, JDR_INTR, JDR_INP } JRESULT;
class TJpg_Decoder {
 public:
  typedef bool (*Callback)(int16_t, int16_t, uint16_t, uint16_t, uint16_t*);
  void setCallback(Callback) {}
  void setSwapBytes(bool) {}
  void setJpgScale(uint8_t) {}
  JRESULT getFsJpgSize(uint16_t* w, uint16_t* h, const char*, fs::FS&) {
    *w = *h = 0;
    return JDR_INP;
  }
  JRESULT drawFsJpg(int32_t, int32_t, const char*, fs::FS&) { return JDR_INP; }
};
extern TJpg_Decoder TJpgDec;

// ---- DHT11 --------------------------------------------------------------------
#define DHT11 11
namespace sim {
extern float temperature;
extern float humidity;
}
class DHT {
 public:
  DHT(uint8_t, uint8_t) {}
  void begin() {}
  float readTemperature() { return sim::temperature; }
  float readHumidity() { return sim::humidity; }
};
