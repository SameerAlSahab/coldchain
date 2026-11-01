# ColdChain wiring (ESP32 DevKit V1, 30-pin)

## Pin table
| Function | GPIO | Notes |
|---|---|---|
| OLED SSD1306 SDA / SCL | 21 / 22 | I2C addr 0x3C, 3.3 V |
| DS18B20 data (x2 on same pin) | 4 | 4.7 kΩ pull-up to 3.3 V |
| Door reed MC-38 fridge 1 / 2 | 32 / 33 | other wire to GND, INPUT_PULLUP, LOW = closed |
| Relay IN1 / IN2 (compressors) | 26 / 27 | active-LOW |
| Buzzer (active) | 25 | HIGH = on |
| LEDs G1 / R1 / G2 / R2 | 16 / 17 / 18 / 19 | each via 220 Ω to GND |
| B1 page / B2 admin reset (hold 2 s) | 23 / 13 | to GND |
| B3 power-fail sim / B4 mute / B5 net info | 14 / 5 / 15 | to GND |
| Battery sense (ADC1_CH7) | 35 | 100k/100k divider from battery+ (x2) |

GPIO5 and GPIO15 are boot-strapping pins: do not hold B4/B5 while powering on.

## Parts list
ESP32 DevKit V1, SSD1306 128x64 I2C OLED, 2x DS18B20 (waterproof probes), 2x MC-38 reed + magnet, 2-channel 5 V relay module (active-LOW, opto-isolated), active buzzer, 2 green + 2 red LEDs, 4x 220 Ω, 5 push buttons, TP4056 charger module WITH protection (DW01), 1x 18650 cell + holder, breadboard/PCB, wires.

## Extra parts you must buy
- 1x 4.7 kΩ resistor (DS18B20 data pull-up)
- 2x 100 kΩ resistors (battery divider) + 100 nF ceramic cap from GPIO35 to GND (smooths ADC)
- 5 V boost converter (MT3608 or similar, >=1 A) if relays/ESP32 run from the battery
- 1x 1000 µF 16 V electrolytic across relay-module VCC/GND (placed next to the relays)
- Optional: NPN (2N2222) + 1 kΩ base resistor if your buzzer draws >12 mA
- Fuse + proper enclosure for the mains side

## ASCII wiring notes
```
3V3 ──┬── 4.7k ──┬── GPIO4 ── DS18B20#1 DQ ── DS18B20#2 DQ
      │          │   (red=3V3, black=GND, yellow=DQ on both probes)
      └──────────┘
GPIO21 ── OLED SDA        GPIO22 ── OLED SCL      (OLED VCC 3V3, GND)
GPIO32 ── MC-38 #1 ── GND     GPIO33 ── MC-38 #2 ── GND
GPIO26 ── Relay IN1           GPIO27 ── Relay IN2     (relay VCC = 5 V, GND common)
GPIO25 ── Buzzer(+) , Buzzer(-) ── GND
GPIOxx ── 220R ── LED anode, LED cathode ── GND       (xx = 16,17,18,19)
GPIOxx ── button ── GND                                (xx = 23,13,14,5,15)

Battery+ ── 100k ──┬── 100k ── GND
                   └── GPIO35 ── 100nF ── GND
```
Compressor: relay COM/NO in series with the compressor's mains LIVE wire. Mains wiring must be done by a qualified person, fused and enclosed. If the relays stay on when the GPIO is HIGH (3.3 V vs 5 V pull-up on some modules), use a module with a separate JD-VCC jumper or a level shifter.

## Power notes (TP4056 + 18650)
```
5V USB adapter ─► TP4056 IN+/IN-
18650 ◄► TP4056 B+/B-   (use the protected variant; OUT+/OUT- are the protected battery terminals)
TP4056 OUT+/OUT- ─► MT3608 (set 5.0 V BEFORE connecting) ─► ESP32 VIN (5 V) + relay VCC
```
- A TP4056 has no true power-path: load while charging works but keeps the cell cycling. For production use a power-path charger (e.g. IP5306-type or a UPS module).
- Relays draw ~70 mA each, the ESP32 with WiFi up to ~250 mA: size the boost for >=1 A and use a cell with >=2000 mAh.
- Firmware reads the battery through the divider: 3.3 V = 0 %, 4.2 V = 100 %. Divider tap must be BEFORE the boost converter.
- "Power failure" is simulated (B3/web). Real mains-loss detection is not wired; a free input (e.g. GPIO34) with a divider on the 5 V adapter rail could be added later.
