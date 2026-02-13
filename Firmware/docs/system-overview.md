# SonarMK2 System Overview

## What It Does

The SonarMK2 is a standalone sonar imaging system. An ESP32 drives a Blue Robotics Ping360 mechanical scanning sonar over RS-485, reads depth from a pressure sensor, and hosts a WiFi access point serving a real-time polar sonar display to any browser that connects.

There are no external dependencies at runtime — no internet, no app install, no cloud. A diver or ROV operator connects their phone/laptop to the "SonarMK2" WiFi network and gets a live sonar image in their browser.

## Hardware

```
                        +------------------+
                        |   ESP32-WROOM-32 |
                        |   2MB flash      |
                        |   no PSRAM       |
                        |                  |
  Ping360 sonar <------>| UART1 (RS-485)   |  TX=25, RX=27, DE=26
                        |   115200 baud    |  half-duplex, auto DE
                        |                  |
  MS5837 depth  <------>| I2C0             |  SDA=21, SCL=22, 400kHz
  sensor (0x76)         |                  |
                        |                  |
  4x status LEDs <------| GPIO 15,13,14,12 |  active high
                        |                  |
  5V / 12V rails <------| GPIO 23,32,33    |  enable/disable
  Battery ADC    ------>| GPIO 36 (ADC1)   |  x3 voltage divider
  CAN transceiver <---->| GPIO 19,18       |  TWAI, 250kbps (scaffold)
                        +------------------+
                               |
                            WiFi AP
                          192.168.4.1
                               |
                        +------+------+
                        |  Browser(s) |
                        |  (up to 4)  |
                        +-------------+
```

## Software Architecture

### Component Map

```
main.c
  |
  +-- led           GPIO output control (4 LEDs)
  +-- power         Rail enable/disable, battery ADC
  +-- rs485         UART1 half-duplex driver
  |     |
  +-- ping_protocol Frame encoder/decoder (pure C, no ESP-IDF deps)
  |     |
  +-- ping360       Scan task, config management, probe
  |     |
  +-- ms5837        I2C pressure/depth sensor
  +-- canbus        CAN bus scaffold (init/send, not actively used)
  +-- wifi_ap       WiFi AP mode + captive portal DNS
  +-- web_server    HTTP file serving + WebSocket broadcast
        |
       web/          index.html, style.css, sonar.js (SPIFFS)
```

No component has upward dependencies. `ping_protocol` is pure C with zero ESP-IDF coupling, making it testable on a host machine. `ping360` depends on `rs485` and `ping_protocol` but knows nothing about networking. `web_server` depends on `ping360` only for config get/set.

### FreeRTOS Tasks

```
Core 0                              Core 1
------                              ------
app_main (pri 1)                    sonar_task (pri 5)
  - init everything                   - loop over angles
  - 1Hz sensor poll                   - RS-485 transact per angle
  - broadcast status                  - invoke data callback

httpd (pri 5, either core)
  - serve HTML/CSS/JS
  - handle WebSocket frames
  - execute queued broadcast work

dns_server (pri 3, either core)
  - answer all DNS queries with
    192.168.4.1 (captive portal)
```

The sonar task is pinned to core 1 to keep RS-485 timing tight. Everything else floats or runs on core 0.

## Data Flow

### Sonar Data: Ping360 to Browser

```
Ping360 hardware
    |
    | RS-485 (msg 2601 cmd -> msg 2300 response)
    v
sonar_task (core 1)
    | parse response, extract angle + intensity[1200]
    |
    | ping360_data_cb_t callback (still on core 1)
    v
web_server_broadcast_sonar()
    | malloc + memcpy intensity data
    | httpd_queue_work()  <-- thread-safe handoff
    v
httpd task (any core)
    | build binary WS frame:
    |   [0x01][angle u16 LE][num_samples u16 LE][data...]
    | send to all connected WS clients
    v
browser JS (sonar.js)
    | parse ArrayBuffer
    | polar coordinate transform
    | write pixels into offscreen ImageData
    | composite onto visible canvas + grid overlay
    v
user sees sonar image
```

This happens ~7 times per second (one angle at a time). The browser accumulates a full 360-degree image over ~57 seconds.

### Status Data: Sensors to Browser

```
app_main loop (1Hz, core 0)
    |
    | ms5837_read()     -> depth, temperature, pressure
    | power_read_battery_mv() -> battery voltage
    | ping360_get_scan_rate() -> angles/sec
    |
    v
web_server_broadcast_status()
    | httpd_queue_work()
    v
httpd task
    | JSON: {"type":"status","depth_m":1.23,"temp_c":22.5,...}
    | send to all WS clients
    v
browser JS
    | update DOM spans in status bar
```

### Config Changes: Browser to Ping360

```
browser UI (slider/dropdown change)
    |
    | debounce 200ms
    | JSON: {"cmd":"set_config","range_mm":10000}
    v
httpd task (WS receive handler)
    | strstr/strtol parse
    | ping360_get_config() -> merge changed fields
    | ping360_set_config() -> mutex-protected write
    |
    | broadcast updated config JSON to all clients
    v
sonar_task (core 1)
    | picks up new config at start of next sweep
    | (mutex-protected snapshot copy)
    v
Ping360 receives new parameters on next transducer command
```

Config changes take effect within one sweep cycle. The full config is broadcast to all clients on every change and on new WS connection, keeping multiple browsers in sync.

## Startup Sequence

```
 LED0 ON ---- "booting"
   |
   v
 power_init()
 5V enable -> wait 10ms -> check 5V_OK
 12V enable -> wait 50ms
 LED1 ON ---- "power OK"
   |
   v
 NVS + netif + event loop init
   |
   v
 I2C bus init (400kHz)
 MS5837 init (reset + read PROM calibration)
   * non-critical: logs warning and continues if absent
   |
   v
 RS-485 init (UART1, 115200, half-duplex)
 Ping360 init (defaults, mutex)
 Ping360 probe (send general_request, 2s timeout)
 LED2 ON ---- "sonar detected" (if probe succeeds)
   |
   v
 WiFi AP start ("SonarMK2", open network)
 DNS server start (captive portal)
 Web server start (SPIFFS mount + httpd)
   |
   v
 Register sonar data callback -> WS broadcast
 Start scan task (core 1)
 LED3 ON ---- "running"
   |
   v
 Main loop: 1Hz sensor poll + status broadcast
```

Non-critical failures (MS5837 absent, Ping360 not responding to probe) are logged and execution continues. The scan task will keep retrying the sonar. Critical failures (WiFi, SPIFFS) halt via `ESP_ERROR_CHECK`.

## Thread Safety Model

There are three concurrent writers/readers to worry about:

| Shared state | Writer | Reader | Protection |
|---|---|---|---|
| `s_config` (ping360) | httpd task (set_config) | sonar_task (snapshot) | Mutex — struct is 20 bytes, can't be atomically copied |
| `s_stop` (ping360) | main task (stop_scan) | sonar_task | `volatile bool` — single 32-bit word, atomic on Xtensa |
| `s_scan_rate` (ping360) | sonar_task | main task (get_scan_rate) | `volatile float` — single 32-bit word |
| `s_ws_fds[]` (web_server) | httpd task only | httpd task only | No protection needed — single-threaded access via httpd_queue_work |

The sonar data callback runs on core 1 (sonar_task), but it immediately `malloc`s a copy and hands it to the httpd task via `httpd_queue_work()`. The httpd task is the only thing touching WebSocket file descriptors, so the `s_ws_fds[]` array needs no locking.

All peripheral I/O (UART, I2C, ADC) is interrupt-driven at the driver level. The calling task yields via FreeRTOS semaphores/queues while the hardware ISR handles the actual byte transfer. The one exception is `adc_oneshot_read()` which busy-waits for ~10us during ADC conversion — negligible.

## Memory Layout

```
Flash (2MB):
  0x0000 - 0x8FFF   Bootloader + partition table
  0x9000 - 0xEFFF   NVS (24KB)
  0xF000 - 0xFFFF   PHY init (4KB)
  0x10000 - 0x15FFFF Factory app (1.3MB) -- ~895KB used, 35% free
  0x160000 - 0x1FFFFF SPIFFS storage (640KB) -- ~23KB used (web UI)

RAM (~320KB total):
  ~130KB  ESP-IDF + WiFi + lwIP + FreeRTOS
  ~10KB   Task stacks (sonar 4KB + main 4KB + dns 2KB)
  ~8KB    HTTPD + sockets
  ~4KB    SPIFFS cache
  ~3KB    UART RX buffer + ping parser
  ~5KB    Misc (MS5837, ADC, transient WS frames)
  ~160KB  Free headroom
```

## WebSocket Protocol Reference

All communication on `ws://192.168.4.1/ws`.

**Binary frame — sonar data** (server to client, ~7/sec):
```
Byte 0:      0x01 (type)
Bytes 1-2:   angle (u16 LE, 0-399 gradians)
Bytes 3-4:   num_samples (u16 LE, typically 1200)
Bytes 5+:    intensity data (num_samples bytes, 0-255)
```

**JSON — status** (server to client, 1/sec):
```json
{"type":"status","depth_m":1.23,"temp_c":22.5,"pressure_mbar":1125.4,
 "batt_mv":7800,"wifi_clients":1,"scan_rate":6.8,"sonar_connected":true}
```

**JSON — config** (server to client, on connect + on change):
```json
{"type":"config","gain":1,"start_angle":0,"end_angle":399,
 "num_samples":1200,"transmit_frequency":740,"transmit_duration":80,
 "sample_period":80,"range_mm":5000,"speed_of_sound":1500}
```

**JSON — set config** (client to server):
```json
{"cmd":"set_config","gain":2,"range_mm":10000}
```
Only include fields you want to change. Server merges with current config.
