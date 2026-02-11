# SonarMK2 Firmware — Project Reference

## What This Is

ESP32-WROOM-32 firmware (2MB flash, no PSRAM) that drives a Blue Robotics Ping360 scanning sonar over RS-485 and serves a real-time web-based polar sonar display via WiFi AP mode.

## Build System

- **Framework**: ESP-IDF v6.0.0
- **Target**: ESP32 (WROOM-32)
- **Partition table**: Custom `partitions.csv` — 1.3MB app, 640KB SPIFFS
- **Web assets**: Packed into SPIFFS via `spiffs_create_partition_image(storage web FLASH_IN_PROJECT)` in root `CMakeLists.txt`

### Building

```bash
# Activate ESP-IDF (must source the activation script first)
source /home/aren/.espressif/tools/activate_idf_release-v6.0.sh
cd Firmware/
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

If your shell doesn't support `source`/aliases, use direct paths:
```bash
export IDF_PATH=/home/aren/.espressif/release-v6.0/esp-idf
export IDF_TOOLS_PATH=/home/aren/.espressif/tools
export IDF_PYTHON_ENV_PATH=/home/aren/.espressif/tools/python/release-v6.0/venv
export ESP_ROM_ELF_DIR=/home/aren/.espressif/tools/esp-rom-elfs/20241011
export PATH="/home/aren/.espressif/tools/cmake/4.0.3/bin:/home/aren/.espressif/tools/ninja/1.12.1:/home/aren/.espressif/tools/xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin:/home/aren/.espressif/tools/python/release-v6.0/venv/bin:$PATH"
/home/aren/.espressif/tools/python/release-v6.0/venv/bin/python3 $IDF_PATH/tools/idf.py build
```

### IDE Diagnostics Warning

Clang/IDE errors like `'esp_err.h' file not found`, `unknown type name`, etc. are **expected** — the IDE doesn't have ESP-IDF include paths. These all resolve at build time with `idf.py build`. Ignore all clang/cSpell diagnostics in this project.

## Directory Structure

```
Firmware/
├── CMakeLists.txt              # Root — includes SPIFFS image creation
├── partitions.csv              # Custom: nvs + phy + factory(1.3MB) + spiffs(640KB)
├── sdkconfig.defaults          # Custom partitions, WS support, 1kHz tick
├── sdkconfig                   # Auto-generated full config (do not edit)
├── main/
│   ├── CMakeLists.txt          # REQUIRES all 9 components + esp_driver_i2c etc.
│   ├── main.c                  # Init sequence + 1Hz sensor poll loop
│   └── Kconfig.projbuild       # WiFi SSID/password/channel config menu
├── components/
│   ├── rs485/                  # UART1 half-duplex RS-485 driver
│   ├── ping_protocol/          # Ping binary protocol encoder/decoder (pure C)
│   ├── ping360/                # High-level scan task + config management
│   ├── ms5837/                 # I2C pressure/depth sensor
│   ├── led/                    # 4x GPIO LED control
│   ├── power/                  # Power rails (5V/12V) + battery ADC
│   ├── canbus/                 # CAN bus scaffold (TWAI)
│   ├── wifi_ap/                # WiFi AP mode + captive portal DNS
│   └── web_server/             # HTTP + WebSocket server, SPIFFS mount
└── web/                        # Frontend files → packed into SPIFFS at /www
    ├── index.html              # UI: canvas + control panel + status bar
    ├── style.css               # Dark theme, responsive
    └── sonar.js                # Polar renderer + WS client + controls
```

## Component Dependencies

Each component has its own `CMakeLists.txt` specifying `REQUIRES`:

| Component | REQUIRES |
|-----------|----------|
| rs485 | esp_driver_uart esp_driver_gpio |
| ping_protocol | *(none — pure C)* |
| ping360 | rs485 ping_protocol freertos |
| ms5837 | esp_driver_i2c |
| led | esp_driver_gpio |
| power | esp_driver_gpio esp_adc |
| canbus | esp_driver_twai *(needs `driver` too for header resolution)* |
| wifi_ap | esp_wifi esp_netif esp_event nvs_flash lwip |
| web_server | esp_http_server spiffs ping360 esp_event |

**Note**: In ESP-IDF v6.0, some driver headers require the `driver` meta-component in REQUIRES/PRIV_REQUIRES. If a build fails with "includes driver/xxx.h but driver is not in requirements", add `driver` to PRIV_REQUIRES.

## Hardware Pin Map

| Function | GPIO | Notes |
|----------|------|-------|
| RS-485 TX | 25 | UART1 |
| RS-485 RX | 27 | UART1 |
| RS-485 DE | 26 | Auto half-duplex |
| I2C SDA | 21 | MS5837 @ 0x76 |
| I2C SCL | 22 | 400kHz |
| LED0 | 15 | Active high — "booting" |
| LED1 | 13 | "power OK" |
| LED2 | 14 | "sonar detected" |
| LED3 | 12 | "running" |
| 5V Enable | 23 | Output |
| 12V Enable | 32 | Output (set HIGH to enable) |
| 12V Disable | 33 | Output (set HIGH to disable) |
| CAN Enable | 16 | Output |
| 5V OK | 35 | Input |
| 12V OK | 34 | Input |
| Battery ADC | 36 | ADC1_CH0, ×3 divider |
| CAN TX | 19 | TWAI |
| CAN RX | 18 | TWAI |

## Ping360 Protocol

Binary protocol over RS-485 at 115200 baud.

**Frame format**: `'B' 'R' [u16 LE payload_len] [u16 LE msg_id] [u8 src=0] [u8 dst=1] [payload...] [u16 LE checksum]`

**Checksum**: Sum of all bytes before checksum field, stored as u16 LE.

**Key messages**:
- **2601** (Transducer cmd): 13-byte payload — mode, gain, angle(0-399 gradians), tx_duration, sample_period, tx_freq, num_samples, transmit
- **2300** (Device data response): 12-byte fixed header + variable intensity data
- **6** (General request): 2-byte payload — requested msg_id
- **2903** (Motor off): zero payload

**Scan flow**: For each angle, send msg 2601 → receive msg 2300 → invoke data callback.

## WebSocket Protocol

All WS communication is on `ws://{ip}/ws`.

### Binary: Sonar data (server→client, per angle)
```
[0]      u8  type = 0x01
[1..2]   u16 angle (LE, 0-399 gradians)
[3..4]   u16 num_samples (LE)
[5..N]   u8[] intensity data
```

### JSON: Status (server→client, 1Hz)
```json
{"type":"status","depth_m":1.23,"temp_c":22.5,"pressure_mbar":1125.4,
 "batt_mv":7800,"wifi_clients":1,"scan_rate":6.8,"sonar_connected":true}
```

### JSON: Config sync (server→client, on connect + change)
```json
{"type":"config","gain":1,"start_angle":0,"end_angle":399,
 "num_samples":1200,"transmit_frequency":740,"transmit_duration":80,
 "sample_period":80,"range_mm":5000,"speed_of_sound":1500}
```

### JSON: Config command (client→server)
```json
{"cmd":"set_config","gain":2,"range_mm":10000}
```
Only changed fields required; server merges with current config.

## FreeRTOS Tasks

| Task | Priority | Stack | Core | Role |
|------|----------|-------|------|------|
| app_main | 1 | 4KB | 0 | Init sequence, then 1Hz sensor poll loop |
| sonar_task | 5 | 4KB | 1 | Ping360 scan loop (RS-485 transact per angle) |
| httpd | 5 | 4KB | any | HTTP + WebSocket (ESP-IDF internal) |
| dns_server | 3 | 2KB | any | Captive portal DNS (all queries → 192.168.4.1) |

**Thread safety**: Sonar callback runs on core 1. Uses `httpd_queue_work()` to safely dispatch WS broadcasts to the httpd task. Config access is mutex-protected.

## Initialization Sequence (main.c)

```
1.  led_init() → LED0 ON ("booting")
2.  power_init()
3.  power_5v_enable(true) → wait 10ms → check power_5v_ok()
4.  power_12v_enable(true) → wait 50ms → LED1 ON
5.  nvs_flash_init() + esp_netif_init() + esp_event_loop_create_default()
6.  I2C master bus init (SDA=21, SCL=22, 400kHz)
7.  ms5837_init() — warn & continue if fails
8.  rs485_init()
9.  ping360_init() + ping360_probe(2000ms) → LED2 if found
10. wifi_ap_init() — starts AP + DNS server
11. web_server_init() — mounts SPIFFS, starts httpd
12. ping360_register_data_callback() → sonar data → WS broadcast
13. ping360_start_scan() → LED3 ON ("running")
14. Loop 1Hz: ms5837_read → web_server_broadcast_status()
```

## Web Frontend Architecture

Three files, no external dependencies, ~23KB total.

- **Polar renderer**: Offscreen canvas with `getImageData`/`putImageData` for direct pixel manipulation. Draws 3 sub-rays per angle to fill gaps.
- **Grid overlay**: Auto-scaled range rings + 45° angle labels drawn on visible canvas over sonar image.
- **Color palettes**: 256-entry RGBA LUTs — grayscale, ocean (blue→cyan→yellow), hot (black→red→yellow→white).
- **Controls**: Debounced 200ms config sends via JSON WebSocket. Range slider, gain dropdown, angle sliders, numeric inputs.
- **Auto-reconnect**: WS reconnect on close with 2s delay.

## Known Build Issues

1. **ESP-IDF v6.0 component changes**: Some components like `cJSON` have been moved to the IDF component manager. The web_server uses a lightweight manual JSON parser (strstr/strtol) instead of cJSON to avoid this dependency.
2. **TWAI/CAN driver headers**: `driver/twai.h` may require both `esp_driver_twai` and `driver` in REQUIRES.
3. **sdkconfig conflicts**: If you change `sdkconfig.defaults`, delete `build/` and `sdkconfig` then rebuild to apply cleanly.

## Memory Budget (~160KB of ~320KB used)

| Item | Bytes |
|------|-------|
| ESP-IDF + WiFi + lwIP + FreeRTOS | ~130KB |
| UART1 RX buffer | 2KB |
| Ping parser + frame buffer | 1.3KB |
| Task stacks (sonar+main+dns) | 10KB |
| HTTPD + sockets | 8KB |
| SPIFFS cache | 4KB |
| Misc (MS5837, ADC, WS frames) | ~5KB |
| **Free headroom** | **~160KB** |
