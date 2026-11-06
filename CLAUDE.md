# CLAUDE.md

Plain ESP-IDF (no Matter) firmware for a Waveshare ESP32-S3-ETH with an OV5640, reading the
flow indicators on a 3-loop UFH manifold. See README.md for the endpoints and the plan.

## Build

Only ESP-IDF is needed (not esp-matter). From a fresh shell:

```sh
source ~/esp/esp-idf-v5.5.4/export.sh
idf.py -C /home/tomasmcguinness/development/esp32-ufh-manifold-monitor build
```

Always pass `-C`, and use `set -o pipefail` if you pipe the output, or a failed build can look green.
`main/CMakeLists.txt` lists sources explicitly: add new `.c` files there.

## Pins (Waveshare ESP32-S3-ETH)

- W5500 on SPI2: SCLK 13, MOSI 11, MISO 12, CS 14, INT 10, RST 9
- Camera power enable: GPIO8, **active low** (from Waveshare's demo code, not the wiki). Must be driven low before `esp_camera_init()`.
- Camera: XCLK 3, SIOD 48, SIOC 47, D0-D7 = 41 45 46 42 40 38 15 18, VSYNC 1, HREF 2, PCLK 39, no PWDN/RESET
- SD card (unused so far) on SPI3: CS 4, MISO 5, MOSI 6, CLK 7. Note: GPIO8 is the camera enable, not SD.

## Notes

- Console is USB-Serial-JTAG on `/dev/ttyACM0`; deasserting DTR resets the board.
- mDNS works here (`ufh-monitor.local`) because there is no CHIP stack competing for UDP 5353.
- The dashboard is `main/www/index.html`, embedded with `EMBED_TXTFILES`; data comes from `/api/flows` (`flows.c`). Analysis should feed readings in through `flows_set_reading()`.
- Flow reading: `main/flow_reader.c` mirrors `tools/flow_prototype.py`; change both together and
  check the prototype against `tools/fixtures/`. It decodes with the ROM TJpgDec (`rom/tjpgd.h`)
  and stops below row 380: a full 2560x1920 RGB decode would not fit in PSRAM.
- OTA: `main/ota.c` handles `POST /api/ota`. `ota_mark_running_app_valid()` runs in `app_main` after
  the IP arrives; keep it after everything a remote update depends on, or a broken image can't roll back.
  Rollback (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`) lives in the bootloader, so it needs a USB flash to change.
- `camera_capture()` holds a mutex until `camera_release()`; always release, including on error paths.
