# ESP32 Spotify Album-Art Clock

A desk gadget built from an ESP32 and a 2.8" touchscreen. When nothing is
playing it's an animated clock with **8 tap-to-switch clock faces**. When you
play something on Spotify it changes to a **now-playing screen** with the album
art, scrolling track info, a progress bar, and the time and room temperature.

This repo is meant as a guide you can follow from an empty breadboard to a
finished clock. You can also skip the guide and just download the code and
flash it.

> **Add your photos!** Put pictures of your build in `docs/images/` and link
> them here. People love seeing the real thing.

---

## Features

- **Spotify now-playing**
  - Album art with rounded corners and a backdrop colored from the art.
  - The title scrolls when it's too long.
  - Progress bar with times, plus the clock and temperature.
- **8 animated clock faces** (tap anywhere on the screen to switch). The
  board remembers your choice after a restart.

  | # | Face | What it does |
  | --- | --- | --- |
  | 1 | **Digital** | Big rolling digits, a seconds bar, and cards for temperature, humidity, Wi-Fi and Spotify |
  | 2 | **Mochi** | A squishy buddy that breathes, blinks, glances around, hops every minute and sleeps from 23:00 to 06:00 |
  | 3 | **Sky** | A lakeside landscape that follows the real time of day, described below |
  | 4 | **Analog** | A watch dial with tapered hands and a smooth sweeping second hand |
  | 5 | **Low Poly** | Faceted mountains, a breathing triangle sky, a lake that glints, and a low-poly fox that swishes its tail |
  | 6 | **Pixel** | An 8-bit city with someone walking a dog, cars passing, windows lighting up at night and neon signs |
  | 7 | **Miso the cat** | Tail swishes, ear flicks, green eyes that follow things, and a paw wave every minute |
  | 8 | **Bolt the robot** | Hovers on a thruster, has a screen face, a pulsing antenna, and chest lights that count the seconds |

  The Sky face (#3) in detail:
  - The sky follows the time of day, with sunrise, golden hour, sunset and
    night.
  - The moon shows its real phase, and the Milky Way and shooting stars come
    out at night.
  - Snowy mountains are mirrored in a lake with sun or moon glitter.
  - A windmill turns, a cottage has chimney smoke and lit windows, and a
    sailboat drifts on the lake.
  - Birds cross by day and fireflies glow at night.
- **Smooth everywhere.** All networking runs on the ESP32's second core, so
  animations never stall while Spotify is being polled.
- **Temperature and humidity** from a DHT11 sensor.

---

## Parts list

| Part | Notes | Approx. cost |
| --- | --- | --- |
| ESP32 dev board (ESP32-WROOM-32, "ESP32 DevKit V1") | Any classic ESP32 with 4 MB flash. **Not** ESP32-S2/C3 without changes | $5 |
| 2.8" ILI9341 SPI TFT, 320x240, **with XPT2046 touch** | The common red-PCB module with an SD slot | $8 |
| DHT11 temperature/humidity module | The 3-pin module with a built-in resistor is easiest | $2 |
| Breadboard + jumper wires (female-female if using header pins) | | $3 |
| USB cable | Must be a data cable, not charge-only | |

You'll also need a Spotify account (free accounts work for the
"currently playing" API in most regions) and a **2.4 GHz** Wi-Fi network,
because the ESP32 can't join 5 GHz networks.

---

## Wiring

The display and the touch controller sit on **two separate SPI buses**. This
matters: the display uses VSPI and the touch uses HSPI, so they never fight
over the bus.

```
 ESP32                      ILI9341 TFT module
 ─────                      ──────────────────
 3V3  ───────────────────── VCC
 GND  ───────────────────── GND
 GPIO5  ─────────────────── CS
 GPIO27 ─────────────────── RESET
 GPIO26 ─────────────────── DC
 GPIO23 ─────────────────── SDI (MOSI)
 GPIO14 ─────────────────── SCK
 GPIO4  ─────────────────── LED (backlight, dimmed with PWM)
                            SDO (MISO) — not connected
 GPIO25 ─────────────────── T_CLK
 GPIO33 ─────────────────── T_CS
 GPIO32 ─────────────────── T_DIN
 GPIO34 ─────────────────── T_DO
 GPIO35 ─────────────────── T_IRQ

 ESP32                      DHT11 module
 3V3  ───────────────────── VCC (+)
 GND  ───────────────────── GND (-)
 GPIO18 ─────────────────── DATA (OUT)
```

| Display pin | ESP32 | | Touch pin | ESP32 | | DHT11 | ESP32 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| VCC | 3V3 | | T_CLK | GPIO25 | | VCC | 3V3 |
| GND | GND | | T_CS | GPIO33 | | GND | GND |
| CS | GPIO5 | | T_DIN | GPIO32 | | DATA | GPIO18 |
| RESET | GPIO27 | | T_DO | GPIO34 | | | |
| DC | GPIO26 | | T_IRQ | GPIO35 | | | |
| SDI / MOSI | GPIO23 | | | | | | |
| SCK | GPIO14 | | | | | | |
| LED | GPIO4 | | | | | | |

Tips:
- Power the display from **3V3**, not 5V. Most of these modules accept 5V on
  VCC, but the logic must be 3.3V, and 3V3 is the safe choice.
- GPIO34 and GPIO35 are input-only on the ESP32. That's fine here because
  T_DO and T_IRQ are inputs.
- To use different pins, change them in two places: the display pins in
  `platformio.ini` (`build_flags`) and the touch pins in `src/config.h`.

---

## Software setup

### 1. Install the tools

1. Install [VS Code](https://code.visualstudio.com/).
2. Install the **PlatformIO IDE** extension from the VS Code marketplace.
3. Install the USB driver for your board if Windows doesn't see it. That's
   usually **CP210x** or **CH340**, printed on the chip next to the USB port.

### 2. Get the code

```bash
git clone https://github.com/SteamMomo/esp32-album-art-clock.git
```

Or click **Code → Download ZIP** on GitHub and unzip it. Then open the folder
in VS Code with **File → Open Folder**. PlatformIO downloads every library
listed in `platformio.ini` by itself.

### 3. Create a Spotify app

1. Go to the [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
   and click **Create app**.
2. Give it any name and description.
3. Add this **exact** redirect URI: `http://127.0.0.1:8888/callback`.
4. Under "Which API/SDKs are you planning to use?", tick **Web API**.
5. Save, open the app's **Settings**, and copy the **Client ID**.

### 4. Get a refresh token

The repo includes a small helper script. It needs only Python 3 and no extra
packages, and it uses PKCE, so you never need the app's client secret.

```bash
python tools/get_spotify_token.py
```

Paste your client ID, log in in the browser that opens, and click **Agree**.
The script prints two lines to copy into the next step. It doesn't save
anything to disk.

### 5. Add your secrets

Copy `src/secrets.example.h` to `src/secrets.h` and fill it in:

```cpp
#define WIFI_SSID "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"
#define SPOTIFY_CLIENT_ID "your-client-id"
#define SPOTIFY_REFRESH_TOKEN "your-refresh-token"
```

`src/secrets.h` is in `.gitignore`, so it won't be uploaded if you fork and
push. **Never share your refresh token.** Anyone who has it can read what
you're listening to.

### 6. Set your time zone

Edit `TZ_INFO` in `src/config.h`. It uses POSIX time-zone strings, for
example:

| Where | `TZ_INFO` |
| --- | --- |
| India | `IST-5:30` |
| UK | `GMT0BST,M3.5.0/1,M10.5.0` |
| Central Europe | `CET-1CEST,M3.5.0,M10.5.0/3` |
| US Eastern | `EST5EDT,M3.2.0,M11.1.0` |
| US Pacific | `PST8PDT,M3.2.0,M11.1.0` |

There are more in this
[list of POSIX time zones](https://github.com/nayarsystems/posix_tz_db/blob/master/zones.csv).

### 7. Build and flash

Plug in the ESP32 and click the **→ (Upload)** arrow in the PlatformIO toolbar.
To use the terminal instead:

```bash
pio run -t upload
```

Open the serial monitor at 115200 baud (`pio device monitor`) to watch it boot.
You should see `Spotify album-art clock starting.` followed by
`Spotify access token refreshed.`

Play a song on any device signed in to your Spotify account. The clock switches
to the now-playing screen within about 10 seconds.

---

## How it works

There's a deeper write-up in [docs/HOW_IT_WORKS.md](docs/HOW_IT_WORKS.md).
In short:

- **Two cores.**
  - A FreeRTOS task on core 0 does all the Wi-Fi, TLS and Spotify work. It
    polls every 10 s and polls again early near the end of a track.
  - Core 1 only draws.
  - The two share a small struct protected by a mutex, so the screen never
    freezes while waiting on the network.
- **Album art without running out of RAM.**
  - Covers are downloaded straight to flash (LittleFS) in 1 KB chunks. They
    use plain HTTP from Spotify's image CDN, which is about 10x faster than a
    TLS handshake.
  - Each cover is decoded twice by TJpg_Decoder. The first pass picks the
    colors, and the second draws to the screen with anti-aliased rounded
    corners.
  - No full-screen buffer is ever needed.
- **Strip rendering for the scene faces.** Sky, Low Poly and Pixel describe
  their background as a function of `(x, y, time)`.
  - Any rectangle can be redrawn by filling a 320x16 sprite one strip at a
    time, drawing the objects on top and pushing it to the screen.
  - That means 10 KB of RAM instead of the 150 KB a full framebuffer would
    take.
  - Only the parts that move are redrawn each frame.

## Make your own clock face

Follow the step-by-step guide in
[docs/ADDING_A_FACE.md](docs/ADDING_A_FACE.md). You need four functions and
about three lines of wiring.

## Troubleshooting

| Problem | Fix |
| --- | --- |
| White or blank screen | Check CS/DC/RST wiring and that `TFT_MISO=-1`. Try lowering `SPI_FREQUENCY` in `platformio.ini` to `10000000` |
| Colors look inverted | Add `-DTFT_INVERSION_ON` to `build_flags` |
| Touch does nothing | Check the T_ pins. The serial log prints `Tap (raw x, y, z)` for every tap |
| `Could not open COM3` when uploading | Close any serial monitor or other program using the port, then retry |
| Stuck on "Connecting to Wi-Fi" | It must be a 2.4 GHz network. Check the SSID and password in `secrets.h` |
| `Spotify token refresh failed` | Run `tools/get_spotify_token.py` again. Make sure the redirect URI matches exactly |
| `SSL - Memory allocation failed` in the log | Something is holding too much heap. Each face must free its sprite when you switch away (see the face guide) |
| Temperature shows `--` | Check the DHT11 DATA pin is on GPIO18 and the module has power |

## Libraries used

- [TFT_eSPI](https://github.com/Bodmer/TFT_eSPI) and
  [TJpg_Decoder](https://github.com/Bodmer/TJpg_Decoder) by Bodmer
- [ArduinoJson](https://arduinojson.org/) by Benoît Blanchon
- [XPT2046_Touchscreen](https://github.com/PaulStoffregen/XPT2046_Touchscreen)
  by Paul Stoffregen
- [DHT sensor library](https://github.com/adafruit/DHT-sensor-library) by
  Adafruit

This project is not affiliated with or endorsed by Spotify. It uses the public
Spotify Web API under Spotify's developer terms.

## License

MIT. See [LICENSE](LICENSE). Build it, remix it, share it.
