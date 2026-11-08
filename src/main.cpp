#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <memory>
#include <math.h>
#include <new>
#include <Preferences.h>
#include <string.h>
#include <SPI.h>
#include <sys/time.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <TJpg_Decoder.h>
#include <DHT.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "config.h"
#include "certs.h"
#include "secrets.h"

namespace {

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------
constexpr uint32_t SPOTIFY_POLL_MS = 10000;
constexpr uint32_t WIFI_RETRY_MS = 10000;
constexpr uint32_t TOKEN_RETRY_MS = 15000;
constexpr uint32_t TOKEN_EARLY_REFRESH_MS = 60000;
constexpr uint32_t HTTP_TIMEOUT_MS = 8000;
constexpr uint32_t DHT_READ_INTERVAL_MS = 2000;
constexpr uint32_t FRAME_MS = 33;  // ~30 fps for animations
constexpr uint32_t FADE_OUT_MS = 220;
constexpr uint32_t FADE_IN_MS = 320;
constexpr uint32_t DIGIT_ROLL_MS = 420;
constexpr uint32_t TICKER_HOLD_MS = 1800;
constexpr uint32_t TICKER_SPEED_PX_PER_S = 30;
constexpr uint32_t PLAYBACK_INTRO_MS = 700;
constexpr size_t MAX_ART_BYTES = 256 * 1024;
constexpr const char* ART_PATH = "/art.jpg";
constexpr const char* ART_TEMP_PATH = "/art.tmp";
constexpr uint8_t DHT_PIN = 18;
constexpr uint8_t BACKLIGHT_CHANNEL = 0;
constexpr uint8_t BACKLIGHT_MAX = 255;

// ---------------------------------------------------------------------------
// Colors
// ---------------------------------------------------------------------------
constexpr uint16_t rgb565(uint8_t red, uint8_t green, uint8_t blue) {
  return ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3);
}
constexpr uint16_t CLOCK_TOP_COLOR = rgb565(20, 24, 48);
constexpr uint16_t CLOCK_BOTTOM_COLOR = rgb565(4, 5, 12);
constexpr uint16_t CLOCK_ACCENT_COLOR = rgb565(255, 150, 80);
constexpr uint16_t DEFAULT_PLAY_ACCENT = rgb565(167, 139, 250);
constexpr uint16_t TEXT_COLOR = rgb565(244, 245, 248);
constexpr uint16_t MUTED_COLOR = rgb565(150, 158, 182);
constexpr uint16_t TEMP_COLOR = rgb565(255, 122, 92);
constexpr uint16_t HUMIDITY_COLOR = rgb565(96, 165, 250);
constexpr uint16_t MUSIC_COLOR = rgb565(30, 215, 96);
constexpr uint16_t OFFLINE_COLOR = rgb565(95, 100, 118);

// ---------------------------------------------------------------------------
// Layout (landscape 320 x 240, rotation 3)
// ---------------------------------------------------------------------------
constexpr int SCREEN_W = 320;
constexpr int SCREEN_H = 240;

// Clock
constexpr int DIGIT_W = 55;   // Font 8 digit advance
constexpr int DIGIT_H = 76;   // Font 8 height (75) + 1
constexpr int COLON_GAP = 30;
constexpr int TIME_Y = 46;
constexpr int TIME_X = (SCREEN_W - (DIGIT_W * 4 + COLON_GAP)) / 2;
constexpr int DIGIT_X[4] = {TIME_X, TIME_X + DIGIT_W,
                            TIME_X + DIGIT_W * 2 + COLON_GAP,
                            TIME_X + DIGIT_W * 3 + COLON_GAP};
constexpr int COLON_X = SCREEN_W / 2;
constexpr int COLON_Y1 = TIME_Y + 26;
constexpr int COLON_Y2 = TIME_Y + 52;
constexpr int SEC_X = 34;
constexpr int SEC_Y = 134;
constexpr int SEC_W = 252;
constexpr int SEC_H = 12;
constexpr int CARD_Y = 160;
constexpr int CARD_W = 92;
constexpr int CARD_H = 64;
constexpr int CARD_X[3] = {12, 114, 216};

// Playback
constexpr int ART_SIZE = 184;
constexpr int ART_X = 14;
constexpr int ART_Y = (SCREEN_H - ART_SIZE) / 2;
constexpr int ART_RADIUS = 14;
constexpr int PANEL_X = ART_X + ART_SIZE + 14;
constexpr int PANEL_W = SCREEN_W - PANEL_X - 12;
// Text block sits in the upper part of the panel; times and progress bar are
// pinned to the bottom, level with the bottom of the artwork.
constexpr int LABEL_Y = ART_Y + 22;
constexpr int TICKER_Y = LABEL_Y + 14;
constexpr int TICKER_H = 30;
constexpr int ARTIST_Y = TICKER_Y + 38;
constexpr int ALBUM_Y = ARTIST_Y + 38;
constexpr int METER_H = 28;
constexpr int METER_Y = ART_Y + ART_SIZE - METER_H;

enum class Scene { None, Message, Clock, Playback };

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
TFT_eSPI tft;
TFT_eSprite digitSprite(&tft);
TFT_eSprite secondsSprite(&tft);
TFT_eSprite tickerSprite(&tft);
TFT_eSprite meterSprite(&tft);
TFT_eSprite faceSprite(&tft);  // owned by the active clock face
SPIClass touchSpi(HSPI);       // the display uses VSPI
XPT2046_Touchscreen touch(TOUCH_CS_PIN, TOUCH_IRQ_PIN);
Preferences displayPrefs;
DHT dht(DHT_PIN, DHT11);
Preferences preferences;

Scene scene = Scene::None;
uint8_t backlightLevel = 0;

uint16_t backdropTop = CLOCK_TOP_COLOR;
uint16_t backdropBottom = CLOCK_BOTTOM_COLOR;
uint16_t accentColor = CLOCK_ACCENT_COLOR;
uint16_t playMutedColor = MUTED_COLOR;

String accessToken;
String refreshToken;
String currentTrackId;
String currentTrackTitle;
String currentTrackArtist;
String currentAlbum;
uint32_t tokenExpiresAt = 0;
uint32_t nextTokenAttempt = 0;
uint32_t nextPlaybackPoll = 0;
uint32_t nextWifiAttempt = 0;
uint32_t retryAfterMs = 0;
bool timeWasSet = false;
bool playbackIsPlaying = false;
// Display-side copies of the network task's login state.
bool spotifyLoginFailed = false;
bool spotifyLinked = false;
bool spotifyLoading = false;

// Playback progress, interpolated between polls.
uint32_t trackProgressMs = 0;
uint32_t trackDurationMs = 0;
uint32_t trackProgressStamp = 0;

// Message scene
String messageTitle;
String messageDetail;
bool messageSpinner = false;
uint32_t lastMessageFrame = 0;

// Clock scene
char renderedDigits[5] = "    ";
String renderedDate;
bool clockSyncingShown = false;
bool renderedWifiConnected = false;
String renderedCardValues[3];
uint32_t lastClockFrame = 0;

// Climate
String temperatureValue = "--";
String humidityValue = "--";
bool climateAvailable = false;
uint32_t lastDhtReadAt = 0;
bool dhtReadCompleted = false;

// Playback scene
String tickerTitle;
int tickerTextWidth = 0;
uint32_t playbackShownAt = 0;
uint32_t lastPlaybackFrame = 0;
String renderedPlaybackStatus;

// JPEG decode target
enum class JpegMode { Idle, Sample, Render };
constexpr int ART_BLOCK_MAX = 48;  // largest resized JPEG block we draw
JpegMode jpegMode = JpegMode::Idle;
int jpegSourceW = 0;
int jpegSourceH = 0;
bool artStorageReady = false;
uint16_t artBlock[ART_BLOCK_MAX * ART_BLOCK_MAX];
uint32_t sampleSumR = 0, sampleSumG = 0, sampleSumB = 0, sampleCount = 0;
uint64_t sampleWeightR = 0, sampleWeightG = 0, sampleWeightB = 0,
         sampleWeightTotal = 0;

// ---------------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------------
uint16_t blendColor(uint16_t foreground, uint16_t background,
                    uint8_t opacity) {
  const uint16_t inverse = 255 - opacity;
  const uint16_t red = (((foreground >> 11) & 0x1F) * opacity +
                        ((background >> 11) & 0x1F) * inverse) / 255;
  const uint16_t green = (((foreground >> 5) & 0x3F) * opacity +
                          ((background >> 5) & 0x3F) * inverse) / 255;
  const uint16_t blue = ((foreground & 0x1F) * opacity +
                         (background & 0x1F) * inverse) / 255;
  return (red << 11) | (green << 5) | blue;
}

uint16_t darkenColor(uint16_t color, uint8_t brightnessPercent) {
  const uint16_t red = ((color >> 11) & 0x1F) * brightnessPercent / 100;
  const uint16_t green = ((color >> 5) & 0x3F) * brightnessPercent / 100;
  const uint16_t blue = (color & 0x1F) * brightnessPercent / 100;
  return (red << 11) | (green << 5) | blue;
}

inline uint16_t swap16(uint16_t value) {
  return static_cast<uint16_t>((value >> 8) | (value << 8));
}

uint16_t backdropAt(int y) {
  const int clamped = constrain(y, 0, SCREEN_H - 1);
  return blendColor(backdropBottom, backdropTop,
                    static_cast<uint8_t>(clamped * 255 / (SCREEN_H - 1)));
}

void drawBackdrop(int x, int y, int width, int height) {
  for (int row = y; row < y + height; ++row) {
    tft.drawFastHLine(x, row, width, backdropAt(row));
  }
}

void fillSpriteBackdrop(TFT_eSprite& sprite, int screenY) {
  const int height = sprite.height();
  const int width = sprite.width();
  for (int row = 0; row < height; ++row) {
    sprite.drawFastHLine(0, row, width, backdropAt(screenY + row));
  }
}

float easeOutCubic(float t) {
  const float inverse = 1.0f - t;
  return 1.0f - inverse * inverse * inverse;
}

float easeInOutSine(float t) {
  return 0.5f - 0.5f * cosf(PI * t);
}

// ---------------------------------------------------------------------------
// Backlight
// ---------------------------------------------------------------------------
void setupBacklight() {
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(TFT_BL_PIN, 12000, 8);
#else
  ledcSetup(BACKLIGHT_CHANNEL, 12000, 8);
  ledcAttachPin(TFT_BL_PIN, BACKLIGHT_CHANNEL);
#endif
}

void writeBacklight(uint8_t level) {
  backlightLevel = level;
  // Square the level so fades look even to the eye.
  const uint32_t duty = static_cast<uint32_t>(level) * level / 255;
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(TFT_BL_PIN, duty);
#else
  ledcWrite(BACKLIGHT_CHANNEL, duty);
#endif
}

void fadeBacklight(uint8_t target, uint32_t durationMs) {
  const uint8_t start = backlightLevel;
  if (start == target) {
    return;
  }
  const uint32_t began = millis();
  while (true) {
    const uint32_t elapsed = millis() - began;
    if (elapsed >= durationMs) {
      break;
    }
    const float t = easeInOutSine(static_cast<float>(elapsed) / durationMs);
    writeBacklight(static_cast<uint8_t>(start + (target - start) * t));
    delay(8);
  }
  writeBacklight(target);
}

// Fades the panel down before a scene redraw, unless the screen is already off.
void beginSceneChange() {
  if (backlightLevel > 0) {
    fadeBacklight(0, FADE_OUT_MS);
  }
  faceSprite.deleteSprite();  // each clock face allocates its own on entry
}

void endSceneChange() {
  fadeBacklight(BACKLIGHT_MAX, FADE_IN_MS);
}

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------
void useFont(TFT_eSPI& target, uint8_t font) {
  target.setTextFont(font);  // also clears any FreeFont
}

void useFreeFont(TFT_eSPI& target, const GFXfont* font) {
  target.setFreeFont(font);
}

// Word-wraps text into at most maxLines, adding "..." when text is cut off.
// The caller selects the font first.
void drawWrappedText(const String& text, int x, int y, int maxWidth,
                     int lineHeight, int maxLines, uint16_t color) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color);
  String line;
  int lineCount = 0;
  int start = 0;
  const int length = text.length();
  while (start < length && lineCount < maxLines) {
    int end = text.indexOf(' ', start);
    if (end < 0) {
      end = length;
    }
    const String word = text.substring(start, end);
    const String candidate = line.length() == 0 ? word : line + " " + word;
    if (line.length() > 0 && tft.textWidth(candidate) > maxWidth) {
      if (lineCount == maxLines - 1) {
        break;  // last line: fall through to ellipsis handling
      }
      tft.drawString(line, x, y + lineCount * lineHeight);
      ++lineCount;
      line = word;
    } else {
      line = candidate;
    }
    start = end + 1;
  }
  if (lineCount >= maxLines || line.length() == 0) {
    return;
  }
  const bool truncated = start < length;
  if (truncated || tft.textWidth(line) > maxWidth) {
    while (line.length() > 1 && tft.textWidth(line + "...") > maxWidth) {
      line.remove(line.length() - 1);
    }
    line.trim();
    line += "...";
  }
  tft.drawString(line, x, y + lineCount * lineHeight);
}

String formatDuration(uint32_t milliseconds) {
  const uint32_t totalSeconds = milliseconds / 1000;
  char text[12];
  snprintf(text, sizeof(text), "%u:%02u",
           static_cast<unsigned>(totalSeconds / 60),
           static_cast<unsigned>(totalSeconds % 60));
  return String(text);
}

bool deadlineReached(uint32_t now, uint32_t deadline) {
  return static_cast<int32_t>(now - deadline) >= 0;
}

bool timeIsSynced() {
  return time(nullptr) >= 1700000000;
}

// ---------------------------------------------------------------------------
// Message scene (connecting, setup, login errors)
// ---------------------------------------------------------------------------
void drawSpinner(uint32_t now) {
  constexpr int centerX = SCREEN_W / 2;
  constexpr int centerY = 172;
  constexpr int radius = 14;
  const float head = (now % 1000) / 1000.0f * 8.0f;
  for (int i = 0; i < 8; ++i) {
    const float angle = i * PI / 4.0f - PI / 2.0f;
    const int dotX = centerX + static_cast<int>(cosf(angle) * radius);
    const int dotY = centerY + static_cast<int>(sinf(angle) * radius);
    float distance = head - i;
    if (distance < 0) {
      distance += 8.0f;
    }
    const uint8_t strength = static_cast<uint8_t>(
        max(0.0f, 1.0f - distance / 6.0f) * 225.0f + 30.0f);
    const uint16_t background = backdropAt(dotY);
    tft.fillSmoothCircle(dotX, dotY, 3,
                         blendColor(CLOCK_ACCENT_COLOR, background, strength),
                         background);
  }
}

void showMessage(const String& title, const String& detail, bool spinner) {
  if (scene == Scene::Message && title == messageTitle &&
      detail == messageDetail) {
    return;
  }
  beginSceneChange();
  scene = Scene::Message;
  messageTitle = title;
  messageDetail = detail;
  messageSpinner = spinner;
  backdropTop = CLOCK_TOP_COLOR;
  backdropBottom = CLOCK_BOTTOM_COLOR;
  drawBackdrop(0, 0, SCREEN_W, SCREEN_H);
  tft.setTextDatum(MC_DATUM);
  useFreeFont(tft, &FreeSansBold12pt7b);
  tft.setTextColor(TEXT_COLOR);
  tft.drawString(title, SCREEN_W / 2, 100);
  useFont(tft, 2);
  tft.setTextColor(MUTED_COLOR);
  tft.drawString(detail, SCREEN_W / 2, 132);
  if (spinner) {
    drawSpinner(millis());
  }
  endSceneChange();
}

void animateMessage(uint32_t now) {
  if (scene != Scene::Message || !messageSpinner ||
      now - lastMessageFrame < FRAME_MS) {
    return;
  }
  lastMessageFrame = now;
  drawSpinner(now);
}

// ---------------------------------------------------------------------------
// Clock scene
// ---------------------------------------------------------------------------
void drawWifiIcon(bool connected) {
  constexpr int iconX = SCREEN_W - 24;
  constexpr int iconY = 30;
  drawBackdrop(iconX - 16, 8, 32, 26);
  const uint16_t color = connected ? TEXT_COLOR : OFFLINE_COLOR;
  const uint16_t background = backdropAt(iconY - 6);
  tft.fillSmoothCircle(iconX, iconY - 2, 2, color, background);
  tft.drawArc(iconX, iconY - 2, 9, 7, 135, 225, color, background, true);
  tft.drawArc(iconX, iconY - 2, 15, 13, 135, 225,
              connected ? color : blendColor(color, background, 120),
              background, true);
  if (!connected) {
    tft.drawWideLine(iconX - 11, iconY - 16, iconX + 9, iconY, 2,
                     TEMP_COLOR, background);
  }
}

void drawThermometerIcon(int x, int y, uint16_t color, uint16_t background) {
  tft.fillSmoothRoundRect(x - 2, y - 9, 5, 13, 2, color, background);
  tft.fillSmoothCircle(x, y + 5, 4, color, background);
}

void drawDropletIcon(int x, int y, uint16_t color, uint16_t background) {
  tft.fillTriangle(x - 5, y + 1, x + 5, y + 1, x, y - 9, color);
  tft.fillSmoothCircle(x, y + 3, 5, color, background);
}

void drawNoteIcon(int x, int y, uint16_t color, uint16_t background) {
  tft.fillSmoothCircle(x - 3, y + 5, 4, color, background);
  tft.fillRect(x, y - 8, 2, 13, color);
  tft.fillTriangle(x + 1, y - 8, x + 7, y - 4, x + 1, y - 2, color);
}

String spotifyStatusText() {
  if (WiFi.status() != WL_CONNECTED) {
    return "Offline";
  }
  if (spotifyLoginFailed) {
    return "Login";
  }
  if (!spotifyLinked) {
    return "Linking";
  }
  if (spotifyLoading) {
    return "Loading";
  }
  return "Idle";
}

void drawClockCard(int index, const String& label, const String& value,
                   bool degree) {
  const int x = CARD_X[index];
  const uint16_t background = backdropAt(CARD_Y + CARD_H / 2);
  const uint16_t cardColor = blendColor(TEXT_COLOR, background, 16);
  const uint16_t borderColor = blendColor(TEXT_COLOR, background, 34);
  drawBackdrop(x, CARD_Y, CARD_W, CARD_H);
  tft.fillSmoothRoundRect(x, CARD_Y, CARD_W, CARD_H, 12, borderColor,
                          backdropAt(CARD_Y));
  tft.fillRoundRect(x + 1, CARD_Y + 1, CARD_W - 2, CARD_H - 2, 11, cardColor);

  const int iconX = x + CARD_W - 16;
  const int iconY = CARD_Y + 16;
  const bool offline = index == 2 && value == "Offline";
  if (index == 0) {
    drawThermometerIcon(iconX, iconY, TEMP_COLOR, cardColor);
  } else if (index == 1) {
    drawDropletIcon(iconX, iconY, HUMIDITY_COLOR, cardColor);
  } else {
    drawNoteIcon(iconX, iconY, offline ? OFFLINE_COLOR : MUSIC_COLOR,
                 cardColor);
  }

  tft.setTextDatum(TL_DATUM);
  useFont(tft, 2);
  tft.setTextColor(MUTED_COLOR);
  tft.drawString(label, x + 10, CARD_Y + 8);

  useFreeFont(tft, &FreeSansBold12pt7b);
  tft.setTextColor(offline ? OFFLINE_COLOR : TEXT_COLOR);
  tft.drawString(value, x + 10, CARD_Y + 32);
  if (degree) {
    const int valueWidth = tft.textWidth(value);
    tft.drawSmoothCircle(x + 14 + valueWidth, CARD_Y + 36, 3, TEXT_COLOR,
                         cardColor);
    useFont(tft, 2);
    tft.drawString("C", x + 20 + valueWidth, CARD_Y + 34);
  }
  renderedCardValues[index] = value;
}

void refreshClockCards(bool force) {
  const String temperature = climateAvailable ? temperatureValue : "--";
  const String humidity = climateAvailable ? humidityValue + "%" : "--";
  const String spotify = spotifyStatusText();
  if (force || renderedCardValues[0] != temperature) {
    drawClockCard(0, "INDOOR", temperature, climateAvailable);
  }
  if (force || renderedCardValues[1] != humidity) {
    drawClockCard(1, "HUMIDITY", humidity, false);
  }
  if (force || renderedCardValues[2] != spotify) {
    drawClockCard(2, "SPOTIFY", spotify, false);
  }
}

void setSpotifyCard(const String& value) {
  if (scene == Scene::Clock && renderedCardValues[2] != value) {
    drawClockCard(2, "SPOTIFY", value, false);
  }
}

void readClimate(uint32_t now) {
  if (dhtReadCompleted && now - lastDhtReadAt < DHT_READ_INTERVAL_MS) {
    return;
  }
  lastDhtReadAt = now;
  dhtReadCompleted = true;
  const float temperature = dht.readTemperature();
  const float humidity = dht.readHumidity();
  if (isnan(temperature) || isnan(humidity)) {
    climateAvailable = false;
    return;
  }
  climateAvailable = true;
  temperatureValue = String(static_cast<int>(lroundf(temperature)));
  humidityValue = String(static_cast<int>(lroundf(humidity)));
}

void renderDigit(int index, char oldDigit, char newDigit, float progress) {
  fillSpriteBackdrop(digitSprite, TIME_Y);
  const float eased = easeOutCubic(progress);
  const int offset = static_cast<int>(eased * DIGIT_H + 0.5f);
  const uint16_t middle = backdropAt(TIME_Y + DIGIT_H / 2);
  digitSprite.setTextDatum(TC_DATUM);
  useFont(digitSprite, 8);
  if (oldDigit != ' ' && offset < DIGIT_H) {
    digitSprite.setTextColor(blendColor(
        TEXT_COLOR, middle, static_cast<uint8_t>((1.0f - eased) * 255)));
    digitSprite.drawChar(oldDigit, DIGIT_W / 2 - digitSprite.textWidth(
                                       String(oldDigit)) / 2,
                         -offset, 8);
  }
  if (newDigit != ' ') {
    digitSprite.setTextColor(blendColor(
        TEXT_COLOR, middle, static_cast<uint8_t>(80 + eased * 175)));
    digitSprite.drawChar(newDigit, DIGIT_W / 2 - digitSprite.textWidth(
                                       String(newDigit)) / 2,
                         DIGIT_H - offset, 8);
  }
  digitSprite.pushSprite(DIGIT_X[index], TIME_Y);
}

void drawColon(const struct timeval& now) {
  // Bright at the top of each second, then eases back.
  const float phase = now.tv_usec / 1000000.0f;
  const float pulse = 0.5f + 0.5f * cosf(phase * 2.0f * PI);
  for (int dotY : {COLON_Y1, COLON_Y2}) {
    const uint16_t background = backdropAt(dotY);
    tft.fillSmoothCircle(COLON_X, dotY, 5,
                         blendColor(CLOCK_ACCENT_COLOR, background,
                                    static_cast<uint8_t>(90 + pulse * 165)),
                         background);
  }
}

void drawSecondsBar(bool synced, const struct timeval& now,
                    const struct tm& localTime) {
  constexpr int trackStart = 6;
  constexpr int trackLength = SEC_W - 12;
  constexpr int lineY = SEC_H / 2;
  fillSpriteBackdrop(secondsSprite, SEC_Y);
  const uint16_t background = backdropAt(SEC_Y + lineY);
  const uint16_t trackColor = blendColor(TEXT_COLOR, background, 34);
  secondsSprite.fillRoundRect(trackStart, lineY - 1, trackLength, 3, 1,
                              trackColor);
  if (synced) {
    const float fraction =
        (localTime.tm_sec + now.tv_usec / 1000000.0f) / 60.0f;
    const int head = trackStart + static_cast<int>(fraction * trackLength);
    secondsSprite.fillRoundRect(trackStart, lineY - 1, head - trackStart + 1,
                                3, 1, CLOCK_ACCENT_COLOR);
    secondsSprite.fillSmoothCircle(head, lineY, 5,
                                   blendColor(CLOCK_ACCENT_COLOR, background,
                                              70),
                                   background);
    secondsSprite.fillSmoothCircle(head, lineY, 3, TEXT_COLOR,
                                   CLOCK_ACCENT_COLOR);
  } else {
    // Indeterminate sweep while waiting for NTP.
    const float t = (millis() % 1600) / 1600.0f;
    const int center = trackStart + static_cast<int>(
        easeInOutSine(t < 0.5f ? t * 2 : 2 - t * 2) * trackLength);
    const int left = max(trackStart, center - 30);
    const int right = min(trackStart + trackLength, center + 30);
    secondsSprite.fillRoundRect(left, lineY - 1, right - left, 3, 1,
                                CLOCK_ACCENT_COLOR);
  }
  secondsSprite.pushSprite(SEC_X, SEC_Y);
}

void drawClockDate(const String& date) {
  drawBackdrop(0, 8, SCREEN_W - 44, 26);
  tft.setTextDatum(TL_DATUM);
  useFreeFont(tft, &FreeSansBold9pt7b);
  tft.setTextColor(TEXT_COLOR);
  tft.drawString(date, 16, 14);
  renderedDate = date;
}

// Rolls changed digits upward; stagger staggers each digit for the intro.
void animateDigits(const char* target, bool stagger) {
  bool changed[4];
  bool finished[4];
  bool any = false;
  for (int i = 0; i < 4; ++i) {
    changed[i] = renderedDigits[i] != target[i];
    finished[i] = !changed[i];
    any = any || changed[i];
  }
  if (!any) {
    return;
  }
  const uint32_t start = millis();
  const uint32_t staggerMs = stagger ? 80 : 0;
  while (true) {
    const uint32_t elapsed = millis() - start;
    bool done = true;
    for (int i = 0; i < 4; ++i) {
      if (finished[i]) {
        continue;
      }
      const int32_t local = static_cast<int32_t>(elapsed) -
                            static_cast<int32_t>(i * staggerMs);
      const float progress =
          constrain(local / static_cast<float>(DIGIT_ROLL_MS), 0.0f, 1.0f);
      renderDigit(i, renderedDigits[i], target[i], progress);
      if (progress >= 1.0f) {
        finished[i] = true;
      } else {
        done = false;
      }
    }
    struct timeval now;
    gettimeofday(&now, nullptr);
    struct tm localTime;
    localtime_r(&now.tv_sec, &localTime);
    drawColon(now);
    drawSecondsBar(true, now, localTime);
    if (done) {
      break;
    }
    delay(10);
  }
  memcpy(renderedDigits, target, 4);
}

void drawPageDots(uint16_t background);

void enterDigitalFace() {
  backdropTop = CLOCK_TOP_COLOR;
  backdropBottom = CLOCK_BOTTOM_COLOR;
  accentColor = CLOCK_ACCENT_COLOR;
  drawBackdrop(0, 0, SCREEN_W, SCREEN_H);
  memcpy(renderedDigits, "    ", 5);
  renderedDate = "";
  clockSyncingShown = false;
  renderedWifiConnected = WiFi.status() == WL_CONNECTED;
  drawWifiIcon(renderedWifiConnected);
  refreshClockCards(true);
  drawPageDots(backdropAt(233));
}

void updateDigitalFace() {
  const bool wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected != renderedWifiConnected) {
    renderedWifiConnected = wifiConnected;
    drawWifiIcon(wifiConnected);
  }
  refreshClockCards(false);

  struct timeval now;
  gettimeofday(&now, nullptr);
  struct tm localTime;
  localtime_r(&now.tv_sec, &localTime);

  if (!timeIsSynced()) {
    if (!clockSyncingShown) {
      drawBackdrop(0, TIME_Y, SCREEN_W, DIGIT_H);
      tft.setTextDatum(MC_DATUM);
      useFreeFont(tft, &FreeSansBold12pt7b);
      tft.setTextColor(MUTED_COLOR);
      tft.drawString("Syncing time", SCREEN_W / 2, TIME_Y + DIGIT_H / 2);
      drawClockDate("Waiting for network time");
      clockSyncingShown = true;
    }
    drawSecondsBar(false, now, localTime);
    return;
  }
  timeWasSet = true;
  if (clockSyncingShown) {
    drawBackdrop(0, TIME_Y, SCREEN_W, DIGIT_H);
    clockSyncingShown = false;
  }

  char dateText[32];
  strftime(dateText, sizeof(dateText), "%A, %d %B", &localTime);
  if (renderedDate != dateText) {
    drawClockDate(dateText);
  }

  char digits[5];
  strftime(digits, sizeof(digits), "%H%M", &localTime);
  if (memcmp(digits, renderedDigits, 4) != 0) {
    const bool intro = renderedDigits[0] == ' ';
    animateDigits(digits, intro);
    return;
  }
  drawColon(now);
  drawSecondsBar(true, now, localTime);
}

// ---------------------------------------------------------------------------
// Clock faces (tap the screen to cycle)
// ---------------------------------------------------------------------------
enum FaceId : uint8_t { FACE_DIGITAL, FACE_BUDDY, FACE_SKY, FACE_ANALOG,
                        FACE_LOWPOLY, FACE_PIXEL, FACE_CAT, FACE_ROBOT,
                        FACE_COUNT };
uint8_t clockFace = FACE_DIGITAL;
String faceRenderedTime;
String faceRenderedDate;
String faceRenderedClimate;

uint16_t lerpColor(uint16_t from, uint16_t to, float t) {
  return blendColor(to, from,
                    static_cast<uint8_t>(constrain(t, 0.0f, 1.0f) * 255));
}

void currentLocalTime(struct timeval& now, struct tm& localTime) {
  gettimeofday(&now, nullptr);
  localtime_r(&now.tv_sec, &localTime);
}

String climateKey() {
  return climateAvailable ? temperatureValue + "|" + humidityValue : "--";
}

void drawPageDots(uint16_t background) {
  constexpr int dotY = 233;
  const int startX = SCREEN_W / 2 - (FACE_COUNT - 1) * 6;
  tft.fillRect(startX - 6, dotY - 5, (FACE_COUNT - 1) * 12 + 13, 11,
               background);
  for (int i = 0; i < FACE_COUNT; ++i) {
    const bool active = i == clockFace;
    tft.fillSmoothCircle(startX + i * 12, dotY, active ? 3 : 2,
                         active ? TEXT_COLOR
                                : blendColor(TEXT_COLOR, background, 80),
                         background);
  }
}

// Draws "24°C"-style text with a drawn degree ring; returns its width.
int drawTemperatureText(const String& value, int x, int y, uint16_t color,
                        uint16_t background, bool showUnit) {
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(color);
  tft.drawString(value, x, y);
  const int valueWidth = tft.textWidth(value);
  if (!showUnit) {
    return valueWidth;
  }
  const int ringRadius = tft.fontHeight() > 24 ? 3 : 2;
  tft.drawSmoothCircle(x + valueWidth + ringRadius + 2, y + ringRadius + 1,
                       ringRadius, color, background);
  const int unitX = x + valueWidth + ringRadius * 2 + 5;
  tft.drawString("C", unitX, y);
  return unitX + tft.textWidth("C") - x;
}

bool allocateFaceSprite(int width, int height) {
  faceSprite.setColorDepth(16);
  if (faceSprite.createSprite(width, height) == nullptr) {
    Serial.printf("Face sprite %dx%d failed (max block %u); using digital.\n",
                  width, height,
                  static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  faceSprite.setTextWrap(false);
  return true;
}

// ---- Buddy: a little mochi creature that breathes, blinks, glances around,
// hops when the minute changes and sleeps at night. -----------------------
constexpr int BUDDY_X = 10;
constexpr int BUDDY_Y = 70;
constexpr int BUDDY_SIZE = 132;
constexpr int BUDDY_PANEL_X = 158;
constexpr uint16_t BUDDY_TOP = rgb565(48, 34, 88);
constexpr uint16_t BUDDY_BOTTOM = rgb565(12, 9, 28);
constexpr uint16_t MOCHI_BODY = rgb565(255, 210, 188);
constexpr uint16_t MOCHI_HIGHLIGHT = rgb565(255, 238, 226);
constexpr uint16_t MOCHI_EYE = rgb565(54, 36, 60);
constexpr uint16_t MOCHI_BLUSH = rgb565(255, 128, 140);
constexpr uint16_t MOCHI_MOUTH = rgb565(196, 82, 96);
constexpr uint16_t SPROUT_LEAF = rgb565(130, 216, 120);
constexpr uint16_t SPROUT_STEM = rgb565(92, 172, 94);
constexpr uint32_t BUDDY_HOP_MS = 760;

uint32_t buddyHopAt = 0;
uint32_t buddyNextBlink = 0;
uint32_t buddyBlinkUntil = 0;
bool buddyDoubleBlink = false;

// The buddy faces share one layout; only the character changes.
enum BuddyKind : uint8_t { BUDDY_MOCHI, BUDDY_CAT, BUDDY_ROBOT };
uint8_t buddyKind = BUDDY_MOCHI;
uint16_t buddyAccent = MOCHI_BLUSH;

bool buddyAsleep(const struct tm& localTime) {
  return localTime.tm_hour >= 23 || localTime.tm_hour < 6;
}

void drawMochiEye(int x, int y, int mode) {
  // mode: 0 open, 1 blink, 2 happy (^), 3 asleep (u)
  switch (mode) {
    case 0:
      faceSprite.fillSmoothCircle(x, y, 7, MOCHI_EYE, MOCHI_BODY);
      faceSprite.fillSmoothCircle(x + 2, y - 3, 2, TEXT_COLOR, MOCHI_EYE);
      faceSprite.fillSmoothCircle(x - 2, y + 3, 1, TEXT_COLOR, MOCHI_EYE);
      break;
    case 1:
      faceSprite.drawWideLine(x - 6, y, x + 6, y, 3, MOCHI_EYE, MOCHI_BODY);
      break;
    case 2:
      faceSprite.drawWideLine(x - 6, y + 3, x, y - 3, 3, MOCHI_EYE,
                              MOCHI_BODY);
      faceSprite.drawWideLine(x, y - 3, x + 6, y + 3, 3, MOCHI_EYE,
                              MOCHI_BODY);
      break;
    default:
      faceSprite.drawWideLine(x - 6, y - 1, x, y + 2, 2.5f, MOCHI_EYE,
                              MOCHI_BODY);
      faceSprite.drawWideLine(x, y + 2, x + 6, y - 1, 2.5f, MOCHI_EYE,
                              MOCHI_BODY);
      break;
  }
}

void drawMochiCharacter(uint32_t now, bool asleep) {
  fillSpriteBackdrop(faceSprite, BUDDY_Y);
  const float t = now / 1000.0f;

  const uint32_t sinceHop = now - buddyHopAt;
  const bool hopping = !asleep && sinceHop < BUDDY_HOP_MS;
  const float hopProgress = sinceHop / static_cast<float>(BUDDY_HOP_MS);
  float hop = 0.0f;
  const float breathe = asleep ? sinf(t * 1.3f) : sinf(t * 2.6f);
  float squash = breathe * 0.035f;
  if (hopping) {
    hop = sinf(PI * hopProgress) * 22.0f;
    squash = -0.08f * sinf(PI * hopProgress);
    if (hopProgress > 0.82f) {
      squash = 0.16f * sinf(PI * (hopProgress - 0.82f) / 0.18f);
    }
  }

  constexpr int centerX = BUDDY_SIZE / 2;
  constexpr int floorY = BUDDY_SIZE - 10;
  const int radiusX = static_cast<int>(46.0f * (1.0f + squash));
  const int radiusY = static_cast<int>(38.0f * (1.0f - squash));
  const int centerY = floorY - 3 - radiusY - static_cast<int>(hop);
  const uint16_t background = backdropAt(BUDDY_Y + centerY);

  // Floor shadow shrinks while airborne.
  const int shadowRadius = max(14, 40 - static_cast<int>(hop * 0.9f));
  faceSprite.fillEllipse(centerX, floorY + 1, shadowRadius, 5,
                         blendColor(TFT_BLACK, backdropAt(BUDDY_Y + floorY),
                                    110));

  // Sprout sways on top.
  const float sway = sinf(t * 1.7f) * (asleep ? 0.15f : 0.4f) +
                     (hopping ? -0.3f * sinf(PI * hopProgress) : 0.0f);
  const int stemBaseY = centerY - radiusY + 4;
  const int tipX = centerX + static_cast<int>(sinf(sway) * 15.0f);
  const int tipY = stemBaseY - static_cast<int>(cosf(sway) * 16.0f);
  faceSprite.drawWideLine(centerX, stemBaseY, tipX, tipY, 3, SPROUT_STEM,
                          background);
  faceSprite.fillSmoothCircle(tipX - 5, tipY - 1, 5, SPROUT_LEAF, background);
  faceSprite.fillSmoothCircle(tipX + 5, tipY - 3, 5, SPROUT_LEAF, background);

  // Body and soft highlight.
  faceSprite.fillSmoothRoundRect(centerX - radiusX, centerY - radiusY,
                                 radiusX * 2, radiusY * 2, radiusY - 5,
                                 MOCHI_BODY, background);
  faceSprite.fillEllipse(centerX - radiusX / 2 + 4, centerY - radiusY / 2 + 2,
                         10, 5, MOCHI_HIGHLIGHT);

  // Eyes: blink on a random timer, glance around slowly.
  if (!asleep && deadlineReached(now, buddyNextBlink)) {
    buddyBlinkUntil = now + 130;
    buddyDoubleBlink = random(0, 4) == 0;
    buddyNextBlink = now + (buddyDoubleBlink ? 260 : random(2200, 5200));
  }
  int eyeMode = 0;
  if (asleep) {
    eyeMode = 3;
  } else if (hopping) {
    eyeMode = 2;
  } else if (!deadlineReached(now, buddyBlinkUntil)) {
    eyeMode = 1;
  }
  const int look = asleep ? 0 : static_cast<int>(sinf(t * 0.45f) * 3.5f);
  const int eyeY = centerY - 2;
  drawMochiEye(centerX - 17 + look, eyeY, eyeMode);
  drawMochiEye(centerX + 17 + look, eyeY, eyeMode);

  // Cheeks and mouth.
  const uint16_t blush = blendColor(MOCHI_BLUSH, MOCHI_BODY, 140);
  faceSprite.fillSmoothCircle(centerX - 30 + look, centerY + 9, 6, blush,
                              MOCHI_BODY);
  faceSprite.fillSmoothCircle(centerX + 30 + look, centerY + 9, 6, blush,
                              MOCHI_BODY);
  const int mouthX = centerX + look;
  const int mouthY = centerY + 9;
  if (hopping) {
    faceSprite.fillSmoothCircle(mouthX, mouthY + 1, 4, MOCHI_MOUTH,
                                MOCHI_BODY);
  } else if (asleep) {
    faceSprite.fillSmoothCircle(mouthX, mouthY + 1, 2, MOCHI_MOUTH,
                                MOCHI_BODY);
  } else {
    faceSprite.drawArc(mouthX, mouthY - 2, 5, 3, 300, 360, MOCHI_EYE,
                       MOCHI_BODY, true);
    faceSprite.drawArc(mouthX, mouthY - 2, 5, 3, 0, 60, MOCHI_EYE,
                       MOCHI_BODY, true);
  }

  // Floating z's while asleep.
  if (asleep) {
    useFont(faceSprite, 2);
    faceSprite.setTextDatum(MC_DATUM);
    for (int k = 0; k < 3; ++k) {
      const float phase = fmodf(t * 0.3f + k / 3.0f, 1.0f);
      const int zx = centerX + 34 + static_cast<int>(phase * 16.0f);
      const int zy = centerY - radiusY - 4 - static_cast<int>(phase * 34.0f);
      const uint16_t zBackground = backdropAt(BUDDY_Y + zy);
      faceSprite.setTextColor(blendColor(
          TEXT_COLOR, zBackground,
          static_cast<uint8_t>(sinf(PI * phase) * 230.0f)));
      faceSprite.drawString("z", zx, zy);
    }
  }
  faceSprite.pushSprite(BUDDY_X, BUDDY_Y);
}

// ---- Miso the cat: sits like a loaf, swishes its tail, flicks an ear,
// follows things with its eyes and waves a paw each minute. ---------------
constexpr uint16_t CAT_FUR = rgb565(250, 178, 112);
constexpr uint16_t CAT_FUR_DARK = rgb565(214, 126, 68);
constexpr uint16_t CAT_CREAM = rgb565(255, 238, 218);
constexpr uint16_t CAT_PINK = rgb565(255, 150, 170);
constexpr uint16_t CAT_EYE = rgb565(46, 36, 50);
constexpr uint16_t CAT_IRIS = rgb565(132, 204, 110);
constexpr uint16_t CAT_WHISKER = rgb565(255, 246, 236);
uint32_t catEarUntil = 0;
uint32_t catNextEar = 0;

void drawCatEye(int x, int y, int mode, int look) {
  // mode: 0 open, 1 blink, 2 happy (^), 3 asleep
  switch (mode) {
    case 0:
      faceSprite.fillSmoothCircle(x, y, 7, CAT_IRIS, CAT_FUR);
      faceSprite.fillEllipse(x + look, y, 2, 5, CAT_EYE);
      faceSprite.fillSmoothCircle(x + 2 + look / 2, y - 3, 2, TEXT_COLOR,
                                  CAT_IRIS);
      break;
    case 1:
      faceSprite.drawWideLine(x - 6, y, x + 6, y, 2.5f, CAT_EYE, CAT_FUR);
      break;
    case 2:
      faceSprite.drawWideLine(x - 6, y + 3, x, y - 3, 2.5f, CAT_EYE, CAT_FUR);
      faceSprite.drawWideLine(x, y - 3, x + 6, y + 3, 2.5f, CAT_EYE, CAT_FUR);
      break;
    default:
      faceSprite.drawWideLine(x - 6, y, x, y + 3, 2.5f, CAT_EYE, CAT_FUR);
      faceSprite.drawWideLine(x, y + 3, x + 6, y, 2.5f, CAT_EYE, CAT_FUR);
      break;
  }
}

void catEar(int bx0, int by0, int tipX, int tipY, int bx1, int by1,
            uint16_t background) {
  faceSprite.fillTriangle(bx0, by0, tipX, tipY, bx1, by1, CAT_FUR);
  faceSprite.drawWideLine(bx0, by0, tipX, tipY, 1.5f, CAT_FUR, background);
  faceSprite.drawWideLine(tipX, tipY, bx1, by1, 1.5f, CAT_FUR, background);
  const int ix0 = bx0 + (tipX - bx0) / 4 + (bx1 - bx0) / 6;
  const int ix1 = bx1 + (tipX - bx1) / 4 - (bx1 - bx0) / 6;
  const int iy0 = by0 + (tipY - by0) / 4;
  const int iy1 = by1 + (tipY - by1) / 4;
  faceSprite.fillTriangle(ix0, iy0, tipX + (bx0 + bx1 - 2 * tipX) / 6,
                          tipY + (by0 + by1 - 2 * tipY) / 5, ix1, iy1,
                          CAT_PINK);
}

void drawCatCharacter(uint32_t now, bool asleep) {
  fillSpriteBackdrop(faceSprite, BUDDY_Y);
  const float t = now / 1000.0f;
  const uint32_t since = now - buddyHopAt;
  const bool waving = !asleep && since < 1500;
  const float breathe = sinf(t * (asleep ? 1.2f : 2.2f));
  constexpr int cx = 62;
  constexpr int floorY = 124;
  faceSprite.fillEllipse(cx + 8, floorY, 46, 5,
                         blendColor(TFT_BLACK, backdropAt(BUDDY_Y + floorY),
                                    110));

  // Tail, a chain of thick round segments behind the body.
  float px = cx + 32;
  float py = floorY - 9;
  float angle = asleep ? 3.0f : -0.15f;
  const float sway = asleep ? sinf(t * 0.8f) * 0.04f : sinf(t * 1.7f) * 0.16f;
  for (int i = 0; i < 8; ++i) {
    angle += asleep ? 0.05f : -0.2f + sway * (i * 0.25f);
    const float nx = px + cosf(angle) * 6.0f;
    const float ny = py + sinf(angle) * 6.0f;
    const uint16_t color = i >= 6 ? CAT_FUR_DARK : CAT_FUR;
    faceSprite.drawWideLine(px, py, nx, ny, 9.0f - i * 0.4f, color,
                            backdropAt(BUDDY_Y + static_cast<int>(ny)));
    px = nx;
    py = ny;
  }

  // Body loaf with a cream belly and stripes.
  const int bodyRx = 40 + static_cast<int>(breathe);
  const int bodyRy = 26 - static_cast<int>(breathe);
  const int bodyCy = floorY - bodyRy - 2;
  const uint16_t background = backdropAt(BUDDY_Y + bodyCy);
  faceSprite.fillSmoothRoundRect(cx - bodyRx, bodyCy - bodyRy, bodyRx * 2,
                                 bodyRy * 2, bodyRy - 4, CAT_FUR, background);
  faceSprite.fillEllipse(cx - 6, bodyCy + 8, 20, 14, CAT_CREAM);
  for (int k = 0; k < 3; ++k) {
    const int sx = cx + 14 + k * 7;
    faceSprite.drawWideLine(sx, bodyCy - bodyRy + 3, sx + 3,
                            bodyCy - bodyRy + 11, 3, CAT_FUR_DARK, CAT_FUR);
  }

  // Front paws; one waves each minute.
  const int pawY = floorY - 5;
  faceSprite.fillSmoothCircle(cx + 4, pawY, 6, CAT_CREAM, CAT_FUR);
  if (!waving) {
    faceSprite.fillSmoothCircle(cx - 20, pawY, 6, CAT_CREAM, CAT_FUR);
  }

  // Head.
  const int hx = cx - 8;
  const int hy = bodyCy - bodyRy - 2 + (asleep ? 14 : 0) +
                 static_cast<int>(breathe * 0.6f);
  const uint16_t headBackground = backdropAt(BUDDY_Y + hy - 20);
  if (!asleep && deadlineReached(now, catNextEar)) {
    catEarUntil = now + 200;
    catNextEar = now + random(3000, 8000);
  }
  const bool flick = !deadlineReached(now, catEarUntil);
  catEar(hx - 28, hy - 8, hx - 22, hy - 38, hx - 6, hy - 22, headBackground);
  catEar(hx + 6, hy - 22, hx + 22 + (flick ? 4 : 0), hy - 38 + (flick ? 6 : 0),
         hx + 28, hy - 8, headBackground);
  faceSprite.fillSmoothRoundRect(hx - 32, hy - 26, 64, 50, 23, CAT_FUR,
                                 headBackground);
  for (int k = -1; k <= 1; ++k) {
    faceSprite.drawWideLine(hx + k * 6, hy - 24, hx + k * 5, hy - 16, 2.5f,
                            CAT_FUR_DARK, CAT_FUR);
  }
  faceSprite.fillEllipse(hx, hy + 11, 14, 9, CAT_CREAM);

  // Eyes.
  if (!asleep && deadlineReached(now, buddyNextBlink)) {
    buddyBlinkUntil = now + 130;
    buddyNextBlink = now + random(2400, 5600);
  }
  int eyeMode = 0;
  if (asleep) {
    eyeMode = 3;
  } else if (waving) {
    eyeMode = 2;
  } else if (!deadlineReached(now, buddyBlinkUntil)) {
    eyeMode = 1;
  }
  const int look = asleep ? 0 : static_cast<int>(sinf(t * 0.5f) * 3.0f);
  drawCatEye(hx - 14, hy - 2, eyeMode, look);
  drawCatEye(hx + 14, hy - 2, eyeMode, look);

  // Nose, mouth, cheeks and whiskers.
  faceSprite.fillTriangle(hx - 3, hy + 5, hx + 3, hy + 5, hx, hy + 8,
                          CAT_PINK);
  faceSprite.drawArc(hx - 3, hy + 9, 3, 2, 270, 360, CAT_EYE, CAT_CREAM, true);
  faceSprite.drawArc(hx - 3, hy + 9, 3, 2, 0, 90, CAT_EYE, CAT_CREAM, true);
  faceSprite.drawArc(hx + 3, hy + 9, 3, 2, 270, 360, CAT_EYE, CAT_CREAM, true);
  faceSprite.drawArc(hx + 3, hy + 9, 3, 2, 0, 90, CAT_EYE, CAT_CREAM, true);
  const uint16_t blush = blendColor(CAT_PINK, CAT_FUR, 120);
  faceSprite.fillSmoothCircle(hx - 24, hy + 8, 5, blush, CAT_FUR);
  faceSprite.fillSmoothCircle(hx + 24, hy + 8, 5, blush, CAT_FUR);
  const float twitch = asleep ? 0.0f : sinf(t * 3.0f) * 1.0f;
  for (int k = 0; k < 3; ++k) {
    const float y0 = hy + 7 + k * 3;
    faceSprite.drawWideLine(hx - 12, y0, hx - 38, y0 - 4 + k * 4 + twitch, 1,
                            CAT_WHISKER, CAT_FUR);
    faceSprite.drawWideLine(hx + 12, y0, hx + 38, y0 - 4 + k * 4 - twitch, 1,
                            CAT_WHISKER, CAT_FUR);
  }

  if (waving) {
    const float wave = sinf(since / 1000.0f * 14.0f) * 5.0f;
    const int wx = hx - 38;
    const int wy = hy + 12 + static_cast<int>(wave);
    faceSprite.drawWideLine(cx - 20, pawY - 4, wx, wy, 10, CAT_FUR,
                            backdropAt(BUDDY_Y + wy));
    faceSprite.fillSmoothCircle(wx, wy, 6, CAT_CREAM,
                                backdropAt(BUDDY_Y + wy));
    faceSprite.fillSmoothCircle(wx, wy + 1, 2, CAT_PINK, CAT_CREAM);
  }

  if (asleep) {
    useFont(faceSprite, 2);
    faceSprite.setTextDatum(MC_DATUM);
    for (int k = 0; k < 3; ++k) {
      const float phase = fmodf(t * 0.3f + k / 3.0f, 1.0f);
      const int zx = hx + 30 + static_cast<int>(phase * 16.0f);
      const int zy = hy - 26 - static_cast<int>(phase * 26.0f);
      faceSprite.setTextColor(blendColor(
          TEXT_COLOR, backdropAt(BUDDY_Y + zy),
          static_cast<uint8_t>(sinf(PI * phase) * 230.0f)));
      faceSprite.drawString("z", zx, zy);
    }
  }
  faceSprite.pushSprite(BUDDY_X, BUDDY_Y);
}

// ---- Bolt the robot: hovers on a little thruster, its screen face blinks
// and glances around, the antenna light pulses and it waves each minute. --
constexpr uint16_t BOT_SHELL = rgb565(232, 236, 246);
constexpr uint16_t BOT_SHADE = rgb565(178, 188, 208);
constexpr uint16_t BOT_SCREEN = rgb565(18, 26, 46);
constexpr uint16_t BOT_GLOW = rgb565(96, 236, 255);
constexpr uint16_t BOT_ACCENT = rgb565(255, 140, 90);

void drawRobotCharacter(uint32_t now, bool asleep) {
  fillSpriteBackdrop(faceSprite, BUDDY_Y);
  const float t = now / 1000.0f;
  const uint32_t since = now - buddyHopAt;
  const bool waving = !asleep && since < 1600;
  const float hover = asleep ? 0.0f : sinf(t * 2.0f) * 4.0f;
  constexpr int cx = 66;
  constexpr int floorY = 126;
  const int lift = asleep ? 10 : 0;  // settles down to sleep

  const int shadow = asleep ? 30 : 22 - static_cast<int>(hover);
  faceSprite.fillEllipse(cx, floorY - 1, shadow, 4,
                         blendColor(TFT_BLACK, backdropAt(BUDDY_Y + floorY),
                                    110));

  const int bodyY = 92 + static_cast<int>(hover) + lift;  // body centre
  // Thruster flame.
  if (!asleep) {
    const float flicker = 0.75f + 0.25f * sinf(t * 31.0f) +
                          0.1f * sinf(t * 53.0f);
    const int flameLength = static_cast<int>(10 * flicker) + 4;
    const int fy = bodyY + 19;
    const uint16_t bg = backdropAt(BUDDY_Y + fy + 6);
    faceSprite.fillTriangle(cx - 7, fy, cx + 7, fy, cx, fy + flameLength + 3,
                            blendColor(BOT_ACCENT, bg, 200));
    faceSprite.fillTriangle(cx - 4, fy, cx + 4, fy, cx, fy + flameLength - 2,
                            rgb565(255, 230, 140));
  }
  faceSprite.fillSmoothRoundRect(cx - 9, bodyY + 14, 18, 7, 3, BOT_SHADE,
                                 backdropAt(BUDDY_Y + bodyY + 17));

  // Arms swing; the right one waves.
  const uint16_t armBg = backdropAt(BUDDY_Y + bodyY);
  const float swing = asleep ? 0.0f : sinf(t * 2.0f + 1.0f) * 3.0f;
  faceSprite.drawWideLine(cx - 22, bodyY - 6, cx - 32, bodyY + 8 + swing, 5,
                          BOT_SHADE, armBg);
  faceSprite.fillSmoothCircle(cx - 32, bodyY + 9 + static_cast<int>(swing), 4,
                              BOT_SHELL, armBg);
  if (waving) {
    const float wave = sinf(since / 1000.0f * 12.0f) * 7.0f;
    const int handX = cx + 36 + static_cast<int>(wave);
    const int handY = bodyY - 26;
    faceSprite.drawWideLine(cx + 22, bodyY - 6, handX, handY, 5, BOT_SHADE,
                            armBg);
    faceSprite.fillSmoothCircle(handX, handY, 4, BOT_SHELL,
                                backdropAt(BUDDY_Y + handY));
  } else {
    faceSprite.drawWideLine(cx + 22, bodyY - 6, cx + 32, bodyY + 8 - swing, 5,
                            BOT_SHADE, armBg);
    faceSprite.fillSmoothCircle(cx + 32, bodyY + 9 - static_cast<int>(swing),
                                4, BOT_SHELL, armBg);
  }

  // Body with a chest panel of lights that count the seconds.
  faceSprite.fillSmoothRoundRect(cx - 22, bodyY - 16, 44, 32, 10, BOT_SHELL,
                                 armBg);
  faceSprite.fillSmoothRoundRect(cx - 14, bodyY - 8, 28, 14, 4, BOT_SCREEN,
                                 BOT_SHELL);
  struct timeval nowTime;
  gettimeofday(&nowTime, nullptr);
  const int lit = asleep ? 0 : static_cast<int>((nowTime.tv_sec % 60) / 12) + 1;
  for (int k = 0; k < 5; ++k) {
    faceSprite.fillSmoothCircle(cx - 10 + k * 5, bodyY - 1, 1,
                                k < lit ? BOT_GLOW
                                        : blendColor(BOT_GLOW, BOT_SCREEN, 50),
                                BOT_SCREEN);
  }

  // Head: a rounded shell around a dark screen face.
  const int hy = bodyY - 40;
  const uint16_t headBg = backdropAt(BUDDY_Y + hy);
  faceSprite.fillSmoothRoundRect(cx - 6, hy + 20, 12, 8, 2, BOT_SHADE, armBg);
  // Antenna with a pulsing light.
  faceSprite.drawWideLine(cx, hy - 24, cx, hy - 36, 2, BOT_SHADE, headBg);
  const float pulse = 0.5f + 0.5f * sinf(t * (waving ? 12.0f : 3.0f));
  const uint16_t bulb = asleep ? blendColor(BOT_ACCENT, headBg, 70)
                               : blendColor(BOT_ACCENT, rgb565(255, 220, 200),
                                            static_cast<uint8_t>(150 + pulse * 105));
  faceSprite.fillSmoothCircle(cx, hy - 37, 6,
                              blendColor(bulb, backdropAt(BUDDY_Y + hy - 37),
                                         static_cast<uint8_t>(pulse * 70)),
                              backdropAt(BUDDY_Y + hy - 37));
  faceSprite.fillSmoothCircle(cx, hy - 37, 4, bulb,
                              blendColor(bulb, backdropAt(BUDDY_Y + hy - 37),
                                         static_cast<uint8_t>(pulse * 70)));
  faceSprite.fillSmoothCircle(cx - 36, hy, 5, BOT_ACCENT, headBg);
  faceSprite.fillSmoothCircle(cx + 36, hy, 5, BOT_ACCENT, headBg);
  faceSprite.fillSmoothRoundRect(cx - 34, hy - 24, 68, 48, 14, BOT_SHELL,
                                 headBg);
  faceSprite.fillSmoothRoundRect(cx - 27, hy - 17, 54, 34, 10, BOT_SCREEN,
                                 BOT_SHELL);

  // Screen face.
  if (!asleep && deadlineReached(now, buddyNextBlink)) {
    buddyBlinkUntil = now + 120;
    buddyNextBlink = now + random(2000, 5000);
  }
  const int look = asleep ? 0 : static_cast<int>(sinf(t * 0.6f) * 5.0f);
  const uint16_t glow = asleep ? blendColor(BOT_GLOW, BOT_SCREEN, 90)
                               : BOT_GLOW;
  for (int side = -1; side <= 1; side += 2) {
    const int ex = cx + side * 12 + look;
    const int ey = hy - 3;
    if (asleep || !deadlineReached(now, buddyBlinkUntil)) {
      faceSprite.drawWideLine(ex - 5, ey + 2, ex + 5, ey + 2, 2.5f, glow,
                              BOT_SCREEN);
    } else if (waving) {
      faceSprite.drawWideLine(ex - 5, ey + 3, ex, ey - 3, 2.5f, glow,
                              BOT_SCREEN);
      faceSprite.drawWideLine(ex, ey - 3, ex + 5, ey + 3, 2.5f, glow,
                              BOT_SCREEN);
    } else {
      faceSprite.fillSmoothRoundRect(ex - 5, ey - 7, 10, 14, 4, glow,
                                     BOT_SCREEN);
      faceSprite.fillSmoothRoundRect(ex - 2, ey - 5, 3, 4, 1,
                                     rgb565(220, 252, 255), glow);
    }
  }
  if (waving) {
    faceSprite.drawArc(cx + look, hy + 6, 6, 4, 300, 360, glow, BOT_SCREEN,
                       true);
    faceSprite.drawArc(cx + look, hy + 6, 6, 4, 0, 60, glow, BOT_SCREEN, true);
    // A little heart floats up.
    const float rise = min(1.0f, since / 1600.0f);
    const int hx = cx + 30;
    const int hyHeart = hy - 30 - static_cast<int>(rise * 18.0f);
    const uint16_t heartBg = backdropAt(BUDDY_Y + hyHeart);
    const float fade = rise < 0.6f ? 1.0f : 1.0f - (rise - 0.6f) / 0.4f;
    const uint16_t heart = blendColor(rgb565(255, 96, 128), heartBg,
                                      static_cast<uint8_t>(fade * 255));
    faceSprite.fillSmoothCircle(hx - 3, hyHeart, 3, heart, heartBg);
    faceSprite.fillSmoothCircle(hx + 3, hyHeart, 3, heart, heartBg);
    faceSprite.fillTriangle(hx - 6, hyHeart + 1, hx + 6, hyHeart + 1, hx,
                            hyHeart + 8, heart);
  } else {
    faceSprite.drawWideLine(cx - 4 + look, hy + 9, cx + 4 + look, hy + 9, 2,
                            glow, BOT_SCREEN);
  }

  if (asleep) {
    useFont(faceSprite, 2);
    faceSprite.setTextDatum(MC_DATUM);
    for (int k = 0; k < 3; ++k) {
      const float phase = fmodf(t * 0.3f + k / 3.0f, 1.0f);
      const int zx = cx + 40 + static_cast<int>(phase * 14.0f);
      const int zy = hy - 22 - static_cast<int>(phase * 26.0f);
      faceSprite.setTextColor(blendColor(
          BOT_GLOW, backdropAt(BUDDY_Y + zy),
          static_cast<uint8_t>(sinf(PI * phase) * 230.0f)));
      faceSprite.drawString("z", zx, zy);
    }
  }
  faceSprite.pushSprite(BUDDY_X, BUDDY_Y);
}

void drawBuddyCharacter(uint32_t now, bool asleep) {
  switch (buddyKind) {
    case BUDDY_CAT:
      drawCatCharacter(now, asleep);
      break;
    case BUDDY_ROBOT:
      drawRobotCharacter(now, asleep);
      break;
    default:
      drawMochiCharacter(now, asleep);
      break;
  }
}

const char* buddyGreeting(const struct tm& localTime) {
  static const char* const lines[3][4] = {
      {"Shh... sleeping", "Good morning", "Good afternoon", "Good evening"},
      {"Zzz... purr", "Meow, morning!", "Nap o'clock", "Purr-fect evening"},
      {"Low power mode", "Booting: morning", "All systems go", "Evening mode"}};
  const int hour = localTime.tm_hour;
  const int slot = buddyAsleep(localTime) ? 0 : (hour < 12 ? 1 : (hour < 17 ? 2 : 3));
  return lines[min<int>(buddyKind, 2)][slot];
}

void drawBuddyText(const struct tm& localTime, bool synced) {
  drawBackdrop(BUDDY_PANEL_X - 2, 34, SCREEN_W - BUDDY_PANEL_X, 104);
  tft.setTextDatum(TL_DATUM);
  useFreeFont(tft, &FreeSans9pt7b);
  tft.setTextColor(MUTED_COLOR);
  tft.drawString(synced ? buddyGreeting(localTime) : "Waking up...",
                 BUDDY_PANEL_X, 42);
  char timeText[6] = "--:--";
  char dateText[24] = "";
  if (synced) {
    strftime(timeText, sizeof(timeText), "%H:%M", &localTime);
    strftime(dateText, sizeof(dateText), "%a, %d %b", &localTime);
  }
  useFreeFont(tft, &FreeSansBold24pt7b);
  tft.setTextColor(TEXT_COLOR);
  tft.drawString(timeText, BUDDY_PANEL_X - 2, 64);
  useFreeFont(tft, &FreeSansBold9pt7b);
  tft.setTextColor(blendColor(buddyAccent, TEXT_COLOR, 150));
  tft.drawString(dateText, BUDDY_PANEL_X, 114);
}

void drawBuddyPill(int x, int index) {
  constexpr int pillY = 150;
  constexpr int pillW = 72;
  constexpr int pillH = 30;
  const uint16_t outside = backdropAt(pillY + pillH / 2);
  const uint16_t pill = blendColor(TEXT_COLOR, outside, 24);
  tft.fillSmoothRoundRect(x, pillY, pillW, pillH, pillH / 2, pill, outside);
  useFreeFont(tft, &FreeSansBold9pt7b);
  if (index == 0) {
    drawThermometerIcon(x + 15, pillY + 15, TEMP_COLOR, pill);
    drawTemperatureText(climateAvailable ? temperatureValue : "--", x + 28,
                        pillY + 8, TEXT_COLOR, pill, climateAvailable);
  } else {
    drawDropletIcon(x + 15, pillY + 16, HUMIDITY_COLOR, pill);
    tft.setTextDatum(TL_DATUM);
    tft.setTextColor(TEXT_COLOR);
    tft.drawString(climateAvailable ? humidityValue + "%" : "--", x + 28,
                   pillY + 8);
  }
}

bool enterBuddyFace() {
  if (!allocateFaceSprite(BUDDY_SIZE, BUDDY_SIZE)) {
    return false;
  }
  switch (buddyKind) {
    case BUDDY_CAT:
      backdropTop = rgb565(70, 36, 52);
      backdropBottom = rgb565(20, 10, 18);
      buddyAccent = rgb565(255, 178, 112);
      catNextEar = millis() + 2500;
      break;
    case BUDDY_ROBOT:
      backdropTop = rgb565(14, 48, 70);
      backdropBottom = rgb565(4, 10, 20);
      buddyAccent = rgb565(96, 236, 255);
      break;
    default:
      backdropTop = BUDDY_TOP;
      backdropBottom = BUDDY_BOTTOM;
      buddyAccent = MOCHI_BLUSH;
      break;
  }
  drawBackdrop(0, 0, SCREEN_W, SCREEN_H);
  faceRenderedTime = "";
  faceRenderedClimate = "";
  buddyHopAt = millis();  // say hello
  buddyNextBlink = millis() + 1800;
  drawPageDots(backdropAt(233));
  return true;
}

void updateBuddyFace(uint32_t nowMs) {
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  char key[8] = "sync";
  if (synced) {
    strftime(key, sizeof(key), "%H:%M", &localTime);
  }
  if (faceRenderedTime != key) {
    if (faceRenderedTime.length() > 0 && synced) {
      buddyHopAt = nowMs;  // celebrate each new minute
    }
    faceRenderedTime = key;
    drawBuddyText(localTime, synced);
  }
  const String climate = climateKey();
  if (faceRenderedClimate != climate) {
    faceRenderedClimate = climate;
    drawBuddyPill(BUDDY_PANEL_X - 2, 0);
    drawBuddyPill(BUDDY_PANEL_X + 76, 1);
  }
  drawBuddyCharacter(nowMs, synced && buddyAsleep(localTime));
}

// ---- Sky: a living lakeside landscape. The background is generated per
// pixel from the time of day, so any rectangle can be redrawn on its own
// through one small strip sprite. The sky follows the clock with a real moon
// phase, sun glow, stars and the Milky Way; snowy mountains and pine hills
// are mirrored in a rippling lake with glitter; a windmill turns, chimney
// smoke rises, a sailboat drifts, birds cross by day and fireflies glow at
// night. ------------------------------------------------------------------
constexpr int SKY_STRIP_H = 16;
constexpr int SKY_LAKE_TOP = 170;
constexpr int SKY_BAND_Y0 = 76;  // clouds and birds stay inside this band
constexpr int SKY_BAND_Y1 = 124;
constexpr int SKY_LAKE_Y0 = 160;  // lake redraw area, including the boat
constexpr int SKY_LAKE_Y1 = 222;
constexpr int SKY_MILL_X = 46;
constexpr int SKY_MILL_ARM = 22;
constexpr int SKY_HOUSE_X = 264;
constexpr int SKY_TREE_X = 234;
constexpr int SKY_LAMP_X = 213;
constexpr int SKY_STARS = 56;
constexpr int SKY_CLOUDS = 4;
constexpr int SKY_PUFFS = 7;
constexpr int SKY_FLIES = 7;
constexpr int SKY_BIRDS = 5;
constexpr float SKY_SMOKE_LIFE = 5.2f;

struct SkyRgb {
  int16_t r;
  int16_t g;
  int16_t b;
};

SkyRgb skyMix(const SkyRgb& a, const SkyRgb& b, float t) {
  t = constrain(t, 0.0f, 1.0f);
  return {static_cast<int16_t>(a.r + (b.r - a.r) * t),
          static_cast<int16_t>(a.g + (b.g - a.g) * t),
          static_cast<int16_t>(a.b + (b.b - a.b) * t)};
}

// Golden-hour light: warmer and a little brighter.
SkyRgb skyWarm(const SkyRgb& c, float amount) {
  const SkyRgb warm = {static_cast<int16_t>(min(255, c.r * 5 / 4 + 40)),
                       static_cast<int16_t>(min(255, c.g + 8)),
                       static_cast<int16_t>(c.b * 3 / 4)};
  return skyMix(c, warm, amount);
}

uint16_t sky565(const SkyRgb& c) {
  return rgb565(constrain(c.r, 0, 255), constrain(c.g, 0, 255),
                constrain(c.b, 0, 255));
}

struct SkyKey {
  float hour;
  SkyRgb top;
  SkyRgb mid;
  SkyRgb horizon;
};
const SkyKey SKY_KEYS[] = {
    {0.0f, {4, 6, 20}, {10, 12, 36}, {26, 24, 58}},
    {4.6f, {6, 8, 26}, {16, 18, 48}, {40, 32, 76}},
    {5.6f, {26, 30, 84}, {84, 70, 138}, {224, 132, 112}},
    {6.4f, {52, 92, 180}, {160, 150, 196}, {255, 186, 130}},
    {7.6f, {40, 112, 222}, {104, 168, 240}, {198, 226, 250}},
    {16.4f, {42, 110, 218}, {104, 164, 236}, {206, 222, 242}},
    {17.6f, {56, 96, 196}, {170, 158, 188}, {255, 196, 130}},
    {18.4f, {44, 44, 124}, {178, 98, 138}, {255, 128, 76}},
    {19.2f, {18, 20, 64}, {56, 40, 100}, {132, 66, 100}},
    {20.2f, {6, 8, 26}, {14, 16, 44}, {32, 28, 68}},
    {24.0f, {4, 6, 20}, {10, 12, 36}, {26, 24, 58}},
};

const uint8_t SKY_BAYER[4][4] = {
    {0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

struct SkyBox {
  int16_t x0 = 0;
  int16_t y0 = 0;
  int16_t x1 = 0;  // exclusive
  int16_t y1 = 0;
  bool valid = false;
};

struct SkyStar {
  int16_t x;
  int16_t y;
  float speed;
  float phase;
  bool big;
  bool free;  // outside the text boxes, so it may twinkle on its own
};

struct SkyCloud {
  float x;
  int16_t y;
  int16_t w;
  int16_t h;
  float speed;
  float opacity;
  uint8_t count;
  int8_t px[6];
  int8_t py[6];
  uint8_t pr[6];
  SkyBox drawn;
};

struct SkyFly {
  float x;
  float y;
  float vx;
  float vy;
  float phase;
  float rate;
  SkyBox drawn;
};

// Terrain, built once.
uint8_t skyMount[SCREEN_W];
uint8_t skyHill[SCREEN_W];
uint8_t skyForest[SCREEN_W];
uint8_t skyGround[SCREEN_W];
uint8_t skyBlade[SCREEN_W];
int8_t skyTreeSide[SCREEN_W];
int8_t skyWave[32];
SkyStar skyStars[SKY_STARS];
bool skyTablesReady = false;

// Lighting, recomputed every minute.
SkyRgb skyRows[SKY_LAKE_TOP];
uint8_t skyHazeCol[SCREEN_W];
uint8_t skyHazeRow[SKY_LAKE_TOP];
uint8_t skyGlowLut[256];
SkyRgb skyGlowColor;
SkyRgb cMount, cSnow, cHillTop, cHillLow, cTree, cTreeLit, cWater;
SkyRgb cGrassTop, cGrassLow, cSand, cHaze;
const SkyRgb SKY_WARM = {255, 150, 90};
float skyDay = 1.0f;
float skyNight = 0.0f;
float skyGold = 0.0f;
float skyHourNow = 12.0f;
bool skySunUp = true;
int skyBodyX = 160;
int skyBodyY = 60;
float skyMoonPhase = 0.5f;
float skyMoonLight = 1.0f;
float skyGlitter = 0.0f;
SkyRgb skyGlitterColor;
uint16_t skyShadow565 = 0;
char skyTimeText[6] = "--:--";
char skyDateText[24] = "Syncing time";
const char* skyPhrase = "";

// Animation state.
float skyT = 0.0f;
uint32_t skyLastFrameAt = 0;
uint32_t skyFrame = 0;
int skyWavePhase = 0;
float skyMillAngle = 0.0f;
SkyBox skyMillBox;
SkyCloud skyClouds[SKY_CLOUDS];
SkyFly skyFlies[SKY_FLIES];
float skySmokeAge[SKY_PUFFS];
float skyBoatX = 150.0f;
float skyBoatDir = 1.0f;
bool skyBirdsActive = false;
float skyBirdsX = 0.0f;
float skyBirdsY = 96.0f;
float skyBirdsSpeed = 18.0f;
float skyBirdsFlap = 0.0f;
uint32_t skyBirdsNextAt = 0;
SkyBox skyBirdsDrawn;
bool skyShooting = false;
uint32_t skyNextShootAt = 0;
uint32_t skyShootStartedAt = 0;
int skyShootX = 0;
int skyShootY = 0;
SkyBox skyShootDrawn;

// The strip currently being rendered.
int skyOx = 0;
int skyOy = 0;
int skyOw = 0;
int skyOh = 0;
uint16_t* skyBuf = nullptr;

inline uint32_t skyHash(int x, int y, int z = 0) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u +
               static_cast<uint32_t>(y) * 668265263u +
               static_cast<uint32_t>(z) * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return h ^ (h >> 16);
}

// Smooth value noise (0-255): bilinear between hashed grid corners.
inline int skyNoise(int x, int y, int cell, int seed) {
  const int gx = x / cell;
  const int gy = y / cell;
  const int fx = x % cell;
  const int fy = y % cell;
  const int a = skyHash(gx, gy, seed) & 255;
  const int b = skyHash(gx + 1, gy, seed) & 255;
  const int c = skyHash(gx, gy + 1, seed) & 255;
  const int d = skyHash(gx + 1, gy + 1, seed) & 255;
  const int top = a + (b - a) * fx / cell;
  const int bottom = c + (d - c) * fx / cell;
  return top + (bottom - top) * fy / cell;
}

inline void skyLerpTo(int& r, int& g, int& b, const SkyRgb& c, int a) {
  r += ((c.r - r) * a) >> 8;
  g += ((c.g - g) * a) >> 8;
  b += ((c.b - b) * a) >> 8;
}

inline uint16_t skyPack(int r, int g, int b, int x, int y) {
  const int d = SKY_BAYER[y & 3][x & 3];
  r = constrain((r + (d >> 1)) >> 3, 0, 31);
  g = constrain((g + (d >> 2)) >> 2, 0, 63);
  b = constrain((b + (d >> 1)) >> 3, 0, 31);
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

float skySmooth(float edge0, float edge1, float x) {
  const float t = constrain((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

bool skyInTextBox(int x, int y) {
  return (x < 160 && y < 74) || (x > 178 && y < 76);
}

void skySpawnCloud(SkyCloud& cloud, bool anywhere) {
  cloud.count = static_cast<uint8_t>(random(4, 7));
  int maxR = 0;
  for (int i = 0; i < cloud.count; ++i) {
    const bool middle = i > 0 && i < cloud.count - 1;
    const int r = constrain(static_cast<int>(random(4, 7)) +
                                (middle ? static_cast<int>(random(1, 5)) : 0),
                            4, 10);
    cloud.pr[i] = static_cast<uint8_t>(r);
    maxR = max(maxR, r);
  }
  cloud.h = static_cast<int16_t>(maxR * 2 - 1);
  // Puffs overlap so the cloud reads as one shape.
  int x = cloud.pr[0];
  for (int i = 0; i < cloud.count; ++i) {
    const int r = cloud.pr[i];
    if (i > 0) {
      x += (cloud.pr[i - 1] + r) * 11 / 20;
    }
    cloud.px[i] = static_cast<int8_t>(x);
    cloud.py[i] = static_cast<int8_t>(cloud.h - r + 2 -
                                      static_cast<int>(random(0, 3)));
  }
  cloud.w = static_cast<int16_t>(x + cloud.pr[cloud.count - 1] + 1);
  cloud.y = static_cast<int16_t>(
      random(SKY_BAND_Y0 + 2, SKY_BAND_Y1 - cloud.h - 1));
  cloud.speed = random(25, 75) / 10.0f;
  cloud.opacity = 0.62f + cloud.speed / 20.0f;  // nearer clouds are denser
  cloud.x = anywhere ? static_cast<float>(random(-20, SCREEN_W - 30))
                     : -cloud.w - static_cast<float>(random(0, 60));
}

void buildSkyTables() {
  if (skyTablesReady) {
    return;
  }
  const auto peak = [](float u) { return 1.0f - fabsf(sinf(u)); };
  const auto bump = [](float x, float centre, float width) {
    const float d = (x - centre) / width;
    return expf(-d * d);
  };
  for (int x = 0; x < SCREEN_W; ++x) {
    skyMount[x] = static_cast<uint8_t>(
        141 - 22 * peak(x * 0.019f + 0.4f) - 12 * peak(x * 0.043f + 1.7f) -
        4 * peak(x * 0.13f + 0.3f));
    skyHill[x] = static_cast<uint8_t>(158 + 6 * sinf(x * 0.016f + 2.2f) +
                                      4 * sinf(x * 0.041f + 0.5f));
    skyGround[x] = static_cast<uint8_t>(
        214 + 3 * sinf(x * 0.035f) - 22 * bump(x, SKY_MILL_X, 34) -
        26 * bump(x, SKY_HOUSE_X, 50));
    skyForest[x] = skyHill[x];
    skyTreeSide[x] = 0;
    const uint32_t h = skyHash(x, 1, 77);
    skyBlade[x] = (h & 3) == 0 ? static_cast<uint8_t>(1 + (h >> 4) % 3) : 0;
  }
  for (int i = 0; i < 32; ++i) {
    skyWave[i] = static_cast<int8_t>(lroundf(2.0f * sinf(i * 2 * PI / 32)));
  }

  randomSeed(4321);
  for (int cx = 3; cx < SCREEN_W; cx += random(6, 14)) {
    if (random(0, 10) < 2) {
      continue;  // a clearing
    }
    const int height = random(7, 16);
    const float half = height * 0.36f + 1.0f;
    const int base = skyHill[cx] + 2;
    for (int dx = -static_cast<int>(half); dx <= static_cast<int>(half);
         ++dx) {
      const int x = cx + dx;
      if (x < 0 || x >= SCREEN_W) {
        continue;
      }
      const float t = fabsf(static_cast<float>(dx)) / half;
      int top = base - static_cast<int>(height * (1.0f - t));
      // Little tiers give the pines a layered silhouette.
      if (dx != 0 && ((base - top) % 4) == 3) {
        ++top;
      }
      if (top < skyForest[x]) {
        skyForest[x] = static_cast<uint8_t>(top);
        skyTreeSide[x] = static_cast<int8_t>(dx < 0 ? -1 : (dx > 0 ? 1 : 0));
      }
    }
  }

  int placed = 0;
  while (placed < SKY_STARS) {
    const int x = random(3, SCREEN_W - 3);
    const int y = random(3, 138);
    if (y > skyMount[x] - 6) {
      continue;
    }
    SkyStar& star = skyStars[placed];
    star.x = static_cast<int16_t>(x);
    star.y = static_cast<int16_t>(y);
    star.speed = random(8, 32) / 10.0f;
    star.phase = random(0, 628) / 100.0f;
    star.big = placed % 6 == 0;
    star.free = !skyInTextBox(x, y);
    ++placed;
  }
  randomSeed(esp_random());
  skyTablesReady = true;
}

float localHour(const struct tm& localTime) {
  return localTime.tm_hour + localTime.tm_min / 60.0f;
}

const char* skyMoonName(float phase) {
  static const char* names[] = {"New moon",       "Waxing crescent",
                                "First quarter",  "Waxing gibbous",
                                "Full moon",      "Waning gibbous",
                                "Last quarter",   "Waning crescent"};
  return names[static_cast<int>(phase * 8.0f + 0.5f) % 8];
}

void skyPrepare(const struct tm& localTime, const struct timeval& now,
                bool synced) {
  buildSkyTables();
  const float hour = synced ? localTime.tm_hour + localTime.tm_min / 60.0f +
                                  localTime.tm_sec / 3600.0f
                            : 12.0f;
  skyHourNow = hour;

  int index = 0;
  const int keyCount = sizeof(SKY_KEYS) / sizeof(SKY_KEYS[0]);
  while (index < keyCount - 2 && hour >= SKY_KEYS[index + 1].hour) {
    ++index;
  }
  const SkyKey& a = SKY_KEYS[index];
  const SkyKey& b = SKY_KEYS[index + 1];
  const float t = (hour - a.hour) / (b.hour - a.hour);
  const SkyRgb top = skyMix(a.top, b.top, t);
  const SkyRgb mid = skyMix(a.mid, b.mid, t);
  const SkyRgb horizon = skyMix(a.horizon, b.horizon, t);
  for (int y = 0; y < SKY_LAKE_TOP; ++y) {
    skyRows[y] = y < 72 ? skyMix(top, mid, y / 72.0f)
                        : skyMix(mid, horizon, (y - 72) / 76.0f);
  }
  cHaze = horizon;

  skyDay = skySmooth(5.4f, 7.0f, hour) * (1.0f - skySmooth(17.6f, 19.4f, hour));
  skyNight = constrain(1.0f - skyDay * 1.6f, 0.0f, 1.0f);
  const auto tri = [](float x, float centre, float width) {
    return max(0.0f, 1.0f - fabsf(x - centre) / width);
  };
  skyGold = min(1.0f, tri(hour, 6.5f, 1.2f) + tri(hour, 17.9f, 1.2f));

  skySunUp = hour >= 6.0f && hour < 18.5f;
  const float progress = skySunUp
      ? (hour - 6.0f) / 12.5f
      : (hour >= 18.5f ? hour - 18.5f : hour + 5.5f) / 11.5f;
  const float altitude = sinf(PI * progress);
  skyBodyX = 160 - static_cast<int>(cosf(PI * progress) * 150.0f);
  skyBodyY = 164 - static_cast<int>(altitude * 104.0f);

  if (synced) {
    const double days = (now.tv_sec - 947182440.0) / 86400.0;
    skyMoonPhase = static_cast<float>(fmod(days, 29.530588853) / 29.530588853);
  } else {
    skyMoonPhase = 0.5f;
  }
  skyMoonLight = 0.5f - 0.5f * cosf(2 * PI * skyMoonPhase);

  float strength;
  if (skySunUp) {
    strength = 0.5f + 0.4f * (1.0f - altitude);
    skyGlowColor = skyMix({255, 250, 228}, {255, 168, 96}, 1.0f - altitude);
    skyGlitter = 0.55f + 0.45f * skyGold;
    skyGlitterColor = skyMix({255, 252, 236}, {255, 190, 110}, skyGold);
  } else {
    strength = 0.12f + 0.3f * skyMoonLight;
    skyGlowColor = {190, 205, 255};
    skyGlitter = 0.15f + 0.55f * skyMoonLight;
    skyGlitterColor = {214, 222, 255};
  }
  const float k = 0.11f;
  const float floorValue = 1.0f / (1.0f + 255 * k);
  for (int i = 0; i < 256; ++i) {
    const float v = (1.0f / (1.0f + i * k) - floorValue) / (1.0f - floorValue);
    skyGlowLut[i] = static_cast<uint8_t>(v * strength * 255);
  }
  for (int x = 0; x < SCREEN_W; ++x) {
    const float d = (x - skyBodyX) / 110.0f;
    skyHazeCol[x] = static_cast<uint8_t>(
        skySunUp ? skyGold * expf(-d * d) * 200 : 0);
  }
  for (int y = 0; y < SKY_LAKE_TOP; ++y) {
    skyHazeRow[y] = static_cast<uint8_t>(skySmooth(70.0f, 150.0f, y) * 255);
  }

  const float day = skyDay;
  const float warm = skyGold * 0.45f;
  cMount = skyWarm(skyMix({30, 34, 66}, {116, 134, 178}, day), warm);
  cSnow = skyWarm(skyMix({100, 108, 156}, {246, 248, 255}, day), warm);
  cHillTop = skyWarm(skyMix({16, 30, 42}, {92, 152, 94}, day), warm);
  cHillLow = skyMix({10, 20, 30}, {52, 110, 72}, day);
  cTree = skyMix({8, 18, 28}, {28, 80, 58}, day);
  cTreeLit = skyWarm(skyMix({14, 28, 40}, {58, 120, 72}, day), warm);
  cWater = skyMix({6, 10, 28}, {34, 84, 128}, day);
  cGrassTop = skyWarm(skyMix({14, 32, 32}, {112, 172, 80}, day), warm);
  cGrassLow = skyMix({6, 14, 18}, {50, 112, 56}, day);
  cSand = skyWarm(skyMix({36, 36, 48}, {216, 198, 150}, day), warm);
  skyShadow565 = blendColor(TFT_BLACK, sky565(skyRows[30]), 110);

  if (synced) {
    strftime(skyTimeText, sizeof(skyTimeText), "%H:%M", &localTime);
    strftime(skyDateText, sizeof(skyDateText), "%a, %d %b", &localTime);
  } else {
    strcpy(skyTimeText, "--:--");
    strcpy(skyDateText, "Syncing time");
  }
  if (!synced) {
    skyPhrase = "";
  } else if (hour >= 5.0f && hour < 6.2f) {
    skyPhrase = "Dawn";
  } else if (hour >= 6.2f && hour < 7.2f) {
    skyPhrase = "Sunrise";
  } else if (hour >= 7.2f && hour < 11.5f) {
    skyPhrase = "Morning";
  } else if (hour >= 11.5f && hour < 16.6f) {
    skyPhrase = "Afternoon";
  } else if (hour >= 16.6f && hour < 17.8f) {
    skyPhrase = "Golden hour";
  } else if (hour >= 17.8f && hour < 18.8f) {
    skyPhrase = "Sunset";
  } else if (hour >= 18.8f && hour < 19.6f) {
    skyPhrase = "Dusk";
  } else {
    skyPhrase = skyMoonName(skyMoonPhase);
  }
}

// ---- per-pixel background --------------------------------------------------
void skySkyPixel(int x, int y, int& r, int& g, int& b) {
  const SkyRgb& row = skyRows[y];
  r = row.r;
  g = row.g;
  b = row.b;
  const int haze = (skyHazeCol[x] * skyHazeRow[y]) >> 8;
  if (haze > 0) {
    skyLerpTo(r, g, b, SKY_WARM, haze);
  }
  if (skyNight > 0.05f) {
    // The Milky Way: a mottled diagonal band.
    const int lineY = 120 - (x * 34) / 100;
    const int dist = abs(y - lineY);
    if (dist < 26) {
      const int base = (26 - dist) * (26 - dist) * 256 / 676;
      const int noise = (skyNoise(x, y, 18, 7) * 2 + skyNoise(x, y, 7, 9)) / 3;
      const int amount = static_cast<int>(base * noise / 256 * skyNight * 0.32f);
      skyLerpTo(r, g, b, {176, 176, 222}, amount);
    }
    // Faint background stars.
    const uint32_t h = skyHash(x, y, 1);
    if (h % 700 == 0) {
      const int amount = static_cast<int>((70 + (h >> 24) % 120) * skyNight);
      skyLerpTo(r, g, b, {226, 230, 255}, amount);
    }
  }
  const int dx = x - skyBodyX;
  const int dy = y - skyBodyY;
  const int d2 = dx * dx + dy * dy;
  const int glowIndex = d2 >> 5;
  if (glowIndex < 256 && skyGlowLut[glowIndex] > 0) {
    skyLerpTo(r, g, b, skyGlowColor, skyGlowLut[glowIndex]);
  }
  if (skySunUp) {
    constexpr int R = 11;
    if (d2 < (R + 1) * (R + 1)) {
      const float d = sqrtf(static_cast<float>(d2));
      const float cover = constrain(R + 0.5f - d, 0.0f, 1.0f);
      const float limb = d / R;
      const SkyRgb core = skyMix({255, 250, 232}, {255, 176, 96},
                                 max(skyGold, limb * limb * 0.5f));
      skyLerpTo(r, g, b, core, static_cast<int>(cover * 256));
    }
  } else {
    constexpr int R = 9;
    if (d2 < (R + 1) * (R + 1)) {
      const float d = sqrtf(static_cast<float>(d2));
      const float cover = constrain(R + 0.5f - d, 0.0f, 1.0f);
      const float nx = dx / static_cast<float>(R);
      const float ny = dy / static_cast<float>(R);
      const float span = sqrtf(max(0.0f, 1.0f - ny * ny));
      float lit;
      if (skyMoonPhase < 0.5f) {  // waxing: lit from the right
        const float edge = cosf(2 * PI * skyMoonPhase) * span;
        lit = constrain((nx - edge) * R * 0.8f + 0.5f, 0.0f, 1.0f);
      } else {
        const float edge = cosf(2 * PI * (skyMoonPhase - 0.5f)) * span;
        lit = constrain((edge - nx) * R * 0.8f + 0.5f, 0.0f, 1.0f);
      }
      SkyRgb surface = {240, 238, 224};
      const auto crater = [&](float cx, float cy, float cr) {
        return (nx - cx) * (nx - cx) + (ny - cy) * (ny - cy) < cr * cr;
      };
      if (crater(-0.35f, -0.3f, 0.24f) || crater(0.3f, 0.22f, 0.3f) ||
          crater(-0.1f, 0.5f, 0.16f)) {
        surface = {206, 204, 194};
      }
      const SkyRgb shine = {52, 56, 84};
      const SkyRgb moon = skyMix(shine, surface, lit);
      skyLerpTo(r, g, b, moon, static_cast<int>(cover * 256));
    }
  }
}

// Lights a slope by how steeply it faces the sun or moon. A wide sample
// keeps neighbouring columns from flickering between lit and shaded.
inline int skySlopeLight(const uint8_t* table, int x, int amount) {
  const int slope = table[min(x + 4, SCREEN_W - 1)] - table[max(x - 4, 0)];
  const int toward = skyBodyX > x ? 1 : -1;  // >0 slope faces right
  return constrain(slope * toward * amount / 8, -amount * 3 / 4, amount);
}

void skyFarPixel(int x, int y, int& r, int& g, int& b) {
  const float light = 0.25f + 0.75f * skyDay;
  if (y >= skyHill[x]) {
    const int depth = y - skyHill[x];
    const SkyRgb c = skyMix(cHillTop, cHillLow, depth / 20.0f);
    const int shade = static_cast<int>(skySlopeLight(skyHill, x, 10) * light);
    const int noise = static_cast<int>(skyHash(x, y, 4) & 7) - 4;
    r = c.r + shade + noise;
    g = c.g + shade + noise * 2;
    b = c.b + shade / 2 + noise;
    skyLerpTo(r, g, b, cHaze, 30);
  } else if (y >= skyForest[x]) {
    const int side = skyTreeSide[x];
    const bool lit = side != 0 && ((side > 0) == (skyBodyX > x));
    const SkyRgb& c = lit ? cTreeLit : cTree;
    r = c.r;
    g = c.g;
    b = c.b;
    skyLerpTo(r, g, b, cHaze, 26);
  } else if (y >= skyMount[x]) {
    const int top = skyMount[x];
    const int depth = y - top;
    const bool snow = top < 122 &&
                      depth < (122 - top) / 2 + 2 + static_cast<int>(skyHash(x, 2) & 1);
    const SkyRgb& c = snow ? cSnow : cMount;
    const int shade = static_cast<int>(skySlopeLight(skyMount, x, snow ? 16 : 13) * light);
    r = c.r + shade;
    g = c.g + shade;
    b = c.b + shade;
    skyLerpTo(r, g, b, cHaze, constrain(60 + depth * 4, 0, 190));
  } else {
    skySkyPixel(x, y, r, g, b);
  }
}

uint16_t skyBasePixel(int x, int y) {
  int r;
  int g;
  int b;
  const int ground = skyGround[x];
  if (y >= ground) {
    const int depth = y - ground;
    const SkyRgb c = skyMix(cGrassTop, cGrassLow, depth / 32.0f);
    const uint32_t h = skyHash(x, y, 5);
    const int noise = static_cast<int>(h & 15) - 8;
    r = c.r + noise;
    g = c.g + noise * 3 / 2;
    b = c.b + noise / 2;
    if (depth < 2) {
      skyLerpTo(r, g, b, cSand, depth == 0 ? 170 : 90);
    } else if (depth > 4 && (h >> 8) % 149 == 0) {
      static const SkyRgb flowers[] = {
          {255, 122, 152}, {255, 222, 92}, {250, 250, 255}, {176, 146, 255}};
      skyLerpTo(r, g, b, flowers[(h >> 20) & 3],
                static_cast<int>(70 + 170 * skyDay));
    }
  } else if (y >= SKY_LAKE_TOP && y >= ground - skyBlade[x]) {
    r = cGrassTop.r * 7 / 8;
    g = cGrassTop.g * 7 / 8;
    b = cGrassTop.b * 7 / 8;
  } else if (y >= SKY_LAKE_TOP) {
    const int dy = y - SKY_LAKE_TOP;
    const int amp = 1 + dy / 14;
    const int offset = (skyWave[(dy * 5 + skyWavePhase) & 31] * amp) >> 1;
    const int sx = constrain(x + offset, 0, SCREEN_W - 1);
    const int sy = max(0, SKY_LAKE_TOP - 1 - dy - dy / 3);
    skyFarPixel(sx, sy, r, g, b);
    skyLerpTo(r, g, b, cWater, constrain(70 + dy * 3, 0, 190));
    if (dy == 0) {
      skyLerpTo(r, g, b, cHaze, 70);  // light along the far shore
    }
    const int drift = (dy & 1) ? skyWavePhase : -skyWavePhase;
    if ((skyHash((x + drift) >> 3, y, 11) & 31) == 0) {
      skyLerpTo(r, g, b, skyRows[40], 60);  // ripple highlights
    }
    if (skyGlitter > 0.0f) {
      const int dxg = abs(x - skyBodyX);
      const int halfWidth = 3 + dy * 7 / 10;
      if (dxg < halfWidth) {
        const uint32_t h = skyHash(x >> 1, y, skyWavePhase * 13);
        const int spark = (h & 7) == 0 ? 255 : ((h & 7) == 1 ? 110 : 0);
        if (spark > 0) {
          skyLerpTo(r, g, b, skyGlitterColor,
                    static_cast<int>(spark * (halfWidth - dxg) / halfWidth *
                                     skyGlitter));
        }
      }
    }
    if (ground - y <= 1) {
      skyLerpTo(r, g, b, {230, 236, 240},
                static_cast<int>(40 + 60 * skyDay));  // foam on the shore
    }
  } else {
    skyFarPixel(x, y, r, g, b);
  }
  return skyPack(r, g, b, x, y);
}

// ---- drawing helpers (screen coordinates, clipped to the strip) -----------
inline int SX(int x) { return x - skyOx; }
inline int SY(int y) { return y - skyOy; }
inline float SXf(float x) { return x - skyOx; }
inline float SYf(float y) { return y - skyOy; }

bool skyHits(int x0, int y0, int x1, int y1) {
  return x0 < skyOx + skyOw && x1 > skyOx && y0 < skyOy + skyOh && y1 > skyOy;
}

inline void skyBlendPixel(int x, int y, uint16_t color, int alpha) {
  if (alpha <= 0 || x < skyOx || x >= skyOx + skyOw || y < skyOy ||
      y >= skyOy + skyOh) {
    return;
  }
  uint16_t& pixel = skyBuf[(y - skyOy) * SCREEN_W + (x - skyOx)];
  pixel = swap16(blendColor(color, swap16(pixel),
                            static_cast<uint8_t>(min(alpha, 255))));
}

void skySoftDot(float cx, float cy, float radius, uint16_t color,
                float strength, bool flat = false) {
  if (strength <= 0.01f) {
    return;
  }
  const int x0 = max(static_cast<int>(floorf(cx - radius)), skyOx);
  const int x1 = min(static_cast<int>(ceilf(cx + radius)), skyOx + skyOw - 1);
  const int y0 = max(static_cast<int>(floorf(cy - radius)), skyOy);
  const int y1 = min(static_cast<int>(ceilf(cy + radius)), skyOy + skyOh - 1);
  const float inverse = 1.0f / (radius * radius);
  for (int y = y0; y <= y1; ++y) {
    const float fy = y - cy;
    for (int x = x0; x <= x1; ++x) {
      const float fx = x - cx;
      const float d = (fx * fx + fy * fy) * inverse;
      if (d >= 1.0f) {
        continue;
      }
      const float f = 1.0f - d;
      skyBlendPixel(x, y, color,
                    static_cast<int>(strength * (flat ? f : f * f) * 255));
    }
  }
}

// ---- scene objects ----------------------------------------------------------
void skyDrawStars() {
  if (skyNight <= 0.02f || !skyHits(0, 0, SCREEN_W, 140)) {
    return;
  }
  for (const SkyStar& star : skyStars) {
    const int dx = star.x - skyBodyX;
    const int dy = star.y - skyBodyY;
    if (dx * dx + dy * dy < 22 * 22) {
      continue;
    }
    const float twinkle = 0.5f + 0.5f * sinf(skyT * star.speed + star.phase);
    const int alpha = static_cast<int>(skyNight * (60.0f + twinkle * 195.0f));
    skyBlendPixel(star.x, star.y, TEXT_COLOR, alpha);
    if (star.big) {
      skyBlendPixel(star.x - 1, star.y, TEXT_COLOR, alpha / 3);
      skyBlendPixel(star.x + 1, star.y, TEXT_COLOR, alpha / 3);
      skyBlendPixel(star.x, star.y - 1, TEXT_COLOR, alpha / 3);
      skyBlendPixel(star.x, star.y + 1, TEXT_COLOR, alpha / 3);
    }
  }
}

constexpr uint32_t SKY_SHOOT_MS = 700;
constexpr int SKY_SHOOT_TRAVEL = 84;
constexpr int SKY_SHOOT_TRAIL = 28;

void skyShootHead(uint32_t nowMs, float& progress, int& hx, int& hy) {
  progress = (nowMs - skyShootStartedAt) / static_cast<float>(SKY_SHOOT_MS);
  hx = skyShootX - static_cast<int>(progress * SKY_SHOOT_TRAVEL * 0.85f);
  hy = skyShootY + static_cast<int>(progress * SKY_SHOOT_TRAVEL * 0.5f);
}

void skyDrawShootingStar() {
  if (!skyShooting) {
    return;
  }
  float progress;
  int hx;
  int hy;
  skyShootHead(millis(), progress, hx, hy);
  if (progress >= 1.0f) {
    return;
  }
  const float fade = sinf(PI * progress);
  for (int i = 0; i <= SKY_SHOOT_TRAIL; ++i) {
    const int alpha = static_cast<int>(
        fade * 255.0f * (1.0f - i / static_cast<float>(SKY_SHOOT_TRAIL)));
    skyBlendPixel(hx + i * 85 / 100, hy - i / 2, TEXT_COLOR, alpha);
  }
  skyBlendPixel(hx - 1, hy, TEXT_COLOR, static_cast<int>(fade * 120));
  skyBlendPixel(hx, hy + 1, TEXT_COLOR, static_cast<int>(fade * 120));
}

SkyBox skyCloudBox(const SkyCloud& cloud) {
  SkyBox box;
  box.x0 = static_cast<int16_t>(floorf(cloud.x));
  box.y0 = cloud.y;
  box.x1 = static_cast<int16_t>(box.x0 + cloud.w + 1);
  box.y1 = static_cast<int16_t>(cloud.y + cloud.h);
  box.valid = true;
  return box;
}

void skyDrawCloud(const SkyCloud& cloud) {
  const SkyBox box = skyCloudBox(cloud);
  if (!skyHits(box.x0, box.y0, box.x1, box.y1)) {
    return;
  }
  const SkyRgb lit = skyMix(skyWarm(skyMix({78, 82, 120}, {255, 255, 255},
                                           skyDay),
                                    skyGold * 0.8f),
                            {255, 196, 206}, skyGold * 0.35f);
  const SkyRgb shade = skyMix(skyMix({44, 46, 78}, {196, 208, 228}, skyDay),
                              {150, 110, 150}, skyGold * 0.6f);
  const float opacity = cloud.opacity * (0.7f + 0.3f * skyDay);
  const int ox = box.x0;
  const float fracX = cloud.x - ox;
  const int y0 = max<int>(box.y0, max(skyOy, SKY_BAND_Y0));
  const int y1 = min<int>(box.y1, min(skyOy + skyOh, SKY_BAND_Y1));
  const int x0 = max<int>(box.x0, skyOx);
  const int x1 = min<int>(box.x1, skyOx + skyOw);
  for (int y = y0; y < y1; ++y) {
    const float ly = y - cloud.y + 0.5f;
    const float v = ly / cloud.h;
    const SkyRgb c = skyMix(lit, shade, v * v * 1.2f);
    const uint16_t color = sky565(c);
    for (int x = x0; x < x1; ++x) {
      const float lx = x - ox - fracX + 0.5f;
      float cover = 0.0f;
      const int last = cloud.count - 1;
      if (ly >= cloud.h - 5 && lx >= cloud.px[0] && lx <= cloud.px[last]) {
        cover = 1.0f;  // flat base joining the puffs
      }
      for (int i = 0; i < cloud.count && cover < 1.0f; ++i) {
        const float dx = lx - cloud.px[i];
        const float dy = ly - cloud.py[i];
        const float d2 = dx * dx + dy * dy;
        const float r = cloud.pr[i];
        if (d2 <= (r - 1) * (r - 1)) {
          cover = 1.0f;
        } else if (d2 < (r + 1) * (r + 1)) {
          cover = max(cover, constrain(r + 0.5f - sqrtf(d2), 0.0f, 1.0f));
        }
      }
      if (cover > 0.0f) {
        if (y == box.y1 - 1) {
          cover *= 0.55f;  // soft flat base
        }
        skyBlendPixel(x, y, color, static_cast<int>(cover * opacity * 255));
      }
    }
  }
}

void skyBirdPos(int i, float& x, float& y, int& wing) {
  static const int8_t ox[SKY_BIRDS] = {0, 9, 10, 19, 20};
  static const int8_t oy[SKY_BIRDS] = {0, -5, 5, -9, 10};
  x = skyBirdsX + ox[i];
  y = skyBirdsY + oy[i] + sinf(skyT * 1.3f + i) * 1.2f;
  wing = static_cast<int>(lroundf(sinf(skyBirdsFlap + i * 0.8f) * 2.4f));
}

SkyBox skyBirdsBox() {
  SkyBox box;
  box.x0 = static_cast<int16_t>(skyBirdsX - 6);
  box.y0 = static_cast<int16_t>(skyBirdsY - 14);
  box.x1 = static_cast<int16_t>(skyBirdsX + 27);
  box.y1 = static_cast<int16_t>(skyBirdsY + 15);
  box.valid = skyBirdsActive;
  return box;
}

void skyDrawBirds() {
  if (!skyBirdsActive) {
    return;
  }
  const SkyBox box = skyBirdsBox();
  if (!skyHits(box.x0, box.y0, box.x1, box.y1)) {
    return;
  }
  const uint16_t color = sky565(skyMix({44, 48, 66}, {70, 36, 46}, skyGold));
  for (int i = 0; i < SKY_BIRDS; ++i) {
    float fx;
    float fy;
    int wing;
    skyBirdPos(i, fx, fy, wing);
    const int x = static_cast<int>(fx);
    const int y = static_cast<int>(fy);
    faceSprite.drawLine(SX(x - 4), SY(y - wing), SX(x), SY(y), color);
    faceSprite.drawLine(SX(x), SY(y), SX(x + 4), SY(y - wing), color);
    faceSprite.drawPixel(SX(x), SY(y + 1), color);
  }
}

void skyDrawMill() {
  const int cx = SKY_MILL_X;
  const int baseY = skyGround[cx] + 2;
  const int hubY = baseY - 34;
  if (!skyHits(cx - SKY_MILL_ARM - 2, hubY - SKY_MILL_ARM - 2,
               cx + SKY_MILL_ARM + 3, baseY + 1)) {
    return;
  }
  const uint16_t wall = sky565(skyWarm(skyMix({40, 40, 60}, {236, 228, 212},
                                              skyDay), skyGold * 0.5f));
  const uint16_t wallShade = sky565(skyMix({28, 28, 44}, {190, 180, 166},
                                           skyDay));
  const uint16_t roof = sky565(skyMix({40, 26, 36}, {176, 74, 60}, skyDay));
  const uint16_t spar = sky565(skyMix({30, 24, 30}, {92, 62, 44}, skyDay));
  const uint16_t sail = sky565(skyMix({64, 66, 88}, {250, 246, 234}, skyDay));
  const uint16_t dark = sky565(skyMix({16, 14, 22}, {84, 60, 48}, skyDay));
  const int top = hubY + 3;
  faceSprite.fillTriangle(SX(cx - 8), SY(baseY), SX(cx), SY(baseY),
                          SX(cx - 4), SY(top), wall);
  faceSprite.fillTriangle(SX(cx), SY(baseY), SX(cx), SY(top), SX(cx - 4),
                          SY(top), wall);
  faceSprite.fillTriangle(SX(cx), SY(baseY), SX(cx + 8), SY(baseY),
                          SX(cx + 4), SY(top), wallShade);
  faceSprite.fillTriangle(SX(cx), SY(baseY), SX(cx), SY(top), SX(cx + 4),
                          SY(top), wallShade);
  faceSprite.fillRect(SX(cx - 2), SY(baseY - 8), 5, 8, dark);
  faceSprite.fillSmoothCircle(SX(cx), SY(baseY - 8), 2, dark);
  const bool night = skyDay < 0.45f;
  faceSprite.fillRect(SX(cx - 1), SY(hubY + 12), 3, 4,
                      night ? rgb565(255, 204, 110) : dark);
  if (night) {
    skySoftDot(cx + 0.5f, hubY + 14, 7, rgb565(255, 190, 100),
               0.35f * skyNight);
  }
  faceSprite.fillTriangle(SX(cx - 7), SY(hubY + 4), SX(cx + 7), SY(hubY + 4),
                          SX(cx), SY(hubY - 6), roof);
  for (int i = 0; i < 4; ++i) {
    const float angle = skyMillAngle + i * PI / 2;
    const float dx = cosf(angle);
    const float dy = sinf(angle);
    const float px = -dy;
    const float py = dx;
    const auto at = [&](float along, float across, float& x, float& y) {
      x = SXf(cx + dx * along + px * across);
      y = SYf(hubY + dy * along + py * across);
    };
    float ax, ay, bx, by, qx, qy, rx, ry;
    at(5, 1, ax, ay);
    at(SKY_MILL_ARM - 1, 1, bx, by);
    at(SKY_MILL_ARM - 1, 6, qx, qy);
    at(5, 5, rx, ry);
    faceSprite.fillTriangle(ax, ay, bx, by, qx, qy, sail);
    faceSprite.fillTriangle(ax, ay, qx, qy, rx, ry, sail);
    for (int k = 1; k <= 3; ++k) {
      float lx0, ly0, lx1, ly1;
      at(5 + k * 4, 1, lx0, ly0);
      at(5 + k * 4, 5.5f, lx1, ly1);
      faceSprite.drawLine(lx0, ly0, lx1, ly1, spar);
    }
    faceSprite.drawWideLine(SXf(cx), SYf(hubY), SXf(cx + dx * SKY_MILL_ARM),
                            SYf(hubY + dy * SKY_MILL_ARM), 1.6f, spar);
  }
  faceSprite.fillSmoothCircle(SX(cx), SY(hubY), 2, spar);
}

SkyBox skyMillRegion() {
  const int hubY = skyGround[SKY_MILL_X] + 2 - 34;
  SkyBox box;
  box.x0 = SKY_MILL_X - SKY_MILL_ARM - 3;
  box.x1 = SKY_MILL_X + SKY_MILL_ARM + 4;
  box.y0 = static_cast<int16_t>(hubY - SKY_MILL_ARM - 3);
  box.y1 = static_cast<int16_t>(hubY + SKY_MILL_ARM + 4);
  box.valid = true;
  return box;
}

void skyDrawPath() {
  const int baseY = skyGround[SKY_HOUSE_X] + 1;
  if (!skyHits(SKY_HOUSE_X - 20, baseY, SKY_HOUSE_X + 14, SCREEN_H)) {
    return;
  }
  const uint16_t path = sky565(skyMix(cSand, cGrassLow, 0.25f));
  const uint16_t edge = sky565(skyMix(cSand, cGrassLow, 0.55f));
  const int x = SKY_HOUSE_X;
  faceSprite.fillTriangle(SX(x - 4), SY(baseY), SX(x + 4), SY(baseY),
                          SX(x + 11), SY(SCREEN_H), edge);
  faceSprite.fillTriangle(SX(x - 4), SY(baseY), SX(x + 11), SY(SCREEN_H),
                          SX(x - 18), SY(SCREEN_H), edge);
  faceSprite.fillTriangle(SX(x - 3), SY(baseY), SX(x + 3), SY(baseY),
                          SX(x + 9), SY(SCREEN_H), path);
  faceSprite.fillTriangle(SX(x - 3), SY(baseY), SX(x + 9), SY(SCREEN_H),
                          SX(x - 16), SY(SCREEN_H), path);
}

void skyDrawFence() {
  const int startX = SKY_HOUSE_X + 26;
  if (!skyHits(startX - 2, 176, SCREEN_W, 210)) {
    return;
  }
  const uint16_t wood = sky565(skyWarm(skyMix({30, 26, 32}, {150, 112, 78},
                                              skyDay), skyGold * 0.4f));
  int lastX = -1;
  int lastY = 0;
  for (int x = startX; x < SCREEN_W; x += 7) {
    const int g = skyGround[x] + 1;
    faceSprite.fillRect(SX(x), SY(g - 9), 2, 9, wood);
    if (lastX >= 0) {
      faceSprite.drawLine(SX(lastX), SY(lastY - 7), SX(x), SY(g - 7), wood);
      faceSprite.drawLine(SX(lastX), SY(lastY - 3), SX(x), SY(g - 3), wood);
    }
    lastX = x;
    lastY = g;
  }
}

void skyDrawTree() {
  const int x = SKY_TREE_X;
  const int baseY = skyGround[x] + 1;
  if (!skyHits(x - 14, baseY - 34, x + 14, baseY + 1)) {
    return;
  }
  const uint16_t trunk = sky565(skyMix({22, 18, 22}, {110, 76, 52}, skyDay));
  const SkyRgb leafDay = skyWarm({60, 132, 70}, skyGold * 0.4f);
  const uint16_t leaf = sky565(skyMix({10, 24, 28}, leafDay, skyDay));
  const uint16_t leafLit = sky565(skyMix({16, 32, 36},
                                         skyWarm({96, 170, 88}, skyGold * 0.5f),
                                         skyDay));
  const uint16_t leafDark = sky565(skyMix({6, 16, 20}, {40, 100, 58}, skyDay));
  faceSprite.fillRect(SX(x - 1), SY(baseY - 14), 3, 14, trunk);
  faceSprite.fillSmoothCircle(SX(x), SY(baseY - 22), 10, leafDark);
  faceSprite.fillSmoothCircle(SX(x - 6), SY(baseY - 17), 7, leaf);
  faceSprite.fillSmoothCircle(SX(x + 6), SY(baseY - 18), 7, leaf);
  faceSprite.fillSmoothCircle(SX(x), SY(baseY - 25), 8, leaf);
  const int side = skyBodyX > x ? 1 : -1;
  faceSprite.fillSmoothCircle(SX(x + side * 3), SY(baseY - 27), 4, leafLit);
  faceSprite.fillSmoothCircle(SX(x + side * 7), SY(baseY - 20), 3, leafLit);
}

void skyDrawLamp() {
  const int x = SKY_LAMP_X;
  const int baseY = skyGround[x] + 1;
  const int headY = baseY - 17;
  if (!skyHits(x - 18, headY - 18, x + 19, baseY + 1)) {
    return;
  }
  const uint16_t pole = sky565(skyMix({20, 20, 28}, {60, 62, 72}, skyDay));
  faceSprite.drawFastVLine(SX(x), SY(headY), 17, pole);
  faceSprite.fillRect(SX(x - 2), SY(headY - 4), 5, 4, pole);
  const bool on = skyDay < 0.5f;
  faceSprite.fillRect(SX(x - 1), SY(headY - 3), 3, 2,
                      on ? rgb565(255, 222, 150) : rgb565(150, 156, 168));
  if (on) {
    skySoftDot(x + 0.5f, headY - 2, 17, rgb565(255, 196, 110),
               0.42f * skyNight);
    skySoftDot(x + 0.5f, baseY, 10, rgb565(255, 196, 110), 0.25f * skyNight,
               true);
  }
}

void skyDrawHouse() {
  const int x = SKY_HOUSE_X;
  const int baseY = skyGround[x] + 1;
  if (!skyHits(x - 34, baseY - 40, x + 22, baseY + 12)) {
    return;
  }
  const uint16_t wall = sky565(skyWarm(skyMix({38, 34, 56}, {238, 222, 196},
                                              skyDay), skyGold * 0.5f));
  const uint16_t wallShade = sky565(skyMix({26, 24, 40}, {200, 182, 156},
                                           skyDay));
  const uint16_t roof = sky565(skyWarm(skyMix({52, 28, 44}, {198, 84, 70},
                                              skyDay), skyGold * 0.3f));
  const uint16_t roofDark = sky565(skyMix({36, 20, 32}, {150, 60, 52}, skyDay));
  const uint16_t brick = sky565(skyMix({34, 24, 30}, {140, 82, 66}, skyDay));
  const uint16_t door = sky565(skyMix({24, 16, 22}, {112, 70, 48}, skyDay));
  const uint16_t frame = sky565(skyMix({30, 28, 40}, {255, 250, 240}, skyDay));
  // Chimney sits behind the roof.
  faceSprite.fillRect(SX(x + 8), SY(baseY - 37), 6, 16, brick);
  faceSprite.fillRect(SX(x + 7), SY(baseY - 38), 8, 2, roofDark);
  faceSprite.fillRect(SX(x - 16), SY(baseY - 20), 32, 20, wall);
  faceSprite.fillRect(SX(x + 8), SY(baseY - 20), 8, 20, wallShade);
  faceSprite.fillTriangle(SX(x - 21), SY(baseY - 19), SX(x + 21),
                          SY(baseY - 19), SX(x), SY(baseY - 37), roof);
  for (int k = 1; k <= 3; ++k) {
    const int yy = baseY - 19 - k * 4;
    const int half = 21 * (18 - k * 4) / 18;
    faceSprite.drawFastHLine(SX(x - half + 1), SY(yy), half * 2 - 1, roofDark);
  }
  faceSprite.drawFastHLine(SX(x - 21), SY(baseY - 19), 43, roofDark);
  faceSprite.drawFastHLine(SX(x - 16), SY(baseY - 18), 32, wallShade);
  // Door with a knob and a step.
  faceSprite.fillRect(SX(x - 3), SY(baseY - 11), 7, 11, door);
  faceSprite.drawPixel(SX(x + 2), SY(baseY - 5), frame);
  faceSprite.drawFastHLine(SX(x - 4), SY(baseY), 9, wallShade);
  // Windows: warm and flickering at night.
  const float flicker = 0.84f + 0.1f * sinf(skyT * 7.3f) +
                        0.06f * sinf(skyT * 17.0f);
  const uint16_t glass = sky565(skyMix({60, 70, 92}, {150, 200, 236}, skyDay));
  const uint16_t lamp = blendColor(rgb565(255, 210, 120), rgb565(220, 150, 70),
                                   static_cast<uint8_t>(flicker * 255));
  const bool lit = skyDay < 0.55f;
  const int windowX[2] = {x - 13, x + 6};
  for (int wx : windowX) {
    faceSprite.fillRect(SX(wx), SY(baseY - 15), 8, 7, frame);
    faceSprite.fillRect(SX(wx + 1), SY(baseY - 14), 6, 5, lit ? lamp : glass);
    faceSprite.drawFastVLine(SX(wx + 3), SY(baseY - 14), 5, frame);
    faceSprite.drawFastHLine(SX(wx + 1), SY(baseY - 12), 6, frame);
    if (lit) {
      skySoftDot(wx + 4, baseY - 11, 11, rgb565(255, 200, 110),
                 0.3f * skyNight);
    }
  }
  if (lit) {
    skySoftDot(x + 0.5f, baseY + 5, 12, rgb565(255, 190, 100),
               0.18f * skyNight, true);  // light spilling on the grass
  }
}

void skySmokePuff(int i, float& px, float& py, float& radius, float& alpha) {
  const float age = skySmokeAge[i];
  const int baseY = skyGround[SKY_HOUSE_X] + 1;
  px = SKY_HOUSE_X + 11 + age * 3.2f + sinf(age * 1.6f + i * 1.9f) * 2.0f;
  py = baseY - 40 - age * 7.0f;
  radius = 2.2f + age * 1.0f;
  alpha = 0.5f * (1.0f - age / SKY_SMOKE_LIFE) * min(1.0f, age * 3.0f);
}

void skyDrawSmoke() {
  const uint16_t color = sky565(skyMix({90, 94, 124}, {236, 236, 240}, skyDay));
  for (int i = 0; i < SKY_PUFFS; ++i) {
    float px, py, radius, alpha;
    skySmokePuff(i, px, py, radius, alpha);
    skySoftDot(px, py, radius, color, alpha, true);
  }
}

SkyBox skyHouseRegion() {
  const int baseY = skyGround[SKY_HOUSE_X] + 1;
  SkyBox box;
  box.x0 = SKY_HOUSE_X - 20;
  box.x1 = SKY_HOUSE_X + 38;
  box.y0 = static_cast<int16_t>(baseY - 40 - SKY_SMOKE_LIFE * 7.0f - 9);
  box.y1 = static_cast<int16_t>(baseY - 7);
  box.valid = true;
  return box;
}

int skyBoatWaterline() {
  return SKY_LAKE_TOP + 16 + static_cast<int>(lroundf(sinf(skyT * 1.7f)));
}

void skyDrawBoat() {
  const int x = static_cast<int>(skyBoatX);
  const int wl = skyBoatWaterline();
  if (!skyHits(x - 14, wl - 26, x + 14, wl + 24)) {
    return;
  }
  const int d = skyBoatDir > 0 ? 1 : -1;  // sails lean with the heading
  const uint16_t hull = sky565(skyMix({30, 24, 30}, {128, 78, 50}, skyDay));
  const uint16_t trim = sky565(skyMix({40, 36, 46}, {236, 226, 206}, skyDay));
  const uint16_t sail = sky565(skyWarm(skyMix({70, 72, 98}, {250, 246, 236},
                                              skyDay), skyGold * 0.5f));
  const uint16_t sailShade = sky565(skyMix({54, 56, 80}, {214, 210, 200},
                                           skyDay));
  const uint16_t mast = sky565(skyMix({24, 20, 26}, {84, 58, 40}, skyDay));
  const uint16_t water = sky565(cWater);
  // Reflection first: a faint, slightly wavy mirror image.
  for (int k = 0; k < 20; ++k) {
    const int y = wl + 1 + k;
    const int wobble = skyWave[(k * 5 + skyWavePhase) & 31] / 2;
    const uint8_t alpha = static_cast<uint8_t>(90 - k * 4);
    if (k < 3) {
      const int half = 9 - k;
      for (int xx = x - half; xx <= x + half; ++xx) {
        skyBlendPixel(xx + wobble, y, blendColor(hull, water, 160), alpha);
      }
    } else if (k < 20) {
      const int len = k - 3;
      const int sailHalfTop = max(0, 8 - len * 8 / 16);
      for (int xx = 1; xx <= sailHalfTop; ++xx) {
        skyBlendPixel(x + d * xx + wobble, y, blendColor(sail, water, 140),
                      alpha);
      }
      skyBlendPixel(x + wobble, y, blendColor(mast, water, 150), alpha);
    }
  }
  // Hull.
  faceSprite.fillTriangle(SX(x - 10), SY(wl - 3), SX(x + 10), SY(wl - 3),
                          SX(x + 7 * d), SY(wl + 1), hull);
  faceSprite.fillTriangle(SX(x - 10 * d), SY(wl - 3), SX(x + 7 * d),
                          SY(wl + 1), SX(x - 6 * d), SY(wl + 1), hull);
  faceSprite.drawFastHLine(SX(x - 10), SY(wl - 3), 21, trim);
  // Mast, mainsail, jib and a pennant.
  faceSprite.drawFastVLine(SX(x), SY(wl - 22), 19, mast);
  faceSprite.fillTriangle(SX(x + d), SY(wl - 21), SX(x + d), SY(wl - 5),
                          SX(x + 10 * d), SY(wl - 5), sail);
  faceSprite.fillTriangle(SX(x - d), SY(wl - 18), SX(x - d), SY(wl - 5),
                          SX(x - 7 * d), SY(wl - 5), sailShade);
  const int flutter = static_cast<int>(lroundf(sinf(skyT * 6.0f)));
  faceSprite.fillTriangle(SX(x), SY(wl - 24), SX(x), SY(wl - 21),
                          SX(x - 5 * d), SY(wl - 23 + flutter),
                          rgb565(232, 84, 72));
  if (skyDay < 0.5f) {
    const float glow = skyNight * (0.85f + 0.15f * sinf(skyT * 5.0f));
    skyBlendPixel(x - 3 * d, wl - 5, rgb565(255, 224, 150), 255);
    skySoftDot(x - 3 * d, wl - 5, 7, rgb565(255, 196, 110), 0.45f * glow);
    skySoftDot(x - 3 * d, wl + 7, 5, rgb565(255, 196, 110), 0.25f * glow);
  }
}

SkyBox skyFlyBox(const SkyFly& fly) {
  SkyBox box;
  box.x0 = static_cast<int16_t>(fly.x - 5);
  box.y0 = static_cast<int16_t>(fly.y - 5);
  box.x1 = static_cast<int16_t>(fly.x + 6);
  box.y1 = static_cast<int16_t>(fly.y + 6);
  box.valid = true;
  return box;
}

bool skyFliesOut() { return skyDay < 0.3f; }

void skyDrawFlies() {
  if (!skyFliesOut()) {
    return;
  }
  for (const SkyFly& fly : skyFlies) {
    if (!skyHits(fly.x - 5, fly.y - 5, fly.x + 6, fly.y + 6)) {
      continue;
    }
    const float pulse = max(0.0f, sinf(skyT * fly.rate + fly.phase));
    const float glow = pulse * pulse * skyNight;
    skySoftDot(fly.x, fly.y, 4.5f, rgb565(190, 255, 110), 0.55f * glow);
    skyBlendPixel(static_cast<int>(fly.x), static_cast<int>(fly.y),
                  rgb565(240, 255, 200), static_cast<int>(255 * glow));
  }
}

void skyDrawText() {
  if (skyHits(8, 4, 160, 74)) {
    faceSprite.setTextDatum(TL_DATUM);
    useFreeFont(faceSprite, &FreeSansBold24pt7b);
    faceSprite.setTextColor(skyShadow565);
    faceSprite.drawString(skyTimeText, SX(16), SY(12));
    faceSprite.setTextColor(TEXT_COLOR);
    faceSprite.drawString(skyTimeText, SX(14), SY(10));
    useFreeFont(faceSprite, &FreeSansBold9pt7b);
    faceSprite.setTextColor(skyShadow565);
    faceSprite.drawString(skyDateText, SX(17), SY(55));
    faceSprite.setTextColor(blendColor(TEXT_COLOR, sky565(skyRows[60]), 225));
    faceSprite.drawString(skyDateText, SX(16), SY(54));
  }
  if (skyHits(178, 4, SCREEN_W, 76)) {
    useFreeFont(faceSprite, &FreeSansBold12pt7b);
    const String temperature = climateAvailable ? temperatureValue : "--";
    const int valueWidth = faceSprite.textWidth(temperature);
    const int ring = faceSprite.fontHeight() > 24 ? 3 : 2;
    const int total = valueWidth +
        (climateAvailable ? ring * 2 + 5 + faceSprite.textWidth("C") : 0);
    const int x = SCREEN_W - 14 - total;
    constexpr int y = 12;
    faceSprite.setTextDatum(TL_DATUM);
    faceSprite.setTextColor(skyShadow565);
    faceSprite.drawString(temperature, SX(x + 1), SY(y + 1));
    faceSprite.setTextColor(TEXT_COLOR);
    faceSprite.drawString(temperature, SX(x), SY(y));
    if (climateAvailable) {
      faceSprite.drawSmoothCircle(SX(x + valueWidth + ring + 2),
                                  SY(y + ring + 1), ring, TEXT_COLOR,
                                  sky565(skyRows[y]));
      faceSprite.drawString("C", SX(x + valueWidth + ring * 2 + 5), SY(y));
    }
    useFreeFont(faceSprite, &FreeSans9pt7b);
    faceSprite.setTextDatum(TR_DATUM);
    const uint16_t muted = blendColor(TEXT_COLOR, sky565(skyRows[50]), 215);
    if (climateAvailable) {
      const String humidity = humidityValue + "% humidity";
      faceSprite.setTextColor(skyShadow565);
      faceSprite.drawString(humidity, SX(SCREEN_W - 13), SY(39));
      faceSprite.setTextColor(muted);
      faceSprite.drawString(humidity, SX(SCREEN_W - 14), SY(38));
    }
    faceSprite.setTextColor(skyShadow565);
    faceSprite.drawString(skyPhrase, SX(SCREEN_W - 13), SY(58));
    faceSprite.setTextColor(blendColor(rgb565(255, 214, 150), muted,
                                       skySunUp ? 150 : 40));
    faceSprite.drawString(skyPhrase, SX(SCREEN_W - 14), SY(57));
  }
}

// Page dots drawn into the current strip (any strip-rendered face).
void stripDrawDots(uint16_t idleBackground) {
  constexpr int dotY = 233;
  const int startX = SCREEN_W / 2 - (FACE_COUNT - 1) * 6;
  if (!skyHits(startX - 6, dotY - 5, startX + FACE_COUNT * 12, dotY + 6)) {
    return;
  }
  const uint16_t idle = blendColor(TEXT_COLOR, idleBackground, 90);
  for (int i = 0; i < FACE_COUNT; ++i) {
    const bool active = i == clockFace;
    faceSprite.fillSmoothCircle(SX(startX + i * 12), SY(dotY), active ? 3 : 2,
                                active ? TEXT_COLOR : idle);
  }
}

void skyDrawDots() { stripDrawDots(sky565(cGrassLow)); }

void skyDrawObjects() {
  skyDrawStars();
  skyDrawShootingStar();
  for (const SkyCloud& cloud : skyClouds) {
    skyDrawCloud(cloud);
  }
  skyDrawBirds();
  skyDrawBoat();
  skyDrawMill();
  skyDrawPath();
  skyDrawFence();
  skyDrawTree();
  skyDrawLamp();
  skyDrawHouse();
  skyDrawSmoke();
  skyDrawFlies();
  skyDrawText();
  skyDrawDots();
}

// The strip renderer is shared by every scene-style face: each one supplies
// a function that fills one row of background pixels and a function that
// draws its objects into the current strip.
void skyRowPixels(uint16_t* line, int x0, int y, int width) {
  for (int col = 0; col < width; ++col) {
    line[col] = swap16(skyBasePixel(x0 + col, y));
  }
}
void (*stripRow)(uint16_t*, int, int, int) = skyRowPixels;
void (*stripDraw)() = skyDrawObjects;

// Renders a screen rectangle strip by strip through the face sprite.
void skyRender(int x0, int y0, int x1, int y1) {
  x0 = max(x0, 0);
  y0 = max(y0, 0);
  x1 = min(x1, SCREEN_W);
  y1 = min(y1, SCREEN_H);
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  skyBuf = static_cast<uint16_t*>(faceSprite.getPointer());
  for (int top = y0; top < y1; top += SKY_STRIP_H) {
    skyOx = x0;
    skyOy = top;
    skyOw = x1 - x0;
    skyOh = min(SKY_STRIP_H, y1 - top);
    for (int row = 0; row < skyOh; ++row) {
      stripRow(skyBuf + row * SCREEN_W, x0, top + row, skyOw);
    }
    stripDraw();
    faceSprite.pushSprite(x0, top, 0, 0, skyOw, skyOh);
  }
}

void skyRender(const SkyBox& box) {
  if (box.valid) {
    skyRender(box.x0, box.y0, box.x1, box.y1);
  }
}

// Redraws the union of where something was and where it is now.
void skyRenderMoved(SkyBox& drawn, const SkyBox& current) {
  if (drawn.valid && current.valid) {
    skyRender(min(drawn.x0, current.x0), min(drawn.y0, current.y0),
              max(drawn.x1, current.x1), max(drawn.y1, current.y1));
  } else {
    skyRender(drawn);
    skyRender(current);
  }
  drawn = current;
}

void skyTwinkle() {
  if (skyNight <= 0.02f) {
    return;
  }
  for (const SkyStar& star : skyStars) {
    if (!star.free) {
      continue;
    }
    const int dx = star.x - skyBodyX;
    const int dy = star.y - skyBodyY;
    if (dx * dx + dy * dy < 22 * 22) {
      continue;
    }
    bool covered = false;
    for (const SkyCloud& cloud : skyClouds) {
      const SkyBox box = skyCloudBox(cloud);
      if (star.x >= box.x0 - 1 && star.x <= box.x1 && star.y >= box.y0 - 1 &&
          star.y <= box.y1) {
        covered = true;
        break;
      }
    }
    if (skyShootDrawn.valid && star.x >= skyShootDrawn.x0 - 1 &&
        star.x <= skyShootDrawn.x1 && star.y >= skyShootDrawn.y0 - 1 &&
        star.y <= skyShootDrawn.y1) {
      covered = true;
    }
    if (covered) {
      continue;
    }
    const float twinkle = 0.5f + 0.5f * sinf(skyT * star.speed + star.phase);
    const uint8_t alpha = static_cast<uint8_t>(
        min(255.0f, skyNight * (60.0f + twinkle * 195.0f)));
    tft.drawPixel(star.x, star.y,
                  blendColor(TEXT_COLOR, skyBasePixel(star.x, star.y), alpha));
    if (star.big) {
      const int8_t ox[4] = {-1, 1, 0, 0};
      const int8_t oy[4] = {0, 0, -1, 1};
      for (int i = 0; i < 4; ++i) {
        const int x = star.x + ox[i];
        const int y = star.y + oy[i];
        tft.drawPixel(x, y, blendColor(TEXT_COLOR, skyBasePixel(x, y),
                                       alpha / 3));
      }
    }
  }
}

// ---- animation state -------------------------------------------------------
void skyResetAnimation() {
  for (int i = 0; i < SKY_CLOUDS; ++i) {
    skySpawnCloud(skyClouds[i], true);
    skyClouds[i].drawn = SkyBox();
  }
  for (int i = 0; i < SKY_PUFFS; ++i) {
    skySmokeAge[i] = i * SKY_SMOKE_LIFE / SKY_PUFFS;
  }
  for (SkyFly& fly : skyFlies) {
    fly.x = random(10, 220);
    fly.y = skyGround[static_cast<int>(fly.x)] + random(6, 14);
    fly.vx = 0;
    fly.vy = 0;
    fly.phase = random(0, 628) / 100.0f;
    fly.rate = random(10, 26) / 10.0f;
    fly.drawn = SkyBox();
  }
  skyBirdsActive = false;
  skyBirdsDrawn = SkyBox();
  skyBirdsNextAt = millis() + random(4000, 15000);
  skyShooting = false;
  skyNextShootAt = 0;
  skyShootDrawn = SkyBox();
  skyMillBox = skyMillRegion();
  skyLastFrameAt = 0;
  skyFrame = 0;
}

void skyAdvance(float dt, uint32_t nowMs) {
  skyMillAngle += dt * 0.9f;
  if (skyMillAngle > 2 * PI) {
    skyMillAngle -= 2 * PI;
  }
  for (SkyCloud& cloud : skyClouds) {
    cloud.x += cloud.speed * dt;
    if (cloud.x > SCREEN_W + 4) {
      skySpawnCloud(cloud, false);
    }
  }
  for (float& age : skySmokeAge) {
    age += dt;
    if (age >= SKY_SMOKE_LIFE) {
      age -= SKY_SMOKE_LIFE;
    }
  }
  skyBoatX += skyBoatDir * 2.0f * dt;
  if (skyBoatX > 196.0f) {
    skyBoatDir = -1.0f;
  } else if (skyBoatX < 104.0f) {
    skyBoatDir = 1.0f;
  }

  if (skyBirdsActive) {
    skyBirdsX -= skyBirdsSpeed * dt;
    skyBirdsFlap += dt * 9.0f;
    if (skyBirdsX < -40.0f) {
      skyBirdsActive = false;
      skyBirdsNextAt = nowMs + random(25000, 70000);
    }
  } else if (skyDay > 0.45f && deadlineReached(nowMs, skyBirdsNextAt)) {
    skyBirdsActive = true;
    skyBirdsX = SCREEN_W + 8;
    skyBirdsY = random(SKY_BAND_Y0 + 16, SKY_BAND_Y1 - 16);
    skyBirdsSpeed = random(15, 23);
  }

  if (skyFliesOut()) {
    for (SkyFly& fly : skyFlies) {
      fly.vx = constrain(fly.vx + (random(-100, 101) / 100.0f) * dt * 30.0f,
                         -7.0f, 7.0f);
      fly.vy = constrain(fly.vy + (random(-100, 101) / 100.0f) * dt * 24.0f,
                         -5.0f, 5.0f);
      fly.x += fly.vx * dt;
      fly.y += fly.vy * dt;
      if (fly.x < 8 || fly.x > 222) {
        fly.vx = -fly.vx;
        fly.x = constrain(fly.x, 8.0f, 222.0f);
      }
      const float floorY = skyGround[static_cast<int>(fly.x)] + 5.0f;
      if (fly.y < floorY || fly.y > 226) {
        fly.vy = -fly.vy;
        fly.y = constrain(fly.y, floorY, 226.0f);
      }
    }
  }

  if (skyNight > 0.7f && !skyShooting) {
    if (skyNextShootAt == 0) {
      skyNextShootAt = nowMs + random(6000, 15000);
    }
    if (deadlineReached(nowMs, skyNextShootAt)) {
      skyShooting = true;
      skyShootStartedAt = nowMs;
      skyShootX = random(200, 300);
      skyShootY = random(10, 26);
    }
  }
}

SkyBox skyShootBox(uint32_t nowMs) {
  SkyBox box;
  if (!skyShooting) {
    return box;
  }
  float progress;
  int hx;
  int hy;
  skyShootHead(nowMs, progress, hx, hy);
  box.x0 = static_cast<int16_t>(hx - 2);
  box.y0 = static_cast<int16_t>(hy - SKY_SHOOT_TRAIL / 2 - 2);
  box.x1 = static_cast<int16_t>(hx + SKY_SHOOT_TRAIL + 3);
  box.y1 = static_cast<int16_t>(hy + 3);
  box.valid = true;
  return box;
}

void skyRenderAll(const struct tm& localTime, const struct timeval& now,
                  bool synced) {
  skyPrepare(localTime, now, synced);
  skyWavePhase = static_cast<int>(skyT * 10.0f);
  skyRender(0, 0, SCREEN_W, SCREEN_H);
  for (SkyCloud& cloud : skyClouds) {
    cloud.drawn = skyCloudBox(cloud);
  }
  skyBirdsDrawn = skyBirdsBox();
  for (SkyFly& fly : skyFlies) {
    fly.drawn = skyFliesOut() ? skyFlyBox(fly) : SkyBox();
  }
  skyShootDrawn = skyShootBox(millis());
}

bool enterSkyFace() {
  if (!allocateFaceSprite(SCREEN_W, SKY_STRIP_H)) {
    return false;
  }
  stripRow = skyRowPixels;
  stripDraw = skyDrawObjects;
  buildSkyTables();
  skyResetAnimation();
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  skyT = millis() / 1000.0f;
  const uint32_t startedAt = millis();
  skyRenderAll(localTime, now, synced);
  Serial.printf("Sky scene drawn in %lu ms.\n",
                static_cast<unsigned long>(millis() - startedAt));
  char key[8] = "sync";
  if (synced) {
    strftime(key, sizeof(key), "%H:%M", &localTime);
  }
  faceRenderedTime = key;
  faceRenderedClimate = climateKey();
  return true;
}

void updateSkyFace(uint32_t nowMs) {
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  char key[8] = "sync";
  if (synced) {
    strftime(key, sizeof(key), "%H:%M", &localTime);
  }
  const float dt = skyLastFrameAt == 0
      ? 0.0f : min(0.2f, (nowMs - skyLastFrameAt) / 1000.0f);
  skyLastFrameAt = nowMs;
  skyT = nowMs / 1000.0f;
  skyAdvance(dt, nowMs);

  if (faceRenderedTime != key) {
    // A new minute: the light, the sun and the time all move on together.
    faceRenderedTime = key;
    faceRenderedClimate = climateKey();
    skyRenderAll(localTime, now, synced);
    return;
  }
  const String climate = climateKey();
  if (faceRenderedClimate != climate) {
    faceRenderedClimate = climate;
    skyRender(178, 4, SCREEN_W, 76);
  }

  ++skyFrame;
  skyRender(skyMillBox);  // the sails turn every frame
  if (skyFrame % 2 == 0) {
    for (SkyCloud& cloud : skyClouds) {
      const SkyBox box = skyCloudBox(cloud);
      if (box.x0 != cloud.drawn.x0 || box.y0 != cloud.drawn.y0 ||
          !cloud.drawn.valid) {
        skyRenderMoved(cloud.drawn, box);
      }
    }
  } else if (skyFrame % 4 == 1) {
    skyWavePhase = static_cast<int>(skyT * 10.0f);
    skyRender(0, SKY_LAKE_Y0, SCREEN_W, SKY_LAKE_Y1);
  } else {
    skyRender(skyHouseRegion());
  }
  if (skyBirdsActive || skyBirdsDrawn.valid) {
    skyRenderMoved(skyBirdsDrawn, skyBirdsBox());
  }
  if (skyFliesOut()) {
    for (SkyFly& fly : skyFlies) {
      skyRenderMoved(fly.drawn, skyFlyBox(fly));
    }
  }
  if (skyShooting || skyShootDrawn.valid) {
    float progress = 1.0f;
    int hx;
    int hy;
    if (skyShooting) {
      skyShootHead(nowMs, progress, hx, hy);
    }
    if (progress >= 1.0f) {
      skyShooting = false;
      skyNextShootAt = nowMs + random(9000, 24000);
    }
    skyRenderMoved(skyShootDrawn, skyShootBox(nowMs));
  }
  if (skyFrame % 2 == 1) {
    skyTwinkle();
  }
}

// ---- Low Poly: a faceted landscape made only of flat-shaded triangles. The
// sky facets breathe slowly, lake facets glint, the sun is an octagon and a
// low-poly fox sits on a rock swishing its tail. Shares the Sky face's time
// of day palette and strip renderer. ---------------------------------------
constexpr int LP_MAX_TRIS = 330;
constexpr int LP_LAKE_TOP = 180;
constexpr int LP_GROUND_TOP = 205;
constexpr int LP_FOX_X = 262;
constexpr int LP_FOX_Y = 211;  // where the fox sits
constexpr uint8_t LP_SNOW = 1;

enum LpLayer : uint8_t { LP_SKY, LP_FAR, LP_MID, LP_NEAR, LP_LAKE, LP_GROUND,
                         LP_ROCK };

struct LpTri {
  int16_t x[3];
  int16_t y[3];
  uint8_t layer;
  int8_t side;  // which way the facet faces: -1 left, 1 right
  uint8_t jitter;
  uint8_t flags;
};

LpTri lpTris[LP_MAX_TRIS];
int lpCount = 0;
bool lpReady = false;
int lpRollingStrip = 0;
uint32_t lpFrame = 0;
uint32_t lpNextBlink = 0;
uint32_t lpBlinkUntil = 0;
uint32_t lpNextEar = 0;
uint32_t lpEarUntil = 0;
uint32_t lpLookUpAt = 0;
bool lpAsleep = false;

void lpAdd(int x0, int y0, int x1, int y1, int x2, int y2, uint8_t layer,
           uint8_t flags) {
  if (lpCount >= LP_MAX_TRIS) {
    return;
  }
  LpTri& tri = lpTris[lpCount];
  tri.x[0] = x0;
  tri.y[0] = y0;
  tri.x[1] = x1;
  tri.y[1] = y1;
  tri.x[2] = x2;
  tri.y[2] = y2;
  tri.layer = layer;
  tri.flags = flags;
  int top = 0;
  for (int i = 1; i < 3; ++i) {
    if (tri.y[i] < tri.y[top]) {
      top = i;
    }
  }
  const int centreX = (x0 + x1 + x2) / 3;
  tri.side = static_cast<int8_t>(centreX > tri.x[top] + 1
                                     ? 1 : (centreX < tri.x[top] - 1 ? -1 : 0));
  tri.jitter = static_cast<uint8_t>(skyHash(lpCount, layer, 3) & 31);
  ++lpCount;
}

// Triangulates the band between an upper and a lower polyline.
void lpStrip(const int16_t* ax, const int16_t* ay, int an, const int16_t* bx,
             const int16_t* by, int bn, uint8_t layer, int snowLine) {
  int i = 0;
  int j = 0;
  while (i < an - 1 || j < bn - 1) {
    if (j >= bn - 1 || (i < an - 1 && ax[i + 1] <= bx[j + 1])) {
      const uint8_t flags =
          snowLine > 0 && min(ay[i], ay[i + 1]) < snowLine ? LP_SNOW : 0;
      lpAdd(ax[i], ay[i], ax[i + 1], ay[i + 1], bx[j], by[j], layer, flags);
      ++i;
    } else {
      const uint8_t flags = snowLine > 0 && ay[i] < snowLine - 6 ? LP_SNOW : 0;
      lpAdd(ax[i], ay[i], bx[j + 1], by[j + 1], bx[j], by[j], layer, flags);
      ++j;
    }
  }
}

void lpGrid(int y0, int y1, int rows, int jitterY, uint8_t layer) {
  constexpr int cols = 9;
  int16_t gx[4][cols];
  int16_t gy[4][cols];
  for (int r = 0; r <= rows; ++r) {
    for (int c = 0; c < cols; ++c) {
      const bool edgeX = c == 0 || c == cols - 1;
      const bool edgeY = r == 0 || r == rows;
      gx[r][c] = static_cast<int16_t>(c * 40 + (edgeX ? 0 : random(-13, 14)));
      gy[r][c] = static_cast<int16_t>(y0 + (y1 - y0) * r / rows +
                                      (edgeY ? 0 : random(-jitterY, jitterY + 1)));
    }
  }
  for (int r = 0; r < rows; ++r) {
    for (int c = 0; c < cols - 1; ++c) {
      if ((r + c) & 1) {
        lpAdd(gx[r][c], gy[r][c], gx[r + 1][c], gy[r + 1][c], gx[r + 1][c + 1],
              gy[r + 1][c + 1], layer, 0);
        lpAdd(gx[r][c], gy[r][c], gx[r + 1][c + 1], gy[r + 1][c + 1],
              gx[r][c + 1], gy[r][c + 1], layer, 0);
      } else {
        lpAdd(gx[r][c], gy[r][c], gx[r + 1][c], gy[r + 1][c], gx[r][c + 1],
              gy[r][c + 1], layer, 0);
        lpAdd(gx[r][c + 1], gy[r][c + 1], gx[r + 1][c], gy[r + 1][c],
              gx[r + 1][c + 1], gy[r + 1][c + 1], layer, 0);
      }
    }
  }
}

void lpBuild() {
  if (lpReady) {
    return;
  }
  randomSeed(2024);
  lpCount = 0;
  lpGrid(0, 172, 3, 14, LP_SKY);

  struct Range {
    uint8_t layer;
    int peakMin;
    int peakMax;
    int drop;
    int step;
    int snow;
  };
  const Range ranges[] = {{LP_FAR, 62, 96, 30, 46, 90},
                          {LP_MID, 98, 124, 24, 38, 108},
                          {LP_NEAR, 132, 152, 16, 30, 0}};
  for (const Range& range : ranges) {
    int16_t rx[20];
    int16_t ry[20];
    int n = 0;
    int peak = range.peakMax;
    for (int x = -30; x < SCREEN_W + 30 && n < 20; ++n) {
      rx[n] = static_cast<int16_t>(x);
      if (n % 2 == 0) {
        peak = random(range.peakMin, range.peakMax);
        ry[n] = static_cast<int16_t>(peak);
      } else {
        ry[n] = static_cast<int16_t>(peak + random(range.drop / 2, range.drop));
      }
      x += range.step / 2 + random(-5, 6);
    }
    rx[n - 1] = SCREEN_W + 30;
    int16_t mx[20];
    int16_t my[20];
    for (int k = 0; k < n - 1; ++k) {
      mx[k] = static_cast<int16_t>((rx[k] + rx[k + 1]) / 2 + random(-5, 6));
      my[k] = static_cast<int16_t>(max(ry[k], ry[k + 1]) + random(8, 20));
    }
    mx[0] = rx[0];
    mx[n - 2] = rx[n - 1];
    int16_t bx[8];
    int16_t by[8];
    for (int k = 0; k < 8; ++k) {
      bx[k] = static_cast<int16_t>(-30 + k * 54);
      by[k] = LP_LAKE_TOP + 4;
    }
    bx[7] = SCREEN_W + 30;
    lpStrip(rx, ry, n, mx, my, n - 1, range.layer, range.snow);
    lpStrip(mx, my, n - 1, bx, by, 8, range.layer, 0);
  }
  lpGrid(LP_LAKE_TOP, LP_GROUND_TOP + 6, 2, 4, LP_LAKE);
  lpGrid(LP_GROUND_TOP, SCREEN_H, 2, 4, LP_GROUND);
  // The fox's rock.
  const int rx = LP_FOX_X;
  const int ry = LP_FOX_Y;
  lpAdd(rx - 30, ry + 9, rx - 16, ry - 3, rx - 4, ry + 9, LP_ROCK, 0);
  lpAdd(rx - 16, ry - 3, rx + 6, ry - 1, rx - 4, ry + 9, LP_ROCK, 0);
  lpAdd(rx + 6, ry - 1, rx + 22, ry + 1, rx - 4, ry + 9, LP_ROCK, 0);
  lpAdd(rx + 22, ry + 1, rx + 34, ry + 10, rx - 4, ry + 9, LP_ROCK, 0);
  lpAdd(rx - 16, ry - 3, rx - 4, ry - 1, rx + 6, ry - 1, LP_ROCK, 0);
  randomSeed(esp_random());
  lpReady = true;
}

inline SkyRgb lpOffset(const SkyRgb& c, int amount) {
  return {static_cast<int16_t>(c.r + amount), static_cast<int16_t>(c.g + amount),
          static_cast<int16_t>(c.b + amount)};
}

uint16_t lpColor(const LpTri& tri) {
  const int centreY = (tri.y[0] + tri.y[1] + tri.y[2]) / 3;
  const int sunSide = skyBodyX > SCREEN_W / 2 ? 1 : -1;
  const float light = 0.3f + 0.7f * skyDay;
  const int jitter = tri.jitter - 16;
  SkyRgb c;
  switch (tri.layer) {
    case LP_SKY: {
      c = skyRows[constrain(centreY, 0, SKY_LAKE_TOP - 1)];
      const float breathe = sinf(skyT * 0.5f + tri.jitter * 0.4f);
      c = lpOffset(c, jitter / 2 + static_cast<int>(breathe * 6.0f));
      break;
    }
    case LP_FAR:
    case LP_MID:
    case LP_NEAR: {
      static const SkyRgb dayColors[] = {{150, 160, 204}, {98, 118, 170},
                                         {54, 96, 114}};
      static const SkyRgb nightColors[] = {{42, 46, 84}, {28, 32, 64},
                                           {14, 24, 40}};
      static const uint8_t haze[] = {110, 55, 22};
      const int index = tri.layer - LP_FAR;
      c = skyWarm(skyMix(nightColors[index], dayColors[index], skyDay),
                  skyGold * 0.4f);
      if (tri.flags & LP_SNOW) {
        c = skyWarm(skyMix({110, 118, 160}, {244, 246, 255}, skyDay),
                    skyGold * 0.35f);
      }
      int r = c.r;
      int g = c.g;
      int b = c.b;
      skyLerpTo(r, g, b, cHaze, haze[index]);
      const int shade = static_cast<int>(tri.side * sunSide * 16 * light);
      c = lpOffset({static_cast<int16_t>(r), static_cast<int16_t>(g),
                    static_cast<int16_t>(b)},
                   shade + jitter / 3 + (LP_LAKE_TOP - centreY) / 8);
      break;
    }
    case LP_LAKE: {
      c = skyMix(cWater, skyRows[60], 0.35f);
      const float glint = powf(max(0.0f, sinf(skyT * 1.3f + tri.jitter * 0.7f)),
                               14.0f);
      c = lpOffset(c, jitter / 2 + static_cast<int>(glint * 80.0f *
                                                     (0.3f + 0.7f * skyGlitter)));
      break;
    }
    case LP_GROUND:
      c = skyMix(cGrassTop, cGrassLow, (centreY - LP_GROUND_TOP) / 36.0f);
      c = lpOffset(c, jitter / 2 + static_cast<int>(tri.side * sunSide * 8 *
                                                     light));
      break;
    default:
      c = skyWarm(skyMix({40, 42, 54}, {150, 146, 150}, skyDay), skyGold * 0.3f);
      c = lpOffset(c, jitter + static_cast<int>(tri.side * sunSide * 14 * light));
      break;
  }
  return sky565(c);
}

void lpFillTri(const LpTri& tri, uint16_t color) {
  faceSprite.fillTriangle(SX(tri.x[0]), SY(tri.y[0]), SX(tri.x[1]),
                          SY(tri.y[1]), SX(tri.x[2]), SY(tri.y[2]), color);
}

bool lpTriHits(const LpTri& tri) {
  const int x0 = min(tri.x[0], min(tri.x[1], tri.x[2]));
  const int x1 = max(tri.x[0], max(tri.x[1], tri.x[2]));
  const int y0 = min(tri.y[0], min(tri.y[1], tri.y[2]));
  const int y1 = max(tri.y[0], max(tri.y[1], tri.y[2]));
  return skyHits(x0, y0, x1 + 1, y1 + 1);
}

void lpOctagon(int cx, int cy, int radius, uint16_t a, uint16_t b) {
  if (!skyHits(cx - radius - 1, cy - radius - 1, cx + radius + 2,
               cy + radius + 2)) {
    return;
  }
  for (int i = 0; i < 8; ++i) {
    const float a0 = i * PI / 4 + PI / 8;
    const float a1 = a0 + PI / 4;
    faceSprite.fillTriangle(SX(cx), SY(cy), SX(cx + cosf(a0) * radius),
                            SY(cy + sinf(a0) * radius),
                            SX(cx + cosf(a1) * radius),
                            SY(cy + sinf(a1) * radius), (i & 1) ? a : b);
  }
}

void lpDrawBody() {
  const uint16_t sky = sky565(skyRows[constrain(skyBodyY, 0, SKY_LAKE_TOP - 1)]);
  const uint16_t glow = sky565(skyGlowColor);
  if (!skySunUp && skyMoonLight < 0.08f) {
    lpOctagon(skyBodyX, skyBodyY, 11, blendColor(TEXT_COLOR, sky, 40),
              blendColor(TEXT_COLOR, sky, 30));
    return;
  }
  lpOctagon(skyBodyX, skyBodyY, 26, blendColor(glow, sky, 46),
            blendColor(glow, sky, 38));
  lpOctagon(skyBodyX, skyBodyY, 19, blendColor(glow, sky, 92),
            blendColor(glow, sky, 80));
  if (skySunUp) {
    lpOctagon(skyBodyX, skyBodyY, 12, rgb565(255, 226, 130),
              rgb565(255, 196, 96));
  } else if (skyMoonLight < 0.08f) {
    // New moon: just a faint dark disc.
    lpOctagon(skyBodyX, skyBodyY, 11, blendColor(TEXT_COLOR, sky, 40),
              blendColor(TEXT_COLOR, sky, 30));
  } else {
    lpOctagon(skyBodyX, skyBodyY, 11, rgb565(240, 238, 224),
              rgb565(212, 210, 198));
    if (skyMoonLight < 0.93f) {
      const int dir = skyMoonPhase < 0.5f ? -1 : 1;  // shadow side
      const int offset = static_cast<int>(22.0f * (1.0f - skyMoonLight)) + 2;
      lpOctagon(skyBodyX + dir * offset, skyBodyY, 11,
                blendColor(glow, sky, 92), blendColor(glow, sky, 80));
    }
  }
}

void lpDrawStars() {
  if (skyNight <= 0.05f) {
    return;
  }
  for (const SkyStar& star : skyStars) {
    if (star.y > 120) {
      continue;
    }
    const float twinkle = 0.5f + 0.5f * sinf(skyT * star.speed + star.phase);
    const int alpha = static_cast<int>(skyNight * (80 + twinkle * 175));
    skyBlendPixel(star.x, star.y, TEXT_COLOR, alpha);
    if (star.big) {
      skyBlendPixel(star.x - 1, star.y, TEXT_COLOR, alpha / 2);
      skyBlendPixel(star.x + 1, star.y, TEXT_COLOR, alpha / 2);
      skyBlendPixel(star.x, star.y - 1, TEXT_COLOR, alpha / 2);
      skyBlendPixel(star.x, star.y + 1, TEXT_COLOR, alpha / 2);
    }
  }
}

void lpDrawPine(int x, int baseY, int height) {
  if (!skyHits(x - height / 2, baseY - height - 2, x + height / 2 + 1,
               baseY + 2)) {
    return;
  }
  const int sunSide = skyBodyX > x ? 1 : -1;
  const uint16_t lit = sky565(skyWarm(skyMix({18, 40, 44}, {74, 152, 92},
                                             skyDay), skyGold * 0.4f));
  const uint16_t shade = sky565(skyMix({8, 22, 28}, {34, 94, 66}, skyDay));
  const uint16_t trunk = sky565(skyMix({24, 18, 22}, {104, 70, 48}, skyDay));
  faceSprite.fillRect(SX(x - 1), SY(baseY - 6), 3, 6, trunk);
  for (int tier = 0; tier < 3; ++tier) {
    const int tierBase = baseY - 4 - tier * height / 4;
    const int tierTop = tierBase - height / 2 + tier * 2;
    const int half = height * (3 - tier) / 9 + 2;
    faceSprite.fillTriangle(SX(x), SY(tierTop), SX(x - half), SY(tierBase),
                            SX(x), SY(tierBase), sunSide < 0 ? lit : shade);
    faceSprite.fillTriangle(SX(x), SY(tierTop), SX(x), SY(tierBase),
                            SX(x + half), SY(tierBase),
                            sunSide > 0 ? lit : shade);
  }
}

void lpTri(int x0, int y0, int x1, int y1, int x2, int y2, uint16_t color) {
  faceSprite.fillTriangle(SX(x0), SY(y0), SX(x1), SY(y1), SX(x2), SY(y2),
                          color);
}

void lpTriF(float x0, float y0, float x1, float y1, float x2, float y2,
            uint16_t color) {
  faceSprite.fillTriangle(static_cast<int>(lroundf(x0 - skyOx)),
                          static_cast<int>(lroundf(y0 - skyOy)),
                          static_cast<int>(lroundf(x1 - skyOx)),
                          static_cast<int>(lroundf(y1 - skyOy)),
                          static_cast<int>(lroundf(x2 - skyOx)),
                          static_cast<int>(lroundf(y2 - skyOy)), color);
}

SkyBox lpFoxBox() {
  SkyBox box;
  box.x0 = LP_FOX_X - 30;
  box.y0 = LP_FOX_Y - 56;
  box.x1 = LP_FOX_X + 42;
  box.y1 = LP_FOX_Y + 1;
  box.valid = true;
  return box;
}

void lpDrawFox() {
  const SkyBox box = lpFoxBox();
  if (!skyHits(box.x0, box.y0, box.x1, box.y1)) {
    return;
  }
  const uint32_t now = millis();
  const float t = skyT;
  const float day = 0.35f + 0.65f * skyDay;
  const auto tone = [&](const SkyRgb& c) {
    return sky565(skyWarm(skyMix({static_cast<int16_t>(c.r / 4),
                                  static_cast<int16_t>(c.g / 4),
                                  static_cast<int16_t>(c.b / 3)},
                                 c, day),
                          skyGold * 0.3f));
  };
  const uint16_t orange = tone({244, 124, 52});
  const uint16_t shade = tone({198, 88, 38});
  const uint16_t white = tone({250, 244, 236});
  const uint16_t dark = tone({54, 34, 34});
  const int fx = LP_FOX_X;
  const int fy = LP_FOX_Y;
  const float breathe = sinf(t * 2.0f) * (lpAsleep ? 0.5f : 0.9f);

  // Tail, behind the body; it swishes from a pivot near the hip.
  const float swish = lpAsleep ? sinf(t * 0.7f) * 0.08f : sinf(t * 1.4f) * 0.32f;
  const float px = fx + 9;
  const float py = fy - 4;
  const auto rot = [&](float x, float y, float& ox, float& oy) {
    const float c = cosf(swish);
    const float s = sinf(swish);
    ox = px + x * c - y * s;
    oy = py + x * s + y * c;
  };
  float t1x, t1y, t2x, t2y, t3x, t3y, t4x, t4y;
  rot(14, -1, t1x, t1y);
  rot(25, -14, t2x, t2y);
  rot(19, -28, t3x, t3y);
  rot(8, -12, t4x, t4y);
  lpTriF(px, py, t1x, t1y, t4x, t4y, orange);
  lpTriF(t1x, t1y, t2x, t2y, t4x, t4y, shade);
  lpTriF(t4x, t4y, t2x, t2y, t3x, t3y, orange);
  lpTriF(t2x + (t3x - t2x) * 0.45f, t2y + (t3y - t2y) * 0.45f, t3x, t3y,
         t4x + (t3x - t4x) * 0.45f, t4y + (t3y - t4y) * 0.45f, white);

  // Body and chest.
  const int top = fy - 24 - static_cast<int>(breathe);
  lpTri(fx - 12, fy, fx + 12, fy, fx + 4, top, orange);
  lpTri(fx + 4, top, fx + 12, fy, fx + 3, fy, shade);
  lpTri(fx - 10, fy - 1, fx - 1, fy - 3, fx - 4, top + 3, white);
  lpTri(fx - 9, fy, fx - 5, fy, fx - 7, fy - 11, dark);
  lpTri(fx - 3, fy, fx + 1, fy, fx - 1, fy - 11, dark);

  // Head: looks up for a moment each minute, droops while asleep.
  const bool lookingUp = now - lpLookUpAt < 1600 && !lpAsleep;
  const int hx = fx - 4;
  const int hy = top - 6 + (lpAsleep ? 8 : 0) - (lookingUp ? 2 : 0);
  const int lift = lookingUp ? -3 : 0;
  const bool earFlick = !deadlineReached(now, lpEarUntil);
  lpTri(hx - 9, hy - 4, hx + 7, hy - 6, hx + 2, hy + 6, orange);
  lpTri(hx + 7, hy - 6, hx + 8, hy + 4, hx + 2, hy + 6, shade);
  lpTri(hx - 9, hy - 4, hx + 2, hy + 6, hx - 12, hy + 4, white);
  lpTri(hx - 9, hy - 4, hx - 22, hy + 2 + lift, hx - 12, hy + 4, shade);
  lpTri(hx - 12, hy + 4, hx - 22, hy + 2 + lift, hx - 13, hy + 7, white);
  lpTri(hx - 24, hy + 1 + lift, hx - 20, hy + 1 + lift, hx - 22, hy + 4 + lift,
        dark);
  // Ears; the back ear flicks now and then.
  lpTri(hx - 8, hy - 4, hx - 6, hy - 18, hx - 1, hy - 5, orange);
  lpTri(hx - 6, hy - 5, hx - 5, hy - 14, hx - 3, hy - 5, dark);
  const int earTipX = hx + 5 + (earFlick ? 4 : 0);
  const int earTipY = hy - 19 + (earFlick ? 5 : 0);
  lpTri(hx + 1, hy - 5, earTipX, earTipY, hx + 7, hy - 6, shade);
  lpTri(hx + 3, hy - 6, earTipX, earTipY + 4, hx + 6, hy - 6, dark);
  // Eye.
  if (!lpAsleep && deadlineReached(now, lpNextBlink)) {
    lpBlinkUntil = now + 140;
    lpNextBlink = now + random(2400, 6000);
  }
  const int ex = hx - 7;
  const int ey = hy - 1 + lift / 2;
  if (lpAsleep || !deadlineReached(now, lpBlinkUntil)) {
    faceSprite.drawLine(SX(ex - 2), SY(ey), SX(ex + 2), SY(ey + 1), dark);
  } else {
    lpTri(ex - 2, ey - 1, ex + 2, ey - 2, ex, ey + 2, dark);
    faceSprite.drawPixel(SX(ex), SY(ey - 1), white);
  }
  if (lpAsleep) {
    useFont(faceSprite, 2);
    faceSprite.setTextDatum(MC_DATUM);
    for (int k = 0; k < 3; ++k) {
      const float phase = fmodf(t * 0.3f + k / 3.0f, 1.0f);
      const int zx = hx + 6 + static_cast<int>(phase * 14.0f);
      const int zy = hy - 14 - static_cast<int>(phase * 24.0f);
      faceSprite.setTextColor(blendColor(TEXT_COLOR, sky565(skyRows[min(zy, 169)]),
                                         static_cast<uint8_t>(sinf(PI * phase) *
                                                              220.0f)));
      faceSprite.drawString("z", SX(zx), SY(zy));
    }
  }
}

void lpDrawObjects() {
  int index = 0;
  for (; index < lpCount && lpTris[index].layer == LP_SKY; ++index) {
    if (lpTriHits(lpTris[index])) {
      lpFillTri(lpTris[index], lpColor(lpTris[index]));
    }
  }
  lpDrawStars();
  lpDrawBody();
  for (; index < lpCount; ++index) {
    if (lpTriHits(lpTris[index])) {
      lpFillTri(lpTris[index], lpColor(lpTris[index]));
    }
  }
  lpDrawPine(18, LP_GROUND_TOP + 6, 40);
  lpDrawPine(40, LP_GROUND_TOP + 10, 50);
  lpDrawPine(64, LP_GROUND_TOP + 7, 34);
  lpDrawPine(304, LP_GROUND_TOP + 8, 42);
  lpDrawFox();
  skyDrawText();
  stripDrawDots(sky565(cGrassLow));
}

void lpRowPixels(uint16_t* line, int, int y, int width) {
  // Every pixel is covered by a triangle; this is just a safe underlay.
  const uint16_t color = swap16(sky565(skyRows[min(y, SKY_LAKE_TOP - 1)]));
  for (int col = 0; col < width; ++col) {
    line[col] = color;
  }
}

void lpRenderAll(const struct tm& localTime, const struct timeval& now,
                 bool synced) {
  skyPrepare(localTime, now, synced);
  lpAsleep = synced && buddyAsleep(localTime);
  skyRender(0, 0, SCREEN_W, SCREEN_H);
}

bool enterLowPolyFace() {
  if (!allocateFaceSprite(SCREEN_W, SKY_STRIP_H)) {
    return false;
  }
  stripRow = lpRowPixels;
  stripDraw = lpDrawObjects;
  buildSkyTables();
  lpBuild();
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  skyT = millis() / 1000.0f;
  lpNextBlink = millis() + 1500;
  lpLookUpAt = millis();
  lpRenderAll(localTime, now, synced);
  char key[8] = "sync";
  if (synced) {
    strftime(key, sizeof(key), "%H:%M", &localTime);
  }
  faceRenderedTime = key;
  faceRenderedClimate = climateKey();
  return true;
}

void updateLowPolyFace(uint32_t nowMs) {
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  char key[8] = "sync";
  if (synced) {
    strftime(key, sizeof(key), "%H:%M", &localTime);
  }
  skyT = nowMs / 1000.0f;
  if (faceRenderedTime != key) {
    faceRenderedTime = key;
    faceRenderedClimate = climateKey();
    lpLookUpAt = nowMs;
    lpRenderAll(localTime, now, synced);
    return;
  }
  const String climate = climateKey();
  if (faceRenderedClimate != climate) {
    faceRenderedClimate = climate;
    skyRender(178, 4, SCREEN_W, 76);
  }
  if (!lpAsleep && deadlineReached(nowMs, lpNextEar)) {
    lpEarUntil = nowMs + 220;
    lpNextEar = nowMs + random(3000, 9000);
  }
  ++lpFrame;
  // A rolling refresh: one strip per frame, so the whole mesh slowly
  // breathes and glints without redrawing the screen at once.
  skyRender(0, lpRollingStrip * SKY_STRIP_H, SCREEN_W,
            (lpRollingStrip + 1) * SKY_STRIP_H);
  lpRollingStrip = (lpRollingStrip + 1) % (SCREEN_H / SKY_STRIP_H);
  if (lpFrame % 2 == 0) {
    skyRender(lpFoxBox());
  }
}

// ---- Pixel: an 8-bit city at 4x4 pixel "cells". A person walks a dog down
// the sidewalk, cars pass, windows light up at night, neon signs flicker
// and the big clock uses a blocky 3x5 font. ---------------------------------
constexpr int PX = 4;
constexpr int PX_W = SCREEN_W / PX;
constexpr int PX_H = SCREEN_H / PX;
constexpr int PX_SIDEWALK = 51;  // cell rows 51-54
constexpr int PX_ROAD = 55;      // cell rows 55-59
constexpr int PX_MAX_BUILDINGS = 14;

struct PxBuilding {
  uint8_t left;
  uint8_t width;
  uint8_t top;
  uint8_t palette;
  uint8_t extras;  // bit 0 water tank, bit 1 antenna, bit 2 neon sign
};

PxBuilding pxBuildings[PX_MAX_BUILDINGS];
int pxBuildingCount = 0;
uint8_t pxNearTop[PX_W];
int8_t pxNearIndex[PX_W];
uint8_t pxFarTop[PX_W];
bool pxReady = false;
int pxRollingStrip = 0;
float pxWalkX = 20.0f;
int pxWalkDir = 1;
uint8_t pxWalkFrame = 0;
uint32_t pxWalkStepAt = 0;
SkyBox pxWalkDrawn;
uint8_t pxWalkDrawnFrame = 0;
bool pxCarActive = false;
float pxCarX = 0.0f;
int pxCarDir = 1;
uint8_t pxCarPalette = 0;
uint32_t pxCarNextAt = 0;
SkyBox pxCarDrawn;
int pxCloudX[2] = {6, 46};
uint32_t pxCloudStepAt = 0;
SkyBox pxCloudDrawn[2];
int pxShownSecond = -1;
float pxLastT = 0.0f;
char pxClimateTemp[8] = "--";
char pxClimateHum[8] = "";

const SkyRgb PX_FACADES[] = {{178, 84, 72},  {72, 140, 150}, {214, 170, 110},
                             {132, 98, 172}, {98, 112, 142}, {112, 178, 140}};
const SkyRgb PX_CARS[] = {{232, 72, 72}, {72, 150, 232}, {250, 200, 70},
                          {240, 240, 240}};

// 3x5 glyphs, one row per byte (bit 2 = left column).
const uint8_t* pxGlyph(char c) {
  static const uint8_t digits[10][5] = {
      {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7},
      {5, 5, 7, 1, 1}, {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 2, 2},
      {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7}};
  static const uint8_t letters[26][5] = {
      {2, 5, 7, 5, 5}, {6, 5, 6, 5, 6}, {3, 4, 4, 4, 3}, {6, 5, 5, 5, 6},
      {7, 4, 6, 4, 7}, {7, 4, 6, 4, 4}, {3, 4, 5, 5, 3}, {5, 5, 7, 5, 5},
      {7, 2, 2, 2, 7}, {1, 1, 1, 5, 2}, {5, 5, 6, 5, 5}, {4, 4, 4, 4, 7},
      {5, 7, 7, 5, 5}, {6, 5, 5, 5, 5}, {2, 5, 5, 5, 2}, {6, 5, 6, 4, 4},
      {2, 5, 5, 6, 3}, {6, 5, 6, 5, 5}, {3, 4, 2, 1, 6}, {7, 2, 2, 2, 2},
      {5, 5, 5, 5, 7}, {5, 5, 5, 5, 2}, {5, 5, 7, 7, 5}, {5, 5, 2, 5, 5},
      {5, 5, 2, 2, 2}, {7, 1, 2, 4, 7}};
  static const uint8_t percent[5] = {5, 1, 2, 4, 5};
  static const uint8_t colon[5] = {0, 2, 0, 2, 0};
  static const uint8_t dash[5] = {0, 0, 7, 0, 0};
  static const uint8_t degree[5] = {2, 5, 2, 0, 0};
  static const uint8_t blank[5] = {0, 0, 0, 0, 0};
  if (c >= '0' && c <= '9') {
    return digits[c - '0'];
  }
  if (c >= 'A' && c <= 'Z') {
    return letters[c - 'A'];
  }
  if (c >= 'a' && c <= 'z') {
    return letters[c - 'a'];
  }
  switch (c) {
    case '%':
      return percent;
    case ':':
      return colon;
    case '-':
      return dash;
    case '*':
      return degree;
    default:
      return blank;
  }
}

int pxTextCells(const char* text, int scale) {
  const int length = strlen(text);
  return length > 0 ? length * 4 * scale - scale : 0;
}

// Fills one logical cell (or a block of them) in the current strip.
void pxFill(int cx, int cy, int cw, int ch, uint16_t color) {
  faceSprite.fillRect(SX(cx * PX), SY(cy * PX), cw * PX, ch * PX, color);
}

void pxDrawText(const char* text, int cx, int cy, int scale, uint16_t color,
                uint16_t shadow) {
  const int width = pxTextCells(text, scale);
  if (!skyHits(cx * PX, cy * PX, (cx + width + 1) * PX,
               (cy + 5 * scale + 1) * PX)) {
    return;
  }
  for (int pass = 0; pass < 2; ++pass) {
    const int offset = pass == 0 ? 1 : 0;
    const uint16_t ink = pass == 0 ? shadow : color;
    int x = cx;
    for (const char* p = text; *p; ++p) {
      const uint8_t* glyph = pxGlyph(*p);
      for (int row = 0; row < 5; ++row) {
        for (int col = 0; col < 3; ++col) {
          if (glyph[row] & (4 >> col)) {
            pxFill(x + col * scale + offset, cy + row * scale + offset, scale,
                   scale, ink);
          }
        }
      }
      x += 4 * scale;
    }
  }
}

void pxBuild() {
  if (pxReady) {
    return;
  }
  randomSeed(808);
  for (int cx = 0; cx < PX_W; ++cx) {
    pxNearTop[cx] = PX_SIDEWALK;
    pxNearIndex[cx] = -1;
  }
  // Far skyline: tall, dark blocks.
  for (int cx = 0; cx < PX_W;) {
    const int width = random(3, 7);
    const int top = random(22, 36);
    for (int k = 0; k < width && cx < PX_W; ++k, ++cx) {
      pxFarTop[cx] = static_cast<uint8_t>(top);
    }
  }
  // Near buildings with facades, separated by small gaps.
  pxBuildingCount = 0;
  for (int cx = random(0, 3); cx < PX_W && pxBuildingCount < PX_MAX_BUILDINGS;) {
    PxBuilding& b = pxBuildings[pxBuildingCount];
    b.left = static_cast<uint8_t>(cx);
    b.width = static_cast<uint8_t>(random(7, 13));
    b.top = static_cast<uint8_t>(random(30, 43));
    b.palette = static_cast<uint8_t>(random(0, 6));
    b.extras = static_cast<uint8_t>(random(0, 8));
    for (int k = 0; k < b.width && cx + k < PX_W; ++k) {
      pxNearTop[cx + k] = b.top;
      pxNearIndex[cx + k] = static_cast<int8_t>(pxBuildingCount);
    }
    cx += b.width + random(0, 3);
    ++pxBuildingCount;
  }
  randomSeed(esp_random());
  pxReady = true;
}

bool pxNight() { return skyDay < 0.45f; }

uint16_t pxSkyCell(int cx, int cy) {
  const int band = cy / 5;
  SkyRgb c = skyRows[min(SKY_LAKE_TOP - 1, band * 20 + 10)];
  if (cy % 5 == 4 && ((cx + cy) & 1)) {
    c = skyRows[min(SKY_LAKE_TOP - 1, (band + 1) * 20 + 10)];  // dithered seam
  }
  const int bx = skyBodyX / PX;
  const int by = skyBodyY / PX;
  const int dx = cx - bx;
  const int dy = cy - by;
  const int d2 = dx * dx + dy * dy;
  if (skySunUp) {
    if (d2 <= 5) {
      return rgb565(255, 236, 120);
    }
    if (d2 <= 10) {
      return rgb565(255, 176, 72);
    }
    if (d2 <= 26 && ((cx + cy) & 1)) {
      c = skyMix(c, skyGlowColor, 0.45f);
    }
  } else {
    if (d2 <= 8 && skyMoonLight >= 0.08f) {
      const int dir = skyMoonPhase < 0.5f ? -1 : 1;
      const int shift = static_cast<int>(6.0f * (1.0f - skyMoonLight)) + 1;
      const int sx = cx - (bx + dir * shift);
      if (skyMoonLight > 0.93f || sx * sx + dy * dy > 8) {
        return rgb565(240, 236, 210);
      }
    }
    if (d2 <= 20 && ((cx + cy) & 1) && skyMoonLight >= 0.08f) {
      c = skyMix(c, {120, 130, 180}, 0.3f);
    }
  }
  if (skyNight > 0.2f && cy < 40) {
    const uint32_t h = skyHash(cx, cy, 21);
    if (h % 41 == 0) {
      const float twinkle = sinf(skyT * (1.0f + (h >> 8) % 3) + (h >> 12) % 7);
      return twinkle > 0.2f ? rgb565(250, 250, 255) : rgb565(130, 136, 180);
    }
  }
  return sky565(c);
}

uint16_t pxCellColor(int cx, int cy) {
  const bool night = pxNight();
  const float day = skyDay;
  if (cy >= PX_ROAD) {
    if (cy == PX_ROAD) {
      return sky565(skyMix({60, 60, 74}, {128, 128, 138}, day));  // curb
    }
    if (cy == PX_ROAD + 2 && (cx % 6) < 3) {
      return sky565(skyMix({120, 100, 40}, {240, 200, 70}, day));
    }
    return sky565(skyMix({20, 20, 28}, {58, 60, 70}, day));
  }
  // Street lamps stand in front of the buildings.
  const int lampX = ((cx + 4) / 20) * 20 + 6;
  if (cx == lampX && cy >= 44 && cy < PX_SIDEWALK) {
    return sky565(skyMix({70, 72, 92}, {60, 62, 74}, day));
  }
  if ((cx == lampX || cx == lampX + 1) && cy == 43) {
    return night ? rgb565(255, 226, 140) : rgb565(150, 156, 170);
  }
  SkyRgb c;
  if (cy >= PX_SIDEWALK) {
    const bool seam = cx % 4 == 0 || cy == PX_SIDEWALK;
    c = skyMix({46, 46, 58}, {172, 172, 180}, day);
    if (seam) {
      c = skyMix(c, {0, 0, 0}, 0.18f);
    }
  } else if (cy >= pxNearTop[cx] && pxNearIndex[cx] >= 0) {
    const PxBuilding& b = pxBuildings[pxNearIndex[cx]];
    const int lx = cx - b.left;
    const int ly = cy - b.top;
    const SkyRgb facade = PX_FACADES[b.palette];
    c = skyMix({static_cast<int16_t>(facade.r / 5),
                static_cast<int16_t>(facade.g / 5),
                static_cast<int16_t>(facade.b / 4)},
               facade, day);
    if (ly == 0) {
      c = skyMix(c, {255, 255, 255}, 0.18f);  // roof ledge
    } else if (lx == 0 || lx == b.width - 1) {
      c = skyMix(c, {0, 0, 0}, 0.2f);  // corners
    } else if ((b.extras & 4) && ly >= 2 && ly <= 3 && lx >= b.width / 2 - 2 &&
               lx <= b.width / 2 + 1) {
      const bool on = !night || fmodf(skyT + b.left, 2.3f) < 1.8f;
      const SkyRgb neon = (b.palette & 1) ? SkyRgb{255, 80, 170}
                                          : SkyRgb{80, 240, 255};
      c = on ? (night ? neon : skyMix(neon, c, 0.5f)) : skyMix(neon, c, 0.85f);
    } else if (cy >= PX_SIDEWALK - 3 && lx == b.width / 2) {
      c = skyMix({20, 14, 18}, {90, 60, 44}, day);  // door
    } else if (ly >= 5 && cy < PX_SIDEWALK - 3 && (lx - 1) % 3 < 2 &&
               (ly - 5) % 3 < 2) {
      if (night) {
        const int slot = static_cast<int>(skyT / 35.0f) + (lx * 7 + ly) % 11;
        const uint32_t h = skyHash(b.left * 31 + (lx - 1) / 3, (ly - 5) / 3,
                                   slot);
        const int threshold = skyHourNow >= 0.5f && skyHourNow < 5.0f ? 1 : 3;
        c = static_cast<int>(h & 3) < threshold
            ? ((h >> 4) & 1 ? SkyRgb{255, 214, 120} : SkyRgb{255, 236, 170})
            : SkyRgb{30, 30, 46};
      } else {
        const bool shine = (lx - 1) % 3 == 0 && (ly - 5) % 3 == 0;
        c = shine ? SkyRgb{220, 240, 255} : SkyRgb{140, 190, 226};
      }
    }
  } else if (pxNearIndex[cx] >= 0 &&
             (pxBuildings[pxNearIndex[cx]].extras & 1) &&
             cy >= pxNearTop[cx] - 3 &&
             cx - pxBuildings[pxNearIndex[cx]].left >= 2 &&
             cx - pxBuildings[pxNearIndex[cx]].left <= 4) {
    c = skyMix({30, 20, 20}, {120, 82, 60}, day);  // water tank
  } else if (pxNearIndex[cx] >= 0 && (pxBuildings[pxNearIndex[cx]].extras & 2) &&
             cx - pxBuildings[pxNearIndex[cx]].left ==
                 pxBuildings[pxNearIndex[cx]].width - 3 &&
             cy >= pxNearTop[cx] - 5) {
    const bool tip = cy == pxNearTop[cx] - 5;
    if (tip && night && fmodf(skyT, 1.4f) < 0.7f) {
      return rgb565(255, 60, 60);
    }
    c = skyMix({30, 30, 40}, {80, 84, 96}, day);  // antenna
  } else if (cy >= pxFarTop[cx]) {
    c = skyMix({40, 38, 78}, {110, 126, 168}, day);
    c = skyMix(c, cHaze, 0.2f);
    if (night && (cx & 1) == 0 && cy % 3 == 1 &&
        skyHash(cx, cy, 33) % 10 == 0) {
      c = {150, 130, 80};  // a few distant lit windows
    }
  } else {
    return pxSkyCell(cx, cy);
  }
  if (night) {
    const int dx = cx - lampX;
    const int dy = cy - 43;
    if (dx * dx + dy * dy <= 5) {
      c = skyMix(c, {255, 200, 110}, 0.3f);
    }
  }
  return sky565(c);
}

void pxRowPixels(uint16_t* line, int x0, int y, int width) {
  const int cy = y / PX;
  int col = 0;
  while (col < width) {
    const int cx = (x0 + col) / PX;
    const uint16_t color = swap16(pxCellColor(cx, cy));
    const int end = min(width, (cx + 1) * PX - x0);
    for (; col < end; ++col) {
      line[col] = color;
    }
  }
}

// Sprites: '.' clear, letters pick a colour.
const char* PX_WALKER[2][9] = {
    {".HHH.", "HHSE.", ".SSS.", ".TTT.", "TTTTT", "STTTS", ".PPP.", ".P.P.",
     ".B.B."},
    {".HHH.", "HHSE.", ".SSS.", ".TTT.", "TTTTT", "STTTS", ".PPP.", "P...P",
     "B...B"}};
const char* PX_DOG[2][3] = {{"T...DE", ".DDDDD", ".D..D."},
                            {"....DE", "TDDDDD", "D....D"}};
const char* PX_CAR[4] = {"..CCCC....", ".CWWCWWC..", "CCCCCCCCCL",
                         ".KK....KK."};
const char* PX_CLOUD[3] = {"..XXX...", ".XXXXXX.", "XXXXXXXX"};

uint16_t pxPaint(char key) {
  const float day = 0.4f + 0.6f * skyDay;
  const auto tone = [day](uint8_t r, uint8_t g, uint8_t b) {
    return sky565({static_cast<int16_t>(r * day), static_cast<int16_t>(g * day),
                   static_cast<int16_t>(b * day)});
  };
  switch (key) {
    case 'H': return tone(90, 52, 30);
    case 'S': return tone(255, 204, 168);
    case 'E': return tone(30, 24, 30);
    case 'T': return tone(232, 70, 84);
    case 'P': return tone(56, 84, 160);
    case 'B': return tone(36, 30, 36);
    case 'D': return tone(214, 160, 96);
    case 'W': return pxNight() ? rgb565(255, 230, 160) : tone(170, 214, 240);
    case 'K': return tone(24, 24, 28);
    case 'L': return pxNight() ? rgb565(255, 250, 200) : tone(255, 220, 120);
    default: return TEXT_COLOR;
  }
}

void pxDrawSprite(const char* const* rows, int count, int cx, int cy,
                  bool mirror, uint16_t bodyColor) {
  const int width = strlen(rows[0]);
  for (int row = 0; row < count; ++row) {
    for (int col = 0; col < width; ++col) {
      const char key = rows[row][mirror ? width - 1 - col : col];
      if (key == '.') {
        continue;
      }
      pxFill(cx + col, cy + row, 1, 1,
             key == 'C' || key == 'X' ? bodyColor : pxPaint(key));
    }
  }
}

SkyBox pxCellBox(int cx, int cy, int cw, int ch) {
  SkyBox box;
  box.x0 = static_cast<int16_t>(cx * PX);
  box.y0 = static_cast<int16_t>(cy * PX);
  box.x1 = static_cast<int16_t>((cx + cw) * PX);
  box.y1 = static_cast<int16_t>((cy + ch) * PX);
  box.valid = true;
  return box;
}

SkyBox pxWalkBox() {
  const int x = static_cast<int>(pxWalkX);
  const int dogX = x - pxWalkDir * 7;
  const int left = min(x, dogX) - 1;
  return pxCellBox(left, 44, 15, 10);
}

void pxDrawWalker() {
  const int x = static_cast<int>(pxWalkX);
  const SkyBox box = pxWalkBox();
  if (!skyHits(box.x0, box.y0, box.x1, box.y1)) {
    return;
  }
  const bool mirror = pxWalkDir < 0;
  const int dogX = x - pxWalkDir * 7;
  pxDrawSprite(PX_DOG[pxWalkFrame], 3, dogX, 51, mirror, 0);
  // Leash from hand to the dog's collar.
  const uint16_t leash = pxPaint('E');
  const int handX = x + (mirror ? 0 : 4);
  const int collarX = dogX + (mirror ? 1 : 4);
  const int steps = abs(handX - collarX);
  for (int k = 1; k < steps; ++k) {
    const int lx = handX + (collarX > handX ? k : -k);
    const int ly = 50 + (k * 1) / max(1, steps);
    pxFill(lx, ly, 1, 1, leash);
  }
  pxDrawSprite(PX_WALKER[pxWalkFrame], 9, x, 45, mirror, 0);
}

SkyBox pxCarBox() {
  const int x = static_cast<int>(pxCarX);
  return pxCellBox(x - (pxCarDir > 0 ? 0 : 6), 56, 17, 4);
}

void pxDrawCar() {
  if (!pxCarActive) {
    return;
  }
  const SkyBox box = pxCarBox();
  if (!skyHits(box.x0, box.y0, box.x1, box.y1)) {
    return;
  }
  const int x = static_cast<int>(pxCarX);
  const SkyRgb body = PX_CARS[pxCarPalette];
  const float day = 0.4f + 0.6f * skyDay;
  const uint16_t paint = sky565({static_cast<int16_t>(body.r * day),
                                 static_cast<int16_t>(body.g * day),
                                 static_cast<int16_t>(body.b * day)});
  if (pxNight()) {
    // Headlight beam on the road ahead.
    const int beamX = pxCarDir > 0 ? x + 10 : x - 6;
    for (int k = 0; k < 6; ++k) {
      if ((k + 58) & 1) {
        pxFill(beamX + k, 58, 1, 1, rgb565(150, 140, 90));
      }
      pxFill(beamX + k, 57 + (k & 1), 1, 1, rgb565(120, 112, 76));
    }
  }
  pxDrawSprite(PX_CAR, 4, x, 56, pxCarDir < 0, paint);
}

SkyBox pxCloudBox(int index) {
  return pxCellBox(pxCloudX[index], index == 0 ? 21 : 26, 8, 3);
}

void pxDrawClouds() {
  const SkyRgb tint = skyMix({70, 70, 110}, {250, 250, 255}, skyDay);
  const uint16_t color = sky565(skyMix(tint, {255, 190, 200}, skyGold * 0.5f));
  for (int i = 0; i < 2; ++i) {
    const SkyBox box = pxCloudBox(i);
    if (skyHits(box.x0, box.y0, box.x1, box.y1)) {
      pxDrawSprite(PX_CLOUD, 3, pxCloudX[i], i == 0 ? 21 : 26, false, color);
    }
  }
}

void pxDrawObjects() {
  pxDrawClouds();
  pxDrawCar();
  pxDrawWalker();
  const uint16_t shadow = sky565(skyMix({0, 0, 10}, skyRows[30], 0.35f));
  char timeText[6];
  strcpy(timeText, skyTimeText);
  const bool colonOn = (time(nullptr) & 1) == 0 || !timeIsSynced();
  if (!colonOn) {
    timeText[2] = ' ';
  }
  pxDrawText(timeText, 3, 3, 2, TEXT_COLOR, shadow);
  char date[24];
  strncpy(date, skyDateText, sizeof(date));
  date[sizeof(date) - 1] = '\0';
  for (char* p = date; *p; ++p) {
    if (*p == ',') {
      *p = ' ';
    }
  }
  // "Sun  08 Nov" -> "SUN 08 NOV"
  char compact[24];
  int n = 0;
  for (const char* p = date; *p && n < 23; ++p) {
    if (*p == ' ' && n > 0 && compact[n - 1] == ' ') {
      continue;
    }
    compact[n++] = *p;
  }
  compact[n] = '\0';
  pxDrawText(compact, 3, 15, 1, rgb565(255, 214, 120), shadow);
  // Climate in a little game-style HUD box.
  const int tempWidth = pxTextCells(pxClimateTemp, 1);
  const int humWidth = pxTextCells(pxClimateHum, 1);
  const int boxX = PX_W - 6 - max(tempWidth, humWidth);
  if (skyHits(boxX * PX, PX, SCREEN_W, 16 * PX)) {
    const uint16_t panel = rgb565(18, 20, 38);
    pxFill(boxX, 1, PX_W - 1 - boxX, 15, rgb565(96, 108, 150));
    pxFill(boxX + 1, 2, PX_W - 3 - boxX, 13, panel);
    pxDrawText(pxClimateTemp, PX_W - 3 - tempWidth, 3, 1, TEXT_COLOR, panel);
    pxDrawText(pxClimateHum, PX_W - 3 - humWidth, 9, 1, rgb565(140, 200, 255),
               panel);
  }
  stripDrawDots(rgb565(40, 40, 50));
}

void pxUpdateClimateText() {
  if (climateAvailable) {
    snprintf(pxClimateTemp, sizeof(pxClimateTemp), "%s*C",
             temperatureValue.c_str());
    snprintf(pxClimateHum, sizeof(pxClimateHum), "%s%%",
             humidityValue.c_str());
  } else {
    strcpy(pxClimateTemp, "--");
    pxClimateHum[0] = '\0';
  }
}

void pxRenderAll(const struct tm& localTime, const struct timeval& now,
                 bool synced) {
  skyPrepare(localTime, now, synced);
  pxUpdateClimateText();
  skyRender(0, 0, SCREEN_W, SCREEN_H);
  pxWalkDrawn = pxWalkBox();
  pxWalkDrawnFrame = pxWalkFrame;
  pxCarDrawn = pxCarActive ? pxCarBox() : SkyBox();
  pxCloudDrawn[0] = pxCloudBox(0);
  pxCloudDrawn[1] = pxCloudBox(1);
}

bool enterPixelFace() {
  if (!allocateFaceSprite(SCREEN_W, SKY_STRIP_H)) {
    return false;
  }
  stripRow = pxRowPixels;
  stripDraw = pxDrawObjects;
  buildSkyTables();
  pxBuild();
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  skyT = millis() / 1000.0f;
  pxCarActive = false;
  pxCarNextAt = millis() + random(3000, 8000);
  pxRenderAll(localTime, now, synced);
  pxShownSecond = now.tv_sec & 1;
  char key[8] = "sync";
  if (synced) {
    strftime(key, sizeof(key), "%H:%M", &localTime);
  }
  faceRenderedTime = key;
  faceRenderedClimate = climateKey();
  return true;
}

void updatePixelFace(uint32_t nowMs) {
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  char key[8] = "sync";
  if (synced) {
    strftime(key, sizeof(key), "%H:%M", &localTime);
  }
  skyT = nowMs / 1000.0f;

  // Move things one whole cell at a time, like a real 8-bit game.
  if (deadlineReached(nowMs, pxWalkStepAt)) {
    pxWalkStepAt = nowMs + 230;
    pxWalkX += pxWalkDir;
    pxWalkFrame ^= 1;
    if (pxWalkX > PX_W - 7) {
      pxWalkDir = -1;
    } else if (pxWalkX < 9) {
      pxWalkDir = 1;
    }
  }
  if (pxCarActive) {
    pxCarX += pxCarDir * 0.9f;
    if (pxCarX > PX_W + 2 || pxCarX < -12) {
      pxCarActive = false;
      pxCarNextAt = nowMs + random(4000, 14000);
    }
  } else if (deadlineReached(nowMs, pxCarNextAt)) {
    pxCarActive = true;
    pxCarDir = random(0, 2) ? 1 : -1;
    pxCarX = pxCarDir > 0 ? -11 : PX_W + 1;
    pxCarPalette = static_cast<uint8_t>(random(0, 4));
  }
  bool cloudsMoved = false;
  if (deadlineReached(nowMs, pxCloudStepAt)) {
    pxCloudStepAt = nowMs + 1300;
    for (int i = 0; i < 2; ++i) {
      ++pxCloudX[i];
      if (pxCloudX[i] > PX_W) {
        pxCloudX[i] = -8;
      }
    }
    cloudsMoved = true;
  }

  if (faceRenderedTime != key) {
    faceRenderedTime = key;
    faceRenderedClimate = climateKey();
    pxRenderAll(localTime, now, synced);
    return;
  }
  const String climate = climateKey();
  if (faceRenderedClimate != climate) {
    faceRenderedClimate = climate;
    pxUpdateClimateText();
    skyRender(PX_W * PX / 2, 0, SCREEN_W, 17 * PX);
  }
  const int second = now.tv_sec & 1;
  if (second != pxShownSecond) {
    pxShownSecond = second;
    skyRender(19 * PX, 3 * PX, 26 * PX, 14 * PX);  // blink the colon
  }
  skyRender(0, pxRollingStrip * SKY_STRIP_H, SCREEN_W,
            (pxRollingStrip + 1) * SKY_STRIP_H);
  pxRollingStrip = (pxRollingStrip + 1) % (SCREEN_H / SKY_STRIP_H);

  const SkyBox walk = pxWalkBox();
  if (walk.x0 != pxWalkDrawn.x0 || pxWalkFrame != pxWalkDrawnFrame) {
    skyRenderMoved(pxWalkDrawn, walk);
    pxWalkDrawnFrame = pxWalkFrame;
  }
  if (pxCarActive || pxCarDrawn.valid) {
    skyRenderMoved(pxCarDrawn, pxCarActive ? pxCarBox() : SkyBox());
  }
  if (cloudsMoved) {
    for (int i = 0; i < 2; ++i) {
      skyRenderMoved(pxCloudDrawn[i], pxCloudBox(i));
    }
  }
}

// ---- Analog: a watch dial with tapered hands and a smooth sweeping
// second hand. Only the hand area is redrawn, through a sprite. ------------
constexpr int ANALOG_CX = 114;
constexpr int ANALOG_CY = 120;
constexpr int ANALOG_HALF = 68;  // sprite covers the hands
constexpr int ANALOG_PANEL_X = 232;
constexpr uint16_t ANALOG_TOP = rgb565(16, 34, 50);
constexpr uint16_t ANALOG_BOTTOM = rgb565(4, 9, 16);
constexpr uint16_t ANALOG_RING = rgb565(52, 66, 88);
constexpr uint16_t ANALOG_DIAL = rgb565(20, 28, 42);
constexpr uint16_t ANALOG_MINOR = rgb565(92, 104, 126);
constexpr uint16_t ANALOG_MAJOR = rgb565(226, 230, 240);
constexpr uint16_t ANALOG_HAND = rgb565(240, 242, 248);
constexpr uint16_t ANALOG_SECOND = rgb565(255, 120, 96);
constexpr uint32_t ANALOG_FRAME_MS = 50;
uint32_t analogLastFrameAt = 0;

void polarPoint(int cx, int cy, float degrees, float radius, float& x,
                float& y) {
  const float radians = degrees * DEG_TO_RAD;
  x = cx + sinf(radians) * radius;
  y = cy - cosf(radians) * radius;
}

// Draws the dial (ring, ticks, numerals, date window) centred at (cx, cy)
// on either the screen or the hands sprite, so both always match.
void drawAnalogDial(TFT_eSPI& g, int cx, int cy, uint16_t outside,
                    const char* dateWindow) {
  g.fillSmoothCircle(cx, cy, 100, ANALOG_RING, outside);
  g.fillSmoothCircle(cx, cy, 96, ANALOG_DIAL, ANALOG_RING);
  for (int i = 0; i < 60; ++i) {
    const bool major = i % 5 == 0;
    float x1, y1, x2, y2;
    polarPoint(cx, cy, i * 6.0f, major ? 77.0f : 86.0f, x1, y1);
    polarPoint(cx, cy, i * 6.0f, 91.0f, x2, y2);
    const uint16_t color = i % 15 == 0 ? ANALOG_SECOND
                           : major      ? ANALOG_MAJOR
                                        : ANALOG_MINOR;
    g.drawWideLine(x1, y1, x2, y2, major ? 3.0f : 1.2f, color, ANALOG_DIAL);
  }
  useFreeFont(g, &FreeSansBold12pt7b);
  g.setTextDatum(MC_DATUM);
  g.setTextColor(ANALOG_MAJOR);
  g.drawString("12", cx, cy - 60);
  g.drawString("3", cx + 62, cy + 1);
  g.drawString("6", cx, cy + 62);
  g.drawString("9", cx - 62, cy + 1);
  // Date window below the centre.
  const uint16_t window = blendColor(TFT_BLACK, ANALOG_DIAL, 120);
  g.fillSmoothRoundRect(cx - 24, cy + 26, 48, 20, 6, window, ANALOG_DIAL);
  useFont(g, 2);
  g.setTextColor(ANALOG_SECOND);
  g.drawString(dateWindow, cx, cy + 36);
}

void drawAnalogHand(float degrees, float length, float tail, float baseRadius,
                    float tipRadius, uint16_t color) {
  constexpr int c = ANALOG_HALF;
  float tipX, tipY, tailX, tailY;
  polarPoint(c, c, degrees, length, tipX, tipY);
  polarPoint(c, c, degrees + 180.0f, tail, tailX, tailY);
  const uint16_t shadow = blendColor(TFT_BLACK, ANALOG_DIAL, 130);
  faceSprite.drawWedgeLine(tailX + 2, tailY + 3, tipX + 2, tipY + 3,
                           baseRadius, tipRadius, shadow, ANALOG_DIAL);
  faceSprite.drawWedgeLine(tailX, tailY, tipX, tipY, baseRadius, tipRadius,
                           color, ANALOG_DIAL);
}

void analogDateWindow(const struct tm& localTime, bool synced, char* out,
                      size_t size) {
  if (synced) {
    strftime(out, size, "%a %d", &localTime);
    for (char* c = out; *c; ++c) {
      *c = toupper(*c);
    }
  } else {
    snprintf(out, size, "--");
  }
}

void drawAnalogHands(const struct timeval& now, const struct tm& localTime,
                     bool synced) {
  char window[12];
  analogDateWindow(localTime, synced, window, sizeof(window));
  drawAnalogDial(faceSprite, ANALOG_HALF, ANALOG_HALF, ANALOG_RING, window);
  const float seconds = localTime.tm_sec + now.tv_usec / 1000000.0f;
  const float minutes = localTime.tm_min + seconds / 60.0f;
  const float hours = (localTime.tm_hour % 12) + minutes / 60.0f;
  if (synced) {
    drawAnalogHand(hours * 30.0f, 42, 9, 4.0f, 1.8f, ANALOG_HAND);
    drawAnalogHand(minutes * 6.0f, 62, 11, 3.0f, 1.2f, ANALOG_HAND);
    float tipX, tipY, tailX, tailY;
    polarPoint(ANALOG_HALF, ANALOG_HALF, seconds * 6.0f, 66, tipX, tipY);
    polarPoint(ANALOG_HALF, ANALOG_HALF, seconds * 6.0f + 180.0f, 16, tailX,
               tailY);
    faceSprite.drawWideLine(tailX, tailY, tipX, tipY, 1.6f, ANALOG_SECOND,
                            ANALOG_DIAL);
    faceSprite.fillSmoothCircle(tailX, tailY, 3, ANALOG_SECOND, ANALOG_DIAL);
  }
  faceSprite.fillSmoothCircle(ANALOG_HALF, ANALOG_HALF, 5, ANALOG_SECOND,
                              ANALOG_DIAL);
  faceSprite.fillSmoothCircle(ANALOG_HALF, ANALOG_HALF, 2, ANALOG_DIAL,
                              ANALOG_SECOND);
  faceSprite.pushSprite(ANALOG_CX - ANALOG_HALF, ANALOG_CY - ANALOG_HALF);
}

void drawAnalogPanel(const struct tm& localTime, bool synced) {
  drawBackdrop(ANALOG_PANEL_X - 2, 30, SCREEN_W - ANALOG_PANEL_X, 104);
  char weekday[12] = "SYNCING";
  char day[4] = "--";
  char monthYear[12] = "";
  if (synced) {
    strftime(weekday, sizeof(weekday), "%A", &localTime);
    strftime(day, sizeof(day), "%d", &localTime);
    strftime(monthYear, sizeof(monthYear), "%b %Y", &localTime);
    for (char* c = weekday; *c; ++c) {
      *c = toupper(*c);
    }
    for (char* c = monthYear; *c; ++c) {
      *c = toupper(*c);
    }
  }
  tft.setTextDatum(TL_DATUM);
  useFreeFont(tft, &FreeSansBold9pt7b);
  tft.setTextColor(MUTED_COLOR);
  tft.drawString(weekday, ANALOG_PANEL_X, 36);
  useFreeFont(tft, &FreeSansBold24pt7b);
  tft.setTextColor(TEXT_COLOR);
  tft.drawString(day, ANALOG_PANEL_X - 2, 58);
  useFreeFont(tft, &FreeSansBold9pt7b);
  tft.setTextColor(ANALOG_SECOND);
  tft.drawString(monthYear, ANALOG_PANEL_X, 106);
}

void drawAnalogClimate() {
  drawBackdrop(ANALOG_PANEL_X - 2, 136, SCREEN_W - ANALOG_PANEL_X, 82);
  tft.drawFastHLine(ANALOG_PANEL_X, 140, SCREEN_W - ANALOG_PANEL_X - 12,
                    blendColor(TEXT_COLOR, backdropAt(140), 40));
  useFreeFont(tft, &FreeSansBold12pt7b);
  drawThermometerIcon(ANALOG_PANEL_X + 6, 166, TEMP_COLOR, backdropAt(166));
  drawTemperatureText(climateAvailable ? temperatureValue : "--",
                      ANALOG_PANEL_X + 20, 156, TEXT_COLOR, backdropAt(160),
                      climateAvailable);
  drawDropletIcon(ANALOG_PANEL_X + 6, 200, HUMIDITY_COLOR, backdropAt(200));
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TEXT_COLOR);
  tft.drawString(climateAvailable ? humidityValue + "%" : "--",
                 ANALOG_PANEL_X + 20, 190);
}

bool enterAnalogFace() {
  if (!allocateFaceSprite(ANALOG_HALF * 2, ANALOG_HALF * 2)) {
    return false;
  }
  backdropTop = ANALOG_TOP;
  backdropBottom = ANALOG_BOTTOM;
  drawBackdrop(0, 0, SCREEN_W, SCREEN_H);
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  char window[12];
  analogDateWindow(localTime, synced, window, sizeof(window));
  drawAnalogDial(tft, ANALOG_CX, ANALOG_CY, backdropAt(ANALOG_CY), window);
  faceRenderedDate = "";
  faceRenderedClimate = "";
  analogLastFrameAt = 0;
  drawPageDots(backdropAt(233));
  return true;
}

void updateAnalogFace(uint32_t nowMs) {
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  const bool synced = timeIsSynced();
  char dateKey[12] = "sync";
  if (synced) {
    strftime(dateKey, sizeof(dateKey), "%Y%m%d", &localTime);
  }
  if (faceRenderedDate != dateKey) {
    faceRenderedDate = dateKey;
    char window[12];
    analogDateWindow(localTime, synced, window, sizeof(window));
    drawAnalogDial(tft, ANALOG_CX, ANALOG_CY, backdropAt(ANALOG_CY), window);
    drawAnalogPanel(localTime, synced);
  }
  const String climate = climateKey();
  if (faceRenderedClimate != climate) {
    faceRenderedClimate = climate;
    drawAnalogClimate();
  }
  if (nowMs - analogLastFrameAt >= ANALOG_FRAME_MS) {
    analogLastFrameAt = nowMs;
    drawAnalogHands(now, localTime, synced);
  }
}

// ---- face switching -------------------------------------------------------
void showClock() {
  beginSceneChange();  // also frees the previous face's sprite
  scene = Scene::Clock;
  accentColor = CLOCK_ACCENT_COLOR;
  lastClockFrame = 0;
  bool entered = true;
  switch (clockFace) {
    case FACE_BUDDY:
    case FACE_CAT:
    case FACE_ROBOT:
      buddyKind = clockFace == FACE_CAT ? BUDDY_CAT
                  : clockFace == FACE_ROBOT ? BUDDY_ROBOT : BUDDY_MOCHI;
      entered = enterBuddyFace();
      break;
    case FACE_LOWPOLY:
      entered = enterLowPolyFace();
      break;
    case FACE_PIXEL:
      entered = enterPixelFace();
      break;
    case FACE_SKY:
      entered = enterSkyFace();
      break;
    case FACE_ANALOG:
      entered = enterAnalogFace();
      break;
    default:
      clockFace = FACE_DIGITAL;
      enterDigitalFace();
      break;
  }
  if (!entered) {
    clockFace = FACE_DIGITAL;
    enterDigitalFace();
  }
  endSceneChange();
}

void updateClock(uint32_t nowMs) {
  if (scene != Scene::Clock || nowMs - lastClockFrame < FRAME_MS) {
    return;
  }
  lastClockFrame = nowMs;
  readClimate(nowMs);
  switch (clockFace) {
    case FACE_BUDDY:
    case FACE_CAT:
    case FACE_ROBOT:
      updateBuddyFace(nowMs);
      break;
    case FACE_LOWPOLY:
      updateLowPolyFace(nowMs);
      break;
    case FACE_PIXEL:
      updatePixelFace(nowMs);
      break;
    case FACE_SKY:
      updateSkyFace(nowMs);
      break;
    case FACE_ANALOG:
      updateAnalogFace(nowMs);
      break;
    default:
      updateDigitalFace();
      break;
  }
}

void nextClockFace() {
  clockFace = (clockFace + 1) % FACE_COUNT;
  displayPrefs.putUChar("face", clockFace);
  Serial.printf("Clock face %u (free %u, max block %u).\n", clockFace,
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
  showClock();
}

// ---- touch ----------------------------------------------------------------
bool touchWasDown = false;
uint32_t lastTapAt = 0;

void pollTouch(uint32_t nowMs) {
  const bool down = touch.touched();
  if (down && !touchWasDown && nowMs - lastTapAt > 350) {
    lastTapAt = nowMs;
    const TS_Point point = touch.getPoint();
    Serial.printf("Tap (raw %d, %d, z %d).\n", point.x, point.y, point.z);
    if (scene == Scene::Clock) {
      nextClockFace();
    }
  }
  touchWasDown = down;
}

// ---------------------------------------------------------------------------
// Album art decoding
// ---------------------------------------------------------------------------
int ceilDiv(int numerator, int denominator) {
  return (numerator + denominator - 1) / denominator;
}

// Opacity of art pixel (ax, ay) against the backdrop: rounded, anti-aliased
// corners; 255 everywhere else.
uint8_t artCornerOpacity(int ax, int ay) {
  const int lx = ax < ART_RADIUS ? ax
      : ax >= ART_SIZE - ART_RADIUS ? ART_SIZE - 1 - ax : -1;
  const int ly = ay < ART_RADIUS ? ay
      : ay >= ART_SIZE - ART_RADIUS ? ART_SIZE - 1 - ay : -1;
  if (lx < 0 || ly < 0) {
    return 255;
  }
  const float dx = ART_RADIUS - lx - 0.5f;
  const float dy = ART_RADIUS - ly - 0.5f;
  const float coverage = constrain(
      ART_RADIUS - sqrtf(dx * dx + dy * dy) + 0.5f, 0.0f, 1.0f);
  return static_cast<uint8_t>(coverage * 255);
}

void accumulateArtSample(uint16_t color) {
  const uint32_t r = ((color >> 11) & 0x1F) * 255 / 31;
  const uint32_t g = ((color >> 5) & 0x3F) * 255 / 63;
  const uint32_t b = (color & 0x1F) * 255 / 31;
  sampleSumR += r;
  sampleSumG += g;
  sampleSumB += b;
  ++sampleCount;
  const uint32_t high = max(r, max(g, b));
  const uint32_t low = min(r, min(g, b));
  if (high < 48) {
    return;  // ignore near-black pixels for the accent
  }
  const uint32_t saturation = high - low;
  const uint32_t weight = saturation * saturation;
  sampleWeightR += r * weight;
  sampleWeightG += g * weight;
  sampleWeightB += b * weight;
  sampleWeightTotal += weight;
}

// Resamples each decoded block (nearest neighbour) to ART_SIZE. In Sample
// mode it only gathers colors; in Render mode it draws to the screen.
bool jpegOutput(int16_t x, int16_t y, uint16_t width, uint16_t height,
                uint16_t* bitmap) {
  if (jpegMode == JpegMode::Idle) {
    return true;
  }
  const int destX0 = ceilDiv(x * ART_SIZE, jpegSourceW);
  const int destY0 = ceilDiv(y * ART_SIZE, jpegSourceH);
  const int destX1 = min(ART_SIZE, ceilDiv((x + width) * ART_SIZE, jpegSourceW));
  const int destY1 = min(ART_SIZE, ceilDiv((y + height) * ART_SIZE, jpegSourceH));
  const int destW = destX1 - destX0;
  const int destH = destY1 - destY0;
  if (destW <= 0 || destH <= 0) {
    return true;
  }
  if (jpegMode == JpegMode::Sample) {
    for (int destY = destY0; destY < destY1; ++destY) {
      if (destY % 3 != 0) {
        continue;
      }
      const uint16_t* sourceRow =
          bitmap + (destY * jpegSourceH / ART_SIZE - y) * width;
      for (int destX = destX0; destX < destX1; ++destX) {
        if (destX % 3 == 0) {
          accumulateArtSample(sourceRow[destX * jpegSourceW / ART_SIZE - x]);
        }
      }
    }
    return true;
  }
  if (destW > ART_BLOCK_MAX || destH > ART_BLOCK_MAX) {
    return true;  // only possible when upscaling tiny art; skipped
  }
  for (int row = 0; row < destH; ++row) {
    const int destY = destY0 + row;
    const uint16_t* sourceRow =
        bitmap + (destY * jpegSourceH / ART_SIZE - y) * width;
    const uint16_t background = backdropAt(ART_Y + destY);
    uint16_t* outRow = artBlock + row * destW;
    for (int column = 0; column < destW; ++column) {
      const int destX = destX0 + column;
      uint16_t color = sourceRow[destX * jpegSourceW / ART_SIZE - x];
      const uint8_t opacity = artCornerOpacity(destX, destY);
      if (opacity < 255) {
        color = blendColor(color, background, opacity);
      }
      outRow[column] = color;
    }
  }
  tft.pushImage(ART_X + destX0, ART_Y + destY0, destW, destH, artBlock);
  return true;
}

void configureTls(WiFiClientSecure& client) {
  client.setCACert(SPOTIFY_CA_CERTS);
  client.setTimeout(HTTP_TIMEOUT_MS / 1000);
}

// Fetches url into ART_PATH on LittleFS using the given client.
bool fetchJpegInto(WiFiClient& client, const String& url, size_t& length) {
  const uint32_t started = millis();
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    Serial.println("Could not start album-art request.");
    return false;
  }
  const int status = http.GET();
  const int contentLength = http.getSize();
  if (status != HTTP_CODE_OK || contentLength <= 0 ||
      static_cast<size_t>(contentLength) > MAX_ART_BYTES) {
    Serial.printf("Album-art download failed (HTTP %d, %d bytes, %s).\n",
                  status, contentLength, url.startsWith("https") ? "https"
                                                                 : "http");
    http.end();
    return false;
  }
  // Stream straight to flash: keeping ~60 KB of JPEG in RAM starves TLS.
  File file = LittleFS.open(ART_TEMP_PATH, FILE_WRITE);
  if (!file) {
    Serial.println("Could not open album-art file for writing.");
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  uint8_t chunk[1024];
  size_t received = 0;
  while (received < static_cast<size_t>(contentLength)) {
    const size_t wanted = min(sizeof(chunk),
                              static_cast<size_t>(contentLength) - received);
    const size_t count = stream->readBytes(chunk, wanted);
    if (count == 0 || file.write(chunk, count) != count) {
      break;
    }
    received += count;
  }
  file.close();
  http.end();
  if (received == static_cast<size_t>(contentLength)) {
    LittleFS.remove(ART_PATH);
    if (!LittleFS.rename(ART_TEMP_PATH, ART_PATH)) {
      Serial.println("Could not save album-art file.");
      return false;
    }
  }
  if (received != static_cast<size_t>(contentLength)) {
    Serial.printf("Album-art download was incomplete (%u of %d bytes).\n",
                  static_cast<unsigned>(received), contentLength);
    return false;
  }
  length = received;
  Serial.printf("Album art downloaded over %s: %u bytes in %u ms.\n",
                url.startsWith("https") ? "https" : "http",
                static_cast<unsigned>(received),
                static_cast<unsigned>(millis() - started));
  return true;
}

// Album art is public, so try plain HTTP first: it avoids a slow TLS
// handshake and its memory. Falls back to HTTPS if that fails.
bool downloadJpeg(const String& url, size_t& length) {
  if (url.length() == 0) {
    Serial.println("Track has no album-art URL.");
    return false;
  }
  if (!artStorageReady) {
    Serial.println("Album-art storage is not mounted.");
    return false;
  }
  if (url.startsWith("https://i.scdn.co/")) {
    WiFiClient plainClient;
    plainClient.setTimeout(HTTP_TIMEOUT_MS / 1000);
    String plainUrl = url;
    plainUrl.replace("https://", "http://");
    if (fetchJpegInto(plainClient, plainUrl, length)) {
      return true;
    }
  }
  WiFiClientSecure secureClient;
  configureTls(secureClient);
  return fetchJpegInto(secureClient, url, length);
}

// Builds an accent color and backdrop gradient from the decoded artwork.
void derivePalette() {
  const uint32_t samples = sampleCount;
  const uint64_t weightTotal = sampleWeightTotal;
  if (samples == 0) {
    return;
  }
  const uint32_t avgR = sampleSumR / samples;
  const uint32_t avgG = sampleSumG / samples;
  const uint32_t avgB = sampleSumB / samples;

  uint32_t vibrantR, vibrantG, vibrantB;
  if (weightTotal < static_cast<uint64_t>(samples) * 300) {
    // Mostly grey artwork: use a soft neutral accent.
    vibrantR = 225;
    vibrantG = 228;
    vibrantB = 236;
  } else {
    vibrantR = sampleWeightR / weightTotal;
    vibrantG = sampleWeightG / weightTotal;
    vibrantB = sampleWeightB / weightTotal;
    const uint32_t high = max(vibrantR, max(vibrantG, vibrantB));
    if (high > 0) {
      vibrantR = min<uint32_t>(255, vibrantR * 235 / high);
      vibrantG = min<uint32_t>(255, vibrantG * 235 / high);
      vibrantB = min<uint32_t>(255, vibrantB * 235 / high);
    }
  }
  accentColor = rgb565(vibrantR, vibrantG, vibrantB);
  const uint16_t average = rgb565(avgR, avgG, avgB);
  const uint16_t base = blendColor(accentColor, average, 140);
  backdropTop = darkenColor(base, 34);
  backdropBottom = darkenColor(base, 7);
  playMutedColor = blendColor(TEXT_COLOR, backdropTop, 165);
}

uint8_t chooseJpegScale(uint16_t fullWidth) {
  for (uint8_t scale : {8, 4, 2}) {
    if (fullWidth / scale >= ART_SIZE) {
      return scale;
    }
  }
  return 1;
}

// Decodes the downloaded JPEG at ART_PATH: Sample gathers palette colors,
// Render draws the rounded, resized art at ART_X/ART_Y.
bool decodeArtwork(JpegMode mode) {
  uint16_t fullWidth = 0;
  uint16_t fullHeight = 0;
  if (TJpgDec.getFsJpgSize(&fullWidth, &fullHeight, ART_PATH, LittleFS) !=
          JDR_OK ||
      fullWidth == 0 || fullHeight == 0) {
    Serial.println("Could not read album-art JPEG size.");
    return false;
  }
  if (mode == JpegMode::Sample) {
    sampleSumR = sampleSumG = sampleSumB = sampleCount = 0;
    sampleWeightR = sampleWeightG = sampleWeightB = sampleWeightTotal = 0;
  }
  const uint8_t scale = chooseJpegScale(fullWidth);
  TJpgDec.setJpgScale(scale);
  jpegSourceW = fullWidth / scale;
  jpegSourceH = fullHeight / scale;
  jpegMode = mode;
  const uint32_t started = millis();
  const JRESULT result = TJpgDec.drawFsJpg(0, 0, ART_PATH, LittleFS);
  jpegMode = JpegMode::Idle;
  Serial.printf("%s %ux%u art (scale 1/%u) in %u ms, result %d.\n",
                mode == JpegMode::Sample ? "Sampled" : "Rendered", fullWidth,
                fullHeight, scale, static_cast<unsigned>(millis() - started),
                static_cast<int>(result));
  return result == JDR_OK;
}

// ---------------------------------------------------------------------------
// Playback scene
// ---------------------------------------------------------------------------
uint32_t currentProgressMs(uint32_t now) {
  if (trackDurationMs == 0) {
    return 0;
  }
  const uint32_t elapsed =
      trackProgressMs + (playbackIsPlaying ? now - trackProgressStamp : 0);
  return min(elapsed, trackDurationMs);
}

void drawArtGlow() {
  const uint8_t strengths[3] = {150, 70, 28};
  for (int ring = 0; ring < 3; ++ring) {
    const int inset = ring + 1;
    tft.drawRoundRect(ART_X - inset, ART_Y - inset, ART_SIZE + inset * 2,
                      ART_SIZE + inset * 2, ART_RADIUS + inset,
                      blendColor(accentColor, backdropAt(ART_Y + ART_SIZE / 2),
                                 strengths[ring]));
  }
}

void drawArtPlaceholder() {
  const uint16_t background = backdropAt(ART_Y + ART_SIZE / 2);
  const uint16_t card = blendColor(TEXT_COLOR, background, 18);
  tft.fillSmoothRoundRect(ART_X, ART_Y, ART_SIZE, ART_SIZE, ART_RADIUS, card,
                          background);
  const int cx = ART_X + ART_SIZE / 2;
  const int cy = ART_Y + ART_SIZE / 2;
  tft.fillSmoothCircle(cx - 14, cy + 24, 14, accentColor, card);
  tft.fillRect(cx - 2, cy - 34, 6, 58, accentColor);
  tft.fillTriangle(cx + 2, cy - 34, cx + 30, cy - 18, cx + 2, cy - 8,
                   accentColor);
}

void drawTicker(uint32_t now) {
  fillSpriteBackdrop(tickerSprite, TICKER_Y);
  useFreeFont(tickerSprite, &FreeSansBold12pt7b);
  tickerSprite.setTextDatum(TL_DATUM);
  tickerSprite.setTextColor(TEXT_COLOR);

  const uint32_t sinceShown = now - playbackShownAt;
  int x = 0;
  bool scrolling = false;
  if (tickerTextWidth > PANEL_W) {
    const uint32_t cycleWidth = tickerTextWidth + 36;
    const uint32_t scrollMs = cycleWidth * 1000 / TICKER_SPEED_PX_PER_S;
    const uint32_t phase = sinceShown % (TICKER_HOLD_MS + scrollMs);
    if (phase > TICKER_HOLD_MS) {
      x = -static_cast<int>((phase - TICKER_HOLD_MS) *
                            TICKER_SPEED_PX_PER_S / 1000);
      scrolling = true;
    }
    tickerSprite.drawString(tickerTitle, x, 3);
    if (scrolling) {
      tickerSprite.drawString(tickerTitle, x + static_cast<int>(cycleWidth),
                              3);
    }
  } else {
    tickerSprite.drawString(tickerTitle, x, 3);
  }

  // Slide in from the right during the intro.
  if (sinceShown < PLAYBACK_INTRO_MS) {
    const float t = easeOutCubic(sinceShown / static_cast<float>(
                                                  PLAYBACK_INTRO_MS));
    const int cover = static_cast<int>((1.0f - t) * PANEL_W);
    for (int row = 0; row < TICKER_H; ++row) {
      tickerSprite.drawFastHLine(PANEL_W - cover, row, cover,
                                 backdropAt(TICKER_Y + row));
    }
  }

  // Soft fade at the edges while scrolling.
  if (scrolling) {
    constexpr int fade = 12;
    for (int row = 0; row < TICKER_H; ++row) {
      const uint16_t background = backdropAt(TICKER_Y + row);
      for (int column = 0; column < fade; ++column) {
        const uint8_t opacity = column * 255 / fade;
        const int left = column;
        const int right = PANEL_W - 1 - column;
        tickerSprite.drawPixel(
            left, row,
            blendColor(tickerSprite.readPixel(left, row), background, opacity));
        tickerSprite.drawPixel(
            right, row,
            blendColor(tickerSprite.readPixel(right, row), background,
                       opacity));
      }
    }
  }
  tickerSprite.pushSprite(PANEL_X, TICKER_Y);
}

void drawMeter(uint32_t now) {
  fillSpriteBackdrop(meterSprite, METER_Y);
  const uint32_t sinceShown = now - playbackShownAt;
  const float intro = easeOutCubic(
      min(1.0f, sinceShown / static_cast<float>(PLAYBACK_INTRO_MS)));

  // Times.
  const uint32_t progress = currentProgressMs(now);
  useFont(meterSprite, 2);
  meterSprite.setTextColor(playMutedColor);
  meterSprite.setTextDatum(TL_DATUM);
  meterSprite.drawString(formatDuration(progress), 0, 0);
  meterSprite.setTextDatum(TR_DATUM);
  meterSprite.drawString(
      trackDurationMs > 0 ? formatDuration(trackDurationMs) : "--:--",
      PANEL_W, 0);

  // Progress bar.
  constexpr int barY = METER_H - 6;
  constexpr int barStart = 4;
  constexpr int barLength = PANEL_W - 8;
  const uint16_t background = backdropAt(METER_Y + barY);
  meterSprite.fillRoundRect(barStart, barY - 2, barLength, 4, 2,
                            blendColor(TEXT_COLOR, background, 50));
  const float fraction = trackDurationMs > 0
      ? static_cast<float>(progress) / trackDurationMs : 0.0f;
  const int head = barStart + static_cast<int>(fraction * intro * barLength);
  if (head > barStart) {
    meterSprite.fillRoundRect(barStart, barY - 2, head - barStart + 2, 4, 2,
                              accentColor);
  }
  meterSprite.fillSmoothCircle(head, barY, 4, TEXT_COLOR, background);
  meterSprite.pushSprite(PANEL_X, METER_Y);
}

void drawPlaybackStatus(bool force);

void showPlayback(bool hasArt) {
  beginSceneChange();
  scene = Scene::Playback;
  drawBackdrop(0, 0, SCREEN_W, SCREEN_H);
  drawArtGlow();
  if (!hasArt || !decodeArtwork(JpegMode::Render)) {
    drawArtPlaceholder();
  }

  tft.setTextDatum(TL_DATUM);
  useFont(tft, 1);
  tft.setTextColor(accentColor);
  tft.drawString("NOW PLAYING", PANEL_X, LABEL_Y);

  useFont(tft, 2);
  drawWrappedText(currentTrackArtist, PANEL_X, ARTIST_Y, PANEL_W, 16, 2,
                  TEXT_COLOR);
  useFont(tft, 1);
  drawWrappedText(currentAlbum, PANEL_X, ALBUM_Y, PANEL_W, 11, 2,
                  playMutedColor);

  tickerTitle = currentTrackTitle;
  useFreeFont(tickerSprite, &FreeSansBold12pt7b);
  tickerTextWidth = tickerSprite.textWidth(tickerTitle);

  drawPlaybackStatus(true);
  playbackShownAt = millis();
  drawTicker(playbackShownAt);
  drawMeter(playbackShownAt);
  lastPlaybackFrame = playbackShownAt;
  endSceneChange();
  playbackShownAt = millis();  // start the intro once the panel is lit
}

// Small clock and indoor temperature in the top-right of the player.
void drawPlaybackStatus(bool force) {
  char timeText[6] = "--:--";
  if (timeIsSynced()) {
    const time_t epoch = time(nullptr);
    struct tm localTime;
    localtime_r(&epoch, &localTime);
    strftime(timeText, sizeof(timeText), "%H:%M", &localTime);
  }
  const String temperature = climateAvailable ? temperatureValue : "--";
  const String status = String(timeText) + "|" + temperature;
  if (!force && status == renderedPlaybackStatus) {
    return;
  }
  renderedPlaybackStatus = status;

  constexpr int statusY = 8;
  drawBackdrop(PANEL_X, 4, PANEL_W, 20);
  useFreeFont(tft, &FreeSansBold9pt7b);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TEXT_COLOR);
  tft.drawString(timeText, PANEL_X, statusY);

  // Right-aligned "24°C" with a drawn degree ring (FreeFonts are ASCII only).
  const int numberWidth = tft.textWidth(temperature);
  const int unitWidth = tft.textWidth("C");
  const int right = PANEL_X + PANEL_W;
  const int numberX = right - unitWidth - 7 - numberWidth;
  tft.setTextColor(playMutedColor);
  tft.drawString(temperature, numberX, statusY);
  if (climateAvailable) {
    tft.drawSmoothCircle(numberX + numberWidth + 3, statusY + 2, 2,
                         playMutedColor, backdropAt(statusY + 2));
    tft.drawString("C", right - unitWidth, statusY);
  }
}

void updatePlayback(uint32_t now) {
  if (scene != Scene::Playback || now - lastPlaybackFrame < FRAME_MS) {
    return;
  }
  lastPlaybackFrame = now;
  readClimate(now);
  drawPlaybackStatus(false);
  drawTicker(now);
  drawMeter(now);
}

// ---------------------------------------------------------------------------
// Spotify
// ---------------------------------------------------------------------------
bool credentialsConfigured() {
  return strlen(WIFI_SSID) > 0 &&
         strcmp(WIFI_SSID, "YOUR_WIFI_SSID") != 0 &&
         strlen(SPOTIFY_CLIENT_ID) > 0 &&
         strcmp(SPOTIFY_CLIENT_ID, "YOUR_SPOTIFY_CLIENT_ID") != 0 &&
         strlen(SPOTIFY_REFRESH_TOKEN) > 0 &&
         strcmp(SPOTIFY_REFRESH_TOKEN, "YOUR_SPOTIFY_REFRESH_TOKEN") != 0;
}

String urlEncode(const String& value) {
  String encoded;
  encoded.reserve(value.length() * 3);
  const char hex[] = "0123456789ABCDEF";
  for (size_t i = 0; i < value.length(); ++i) {
    const uint8_t c = static_cast<uint8_t>(value[i]);
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '_' ||
        c == '.' || c == '~') {
      encoded += static_cast<char>(c);
    } else {
      encoded += '%';
      encoded += hex[c >> 4];
      encoded += hex[c & 0x0F];
    }
  }
  return encoded;
}

bool refreshSpotifyToken() {
  WiFiClientSecure client;
  configureTls(client);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, "https://accounts.spotify.com/api/token")) {
    Serial.println("Could not start Spotify token request.");
    return false;
  }
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");

  const String body = "grant_type=refresh_token&client_id=" +
      urlEncode(SPOTIFY_CLIENT_ID) + "&refresh_token=" + urlEncode(refreshToken);
  const int status = http.POST(body);
  if (status != HTTP_CODE_OK) {
    Serial.printf("Spotify token refresh failed (HTTP %d).\n", status);
    http.end();
    return false;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) {
    Serial.printf("Could not parse Spotify token response: %s\n", error.c_str());
    return false;
  }

  const char* token = document["access_token"] | "";
  const uint32_t expiresIn = document["expires_in"] | 3600U;
  if (token[0] == '\0') {
    Serial.println("Spotify token response did not contain an access token.");
    return false;
  }

  accessToken = token;
  tokenExpiresAt = millis() + expiresIn * 1000UL - TOKEN_EARLY_REFRESH_MS;
  const char* rotatedToken = document["refresh_token"] | "";
  if (rotatedToken[0] != '\0' && refreshToken != rotatedToken) {
    refreshToken = rotatedToken;
    preferences.putString("refresh_token", refreshToken);
  }
  Serial.println("Spotify access token refreshed.");
  return true;
}

String joinArtists(JsonArrayConst artists) {
  String result;
  for (JsonObjectConst artist : artists) {
    const char* name = artist["name"] | "";
    if (name[0] == '\0') {
      continue;
    }
    if (result.length() > 0) {
      result += ", ";
    }
    result += name;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Network task (core 0) <-> display (core 1)
//
// All Spotify HTTP work runs in networkTask so TLS handshakes never stall the
// animations. The task publishes results into `shared` under `sharedLock`.
// The album-art JPEG is downloaded into ART_PATH; ownership passes to the
// display via jpegOwnedByDisplay until it has finished decoding.
// ---------------------------------------------------------------------------
struct SpotifyShared {
  uint32_t version = 0;  // bumps when the playing state or track changes
  bool playing = false;
  String trackId;
  String title;
  String artist;
  String album;
  bool artReady = false;
  size_t artLength = 0;
  uint32_t progressMs = 0;  // for trackId
  uint32_t durationMs = 0;
  uint32_t progressStamp = 0;
  bool loading = false;  // a new track was seen and its art is downloading
  bool linked = false;
  bool loginFailed = false;
};

SpotifyShared shared;
SemaphoreHandle_t sharedLock = nullptr;
volatile bool jpegOwnedByDisplay = false;
uint32_t seenVersion = 0;

class SharedGuard {
 public:
  SharedGuard() { xSemaphoreTake(sharedLock, portMAX_DELAY); }
  ~SharedGuard() { xSemaphoreGive(sharedLock); }
};

// ---- display side --------------------------------------------------------
void presentTrack(bool artReady, size_t artLength) {
  accentColor = DEFAULT_PLAY_ACCENT;
  backdropTop = darkenColor(DEFAULT_PLAY_ACCENT, 30);
  backdropBottom = darkenColor(DEFAULT_PLAY_ACCENT, 6);
  playMutedColor = blendColor(TEXT_COLOR, backdropTop, 165);

  (void)artLength;  // the art itself is in ART_PATH
  const bool hasArt = artReady && decodeArtwork(JpegMode::Sample);
  if (hasArt) {
    derivePalette();
  }
  showPlayback(hasArt);
  jpegOwnedByDisplay = false;  // the network task may reuse the buffer now
}

void showIdle() {
  currentTrackId = "";
  playbackIsPlaying = false;
  if (scene != Scene::Clock) {
    showClock();
  }
}

// Applies whatever the network task has published since the last frame.
void syncFromNetwork() {
  bool changed = false;
  bool playing = false;
  bool artReady = false;
  size_t artLength = 0;
  String trackId, title, artist, album;
  {
    SharedGuard guard;
    spotifyLinked = shared.linked;
    spotifyLoginFailed = shared.loginFailed;
    spotifyLoading = shared.loading;
    if (shared.version != seenVersion) {
      seenVersion = shared.version;
      changed = true;
      playing = shared.playing;
      trackId = shared.trackId;
      title = shared.title;
      artist = shared.artist;
      album = shared.album;
      artReady = shared.artReady;
      artLength = shared.artLength;
    }
    if (shared.playing && shared.trackId == (changed ? trackId
                                                     : currentTrackId)) {
      trackProgressMs = shared.progressMs;
      trackDurationMs = shared.durationMs;
      trackProgressStamp = shared.progressStamp;
    }
  }

  if (spotifyLoginFailed && scene == Scene::Playback) {
    showIdle();
  }
  if (!changed) {
    return;
  }
  if (!playing) {
    jpegOwnedByDisplay = false;
    showIdle();
    return;
  }
  playbackIsPlaying = true;
  if (trackId == currentTrackId && scene == Scene::Playback) {
    jpegOwnedByDisplay = false;
    return;
  }
  currentTrackId = trackId;
  currentTrackTitle = title;
  currentTrackArtist = artist;
  currentAlbum = album;
  presentTrack(artReady, artLength);
}

// ---- network side --------------------------------------------------------
enum class PollKind { Error, Idle, Playing };

struct PollResult {
  PollKind kind = PollKind::Error;
  String trackId;
  String title;
  String artist;
  String album;
  String imageUrl;
  uint16_t imageWidth = 0;
  uint32_t progressMs = 0;
  uint32_t durationMs = 0;
};

// One currently-playing request. The TLS session and JSON document are freed
// before this returns, so the art download afterwards has the heap to itself.
PollResult fetchPlayback() {
  PollResult result;
  WiFiClientSecure client;
  configureTls(client);
  HTTPClient http;
  http.setTimeout(HTTP_TIMEOUT_MS);
  const char* headerKeys[] = {"Retry-After"};
  http.collectHeaders(headerKeys, 1);
  if (!http.begin(client,
                  "https://api.spotify.com/v1/me/player/currently-playing")) {
    Serial.println("Could not start Spotify playback request.");
    return result;
  }
  http.addHeader("Authorization", "Bearer " + accessToken);
  const int status = http.GET();

  if (status == HTTP_CODE_NO_CONTENT) {
    http.end();
    result.kind = PollKind::Idle;
    return result;
  }
  if (status == HTTP_CODE_UNAUTHORIZED) {
    Serial.println("Spotify access token expired; refreshing.");
    tokenExpiresAt = millis();
    nextTokenAttempt = 0;
    http.end();
    return result;
  }
  if (status == 429) {
    const int retrySeconds = http.header("Retry-After").toInt();
    retryAfterMs = static_cast<uint32_t>(max(retrySeconds, 1)) * 1000UL;
    Serial.printf("Spotify rate limited; retrying in %d seconds.\n",
                  static_cast<int>(retryAfterMs / 1000));
    http.end();
    return result;
  }
  if (status != HTTP_CODE_OK) {
    Serial.printf("Spotify playback request failed (HTTP %d).\n", status);
    http.end();
    if (status == HTTP_CODE_FORBIDDEN) {
      result.kind = PollKind::Idle;
    }
    return result;
  }

  JsonDocument document;
  const DeserializationError error = deserializeJson(document, http.getStream());
  http.end();
  if (error) {
    Serial.printf("Could not parse Spotify playback response: %s\n",
                  error.c_str());
    return result;
  }
  if (!(document["is_playing"] | false)) {
    result.kind = PollKind::Idle;
    return result;
  }

  JsonObjectConst item = document["item"].as<JsonObjectConst>();
  result.kind = PollKind::Playing;
  result.progressMs = document["progress_ms"] | 0U;
  result.durationMs = item["duration_ms"] | 0U;
  const char* trackId = item["id"] | "";
  result.title = item["name"] | "Unknown track";
  result.trackId = trackId[0] != '\0' ? String(trackId) : result.title;
  result.artist = joinArtists(item["artists"].as<JsonArrayConst>());
  result.album = item["album"]["name"] | "";

  // Prefer the smallest image that still covers ART_SIZE (usually 300 px).
  String fallbackUrl;
  uint16_t fallbackWidth = 0;
  for (JsonObjectConst image : item["album"]["images"].as<JsonArrayConst>()) {
    const uint16_t width = image["width"] | 0;
    const char* candidateUrl = image["url"] | "";
    if (candidateUrl[0] == '\0') {
      continue;
    }
    if (width >= ART_SIZE &&
        (result.imageWidth == 0 || width < result.imageWidth)) {
      result.imageWidth = width;
      result.imageUrl = candidateUrl;
    }
    if (width > fallbackWidth) {
      fallbackWidth = width;
      fallbackUrl = candidateUrl;
    }
  }
  if (result.imageUrl.length() == 0) {
    result.imageUrl = fallbackUrl;
    result.imageWidth = fallbackWidth;
  }
  return result;
}

// Waits (bounded) for the display to finish with the art file.
bool claimJpegBuffer() {
  for (int i = 0; i < 100 && jpegOwnedByDisplay; ++i) {
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  return !jpegOwnedByDisplay;
}

void networkPoll(String& publishedTrackId, bool& publishedPlaying) {
  const uint32_t started = millis();
  PollResult result = fetchPlayback();
  const uint32_t requestMs = millis() - started;
  if (result.kind == PollKind::Error) {
    return;
  }

  if (result.kind == PollKind::Idle) {
    if (publishedPlaying) {
      SharedGuard guard;
      shared.playing = false;
      shared.trackId = "";
      shared.loading = false;
      ++shared.version;
    }
    publishedPlaying = false;
    publishedTrackId = "";
    return;
  }

  const uint32_t now = millis();
  // Poll again shortly after this track ends so the next one appears quickly.
  if (result.durationMs > result.progressMs) {
    const uint32_t remaining = result.durationMs - result.progressMs + 1200;
    if (remaining < SPOTIFY_POLL_MS) {
      nextPlaybackPoll = now + max<uint32_t>(remaining, 1500);
    }
  }

  if (publishedPlaying && result.trackId == publishedTrackId) {
    SharedGuard guard;
    shared.progressMs = result.progressMs;
    shared.durationMs = result.durationMs;
    shared.progressStamp = now;
    return;
  }

  Serial.printf("New track: %s (art %upx, request %u ms)\n",
                result.title.c_str(), result.imageWidth,
                static_cast<unsigned>(requestMs));
  {
    SharedGuard guard;
    shared.loading = true;
  }
  size_t artLength = 0;
  bool artReady = false;
  if (claimJpegBuffer()) {
    artReady = downloadJpeg(result.imageUrl, artLength);
  } else {
    Serial.println("Display still using the JPEG buffer; showing no art.");
  }

  // Progress moved on while the art downloaded; account for it.
  const uint32_t elapsed = millis() - now;
  {
    SharedGuard guard;
    shared.playing = true;
    shared.trackId = result.trackId;
    shared.title = result.title;
    shared.artist = result.artist;
    shared.album = result.album;
    shared.artReady = artReady;
    shared.artLength = artLength;
    shared.progressMs = result.progressMs + elapsed;
    shared.durationMs = result.durationMs;
    shared.progressStamp = millis();
    shared.loading = false;
    if (artReady) {
      jpegOwnedByDisplay = true;
    }
    ++shared.version;
  }
  publishedPlaying = true;
  publishedTrackId = result.trackId;
}

void networkTask(void*) {
  String publishedTrackId;
  bool publishedPlaying = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    const uint32_t now = millis();
    if (accessToken.length() == 0 || deadlineReached(now, tokenExpiresAt)) {
      if (deadlineReached(now, nextTokenAttempt)) {
        nextTokenAttempt = now + TOKEN_RETRY_MS;
        const bool ok = refreshSpotifyToken();
        if (ok) {
          nextPlaybackPoll = 0;
        } else {
          Serial.println(
              "Spotify login failed: check client ID and refresh token.");
        }
        SharedGuard guard;
        shared.linked = ok;
        shared.loginFailed = !ok;
      }
    } else if (deadlineReached(now, nextPlaybackPoll)) {
      nextPlaybackPoll = now + SPOTIFY_POLL_MS + retryAfterMs;
      retryAfterMs = 0;
      networkPoll(publishedTrackId, publishedPlaying);
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void connectWifi(uint32_t now) {
  if (WiFi.status() == WL_CONNECTED || now - nextWifiAttempt < WIFI_RETRY_MS) {
    return;
  }
  nextWifiAttempt = now;
  Serial.println("Connecting to Wi-Fi.");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void createSprites() {
  digitSprite.setColorDepth(16);
  digitSprite.createSprite(DIGIT_W, DIGIT_H);
  secondsSprite.setColorDepth(16);
  secondsSprite.createSprite(SEC_W, SEC_H);
  tickerSprite.setColorDepth(16);
  tickerSprite.createSprite(PANEL_W, TICKER_H);
  meterSprite.setColorDepth(16);
  meterSprite.createSprite(PANEL_W, METER_H);
  for (TFT_eSprite* sprite :
       {&digitSprite, &secondsSprite, &tickerSprite, &meterSprite}) {
    sprite->setTextWrap(false);
  }
  // Album art is downloaded to flash (LittleFS) rather than held in RAM.
  artStorageReady = LittleFS.begin(true);
  Serial.printf("Art storage %s; free heap %u, max block %u.\n",
                artStorageReady ? "mounted" : "FAILED",
                static_cast<unsigned>(ESP.getFreeHeap()),
                static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

}  // namespace

void setup() {
  Serial.begin(115200);

  tft.init();
  tft.setRotation(3);
  tft.setTextWrap(false);
  tft.setSwapBytes(true);
  setupBacklight();
  writeBacklight(0);
  tft.fillScreen(TFT_BLACK);
  createSprites();
  TJpgDec.setCallback(jpegOutput);
  TJpgDec.setSwapBytes(false);
  dht.begin();

  touchSpi.begin(TOUCH_CLK_PIN, TOUCH_MISO_PIN, TOUCH_MOSI_PIN, TOUCH_CS_PIN);
  touch.begin(touchSpi);
  touch.setRotation(1);
  displayPrefs.begin("display", false);
  clockFace = displayPrefs.getUChar("face", FACE_DIGITAL) % FACE_COUNT;

  if (!credentialsConfigured()) {
    showMessage("Setup required", "Edit src/secrets.h and rebuild", false);
    Serial.println("Set Wi-Fi and Spotify values in src/secrets.h, then rebuild.");
    return;
  }

  preferences.begin("spotify", false);
  refreshToken = preferences.getString("refresh_token", SPOTIFY_REFRESH_TOKEN);

  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
  showMessage("Connecting", "Joining Wi-Fi...", true);

  sharedLock = xSemaphoreCreateMutex();
  // The display loop runs on core 1; Spotify networking gets core 0.
  xTaskCreatePinnedToCore(networkTask, "spotify", 16384, nullptr, 1, nullptr,
                          0);
  Serial.println("Spotify album-art clock starting.");
}

void loop() {
  const uint32_t now = millis();

  if (!credentialsConfigured()) {
    delay(500);
    return;
  }

  pollTouch(now);

  if (WiFi.status() != WL_CONNECTED) {
    connectWifi(now);
    if (timeIsSynced()) {
      // Keep the clock running offline once time is known.
      if (scene != Scene::Clock) {
        showClock();
      }
      updateClock(now);
    } else {
      showMessage("Connecting", "Joining Wi-Fi...", true);
      animateMessage(now);
    }
    delay(5);
    return;
  }

  if (scene == Scene::Message) {
    showClock();  // Wi-Fi is up: the clock shows its own sync state
  }

  syncFromNetwork();

  const uint32_t frameNow = millis();
  updateClock(frameNow);
  updatePlayback(frameNow);
  animateMessage(frameNow);
  delay(5);
}
