# CrossInk Cards & Anki

> A customized firmware fork combining the refined typography and reading statistics of **[CrossInk](https://github.com/uxjulia/CrossInk)**, the dynamic always-on e-ink display capabilities of **[CrossPoint Cards](https://github.com/petereading/crosspoint-cards)**, and full two-way cloud **Anki Flashcard review & synchronization**.

Designed and tuned specifically for the **Xteink X3** (and compatible with X4, X4 Pro, X4 Classic, and Seeed Studio Sticky).

<table>
  <tr>
    <td align="center" width="25%">
      <img src="./docs/images/cards.jpg" alt="Xteink X3 displaying Cards" /><br/>
      <em>Always-on Card Display</em>
    </td>
    <td align="center" width="25%">
      <img src="./docs/images/bitter-small-15-margin.jpg" alt="Font: Bitter, Size: 12 pt, Margin: 15" /><br/>
      <em>Bitter Slab-Serif Typography</em>
    </td>
    <td align="center" width="25%">
      <img src="./docs/images/reading-stats.jpg" alt="Reading Stats Dashboard" /><br/>
      <em>Reading Stats Dashboard</em>
    </td>
    <td align="center" width="25%">
      <img src="./docs/images/cards/weather.png" alt="Weather Card" /><br/>
      <em>Weather & Forecast Sleep Screen</em>
    </td>
  </tr>
</table>

---

## Highlights

### 🎴 Anki Flashcard System & Two-Way AnkiWeb Sync
- **Native E-Ink Study Experience:** Review Anki flashcards directly on your Xteink X3 e-ink screen. Clear question presentation, flip to reveal the answer, and standard Anki SM-2 grading using the D-pad buttons:
  - **Up Button:** Again (1)
  - **Down Button:** Hard (2)
  - **Center / Confirm Button:** Good (3)
  - **Page Forward / Extra Button:** Easy (4)
- **Companion Anki Sync Bridge (Docker):** Runs on your home server or NAS behind HTTPS (e.g. `https://anki.torete.tech`). Powered by the official desktop Python `anki` engine.
- **Two-Way AnkiWeb Cloud Sync:** Keeps AnkiWeb, desktop Anki, and AnkiDroid in complete lock-step. When AnkiWeb requires a full sync, the bridge treats AnkiWeb as the definitive Source of Truth (SOT) and synchronizes your collection automatically.
- **XFD Smart Sanitization:** Automatically strips audio tags (`[sound:...]`), base64 blobs, and heavy HTML/CSS tables from popular decks (such as *Easy German*), transforming cards into clean, readable text that never crashes low-memory microcontrollers.
- **Micro-Memory NDJSON Streaming:** The X3 pulls cards incrementally over wolfSSL TLS 1.3 into `/anki/cards.ndjson` on the SD card. The ESP32-C3 indexes file offsets in memory without buffering full decks in RAM, safely maintaining $\ge 35\text{KB}$ free heap.
- **Offline Review Ingestion:** Complete your reviews offline anytime. Reviews are safely logged to `/anki/reviews.ndjson` on SD, and uploaded in a single batch to the bridge when Wi-Fi is connected.

### 🃏 Dynamic Cards System
- **Always-on Display Mode:** Open any card from **Main Menu → Cards** to keep it continuously rendered on screen with the WiFi radio completely powered down between intervals.
- **Card Sleep Screen:** Set **Settings → Display → Sleep Screen** to *Card* and select your preferred slot (*Card 1* through *Card 6*). The reader enters deep sleep, wakes up on a precision hardware timer, updates the card over WiFi, and returns to sleep while maintaining the image on the EPD panel with zero power draw.
- **6 Independent Slots:** Each slot holds a custom HTTPS URL and its own discrete refresh interval (1, 2, 5, 10, 15, 30, 60, 120, or 240 minutes) aligned to natural wall-clock boundaries (:00, :15, :30, :45).
- **Resilient Refreshes:** Images are validated in memory before writing to the display. If a network fetch fails, the last good card remains on screen instead of blanking out.
- **Low-Memory WolfSSL Transport:** Custom wolfSSL TLS 1.3 engine prevents heap exhaustion during TLS handshakes on devices without PSRAM.

### 📖 Refined Reader & Typography
- **Handpicked Fonts:** Built-in **Bitter** (contemporary slab-serif, consistent stroke weights for crisp e-ink rendering) and **Lexend Deca** (research-backed sans-serif designed for reading fluency), plus Inter for display UI.
- **Reader Font Sizes:** 10 pt, 12 pt, 14 pt, and 16 pt with anti-aliasing.
- **Expanded Glyphs:** Music notation, selected Cyrillic characters, and Project Hail Mary CJK glyph support.
- **Reading Stats:** Tracks total books read, total reading time, sessions, pages turned, average session duration, and pages per minute. Sync reading stats or reading progress between devices.
- **Focused Modes:** Focus Reading, Guide Dots, Force Paragraph Indents, and quick in-book adjustment menus.
- **Custom Button Mappings:** Extensive button shortcuts for power button short/long presses, front buttons, and a configurable Quick Actions radial menu.

---

## Anki Setup & Companion Bridge

The Anki feature operates as a client-server pair between the Xteink reader and the self-hosted **Anki Sync Bridge**:

### 1. Deploy the Anki Sync Bridge (Docker)

The bridge runs as a container on your NAS or server. Complete instructions are provided in [`features/anki/README.md`](./features/anki/README.md) and [`docker/anki-sync-bridge/README.md`](./docker/anki-sync-bridge/README.md).

```yaml
# docker-compose.yml
services:
  anki-sync-bridge:
    image: anki-sync-bridge:latest
    container_name: anki-sync-bridge
    restart: unless-stopped
    ports:
      - "8766:8766"
    environment:
      - DATA_DIR=/data
      - API_TOKEN=your_secure_api_token
    volumes:
      - /path/to/nas/anki/data:/data
```

1. Open `https://anki.your-domain.com/login` in your browser.
2. Enter your AnkiWeb credentials under **AnkiWeb Sync Account** and click **Sync with AnkiWeb**.
3. Select which decks you want enabled on your reader.

### 2. Configure Your Xteink Device

1. Connect the reader to Wi-Fi (**Settings → System → WiFi**).
2. Go to **Settings → System → Anki Settings** (or use the on-device web companion):
   - **Server URL:** `https://anki.your-domain.com`
   - **API Token:** `your_secure_api_token`
   - **Card Font:** Select *Bitter* or *Lexend Deca*
   - **Study Direction:** Forward (Default) / Reverse / Both
3. Open **Main Menu → Anki** to pull your due cards and begin reviewing!

---

## The Cards Feature

A card is an HTTPS endpoint serving a 1-bit monochrome BMP formatted for the reader's display resolution. The firmware fetches the bitmap, validates the headers, renders it to the screen, and sleeps until the next scheduled interval.

### The Built-in Cards

`examples/cloudflare-dashboard-worker.js` provides a self-contained Cloudflare Worker that renders eight pre-designed cards in both portrait and landscape orientations.

<table>
  <tr>
    <td width="25%"><img src="./docs/images/cards/clock.png" alt="Clock card"></td>
    <td width="25%"><img src="./docs/images/cards/weather.png" alt="Weather card"></td>
    <td width="25%"><img src="./docs/images/cards/moon.png" alt="Moon card"></td>
    <td width="25%"><img src="./docs/images/cards/today.png" alt="Today in history card"></td>
  </tr>
  <tr>
    <td align="center"><b>Clock</b></td>
    <td align="center"><b>Weather</b></td>
    <td align="center"><b>Moon Phase</b></td>
    <td align="center"><b>Today in History</b></td>
  </tr>
  <tr>
    <td width="25%"><img src="./docs/images/cards/quote.png" alt="Quote card"></td>
    <td width="25%"><img src="./docs/images/cards/bitcoin.png" alt="Bitcoin card"></td>
    <td width="25%"><img src="./docs/images/cards/solar.png" alt="Solar card"></td>
    <td width="25%"><img src="./docs/images/cards/astro.png" alt="Astro card"></td>
  </tr>
  <tr>
    <td align="center"><b>Daily Quote</b></td>
    <td align="center"><b>Bitcoin Price</b></td>
    <td align="center"><b>Solar / Kp-Index</b></td>
    <td align="center"><b>Sun / Astro</b></td>
  </tr>
</table>

---

## Configuring Card Slots

You can configure card URLs and intervals either over the local web interface or on the device itself:

1. **Via Web Interface (Recommended):**
   - Connect the reader to your local network under **Settings → System → WiFi**.
   - Open the displayed IP address in your computer or phone browser.
   - Go to **Settings → Cards** and paste your card URLs and select refresh intervals for slots 1 through 6.
2. **On Device:**
   - Go to **Main Menu → Cards** and select a slot to input or edit the URL using the on-screen keyboard.
   - Adjust refresh intervals under **Settings → System → Card X Refresh** or **Settings → Display → Sleep Screen**.

---

## Over-The-Air (OTA) Updates

Firmware updates are automatically checked and installed directly from GitHub releases:
- Go to **Settings → System → Check for updates**.
- **Dual-Engine Architecture:** The update checker uses a resilient dual-engine approach. It negotiates with `api.github.com` via `esp_http_client` with an extended 30-second socket timeout, and automatically falls back to `freeink::SecureHttpClient` (wolfSSL TLS 1.3) if memory fragmentation or socket errors occur.
- **Safe Staging & Validation:** Firmware binaries are staged to SD (`/.crosspoint/ota-update.bin`), verified against the release SHA-256 digest, and checked against the hardware chip/board signature before flashing the OTA partition.
- **Releases Target:** Points to [paletochen/crossink-cards-anki](https://github.com/paletochen/crossink-cards-anki/releases).

---

## Installation & Flashing

Precompiled binaries are built automatically on every commit and release in GitHub Actions:

1. Download the latest `firmware-x3-x4.bin` or `crossink-cards-anki_v1.6.0_firmware.bin` from the [Releases](https://github.com/paletochen/crossink-cards-anki/releases) page.
2. Flash using the [CrossPoint Web Installer](https://crosspointreader.com) or [Inky Web Companion](https://inky.crossink.dev/#flash-tools).
3. Alternatively, copy the binary to your SD card and flash from the device under **Settings → System → SD Firmware Update**.

---

## Building from Source

This project uses [PlatformIO](https://platformio.org) with the `pioarduino` core:

```bash
# Clone repository with submodules
git clone --recursive https://github.com/paletochen/crossink-cards-anki.git
cd crossink-cards-anki

# Build default firmware (Xteink X3 / X4)
pio run -e default

# Build and flash to USB-connected device
pio run -e default --target upload
```

---

## License & Acknowledgments

- Based on **[CrossInk](https://github.com/uxjulia/CrossInk)** by Julia and contributors.
- Cards engine and Cloudflare worker adapted from **[CrossPoint Cards](https://github.com/petereading/crosspoint-cards)** by Peter Reading.
- Core reader software powered by the **[CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader)** project.
- Licensed under the [MIT License](./LICENSE). CrossInk Cards & Anki is an independent open-source project and is not affiliated with Xteink, Anki, or any hardware manufacturer.
