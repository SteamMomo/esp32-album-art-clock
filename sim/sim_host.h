#pragma once
#include <stdint.h>

namespace sim {
constexpr int kWidth = 320;
constexpr int kHeight = 240;
extern uint16_t frame[kWidth * kHeight];
extern float temperature;
extern float humidity;
void setLocalTime(int year, int month, int day, int hour, int minute,
                  int second);
bool savePng(const char* path, int scale);
bool savePngBuffer(const char* path, const uint16_t* pixels, int width,
                   int height, int scale);
}  // namespace sim
