# ESP32 Storage Diary

A compact offline diary built around an ESP32, an ST7735 TFT display, three physical buttons, and the ESP32's internal flash. A local Wi-Fi access point hosts a browser-based editor so notes can be written from a phone or laptop without an internet connection.

> **Status:** Project sketch prepared for ESP32 Arduino core **3.3.3**. Hardware-specific compile and on-device testing should be completed on the target board before treating it as a verified release.

## Features

- Stores notes in internal flash using **LittleFS**; no SD card required.
- Creates notes from a mobile/desktop browser connected to the ESP32 access point.
- Displays the newly saved note on the TFT.
- Provides physical-button navigation, note viewing, page navigation, and delete confirmation.
- Offers 900 note IDs, **0–899**, browsed on the web interface in pages of 50 IDs: `0–49`, `50–99`, …, `850–899`.
- Web editor limits: title up to 48 characters; body up to 800 characters.
- Caches the newest 10 notes in RAM for TFT navigation to reduce RAM use.

**Important:** 900 is the number of available note IDs, not a guarantee that 900 long notes fit in flash. Actual capacity depends on the board's flash partition and note sizes. LittleFS must mount successfully before saving works.

## Hardware

- ESP32-WROOM-32 / ESP32 Dev Module with 4 MB flash (assumed by this configuration)
- ST7735 SPI TFT
- Three push buttons
- USB cable for programming and power

The SD card module is not used by this version.

## Wiring

### TFT (SPI)

| TFT pin/signal | ESP32 GPIO |
|---|---:|
| SCK / CLK | GPIO 18 |
| MOSI / SDA | GPIO 23 |
| CS | GPIO 21 |
| DC / A0 | GPIO 22 |
| RST | Not connected (`TFT_RST = -1`) |
| VCC | Match the TFT module's rated supply voltage |
| GND | GND |

### Buttons

Buttons are configured as `INPUT_PULLDOWN` and active HIGH. Connect one side of each button to the assigned GPIO and the other side to **3.3 V**.

| Button | ESP32 GPIO |
|---|---:|
| Left | GPIO 25 |
| Select / middle | GPIO 26 |
| Right | GPIO 4 |

All modules and buttons must share a common ground. Check your particular TFT module's voltage requirements before powering it.

## Software requirements

1. Arduino IDE 2.x or another compatible Arduino build environment.
2. **esp32 by Espressif Systems, version 3.3.3** (the version targeted by this sketch).
3. Install these libraries using Library Manager if missing:
   - Adafruit GFX Library
   - Adafruit ST7735 and ST7789 Library
4. `WiFi.h`, `WebServer.h`, and `LittleFS.h` are provided by the ESP32 board package; they are **not** separate Library Manager installs.

## Build and upload

1. Download `Storage_Diary_Final_Code_ESP32_3_3_3.ino`.
2. Open it in Arduino IDE. The sketch folder and `.ino` filename should match.
3. Select **Tools → Board → esp32 → ESP32 Dev Module** for an ESP32-WROOM-32, if that matches your board.
4. Select the correct COM port.
5. Choose a partition scheme that includes a filesystem partition compatible with the ESP32 core's LittleFS implementation. The default 4 MB scheme commonly reserves a filesystem partition, but check the selected scheme for your board/core.
6. Compile and upload. Open Serial Monitor at **115200 baud** and reset the board.
7. Confirm the log says `LittleFS mounted successfully.` and `System ready.`

### If LittleFS reports a mount error

The sketch intentionally calls `LittleFS.begin(false)` so it does **not** automatically format the filesystem and erase notes when mounting fails. Check the selected partition scheme and filesystem configuration first. If the filesystem is brand new or corrupted and contains no notes you need, a deliberate one-time format may be required. Formatting erases notes stored in that filesystem; do not enable full-flash erasure casually.

## Using the diary

1. Power the ESP32.
2. On your phone or laptop, connect to the Wi-Fi network:
   - **SSID:** `ESP32-Diary`
   - **Password:** `12345678`
3. While connected to that network, open **http://192.168.4.1** in a browser.
4. Enter a title and body, then select **Save note**.
5. After a successful save, the TFT opens the newly saved note.
6. Use the browser's Previous/Next controls to browse the 900 note IDs in groups of 50.

This is a local access point. It does not provide internet access by itself.

## Physical controls

- In the browse screen, Left/Right move through the notes currently cached for TFT browsing.
- Select opens the highlighted note.
- In a long note, Left/Right move between body pages; Select opens delete confirmation.
- Left/Right cancel the delete confirmation; Select confirms deletion.

## Repository layout

```text
.
├── README.md
├── Storage_Diary_Final_Code_ESP32_3_3_3.ino
└── index.html                 # Optional GitHub Pages project page
```

## Known constraints

- The TFT keeps only the newest 10 notes in RAM to reduce DRAM use; the web page scans and displays notes from LittleFS in the selected ID range.
- The 900 IDs are `0` through `899` inclusive. Flash capacity, not the ID count, determines how many notes can actually be stored.
- The current access-point password is a simple project default. Change it before sharing the device in a setting where others may access it.
- Verify the sketch compiles and test save, reboot persistence, pagination, and delete behavior on the actual board before calling the build a final hardware release.

## License

Choose a license before publishing this repository (for example, MIT if you want to permit reuse with attribution). No license is asserted by this README.
