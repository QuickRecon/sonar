# SonarMK2 - ESP32 Peripheral Connection Reference

Hardware reference for firmware development. Derived from KiCad schematic `EDA/SonarMK2.kicad_sch`.

## ESP32-WROOM-32 (U3) GPIO Pin Map

| GPIO | Module Pin | Net Name | Function | Direction | Notes |
|------|-----------|----------|----------|-----------|-------|
| GPIO36 (SENSOR_VP) | 4 | BATT_ADC | Battery voltage sensing | Input (ADC1_CH0) | Input-only; voltage divider ÷3 |
| GPIO39 (SENSOR_VN) | 5 | CAN_EN_IN | CAN bus enable status | Input | Input-only; resistor divider from ~CAN_EN |
| GPIO34 | 6 | 3.3V_SRC | Power source indicator | Input | Input-only; from TPS2121 ST pin |
| GPIO35 | 7 | 5V_OK | 5V rail power-good | Input | Input-only; from TPS7A2550 PG pin |
| GPIO32 | 8 | 12V_SMPS_EN | 12V boost SMPS enable | Output | Direct to LMR62014 EN; R17 pull-up to switched V_BATT |
| GPIO33 | 9 | 12V_EN | 12V boost input power | Output | **Active LOW**; drives Q4/Q5 P-FET power switch |
| GPIO25 | 10 | RS485_TXD | RS-485 transmit data | Output | To ST485E DI (pin 4) |
| GPIO26 | 11 | RS485_DTR_RTS | RS-485 direction control | Output | To ST485E DE (pin 3); also to JP1 for RE |
| GPIO27 | 12 | RS485_RXD | RS-485 receive data | Input | From ST485E RO (pin 1) |
| GPIO14 | 13 | LED_2 | Status LED 2 | Output | Active high; 2k series resistor |
| GPIO12 | 14 | LED_3 | Status LED 3 | Output | Active high; 2k series resistor; **boot-sensitive** |
| GPIO13 | 16 | LED_1 | Status LED 1 | Output | Active high; 2k series resistor |
| GPIO15 | 23 | LED_0 | Status LED 0 | Output | Active high; 2k series resistor; **boot-sensitive** |
| GPIO0 | 25 | IO0 | Boot mode select | Special | Auto-program circuit; LOW = bootloader |
| GPIO16 | 27 | CAN_EN_OUT | CAN bus enable control | Output | Drives Q6 N-FET; HIGH = CAN enabled |
| GPIO18 | 30 | CAN_RX | CAN receive | Input | From TCAN334 RXD (pin 4) |
| GPIO19 | 31 | CAN_TX | CAN transmit | Output | To TCAN334 TXD (pin 1) |
| GPIO21 | 33 | SDA | I2C data | Bidir | 10k pull-up to 3V3; MS5837 pressure sensor |
| GPIO3 (RXD0) | 34 | RXD | UART0 RX (USB debug) | Input | From CP2102N TXD (pin 25) |
| GPIO1 (TXD0) | 35 | TXD | UART0 TX (USB debug) | Output | To CP2102N RXD (pin 26) |
| GPIO22 | 36 | SCL | I2C clock | Output | 10k pull-up to 3V3; MS5837 pressure sensor |
| GPIO23 | 37 | 5V_EN | 5V LDO enable | Output | To TPS7A2550 EN (pin 4) |

### Unused GPIOs (no connection on PCB)

| GPIO | Module Pin | Notes |
|------|-----------|-------|
| GPIO2 | 24 | Available; boot-sensitive (must be low/floating for flash boot) |
| GPIO4 | 26 | Available |
| GPIO17 | 28 | Available |
| GPIO5 | 29 | Available; boot-sensitive (outputs PWM at boot) |

### Internal Flash GPIOs (do NOT use)

Pins 17-22 (GPIO6-11) are connected to the module's internal SPI flash.

---

## Peripheral Details

### 1. USB-UART Bridge - CP2102N (U1)

**Interface:** UART0 (default serial port)

| Signal | ESP32 GPIO | CP2102N Pin | Direction (from ESP32) |
|--------|-----------|-------------|----------------------|
| TXD | GPIO1 | 26 (RXD) | Output |
| RXD | GPIO3 | 25 (TXD) | Input |

- USB-C connector J1: VBUS (pin 1), D- (pin 2), D+ (pin 3), GND (pin 4)
- CP2102N powered from +3V3 and VBUS
- Auto-reset circuit: CP2102N DTR/RTS → Q1/Q2 transistors → EN and GPIO0
  - Handles automatic bootloader entry and reset during programming
- No firmware action required for USB serial; UART0 at default 115200 baud works out of the box

### 2. CAN Bus Transceiver - TCAN334 (U7)

**Interface:** CAN controller (ESP32 TWAI peripheral)

| Signal | ESP32 GPIO | TCAN334 Pin | Direction (from ESP32) |
|--------|-----------|-------------|----------------------|
| CAN_TX | GPIO19 | 1 (TXD) | Output |
| CAN_RX | GPIO18 | 4 (RXD) | Input |

- Powered from +3V3
- Bus lines: CANH (pin 6) and CANL (pin 7) → J4 connector pins 1 and 2
- R25 (560 ohm) on-board termination between CANH and CANL
- TCAN334 pin 5 = GND, pin 8 = GND (no silent/standby mode control)

**CAN Enable Circuit:**
- **Read enable status:** GPIO39 (CAN_EN_IN) — input only
  - Voltage divider: R30 (2M) from ~CAN_EN, R31 (680k) to GND
  - Ratio: ~CAN_EN voltage x 0.254; C12 (100nF) filtering
  - LOW = CAN enabled on bus side
- **Control enable:** GPIO16 (CAN_EN_OUT) — output
  - Drives Q6 (SI2302 N-FET): HIGH turns on Q6, pulling ~CAN_EN low
  - Set HIGH to enable CAN bus, LOW to disable
- ~CAN_EN line also exposed on J4 pin 3 for external enable/disable
- R26 (100k) connects ~CAN_EN to CAN_PWR (J4 pin 4)

**CAN Connector J4 (6-pin IDC, 2x07 vertical):**

| Pin | Net | Function |
|-----|-----|----------|
| 1 | CANH | CAN bus high |
| 2 | CANL | CAN bus low |
| 3 | ~CAN_EN | CAN enable (active low) |
| 4 | CAN_PWR | CAN power (linked to ~CAN_EN via R26) |
| 5 | GND | Ground |
| 6 | +5V | 5V supply |

### 3. RS-485 Transceiver - ST485EBDR (U8)

**Interface:** UART (use UART1 or UART2)

| Signal | ESP32 GPIO | ST485E Pin | Function |
|--------|-----------|------------|----------|
| RS485_TXD | GPIO25 | 4 (DI) | Transmit data to bus |
| RS485_RXD | GPIO27 | 1 (RO) | Receive data from bus |
| RS485_DTR_RTS | GPIO26 | 3 (DE) | Driver enable (HIGH = transmit) |

- Powered from +5V (pin 8); 5V tolerant RS-485 levels
- Bus lines: A (pin 6) and B (pin 7) → J3 connector pins 3 and 4

**Receiver Enable (RE, pin 2) — Solder Jumper JP1:**
- JP1 bridges pins 1-2 by default (per footprint name `Bridged2Bar12`)
- **Default (1-2 bridged):** ~RE tied to DE (GPIO26) → half-duplex mode
  - When DE=HIGH (transmitting): RE is disabled (HIGH)
  - When DE=LOW (receiving): RE is enabled (LOW)
- **Alternate (2-3 bridged):** ~RE tied to GND → receiver always enabled (full-duplex)

**Firmware half-duplex usage:**
1. To transmit: Set GPIO26 HIGH, send data on GPIO25
2. To receive: Set GPIO26 LOW, read data from GPIO27

**RS-485 / Power Connector J3 (5-pin IDC, 2x08 vertical):**

| Pin | Net | Function |
|-----|-----|----------|
| 1 | +12V | 12V supply (from boost converter) |
| 2 | +5V | 5V supply |
| 3 | RS485_A | RS-485 bus A (non-inverting) |
| 4 | RS485_B | RS-485 bus B (inverting) |
| 5 | GND | Ground |

### 4. Pressure/Depth Sensor - MS5837-30BA (U9)

**Interface:** I2C

| Signal | ESP32 GPIO | MS5837 Pin | Function |
|--------|-----------|------------|----------|
| SCL | GPIO22 | 3 (SCLK) | I2C clock |
| SDA | GPIO21 | 4 (SDI/SDA) | I2C data |

- Powered from +3V3 (pin 2); GND (pin 1)
- I2C address: 0x76 (fixed, per MS5837 datasheet)
- Pull-ups: R20 (10k) on SCL, R19 (10k) on SDA, both to +3V3
- C19 (100nF) bypass capacitor on VDD
- Measures absolute pressure (0-30 bar) and temperature
- 24-bit ADC, I2C fast mode (up to 400 kHz supported)

### 5. Status LEDs (D1-D4)

All LEDs are active HIGH with 2k series resistors.

| LED | GPIO | Resistor | Color |
|-----|------|----------|-------|
| LED_0 (D1) | GPIO15 | R1 (2k) | LED (C84256) |
| LED_1 (D3) | GPIO13 | R3 (2k) | LED (C84256) |
| LED_2 (D2) | GPIO14 | R2 (2k) | LED (C84256) |
| LED_3 (D4) | GPIO12 | R4 (2k) | LED (C84256) |

**Boot considerations:**
- GPIO12 (LED_3): Controls flash voltage at boot. Must be LOW during boot for 3.3V flash. Avoid external pull-ups.
- GPIO15 (LED_0): Must be HIGH at boot for normal boot (LED will briefly light during boot). Outputs debug log on UART at boot.

---

## Power Management

### Power Input Sources

1. **USB-C (J1):** VBUS (~5V) → TPS2121 power mux input
2. **Battery (J2):** Through F1 (2A fuse) → V_BATT → TPS2121 power mux input
   - Reverse polarity protection via Q3 (SI2302 N-FET) on ground path
   - ESD protection via D5 (PESD15VS2UT) on V_BATT
3. **External (J3):** +12V (pin 1) and +5V (pin 2) directly to power rails

### Power Rails

| Rail | Source | Regulator | Notes |
|------|--------|-----------|-------|
| +3V3 | VBUS or V_BATT (auto-selected) | U5: LM1085-3.3 (LDO) | Always on; main ESP32 supply |
| +5V | V_BATT | U4: TPS7A2550DRVR (LDO) | Controllable; feeds RS-485, CAN connector |
| +12V | V_BATT | U6: LMR62014XMF (boost) | Controllable; available on J3 pin 1 |

### Power Control GPIOs

| GPIO | Net | Controls | Logic |
|------|-----|----------|-------|
| GPIO23 | 5V_EN | U4 TPS7A2550 enable | HIGH = 5V on |
| GPIO32 | 12V_SMPS_EN | U6 LMR62014 enable | HIGH = SMPS enabled (also has pull-up to switched V_BATT via R17) |
| GPIO33 | 12V_EN | Q5 P-FET input power to U6 | **LOW = 12V power input on**, HIGH = off |

**To enable 12V output:**
1. Set GPIO33 LOW (connect V_BATT to boost converter input via Q5)
2. Set GPIO32 HIGH (enable the SMPS controller)

**To disable 12V output:** Set GPIO32 LOW, or set GPIO33 HIGH (either will work).

### Power Monitoring GPIOs (read-only)

| GPIO | Net | Reads | Interpretation |
|------|-----|-------|----------------|
| GPIO35 | 5V_OK | TPS7A2550 power-good | HIGH = 5V rail is good; has 100k pull-up (R18) to 3V3 |
| GPIO34 | 3.3V_SRC | TPS2121 status output | Indicates which input source is selected |
| GPIO36 | BATT_ADC | Battery voltage (divided) | V_BATT / 3 via divider (R10=100k to V_BATT, R7\|\|R8=50k to GND) |

**Battery voltage calculation:**
```
V_BATT = ADC_reading_volts * 3.0
```
With ESP32 ADC (0-3.3V range), measurable battery range is 0-9.9V.

---

## Suggested ESP32 Peripheral Assignment

| ESP32 Peripheral | Pins | Usage |
|-----------------|------|-------|
| UART0 | GPIO1 (TX), GPIO3 (RX) | USB debug console (CP2102N) |
| UART1 or UART2 | GPIO25 (TX), GPIO27 (RX) | RS-485 communication |
| TWAI (CAN) | GPIO19 (TX), GPIO18 (RX) | CAN bus communication |
| I2C (Wire) | GPIO21 (SDA), GPIO22 (SCL) | MS5837-30BA pressure sensor |
| ADC1_CH0 | GPIO36 | Battery voltage monitoring |
| Digital Input | GPIO34 | Power source status |
| Digital Input | GPIO35 | 5V power-good status |
| Digital Input | GPIO39 | CAN enable status |
| Digital Output | GPIO23 | 5V rail enable |
| Digital Output | GPIO32 | 12V SMPS enable |
| Digital Output | GPIO33 | 12V input power enable |
| Digital Output | GPIO16 | CAN bus enable |
| Digital Output | GPIO26 | RS-485 TX/RX direction |
| Digital Output | GPIO15, 13, 14, 12 | Status LEDs 0-3 |

### Available (unused) GPIOs

- **GPIO2** — available (but boot-sensitive: must be low/floating for SPI boot)
- **GPIO4** — fully available
- **GPIO5** — available (outputs PWM signal at boot)
- **GPIO17** — fully available

---

## Connector Pinouts Summary

```
J1 (USB-C):          J2 (Battery, 2-pin):
  1: VBUS               1: V_BATT+ (through F1 fuse)
  2: D-                  2: V_BATT- (through Q3 protection)
  3: D+
  4: GND

J3 (RS-485/Power, 5-pin IDC):     J4 (CAN Bus, 6-pin IDC):
  1: +12V                            1: CANH
  2: +5V                             2: CANL
  3: RS485_A                         3: ~CAN_EN (active low)
  4: RS485_B                         4: CAN_PWR
  5: GND                             5: GND
                                     6: +5V
```
