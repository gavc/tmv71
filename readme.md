# TMV7.1 - Simple ESP8266 Tank Monitor

## What this version is
A clean baseline firmware for Wemos D1 mini + 4 water level sensors + 1 status LED, with:
- `tzapu/WiFiManager` captive portal setup
- simple local web UI
- web button to check online update manifest
- OTA install from hosted firmware URL

## Hardware pins
- `D5` = top sensor
- `D6` = upper-mid sensor
- `D7` = lower-mid sensor
- `D2` = bottom sensor
- `D1` = status LED

## Libraries used
- `ESP8266WiFi`
- `ESP8266WebServer`
- `WiFiManager` (tzapu)
- `ESP8266HTTPClient`
- `ESP8266httpUpdate`
- `WiFiClientSecureBearSSL` (for optional HTTPS URLs)

## Configuration points
In `tmv7/tmv7.ino`, edit:
- `SENSOR_INVERTED[]` if your XKC-Y25 output is active-low
- `LED_ON_STATE` / `LED_OFF_STATE` for LED wiring
- `FW_VERSION_CODE` and `FW_VERSION_NAME`
- `UPDATE_MANIFEST_URL`

## OTA manifest format
Host a plain-text file at `UPDATE_MANIFEST_URL`:

```text
version_code=2026022102
version_name=7.1.1
firmware_url=https://raw.githubusercontent.com/gavc/tmv71/main/ota/tmv7-2026022102.bin
```

Rules:
- `version_code` must be a number greater than the current firmware code
- `firmware_url` must point directly to the `.bin` file
- HTTP and HTTPS are both supported (HTTPS is currently `setInsecure()`)

## Build/upload
1. Open `tmv7/tmv7.ino` in Arduino IDE.
2. Board: `LOLIN(WEMOS) D1 R2 & mini` (ESP8266 core).
3. Install libraries listed above.
4. Upload first build via USB.
5. Open serial monitor to find the assigned IP.

## Safety note for XKC-Y25-12V
Do not feed 12V directly into ESP8266 GPIO. Ensure sensor output to D1 mini is safely level-shifted/conditioned to 3.3V logic.
