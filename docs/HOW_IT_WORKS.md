# How it works

A tour of `src/main.cpp`, from the parts that matter most to the details. Every
design decision here came from a real problem hit while building the clock.

## The big picture

```
 core 0 (network task)                  core 1 (Arduino loop)
 ─────────────────────                  ─────────────────────
 refresh Spotify token                  poll touch
 GET /me/player/currently-playing       read DHT11 every 2 s
 download album art -> LittleFS  ──┐    animate the clock face (30 fps)
 publish result                    │    or the now-playing screen
        │                          │           ▲
        └──► SpotifyShared (mutex) ┴───────────┘  syncFromNetwork()
```

### Why two cores?

A Spotify request takes about 1 second, most of it the TLS handshake. On a
single loop, every poll froze the animations, and a song change could freeze
the screen for much longer. Now the network task (`networkTask`, pinned to
core 0) does all the slow work and writes its results into a `SpotifyShared`
struct under a FreeRTOS mutex (`SharedGuard`). The display loop copies out the
latest version each frame and never blocks.

## Album art

The cover pipeline:

1. **Download straight to flash.**
   - `fetchJpegInto()` streams the JPEG into LittleFS (`/art.tmp`, then
     renamed to `/art.jpg`) in 1 KB chunks.
   - Buffering the whole cover in RAM was tried first. Covers reach 60 KB or
     more, and that cost the heap the contiguous block TLS needs.
2. **Plain HTTP for the image CDN.**
   - Spotify's cover images on `i.scdn.co` are also served over HTTP, and the
     firmware tries that first.
   - It takes about 0.3 s, against about 5 s for a fresh TLS handshake. It
     falls back to HTTPS if HTTP fails.
3. **Decode twice.**
   - TJpg_Decoder runs once in **Sample** mode, which reads colors to pick a
     vibrant accent and a dark backdrop.
   - It runs again in **Render** mode, resizing each decoded block to 184 px
     (nearest neighbour) and writing it to the screen.
   - The rounded corners are anti-aliased per pixel (`artCornerOpacity`).
4. **Handoff.** The `jpegOwnedByDisplay` flag makes sure the network task never
   overwrites the file while the display is decoding it.

## Heap budget

| | Free heap |
| --- | --- |
| At boot | ~250 KB |
| A TLS connection | ~45 KB, contiguous |
| Typical face sprite | 10–35 KB |

Running out of heap shows up as `SSL - Memory allocation failed`. Every
feature was designed around keeping a large contiguous block free:

- No full-screen sprites (that would be 150 KB).
- Album art lives in flash, not RAM.
- Each clock face allocates its sprite on entry and frees it on exit.

## Clock faces

`showClock()` and `updateClock()` dispatch on `clockFace`. Faces come in two
styles.

### Direct drawing

Digital, Analog and the buddies (Mochi, Miso, Bolt) draw straight to the screen,
plus one sprite for the part that animates:

- **Buddies:** the 132x132 character sprite is cleared to the backdrop
  gradient, the character is drawn into it, and it's pushed every frame. The
  side panel is redrawn only when the minute or the climate reading changes.
- **Analog:** a 136x136 sprite redraws the hand area at 20 fps.

The three buddies share a layout. `buddyKind` picks which character
`drawBuddyCharacter()` draws, along with its colors and greetings.

### Strip rendering

The scene faces are **Sky**, **Low Poly** and **Pixel**:

- `stripRow(line, x0, y, w)` fills one row of background pixels. For Sky,
  that means:
  - the sky gradient
  - the sun glow and disc, and the real moon phase
  - the Milky Way and faint background stars
  - mountains with slope lighting and snow, and pine hills
  - lake reflections with waves and glitter
  - grass with flowers
- `stripDraw()` paints the objects into the current strip: clouds, birds,
  the windmill, the cottage, smoke, the boat, fireflies, text and page dots.
- `skyRender(x0, y0, x1, y1)` runs both for each 16-row strip of a rectangle,
  using one 320x16 sprite, and pushes the result.

Because the background is a pure function, any rectangle can be rebuilt at any
time, so overlapping animations never leave trails. Each frame redraws only
small regions:

- the windmill every frame
- moving clouds every other frame
- the lake every fourth frame, which also covers the boat and shimmer
- the cottage and its smoke every fourth frame
- the birds, fireflies and shooting star where they move

Stars twinkle by writing single pixels directly. The full scene is rebuilt
once a minute (about 0.2 s), which also moves the sun and shifts the light.

**Low Poly** uses the same renderer, but its "background" is about 300
flat-shaded triangles built once from a fixed random seed. The sky triangles
breathe, the lake triangles glint, and a rolling refresh redraws one strip per
frame so the mesh animates without a full redraw.

**Pixel** quantizes everything to 4x4 cells. `pxCellColor(cx, cy)` decides the
color of each cell: sky bands with dithered seams, the skyline, building
facades with windows and neon, street lamps, the sidewalk and the road.
Moving sprites are tiny ASCII-art strings such as `PX_WALKER`, `PX_DOG` and
`PX_CAR`. The clock uses a hand-made 3x5 pixel font (`pxGlyph`).

### The shared time-of-day palette

`skyPrepare()` interpolates between 10 key moments of the day, from midnight
through dawn, sunrise, day, golden hour, sunset and dusk to night. It produces:

- a three-stop sky gradient per row (`skyRows`)
- `skyDay`, `skyNight` and `skyGold`, used to light the terrain and objects
- the sun or moon position on an arc
- the moon phase, computed from the date and a known new moon (6 Jan 2000)
- the time, date and phrase text, such as "Golden hour" or "Waxing gibbous"

## Touch

The XPT2046 sits on its own SPI bus (`SPIClass(HSPI)`), so it never conflicts
with the display's VSPI. Any tap counts. Only the rising edge is used, with
a 350 ms debounce, so no calibration is needed. Taps switch faces only while
the clock is showing.

## Backlight

The LED pin is driven by LEDC PWM. Scene changes fade out, draw in the dark,
then fade in. The duty cycle is squared so the fade looks even to the eye.
