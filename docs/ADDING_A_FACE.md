# Adding your own clock face

Every clock face in `src/main.cpp` follows the same small contract. This guide
builds a simple **"Bubbles"** face (a gradient with rising bubbles and the time)
and then shows the more powerful **strip-rendered** style used by Sky, Low Poly
and Pixel.

## The contract

A face is two functions:

```cpp
bool enterMyFace();              // draw everything once; return false if out of memory
void updateMyFace(uint32_t now); // called ~30 times per second; redraw only what changed
```

The framework handles the rest:

- **Before** `enter` runs, `showClock()` fades the backlight out and frees the
  previous face's sprite (`beginSceneChange()`). After it returns, the
  backlight fades back in. You draw in the dark, so there's no flicker.
- `updateClock()` calls your `update` at most every `FRAME_MS` (33 ms) while
  the clock is showing.
- Tapping the screen calls `nextClockFace()`, which saves your choice to flash
  and calls `showClock()` again.

### Memory rules

The ESP32 has about 250 KB of free heap at boot, and the Spotify TLS
connection needs about 45 KB of it in **one contiguous block**. Two rules
follow:

1. Use the shared `faceSprite` for any off-screen drawing, and allocate it
   with `allocateFaceSprite(w, h)`. It's freed for you when the face changes.
2. Keep sprites small. A 132x132 16-bit sprite is 35 KB, and that's about the
   largest that is safe. A full-screen sprite (150 KB) will break Spotify.

## Step 1: add an ID

Add your face to the end of the enum, just before `FACE_COUNT`. The page dots
update automatically.

```cpp
enum FaceId : uint8_t { FACE_DIGITAL, FACE_BUDDY, FACE_SKY, FACE_ANALOG,
                        FACE_LOWPOLY, FACE_PIXEL, FACE_CAT, FACE_ROBOT,
                        FACE_BUBBLES,   // <- new
                        FACE_COUNT };
```

## Step 2: write the face

Put this anywhere after the Analog face and before `showClock()`:

```cpp
// ---- Bubbles: rising bubbles behind a big clock --------------------------
constexpr int BUBBLE_COUNT = 12;
float bubbleY[BUBBLE_COUNT];
int bubbleX[BUBBLE_COUNT];

void drawBubblesTime() {
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  char text[6] = "--:--";
  if (timeIsSynced()) strftime(text, sizeof(text), "%H:%M", &localTime);
  drawBackdrop(40, 80, 240, 60);         // erase the old time
  tft.setTextDatum(MC_DATUM);
  useFreeFont(tft, &FreeSansBold24pt7b);
  tft.setTextColor(TEXT_COLOR);
  tft.drawString(text, SCREEN_W / 2, 110);
}

bool enterBubblesFace() {
  backdropTop = rgb565(10, 60, 90);      // gradient used by drawBackdrop()
  backdropBottom = rgb565(4, 14, 30);
  drawBackdrop(0, 0, SCREEN_W, SCREEN_H);
  for (int i = 0; i < BUBBLE_COUNT; ++i) {
    bubbleX[i] = random(10, SCREEN_W - 10);
    bubbleY[i] = random(0, SCREEN_H);
  }
  faceRenderedTime = "";                 // force the first time draw
  drawPageDots(backdropAt(233));
  return true;
}

void updateBubblesFace(uint32_t nowMs) {
  // 1. Redraw the time only when the minute changes.
  struct timeval now;
  struct tm localTime;
  currentLocalTime(now, localTime);
  char key[8] = "sync";
  if (timeIsSynced()) strftime(key, sizeof(key), "%H:%M", &localTime);
  if (faceRenderedTime != key) {
    faceRenderedTime = key;
    drawBubblesTime();
  }
  // 2. Move each bubble: erase the old circle, draw the new one.
  for (int i = 0; i < BUBBLE_COUNT; ++i) {
    const int oldY = static_cast<int>(bubbleY[i]);
    bubbleY[i] -= 1.2f;
    if (bubbleY[i] < -6) bubbleY[i] = SCREEN_H + 6;
    const int y = static_cast<int>(bubbleY[i]);
    if (oldY >= 80 && oldY <= 140) continue;   // don't scribble on the time
    tft.fillCircle(bubbleX[i], oldY, 4, backdropAt(oldY));
    if (y < 80 || y > 140)
      tft.drawSmoothCircle(bubbleX[i], y, 4, rgb565(160, 220, 255), backdropAt(y));
  }
}
```

## Step 3: hook it up

In `showClock()`:

```cpp
    case FACE_BUBBLES:
      entered = enterBubblesFace();
      break;
```

In `updateClock()`:

```cpp
    case FACE_BUBBLES:
      updateBubblesFace(nowMs);
      break;
```

Build and flash, then tap until you reach your face.

---

## Going further: strip-rendered scenes

Erasing and redrawing shapes, as Bubbles does, gets messy once things overlap.
The Sky, Low Poly and Pixel faces use a better approach:

1. Describe the **background** as a function. Write a row-filler that
   computes the color of every pixel in one row of the screen, given the time
   of day.
2. Describe the **objects** (characters, text, clouds) as a draw function that
   paints into the current strip.
3. Whenever something moves, call `skyRender(x0, y0, x1, y1)` on the rectangle
   that changed. The renderer fills a 320x16 sprite strip by strip:
   background, then objects, then push to the screen. Overlaps are always
   correct, because every strip is rebuilt from scratch.

The skeleton:

```cpp
void myRow(uint16_t* line, int x0, int y, int width) {
  for (int col = 0; col < width; ++col) {
    const int x = x0 + col;
    uint16_t color = rgb565(x / 2, y, 128);   // your background here
    line[col] = swap16(color);                // sprites store byte-swapped colors
  }
}

void myObjects() {
  // Use screen coordinates through SX()/SY(); skip things outside the strip.
  if (skyHits(100, 100, 140, 140)) {
    faceSprite.fillSmoothCircle(SX(120), SY(120), 20, TFT_YELLOW);
  }
  stripDrawDots(TFT_BLACK);
}

bool enterMyScene() {
  if (!allocateFaceSprite(SCREEN_W, SKY_STRIP_H)) return false;
  stripRow = myRow;
  stripDraw = myObjects;
  skyRender(0, 0, SCREEN_W, SCREEN_H);       // full first draw (~0.2 s)
  return true;
}

void updateMyScene(uint32_t nowMs) {
  skyT = nowMs / 1000.0f;
  skyRender(100, 100, 140, 140);             // redraw only what moves
}
```

Useful helpers inside a strip:

| Helper | What it does |
| --- | --- |
| `skyHits(x0, y0, x1, y1)` | `true` if the rectangle touches the current strip. Use it to skip work |
| `SX(x)`, `SY(y)` | Convert screen coordinates into strip-sprite coordinates |
| `skyBlendPixel(x, y, color, alpha)` | Alpha-blend one pixel (0-255) |
| `skySoftDot(x, y, radius, color, strength)` | A soft glow, like a firefly or a lamp |
| `skyRenderMoved(drawnBox, currentBox)` | Redraw where an object was and where it is now |
| `skyPrepare(localTime, now, synced)` | Compute the shared time-of-day palette: `skyRows`, sun or moon position, `skyDay`, `skyNight`, `skyGold` |

Two tricks from the existing faces:

- **Rolling refresh** (Low Poly, Pixel): redraw one 16 px strip per frame.
  The whole screen refreshes every half second, which is plenty for slow
  effects like twinkling, breathing colors or blinking windows.
- **Ordered dithering** (Sky): `skyPack()` adds a 4x4 Bayer pattern before
  rounding to RGB565, which hides the color banding in smooth gradients.
