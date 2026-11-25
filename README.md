# Fortify raw ADC port for Arteco ZS-304Z

A hardware bring-up fork of [sweitzja/Fortify-Soil-Sensor-ZG-303Z](https://github.com/sweitzja/Fortify-Soil-Sensor-ZG-303Z), targeting the **Arteco ZS-304Z, PCB ZSSF21 V1.0, Telink TLSR8251F512ET32 (512 KB flash, 32 KB SRAM)**.

**Status: pin map bench-verified; Zigbee firmware builds, first hardware flash pending.** The pins were recovered from the stock firmware and confirmed with the `--hw-test` bench image (see [hardware bring-up](docs/HARDWARE.md)); the light sensor on PB5 reads 0 also with the stock firmware and is probably not fitted. The Zigbee build itself (pairing, reporting, debug mode, OTA) has not yet been tested on the device. The original ZG-303Z binaries and OTA entries have been removed from this fork. Its OTA index is intentionally empty.

Flashing is over SWire with a USB-UART adapter (TX → 1 kΩ → S, RX → S, GND): see *Firmware backup* and *Stock firmware analysis* in docs/HARDWARE.md for the backup, activation (power-cycle GND, not 3V) and speed settings. If the flasher halts the chip (LED stops) but finds no speed (`no` at every setting), run `tools/swire_speed_scan.py <port>` while the board stays powered; it prints the working `-c` value for a `-t 0` flash.

The goal is to expose the probe's ADC readings before the stock firmware clamps them to 0–100%. Our stock Arteco responded to air and water, but disagreed strongly with an AOYAN in the same soil. Applying a percentage offset cannot reliably recover information already lost inside the firmware.

## What is implemented

- Pinned pvvx source revision matching the original Fortify patch, SDK and toolchain.
- A separate Arteco board target (`BOARD_ARTECO_ZS304Z`, private board ID 126).
- Explicit hardware profile with **no guessed Arteco GPIO defaults**.
- A reference-only compile check that compiles and links all firmware without producing flash/OTA images.
- Raw probe ADC (`0xF000`) and DC-reference ADC (`0xF001`) on endpoint 2, cluster `0x0405`.
- Manufacturer/model **Fortify / ZS-304Z-Raw**, distinct from stock and original Fortify devices.
- Separate OTA image type **0xF304**, distinct from original Fortify's 0xF32C.
- ZHA quirk and a Home Assistant calibration example using measured reference points.

The raw acquisition sequence currently follows Fortify's PWM/DC method. Its suitability for the Arteco's analog circuit still needs verification. The inherited ZG-303Z percentage curve is **disabled for the Arteco target**; the corresponding measured-value attribute reports ZCL unknown (`0xffff`). Do not interpret the DC reading as validated battery compensation yet.

## Build on macOS

Requires Python 3, Git and a running Docker Desktop. The supplied Telink compiler is Linux x86-64, so Docker uses `linux/amd64` (also on Apple Silicon).

```sh
python3 tools/build.py --compile-check
```

This uses `hardware/compile-check.json`, whose pins come from ZG-303Z **only to check compilation**. The build log, ELF/listing and `build/compile-check/build-info.json` remain available; no `.bin` or OTA images are generated. Do not flash the ELF either: those are reference pins, not this board's wiring.

For a hardware build, complete `hardware/arteco-zssf21.json`, record the measurement evidence, and set `verified` to true only after checking every field:

```sh
python3 tools/build.py --profile hardware/arteco-zssf21.json
```

The command refuses the shipped unverified profile. A completed profile permits build output under `build/arteco/ZigbeeTLc/arteco-bin/`; a successful compiler run does not establish hardware compatibility. On Linux amd64, add `--native` after installing build-essential, Python 3, Git, bzip2 and libncurses5. Each invocation requires a fresh generated source directory; remove only the previous `build/compile-check/ZigbeeTLc` or `build/arteco/ZigbeeTLc` before rebuilding.

See [hardware bring-up](docs/HARDWARE.md) before connecting anything. There is no flash/erase command in this fork's build wrapper.

## Home Assistant

For **custom firmware only**, copy `quirk/fortify_arteco_zs304z.py` into your configured ZHA custom-quirks directory and restart HA. It matches only `Fortify / ZS-304Z-Raw`; it does not replace the stock Arteco Tuya quirk. The quirk targets the current `zhaquirks.builder` API used by this installation.

Once hardware measurements are validated, use `homeassistant/soil_moisture_calibrated.yaml`. Its calibration helpers start without assumed dry/wet values; the derived percentage is unavailable until distinct reference values are provided. Keep original ADC entities available for diagnosis. Sensor entity IDs must be replaced with those in your installation.

## Sources and credit

- Original Fortify source: see `firmware/upstream.json` for its exact commit.
- [pvvx/ZigbeeTLc](https://github.com/pvvx/ZigbeeTLc), pinned to `bda98388738e35897bfe230b74211bcecb4dc039`, whose source blobs match Fortify's patch base.
- [pvvx ZG-303Z hardware reference](https://pvvx.github.io/ZG-303Z/) describes a different PCB. Its header/pin map must not be substituted for this board.
- Original Fortify design notes and changes are retained under `docs/upstream/` as **ZG-303Z references**, not Arteco hardware claims.

Keep the original MIT license. SDK/source files retain their original Telink notices.

## Bench test image and dashboard

`python3 tools/build.py --profile hardware/arteco-zssf21.json --hw-test` builds a non-Zigbee test image (allowed with an unverified profile). It prints probe, reference, light, supply, SHTC3 and button readings once per second as GPIO printf on the **SWS pad at 1 Mbaud**, so the SWire adapter's RX shows them. Holding the button keeps PD3 (probe supply) and the LED on for multimeter checks. Live view:

```sh
uv run --with pyserial python tools/hwtest_dashboard.py --port /dev/cu.usbserial-XXXX
```

The test image reconfigures SWS as an output; to flash again, power-cycle the board during the flasher's activation window (`-t`).

## Probe settings, calibration, debug mode and polling (Zigbee)

All on cluster 0x0204 (UI config, EP1), manufacturer-specific, exposed by the quirk. Saved on the device unless noted.

| Attr | Entity | Default | Meaning |
|---|---|---|---|
| `0x0125` bool | **Debug mode** switch | off | Probe read and raw `0xF000`/`0xF001` reported to the coordinator every 500 ms; switches itself off after 10 min and reports off. RAM only. |
| `0x0126` u16 | **Probe interval** (10 s – 12 h) | 600 s | Probe measured at most this often. Temperature/humidity/battery use the measurement interval `0x0107` (default 20 s). |
| `0x0127` u16 | **Probe settle time** (1–1000 ms) | 300 ms | Power + PWM on → ADC read. Tune with the bench sweep. |
| `0x0128` u16 | **Probe report threshold** (raw counts) | 40 | A new raw reading is reported at once if it moved at least this much. |
| `0x0129` u8 | **Probe ADC samples** (1–16) | 4 | ADC reads averaged per probe phase. |
| `0x012A` u8 | **Probe heartbeat** (N, 1–100) | 6 | Unchanged values re-sent every N × probe interval (default hourly). |
| `0x012B` u16 | **Probe excitation frequency** (10–2000 kHz) | 100 kHz | PWM2 on PC4 (stock value). |
| `0x012C` u8 | **Probe excitation duty** (1–99 %) | 50 % | |
| `0x012D` / `0x012E` u16 | **Calibration dry / wet** (raw, 0 = unset) | unset | Two-point linear moisture → standard `measuredValue` on EP2 (0.01 %), shown as **Soil moisture**. Unknown until both are set. |
| `0x012F` u8 | **Calibrate dry / wet** buttons | — | Write 1 / 2: measure now and store that raw reading as the dry / wet point. |
| `0x0130` u8 | **Run probe sweep** button | — | Write 1: one raw reading each at 1, 5, 20, 50, 100, 300 and 1000 ms settle, each after 1 s with the probe unpowered; results on EP2 `0xF010`–`0xF016` (**Probe sweep N ms** diagnostic sensors), reported as taken (~9 s in total). RAM only. |
| `0x0131` / `0x0132` u16 | **Calibration dry / wet: battery** (mV, 0 = no compensation) | 0 | Battery voltage when each point was taken; recorded by the calibrate buttons. |
| `0x0133` / `0x0134` u16 | **Battery compensation slope dry / wet** (‰) | 905 / 804 | Each point moves by slope/1000 probe-mV per battery mV: `point(V) = point_cal + slope·(V − V_cal)`. 0 = off. |

The sweep shows how the probe output charges after power-on. In air it follows `V(t) = V_end·(1 − e^(−t/τ))` with τ ≈ 18 ms; the ratio of the 20 ms to the 300 ms reading gives τ = −20 ms / ln(1 − ratio). If τ is the same in water/soil, a settle time of ~7τ (≈ 120–150 ms) is within the noise.

Battery compensation uses the stock firmware's own model, recovered by disassembly: dry = 900/995·V − 248 mV and wet = 800/995·V − 347 mV, i.e. both end points move linearly with the battery voltage V. Only the slopes matter (the offsets are absorbed by the calibration), so they are configurable and default to 905 ‰ and 804 ‰. They describe the stock firmware's assumption for this circuit, not a measurement on this unit.

The device polls its parent at least every **7 s**, independent of the measurement interval: parents hold frames for a sleepy child only ~7.68 s (Zigbee default), so any config write reaches the device on its next poll. A Poll Control *set long poll interval* only changes polling (≤ 7 s), never the measurement interval.

Debug mode keeps the CPU busy for the settle time twice a second (≈ 60 % duty at 300 ms), so it costs about as much as two days of normal operation per 10 min.
