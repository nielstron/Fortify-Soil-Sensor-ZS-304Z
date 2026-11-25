# Installation and reference

How to back up, build, flash and set up the custom firmware for the **Arteco ZS-304Z** (PCB ZSSF21 V1.0, Telink TLSR8251F512ET32: 512 KB flash, 32 KB SRAM), plus the settings it exposes in Home Assistant. Hardware details and how the pin map was obtained are in [docs/HARDWARE.md](docs/HARDWARE.md).

## 1. Wiring

The board has a labelled programming header (`S`, `GND`, `3V`) next to the chip; `S` is the chip's SWire pin (SWS). Remove the batteries first.

```
adapter TX ──[1 kΩ]──┬── S
adapter RX ──────────┘
adapter GND ──────────── GND
adapter 3V3 ──────────── 3V
```

- Use a **3.3 V** USB-UART adapter (CH340 works; FTDI chips and adapters with an LED on RX do not).
- Note the serial port name (`ls /dev/cu.*` on macOS). It changes with the USB socket.

## 2. Back up the stock firmware

The flasher is pvvx's [TLSR825xComFlasher](https://github.com/pvvx/TlsrComSwireWriter) (`TLSR825xComFlasher-macos.py`), kept locally in the gitignored `backup/` folder. In its activation loop, replace `drain_input(serialPort)` with `serialPort.reset_input_buffer()`. Otherwise it sends the halt command only ~20 % of the time and misses chips whose firmware reuses the SWS pin shortly after boot.

**Halting the chip:** start the command, then when it prints `Activate (60000 ms)...`, unplug **GND** for ~2 s and plug it back in. Do not power-cycle 3V instead: the idle-high TX line back-powers the chip through `S`, so it never resets cleanly. A halted chip stops blinking and stays halted while powered.

```sh
uv run --with pyserial python backup/TLSR825xComFlasher-macos.py -p /dev/cu.usbserial-XXXX -t 60000 -c 57 rf 0 0x80000 backup/stock_1.bin
```

Read it a second time (`-t 0`, no replug) into `stock_2.bin` and compare the hashes. A full read takes ~30 min. The dump contains the Zigbee keys and device identity: keep it private.

**If the chip is halted but no speed works** (`no` at every setting), run the speed scan while the board stays powered. It prints the working `-c` value for a `-t 0` command:

```sh
uv run --with pyserial python tools/swire_speed_scan.py /dev/cu.usbserial-XXXX
```

## 3. Build

Requires Python 3, Git and Docker Desktop (the Telink compiler is Linux x86-64; Docker uses `linux/amd64`, also on Apple Silicon).

```sh
python3 tools/build.py --profile hardware/arteco-zssf21.json
```

The image is `build/arteco/ZigbeeTLc/arteco-bin/ZS304Z.bin`, and the OTA file is `…/1141-f304-<version>-ZS304Z.zigbee`. Remove `build/arteco/ZigbeeTLc` before rebuilding. On Linux amd64, `--native` builds without Docker.

Other build modes:
- `--compile-check` uses `hardware/compile-check.json` (reference pins, no flashable output) and is what CI runs.
- `--hw-test` builds a bench test image without Zigbee (see section 7).

Build details: pvvx [ZigbeeTLc](https://github.com/pvvx/ZigbeeTLc) is pinned in `firmware/upstream.json`, and the files in `firmware/src/` replace their upstream versions. They contain the original Fortify changes (`firmware/fortify_zg303z.patch`, against the older pvvx revision `fortify_patch_base`) and this port's changes. When moving the pin, merge the upstream changes into these files (3-way, base = the old pin). Board target `BOARD_ARTECO_ZS304Z` (private board ID 126), manufacturer/model **Fortify / ZS-304Z-Raw**, OTA image type **0xF304**. `build.py` generates the board header from the hardware profile and checks the TLSR8251 memory limits.

## 4. Flash

With the chip halted (activation as in section 2, or still halted from a previous command):

```sh
uv run --with pyserial python backup/TLSR825xComFlasher-macos.py -p /dev/cu.usbserial-XXXX -t 0 -c 57 wf 0 build/arteco/ZigbeeTLc/arteco-bin/ZS304Z.bin
```

A ~135 KB write takes 2–5 minutes. Use the `-c` value that worked when halting, or the one from the speed scan. Optionally read the same length back and `cmp` it with the image. Then replug GND to start the firmware: the LED blinks briefly while it is not paired.

This is the only flash that needs the cable: later updates go over the air (section 6).

## 5. Home Assistant (ZHA)

1. Copy `quirk/fortify_arteco_zs304z.py` into your ZHA custom quirks folder (`zha: custom_quirks_path:`) and restart Home Assistant. It matches only `Fortify / ZS-304Z-Raw`.
2. If the sensor was paired before with the stock firmware, remove it in ZHA first (it keeps its IEEE address).
3. Add the device in ZHA. Settings changes reach it within ~7 s, because it polls its parent at least that often.

The device page then shows the entities in the [README screenshots](README.md#in-home-assistant). The settings are explained in section 7.

## 6. OTA updates

Point ZHA at a local index (`configuration.yaml`):

```yaml
zha:
  zigpy_config:
    ota:
      extra_providers:
        - type: zigpy_local
          index_file: /config/zigpy_ota/index.json
```

Put the `.zigbee` file next to an `index.json` that lists it:

```json
{"firmwares": [{"path": "1141-f304-01473001-ZS304Z.zigbee", "file_version": 21442561, "file_size": 136786,
  "image_type": 62212, "manufacturer_id": 4417, "manufacturer_names": ["Fortify"], "model_names": ["ZS-304Z-Raw"],
  "checksum": "sha3-256:<sha3-256 of the file>"}]}
```

After a restart, the device's firmware update entity offers the new version. The transfer takes ~25 min for a sleepy device. Never offer a `--hw-test` image this way: it has no Zigbee.

## 7. Settings

All on cluster 0x0204 (endpoint 1) and saved on the device unless noted. The device reports all of them after every (re)join, so the entities always match.

| Attr | Entity | Default | Meaning |
|---|---|---|---|
| `0x0107` u8 | **Reporting interval** (3–255 s) | 20 s | The device wakes, measures temperature, humidity and battery, and sends due reports. |
| `0x0125` bool | **Debug mode** | off | Probe read and raw values reported every 500 ms; switches itself off after 10 min. Not saved. |
| `0x0126` u16 | **Probe interval** (10 s – 12 h) | 600 s | The soil probe is measured at most this often. |
| `0x0127` u16 | **Probe settle time** (1–1000 ms) | 300 ms | Probe power + excitation on → ADC read. |
| `0x0128` u16 | **Probe report threshold** (raw ADC counts) | 40 | A new raw reading is reported at once if it moved at least this much. |
| `0x0129` u8 | **Probe ADC samples** (1–16) | 4 | ADC reads averaged per reading. |
| `0x012A` u8 | **Probe heartbeat** (N, 1–100) | 6 | Unchanged values are re-sent every N × probe interval (default hourly). |
| `0x012B` u16 | **Probe excitation frequency** (10–2000 kHz) | 100 kHz | Stock value. |
| `0x012C` u8 | **Probe excitation duty** (1–99 %) | 50 % | |
| `0x012D` / `0x012E` u16 | **Probe calibration dry / wet** (raw, 0 = unset) | unset | Two-point linear moisture, shown as **Soil moisture** (0.01 %); unknown until both are set. |
| `0x012F` u8 | **Calibrate dry** / **Calibrate wet** | off | Switch on: at its next poll the device measures once, stores the reading as the dry / wet point with the battery voltage, and switches back off. |
| `0x0130` u8 | **Run probe sweep** | off | Switch on: one raw reading each at 1, 5, 20, 50, 100, 300 and 1000 ms settle, each from cold; results in the **Probe sweep N ms** sensors (~9 s). Switches back off when done. |
| `0x0131` / `0x0132` u16 | **Probe calibration dry / wet battery** (mV, 0 = no compensation) | 0 | Battery voltage when each point was taken; recorded by the calibrate switches. |
| `0x0133` / `0x0134` u16 | **Battery compensation slope dry / wet** (‰) | 905 / 804 | Each point moves by slope/1000 probe-mV per battery mV: `point(V) = point_cal + slope·(V − V_cal)`. 0 = off. |
| `0x0137` u8 | **Find better parent** | off | Switch on: the device rejoins through the best parent in range and switches back off. |
| `0x0138` u8 | **Reset probe settings to default** | off | Probe interval, settle time, samples, threshold, heartbeat, excitation and compensation slopes back to the defaults (calibration kept). |

Raw values are on endpoint 2: **Soil raw ADC** (`0xF000`) and **Soil ADC reference** (`0xF001`, the same node read with the excitation held high; not a supply reference on this board).

**Calibration:** for the full range, calibrate dry in air and wet in water (or soil saturated after watering). A wet point in merely moist soil gives a narrow range: readings above it clamp to 100 %, and the noise becomes a larger share of the scale.

**Battery compensation** uses the stock firmware's own model, recovered by disassembly: dry = 900/995·V − 248 mV and wet = 800/995·V − 347 mV. Only the slopes matter (the offsets are absorbed by the calibration). On this unit, air readings at 3.23 V and 3.00 V matched the 905 ‰ slope within 11 mV.

**Probe sweep:** the probe output charges like an RC filter, `V(t) = V_end·(1 − e^(−t/τ))`, with τ ≈ 18 ms in air. The ratio r of the 20 ms to the 300 ms reading gives τ = −20 ms / ln(1 − r). In moist potting soil the sweep gave 774 / 2304 / 5547 / 7777 / 8429 / 8538 / 8516 at 1 / 5 / 20 / 50 / 100 / 300 / 1000 ms, i.e. τ ≈ 19 ms, the same as in air. So a settle time of ~7τ (≈ 130–150 ms) is enough; the default 300 ms has margin.

**Link monitoring:** the device tracks its last 10 data polls and sent messages. 3 failures among them make it rejoin through the best parent at full TX power, at most once per 24 h to protect the battery. A completely lost parent is recovered by pvvx's rejoin backoff: 30 s, 1, 2, 5, 10, 20, then every 30 min, first on the last known channel.

**Battery:** with the defaults, the average draw is estimated at ~11 µA (about half of it the 7 s polling), i.e. beyond the shelf life of 2×AAA. Short probe intervals and long debug sessions are what cost battery: debug mode uses about as much in 10 min as two days of normal operation.

## 8. Bench test image and dashboard

`python3 tools/build.py --profile hardware/arteco-zssf21.json --hw-test` builds a test image without Zigbee. Once per second (plus a settle sweep) it prints probe, reference, light, supply, SHTC3 and button readings on the **SWS pad at 1 Mbaud**, which the flashing adapter's RX receives. Holding the button keeps the probe supply and the LED on, for multimeter checks. Live view:

```sh
uv run --with pyserial python tools/hwtest_dashboard.py --port /dev/cu.usbserial-XXXX
```

The test image uses the SWS pin for its output, so every reflash needs the activation from section 2.

## Credits

- Original Fortify firmware: [sweitzja/Fortify-Soil-Sensor-ZG-303Z](https://github.com/sweitzja/Fortify-Soil-Sensor-ZG-303Z) (exact commit in `firmware/upstream.json`); its notes are kept under `docs/upstream/` as ZG-303Z references.
- [pvvx/ZigbeeTLc](https://github.com/pvvx/ZigbeeTLc) and pvvx's SWire tools.
