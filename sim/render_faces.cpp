// Renders every clock face to PNG using the real firmware code.
// The firmware is compiled into this file so its internals are reachable.
#include "build/main.cpp"
#include "sim_host.h"

namespace {
struct Shot {
  uint8_t face;
  int hour;
  int minute;
  uint32_t settleMs;  // how long to animate before taking the picture
  const char* name;
};

const Shot kShots[] = {
    {FACE_DIGITAL, 10, 24, 2500, "1-digital"},
    {FACE_BUDDY, 10, 24, 2500, "2-mochi"},
    {FACE_SKY, 6, 32, 2500, "3-sky-sunrise"},
    {FACE_SKY, 12, 40, 2500, "3-sky-day"},
    {FACE_SKY, 17, 55, 2500, "3-sky-sunset"},
    {FACE_SKY, 22, 30, 2500, "3-sky-night"},
    {FACE_ANALOG, 10, 9, 2500, "4-analog"},
    {FACE_LOWPOLY, 12, 40, 1500, "5-lowpoly-day"},
    {FACE_LOWPOLY, 18, 10, 1500, "5-lowpoly-sunset"},
    {FACE_LOWPOLY, 22, 30, 1500, "5-lowpoly-night"},
    {FACE_PIXEL, 13, 5, 1500, "6-pixel-day"},
    {FACE_PIXEL, 21, 45, 1500, "6-pixel-night"},
    {FACE_CAT, 15, 20, 2500, "7-cat"},
    {FACE_CAT, 23, 40, 2500, "7-cat-asleep"},
    {FACE_ROBOT, 15, 20, 2500, "8-robot"},
    {FACE_ROBOT, 15, 21, 700, "8-robot-waving"},
};
}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  const char* outDir = argc > 1 ? argv[1] : "renders";
  const int scale = argc > 2 ? atoi(argv[2]) : 2;
  tft.init();
  tft.setRotation(3);
  tft.setTextWrap(false);
  tft.setSwapBytes(true);
  setupBacklight();
  writeBacklight(0);
  tft.fillScreen(TFT_BLACK);
  createSprites();
  TJpgDec.setCallback(jpegOutput);
  dht.begin();

  // Every shot also goes into one contact sheet, four across.
  constexpr int kColumns = 4;
  constexpr int kGap = 6;
  const int count = sizeof(kShots) / sizeof(kShots[0]);
  const int rows = (count + kColumns - 1) / kColumns;
  const int sheetW = kColumns * (sim::kWidth + kGap) + kGap;
  const int sheetH = rows * (sim::kHeight + kGap) + kGap;
  std::unique_ptr<uint16_t[]> sheet(new uint16_t[sheetW * sheetH]);
  std::fill(sheet.get(), sheet.get() + sheetW * sheetH, rgb565(24, 24, 28));
  int index = 0;
  for (const Shot& shot : kShots) {
    sim::setLocalTime(2026, 11, 8, shot.hour, shot.minute, 20);
    clockFace = shot.face;
    lastDhtReadAt = 0;
    dhtReadCompleted = false;
    readClimate(millis());
    showClock();
    const uint32_t end = millis() + shot.settleMs;
    while (millis() < end) {
      delay(FRAME_MS);
      updateClock(millis());
    }
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.png", outDir, shot.name);
    printf("%s %s\n", sim::savePng(path, scale) ? "wrote" : "FAILED", path);
    const int ox = kGap + (index % kColumns) * (sim::kWidth + kGap);
    const int oy = kGap + (index / kColumns) * (sim::kHeight + kGap);
    for (int y = 0; y < sim::kHeight; ++y) {
      memcpy(&sheet[(oy + y) * sheetW + ox], &sim::frame[y * sim::kWidth],
             sim::kWidth * sizeof(uint16_t));
    }
    ++index;
  }
  char sheetPath[512];
  snprintf(sheetPath, sizeof(sheetPath), "%s/contact-sheet.png", outDir);
  printf("%s %s\n",
         sim::savePngBuffer(sheetPath, sheet.get(), sheetW, sheetH, 1)
             ? "wrote" : "FAILED",
         sheetPath);
  return 0;
}
