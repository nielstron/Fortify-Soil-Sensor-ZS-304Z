# Fortify raw ADC port for Arteco ZS-304Z

A hardware bring-up fork of [sweitzja/Fortify-Soil-Sensor-ZG-303Z](https://github.com/sweitzja/Fortify-Soil-Sensor-ZG-303Z), targeting the **Arteco ZS-304Z, PCB ZSSF21 V1.0, Telink TLSR8251F512ET32 (512 KB flash, 32 KB SRAM)**.

**Status: compile-checkable port; not yet hardware validated or ready to flash.** The S/GND/3V programming header is labelled, but the probe, LED, button and I²C pin assignments still need measurements. The original ZG-303Z binaries and OTA entries have been removed from this fork. Its OTA index is intentionally empty.

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
