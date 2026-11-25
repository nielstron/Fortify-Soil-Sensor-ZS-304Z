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

## Stock firmware analysis (2026-11-24)

The stock flash was read over SWire with pvvx `TLSR825xComFlasher` (macOS variant) through a CH340 + 1 kΩ adapter. The `S` pad is SWS. The chip only answers SWire when caught at power-up (replug 3V during `-t 60000`); once halted, reads need `-t 0 -c 57` (SWS divider 0x7c) because its reply rate is above the script's auto-search range. A full 512 KB read takes ~29 min. Dumps stay local in the gitignored `backup/` folder.

The stock image is a plain Telink Zigbee SDK build (`KNLT` header, 0x24614 bytes; NV at 0x44000+, MAC at 0x76000). Driver functions were located by byte-matching our SDK build (`gpio_set_func` 0x20438, `gpio_set_input_en` 0x2037c, `gpio_setup_up_down_resistor` 0x20a78, `cpu_set_gpio_wakeup` 0x20c50); pins were read from the call arguments (Telink pin = port<<8 | bit mask) and the enums in Telink's `chip_8258/gpio.h`.

| Function | Pin | Evidence in stock firmware |
|---|---|---|
| Button | PB6, active low | `.data` pin variable = 0x0140; GPIO input, 1 M pull-up, deep-sleep wake on level 0 (`start init wakeup gpio`) |
| LED | PD2, active high | LED table at 0x23210 = {PD2, PD3}; all callers use index 0, write 1 = on |
| Probe/light power | PD3, active high | Driven high before each measurement, low after |
| Probe excitation | PC4, `AS_PWM2` | PWM2 cycle reg 0x79c = 480 ticks / 240 duty → 100 kHz, 50 % at 48 MHz |
| Probe sense | PB4, ADC (B4P = 5) | Read after 300 ms settle → printed as `soil_moisture` |
| Light sensor | PB5, ADC (B5P = 6) | Printed as `illumiance` |
| Battery | PC5, ADC VBAT mode | Same pin the existing port uses |
| Temp/RH | SHTC3 at 0x70, SDA PC0 / SCL PC1 | Bit-banged I²C; commands 0x3517 wake, 0x6458 measure, 0xB098 sleep |
| Debug log | PC2 software UART TX ≈1 Mbaud; HW UART PB1 TX / PB7 RX | `printf` path; likely the two unlabelled header pads (unverified) |

Stock moisture conversion: both end points scale with battery voltage V (mV): `vmin = 900·V/995 − 248`, `vmax = 800·V/995 − 347`; result is clamped to 0–100 %. Raw readings should therefore be reported alongside battery voltage.

Open items before setting `verified`: the port does not yet drive PD3 during acquisition; confirm the two unlabelled pads; check the PWM/ADC timing (stock: 100 kHz, 300 ms settle) against the inherited Fortify acquisition.

## Bench test results (2026-11-24, `--hw-test` image)

Flashed over SWire (write 272 s, read-back verified), powered from the adapter's 3V3, output read from the SWS pad at 1 Mbaud (`tools/hwtest_dashboard.py`).

| Function | Result |
|---|---|
| SHTC3 (PC0/PC1, 0x70) | ✅ plausible T/RH; reacts to touching the probe end (heat and skin moisture conduct along the PCB) |
| Button PB6 | ✅ reads 1 idle (active low) |
| Supply via PC5 | ✅ ≈3230 mV on the 3.3 V adapter |
| Probe PB4 with PD3 + PWM2 on PC4 | ✅ ≈2725 mV in air (stock dry point ≈2674 mV), reacts to touch |
| Light sensor PB5 | ❌ always 0 mV — also 0 with the stock firmware, so the sensor is most likely not fitted on this board |
