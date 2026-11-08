// PC side of the simulator: a virtual ILI9341 that decodes the SPI byte
// stream into a framebuffer, a controllable clock, and a tiny PNG writer.
#include <Arduino.h>
#include <SPI.h>
#include "stubs/Network.h"
#include "sim_host.h"

#undef time
#undef gettimeofday
#undef localtime_r

namespace sim {
uint32_t nowMs = 1000;
float temperature = 24.0f;
float humidity = 58.0f;
uint16_t frame[kWidth * kHeight];

namespace {
// ---- ILI9341 emulation ------------------------------------------------------
bool dataMode = true;
uint8_t command = 0;
int argIndex = 0;
uint8_t args[4];
int x0 = 0, x1 = kWidth - 1, y0 = 0, y1 = kHeight - 1;
int cx = 0, cy = 0;
bool pixelHigh = true;
uint8_t highByte = 0;

// ---- time -------------------------------------------------------------------
int64_t baseEpoch = 1700000000;
uint32_t baseMs = 0;
constexpr int kUtcOffset = 5 * 3600 + 30 * 60;  // matches TZ_INFO "IST-5:30"

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + static_cast<int64_t>(doe) - 719468;
}

uint32_t rng = 12345;
}  // namespace

void onPinWrite(uint8_t pin, uint8_t value) {
  if (pin == TFT_DC) {
    dataMode = value == HIGH;
  }
}

void onSpiByte(uint8_t value) {
  if (!dataMode) {
    command = value;
    argIndex = 0;
    if (command == 0x2C) {  // RAMWR: start at the window origin
      cx = x0;
      cy = y0;
      pixelHigh = true;
    }
    return;
  }
  switch (command) {
    case 0x2A:  // CASET
    case 0x2B:  // PASET
      if (argIndex < 4) {
        args[argIndex++] = value;
      }
      if (argIndex == 4) {
        const int a = (args[0] << 8) | args[1];
        const int b = (args[2] << 8) | args[3];
        if (command == 0x2A) {
          x0 = a;
          x1 = b;
        } else {
          y0 = a;
          y1 = b;
        }
      }
      break;
    case 0x2C:
    case 0x3C:
      if (pixelHigh) {
        highByte = value;
        pixelHigh = false;
        break;
      }
      pixelHigh = true;
      if (cx >= 0 && cx < kWidth && cy >= 0 && cy < kHeight) {
        frame[cy * kWidth + cx] = static_cast<uint16_t>((highByte << 8) | value);
      }
      if (++cx > x1) {
        cx = x0;
        if (++cy > y1) {
          cy = y0;
        }
      }
      break;
    default:
      break;
  }
}

void setLocalTime(int year, int month, int day, int hour, int minute,
                  int second) {
  baseEpoch = daysFromCivil(year, month, day) * 86400LL + hour * 3600LL +
              minute * 60LL + second - kUtcOffset;
  baseMs = nowMs;
}

int64_t epochMs() {
  return baseEpoch * 1000LL + static_cast<int64_t>(nowMs - baseMs);
}

// ---- PNG (uncompressed deflate, so no zlib is needed) -----------------------
namespace {
uint32_t crcTable[256];
void initCrc() {
  for (uint32_t n = 0; n < 256; ++n) {
    uint32_t c = n;
    for (int k = 0; k < 8; ++k) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
    crcTable[n] = c;
  }
}
uint32_t crc(const uint8_t* data, size_t length, uint32_t c = 0xFFFFFFFFu) {
  for (size_t i = 0; i < length; ++i) c = crcTable[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c;
}
void put32(std::string& out, uint32_t v) {
  out += static_cast<char>(v >> 24);
  out += static_cast<char>(v >> 16);
  out += static_cast<char>(v >> 8);
  out += static_cast<char>(v);
}
void chunk(std::string& out, const char* type, const std::string& data) {
  put32(out, data.size());
  std::string body = std::string(type, 4) + data;
  out += body;
  put32(out, crc(reinterpret_cast<const uint8_t*>(body.data()), body.size()) ^
                 0xFFFFFFFFu);
}
}  // namespace

bool savePng(const char* path, int scale) {
  return savePngBuffer(path, frame, kWidth, kHeight, scale);
}

bool savePngBuffer(const char* path, const uint16_t* pixels, int width,
                   int height, int scale) {
  static bool ready = false;
  if (!ready) {
    initCrc();
    ready = true;
  }
  const int w = width * scale;
  const int h = height * scale;
  std::string raw;
  raw.reserve((w * 3 + 1) * h);
  for (int y = 0; y < h; ++y) {
    raw += '\0';
    for (int x = 0; x < w; ++x) {
      const uint16_t c = pixels[(y / scale) * width + x / scale];
      const uint8_t r = (c >> 11) & 0x1F;
      const uint8_t g = (c >> 5) & 0x3F;
      const uint8_t b = c & 0x1F;
      raw += static_cast<char>((r << 3) | (r >> 2));
      raw += static_cast<char>((g << 2) | (g >> 4));
      raw += static_cast<char>((b << 3) | (b >> 2));
    }
  }
  std::string z = "\x78\x01";
  uint32_t a = 1, b = 0;
  for (unsigned char ch : raw) {
    a = (a + ch) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t pos = 0; pos < raw.size();) {
    const size_t n = std::min<size_t>(65535, raw.size() - pos);
    const bool last = pos + n == raw.size();
    z += static_cast<char>(last ? 1 : 0);
    z += static_cast<char>(n & 0xFF);
    z += static_cast<char>(n >> 8);
    z += static_cast<char>(~n & 0xFF);
    z += static_cast<char>((~n >> 8) & 0xFF);
    z.append(raw, pos, n);
    pos += n;
  }
  put32(z, (b << 16) | a);
  std::string ihdr;
  put32(ihdr, w);
  put32(ihdr, h);
  ihdr += std::string("\x08\x02\x00\x00\x00", 5);
  std::string png = "\x89PNG\r\n\x1a\n";
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", "");
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  fwrite(png.data(), 1, png.size(), f);
  fclose(f);
  return true;
}

uint32_t nextRandom() {
  rng = rng * 1103515245u + 12345u;
  return (rng >> 1) ^ (rng << 15);
}
void seedRandom(uint32_t seed) { rng = seed ? seed : 1; }
}  // namespace sim

// ---- Arduino globals ---------------------------------------------------------
HardwareSerial Serial;
EspClass ESP;
SPIClass SPI(VSPI);
WiFiClass WiFi;
fs::FS LittleFS;
TJpg_Decoder TJpgDec;

long random(long howBig) {
  return howBig <= 0 ? 0 : static_cast<long>(sim::nextRandom() % howBig);
}
long random(long howSmall, long howBig) {
  return howSmall >= howBig ? howSmall : howSmall + random(howBig - howSmall);
}
void randomSeed(unsigned long seed) { sim::seedRandom(seed); }
uint32_t esp_random() { return sim::nextRandom(); }

time_t sim_time(time_t* out) {
  const time_t t = static_cast<time_t>(sim::epochMs() / 1000);
  if (out) *out = t;
  return t;
}
int sim_gettimeofday(struct timeval* tv, void*) {
  const int64_t ms = sim::epochMs();
  tv->tv_sec = static_cast<long>(ms / 1000);
  tv->tv_usec = static_cast<long>((ms % 1000) * 1000);
  return 0;
}
struct tm* sim_localtime_r(const time_t* t, struct tm* out) {
  const time_t local = *t + 5 * 3600 + 30 * 60;
  *out = *gmtime(&local);
  return out;
}
