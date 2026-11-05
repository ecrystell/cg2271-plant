# ESP32 Telegram gateway — build and flash

Tested on an **ESP32-S2** dev board (ESP32-S2FNR2, single core, CP2102N USB-UART) with
arduino-cli 1.5.1, esp32 core 3.3.12.

## 1. Toolchain (one-time)

```sh
arduino-cli config init
arduino-cli config add board_manager.additional_urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32
arduino-cli lib install "Arduino_JSON" "DHT sensor library" "Adafruit Unified Sensor"
```

- **USB driver:** the board's CP2102N needs the Silicon Labs CP210x VCP driver on Windows.
  It should show up as "Silicon Labs CP210x USB to UART Bridge (COMx)".
- **Non-ASCII paths:** if your Documents folder has non-ASCII characters (e.g. OneDrive in
  another language), the ESP32 compiler cannot find libraries there. Move the sketchbook:
  `arduino-cli config set directories.user C:\Users\<you>\Arduino` and reinstall the libraries.

## 2. Secrets

Fill in `secrets.h` (Wi-Fi must be 2.4 GHz; `CHAT_ID` is digits only). Then stop git from
picking up your values — the repo is public:

```sh
git update-index --skip-worktree dengue_esp32/secrets.h
```

## 3. Build and flash

Use `esp32s2` for the S2 board (`esp32` for a classic ESP32). Default board options are fine
(USB CDC On Boot: Disabled, so `Serial` goes through the CP2102N).

```sh
arduino-cli compile --fqbn esp32:esp32:esp32s2 -u -p COM5 dengue_esp32
arduino-cli monitor -p COM5 -c baudrate=115200
```

## 4. Expected serial output

```
Connecting to SSID:<your ssid>
.....
Connected to WiFi network with IP Address: ...
[tg] sent: Plant care system online. Send /status any time.
[tg] stack free (min): ~12700 bytes
[tg] command: /status
```

Without the DHT11 wired, `Humidity read failed or out of range` prints every 30 s — expected. Without the
MCXC444, `/status` replies "Time left: unknown".

## Wiring

| Signal | ESP32 pin |
|---|---|
| MCXC444 TX (PTE0) | GPIO16 (RX) |
| MCXC444 RX (PTE1) | GPIO17 (TX) |
| DHT11 module VCC / DATA / GND | 3V3 / GPIO4 / GND (3-pin module has its own pull-up) |
| GND | common with MCXC444 |
