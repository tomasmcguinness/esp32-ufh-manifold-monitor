# UFH Manifold Monitor

Reads the flow indicators on an underfloor heating manifold with a camera, so each loop's flow
rate can be recorded without fitting flow meters.

## Hardware

- **Waveshare ESP32-S3-ETH** (16MB flash, 8MB octal PSRAM, W5500 Ethernet)
- **OV5640** (5MP) camera on the board's DVP connector
- USB-C power and an Ethernet cable

The target manifold has **3 flow regulators**. The OV5640's 2560x1920 (the driver's maximum) is ample for that: at a typical
mounting distance each indicator's scale is hundreds of pixels tall even at a reduced resolution.

## Status

**Milestone 1: see the manifold.** Ethernet, camera and a dashboard at **http://ufh-monitor.local/**
(the IP is also printed on the console).

The dashboard draws the manifold with its three flow meters, moving each float to the loop's
current reading, and shows per-loop L/min, total flow, the balance between loops, and camera
and device status. Loops can be renamed (stored in NVS). The **Camera** tab shows the live frame
for lining up the shot.

There's no float detection yet, so every reading is `null` and the page says "Not calibrated".
"Preview with sample values" animates the drawing with clearly labelled made-up numbers.

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | Dashboard (embedded from `main/www/index.html`) |
| `/api/flows` | GET | Loops (name, `flowLpm`, `scaleMaxLpm`, `ageSeconds`), total, measurement state, device info |
| `/api/loop?id=1&name=Kitchen` | POST | Rename a loop (1-23 characters) |
| `/api/ota` | POST | Install firmware: raw `.bin` as the body (or use the **Firmware** tab) |
| `/capture.jpg` | GET | Single JPEG frame |
| `/api/framesize?size=svga\|xga\|sxga\|uxga\|qxga\|qsxga` | POST | Change resolution |

## Plan

1. **Mount and light.** Fix the camera facing the manifold and add steady lighting: manifold
   cupboards are dark, and the reading depends on consistent brightness. Capture reference
   frames at a few known flow settings.
2. **Regions of interest.** For each of the 3 indicators, store the pixel box covering its
   sight glass (set from the web page by drawing on a snapshot, saved in NVS).
3. **Find the float.** Decode only the region to greyscale/RGB, collapse it to a 1-D profile
   along the tube, and locate the marker (colour or dark band) in that profile.
4. **Calibrate.** Map marker position to L/min with 2 or 3 reference points per indicator taken
   from its printed scale, interpolating between them.
5. **Publish.** Sample every minute or so, expose per-loop L/min over HTTP JSON, then decide on
   MQTT/Matter and whether to feed `matter-esp32-heating-monitor`.

## Build

```sh
source ~/esp/esp-idf-v5.5.4/export.sh
idf.py -C . build flash monitor
```

## Updating over the network

The first flash must be over USB (it installs the bootloader with rollback enabled). After that:

- **Firmware tab** on the dashboard: choose `build/ufh-manifold-monitor.bin`, then *Upload & install*.
- Or from a shell: `curl --data-binary @build/ufh-manifold-monitor.bin http://ufh-monitor.local/api/ota`

The board rejects images for other projects or chips, writes to the spare slot (`ota_0`/`ota_1`)
and reboots. A new image is only kept once it has an IP address and the web server is running; if it
crashes before that, the next boot rolls back to the previous firmware.

`/api/ota` has no authentication: anyone on the LAN can install firmware.
