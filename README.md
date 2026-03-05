# Sonar

<img width="600" alt="image" src="https://github.com/user-attachments/assets/b6659323-2257-401d-9745-56103f3287d1" />

SonarMK2 is a standalone underwater sonar imaging system. An ESP32 drives a Blue Robotics Ping360 mechanical scanning sonar over RS-485, reads depth from a pressure sensor, and hosts a WiFi access point that serves a real-time polar sonar display to any browser that connects. The ESP hosts a webserver and "realtime" data is streamed over websockets, the page also uses the browser datastore to record the sonar information which can be downloaded and replayed using [text](Firmware/tools/replay.html). There is eventual intention to use the sonar for SLAM and some data analysis has occured in that direction, however this is ongoing.

## Hardware

<img width="600" alt="image" src="https://github.com/user-attachments/assets/24bf1cdd-112e-4e16-aa4c-eaea773dcf79" />
The core is an ESP32-WROOM-32 with 2 MB flash and no PSRAM. The Ping360 communicates at 115200 baud over a half-duplex RS-485 link with automatic direction control on the DE pin. Depth comes from an MS5837-30BA pressure sensor on I2C at 400 kHz. Four status LEDs give a visual indicator of where the device is in its startup sequence.

Power input is from a battery connector (7–12 V) or USB-C, with an auto-select mux (TPS2121) handling the switch. A boost converter generates the 12 V rail for the sonar; a separate 5 V rail powers the RS-485 and external connectors. Both rails are software-controlled, so the firmware brings them up in sequence and checks power-good signals before proceeding.

The PCB also includes a CAN bus transceiver (TCAN334), the intended use is to allow for expansion to either DPV control/monitoring, or to be attached to a DiveCAN system so that rebreather information can be shown on the same display.

The hardware design files are in `EDA/` (KiCad schematic and PCB) and `CAD/` (FreeCAD housing assembly with STEP exports).

## Firmware

The firmware is built on ESP-IDF v6.0.0 and organized into nine components with no upward dependencies:

- **rs485** — UART1 half-duplex driver with automatic DE control
- **ping_protocol** — Binary frame encoder and decoder, pure C with no ESP-IDF coupling
- **ping360** — Scan task, configuration management, device probing
- **ms5837** — I2C pressure and depth sensor with full second-order compensation
- **led** — Four status LEDs indicating boot progress
- **power** — Rail enable/disable and battery voltage ADC
- **canbus** — CAN bus init and send (scaffolded, not called from main, will eventually have DiveCAN layer)
- **wifi_ap** — WiFi access point with captive portal DNS
- **web_server** — HTTPS file serving and WebSocket broadcast

<img width="600" alt="image" src="https://github.com/user-attachments/assets/d347c7cb-b16b-49c5-a821-ea1b73ec9d76" />

### TLS Certificates

The web server uses HTTPS with a self-signed certificate. Generate the cert and key before building:

```bash
mkdir -p Firmware/certs
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
  -keyout Firmware/certs/server.key -out Firmware/certs/server.pem \
  -days 3650 -nodes -subj "/CN=sonar.local"
```

These files are gitignored — each device should have its own keypair.

The layering is intentional. `ping_protocol` can be compiled and tested on a host machine without any embedded toolchain. `ping360` depends on `rs485` and `ping_protocol` but knows nothing about networking. `web_server` reaches into `ping360` only for config get/set. This keeps the components independently testable and prevents the kind of circular dependency that becomes painful as a codebase grows.

