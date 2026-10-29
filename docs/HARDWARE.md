# Arteco ZS-304Z: ZSSF21 V1.0 bring-up

## Confirmed from the owner and board photographs

- PCB silkscreen: **ZSSF21 V1.0**.
- MCU marking: **TLSR8251 / F512ET32 / ZH8M2612 / E08H11**.
- Flash target: 512 KB. Read and confirm actual flash geometry with the programmer before backup.
- Crystal marking: **24.000 MHz**.
- Five accessible pads next to the chip. In the antenna-up / probe-down orientation:

| Position from top | Board marking | Status |
|---|---|---|
| 1 | Unlabelled | Unknown |
| 2 | Unlabelled | Unknown |
| 3 | S | Candidate SWS; verify continuity to PA7 |
| 4 | GND | Labelled ground; verify against battery negative |
| 5, square | 3V | Labelled supply; verify against battery positive |

These positional numbers are **not** a claimed manufacturer connector numbering convention. No connections have been measured yet.

The earlier six-pad PCB photograph is an **AOYAN AY-303Z**, not this Arteco. Do not mix the two pin maps.

## Remaining measurements

With batteries removed and programmer disconnected, map the accessible header and trace the following to the MCU using its verified package pinout:

1. S pad to PA7/SWS; ground and supply to battery contacts.
2. Button signal and active level.
3. LED signal and active level.
4. Digital air-temperature/humidity sensor SDA and SCL; supported I²C pin group.
5. Probe excitation output: GPIO, PWM mux function and PWM channel.
6. Probe sense input: GPIO and ADC channel.
7. Analog circuit topology: confirm whether PWM excitation plus a DC-reference phase is appropriate. Pin continuity alone does not validate this method.

Record each result in `hardware/arteco-zssf21.json` with the measurement evidence. No GPIO in that file currently has an assumed value. `hardware/compile-check.json` is the older ZG-303Z map and is explicitly not a candidate Arteco map.

## Firmware backup before any writes

Use a **read-capable Telink SWire programmer** and temporary contacts/pogo pins on the exposed pads. Reading need not require board removal or desoldering. Confirm adapter logic voltage and its supported wiring before connecting; do not power from both batteries and an adapter supply.

A write-only web flasher cannot back up firmware. [pvvx/TLSRPGM](https://github.com/pvvx/TLSRPGM) supports flash reads and lists TLSR8251 among tested chips; the exact adapter wiring depends on the programmer. Ordinary UART output is not the same as SWire access.

Read the full flash twice, compare hashes, and keep both dumps locally with the chip identification and flash size. A full dump may contain Zigbee keys and device identity; do not publish it in this public fork. The normal build output does not touch a programmer.

After an original dump and verified pin map are available, first compare raw ADC readings in air, water and soil. Keep the built-in percentage unknown until an Arteco-specific conversion is justified. A later OTA release must use image type 0xF304 and model ZS-304Z-Raw; the current index has no release.

## Memory layout

The TLSR8251 has 32 KB SRAM (0x840000–0x847fff), not the 64 KB of TLSR8258. This fork limits the linker stack end to 0x848000 and uses that symbol in both startup stack literals. The build checks the linked stack boundary, RAM usage and flash size. See [Telink’s TLSR825x product table](https://www.telink-semi.com/products/bluetooth-le/tlsr825x).
