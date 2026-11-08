#!/usr/bin/env bash
# Builds the PC simulator and renders every clock face to sim/renders/*.png.
# Needs a host g++ (MinGW on Windows, or gcc/clang on macOS/Linux) and the
# libraries PlatformIO downloads on the first `pio run`.
set -euo pipefail
cd "$(dirname "$0")"

LIB=../.pio/libdeps/esp32dev
if [ ! -d "$LIB/TFT_eSPI" ]; then
  echo "Libraries not found. Run 'pio run' once so PlatformIO downloads them." >&2
  exit 1
fi
CXX=${CXX:-g++}

mkdir -p build renders
cp ../src/main.cpp ../src/config.h ../src/certs.h build/
cat > build/secrets.h <<'EOF'
#pragma once
#define WIFI_SSID "simulator"
#define WIFI_PASSWORD "simulator"
#define SPOTIFY_CLIENT_ID "simulator"
#define SPOTIFY_REFRESH_TOKEN "simulator"
EOF

FLAGS=(
  -std=gnu++17 -O2 -w
  -DARDUINO=10819 -DARDUINOJSON_ENABLE_PROGMEM=0 -DDISABLE_ALL_LIBRARY_WARNINGS
  -DUSER_SETUP_LOADED -DILI9341_DRIVER
  -DTFT_MISO=-1 -DTFT_MOSI=23 -DTFT_SCLK=14 -DTFT_CS=5 -DTFT_DC=26
  -DTFT_RST=27 -DTFT_BL=4 -DTFT_BACKLIGHT_ON=HIGH -DSPI_FREQUENCY=20000000
  -DLOAD_GLCD -DLOAD_FONT2 -DLOAD_FONT4 -DLOAD_FONT6 -DLOAD_FONT8 -DLOAD_GFXFF
  -Istubs -I"$LIB/TFT_eSPI" -I"$LIB/ArduinoJson/src"
)

echo "Compiling TFT_eSPI..."
# TFT_eSPI keeps a font address in a uint32_t, which truncates pointers on a
# 64-bit PC; compile a copy that uses uintptr_t instead.
if [ ! build/tft.o -nt "$LIB/TFT_eSPI/TFT_eSPI.cpp" ]; then
  mkdir -p build/Extensions
  for src in TFT_eSPI.cpp Extensions/Sprite.cpp; do
    sed 's/uint32_t flash_address/uintptr_t flash_address/'       "$LIB/TFT_eSPI/$src" > "build/${src/TFT_eSPI.cpp/TFT_eSPI_host.cpp}"
  done
  "$CXX" "${FLAGS[@]}" -iquote "$LIB/TFT_eSPI" -c build/TFT_eSPI_host.cpp -o build/tft.o
fi
echo "Compiling firmware + simulator..."
"$CXX" "${FLAGS[@]}" -c sim_host.cpp -o build/sim_host.o
"$CXX" "${FLAGS[@]}" -c render_faces.cpp -o build/render_faces.o
"$CXX" build/render_faces.o build/sim_host.o build/tft.o -o build/render_faces
echo "Rendering..."
./build/render_faces renders "${1:-2}"
