# Custom firmware for the Arteco ZS-304Z soil sensor

> Vibecoded project but verified on hardware

> AliExpress link: https://s.click.aliexpress.com/e/_c3O0zJDJ

<a href="https://s.click.aliexpress.com/e/_c3VsyyDL"><img src="https://ae-pic-a1.aliexpress-media.com/kf/See22da7a48c146f9806a8a8e053ca878X.jpg_350x350.jpg" alt="Arteco ZS-304Z soil sensor" width="200" align="right"></a>

Zigbee firmware for the [Arteco ZS-304Z soil moisture sensor](https://s.click.aliexpress.com/e/_c3VsyyDL) (PCB ZSSF21), based on [Fortify](https://github.com/sweitzja/Fortify-Soil-Sensor-ZG-303Z) and [pvvx/ZigbeeTLc](https://github.com/pvvx/ZigbeeTLc), with a Home Assistant (ZHA) quirk.

The stock firmware turns the probe reading into a percentage clamped to 0–100 % with its own fixed curve, so information is lost and two sensors in the same soil can disagree strongly. This firmware reports the **raw probe value** and lets you decide:

- your own **dry/wet calibration** (one switch each in Home Assistant), with battery-voltage compensation
- a configurable **probe interval**, settle time and excitation; temperature and humidity keep their own interval
- **debug mode** (probe values every 500 ms) and a **probe sweep** to check settling
- **OTA updates** from Home Assistant after the first flash
- battery life estimated beyond the shelf life of the 2×AAA cells with the defaults

## What I used to flash it

- a 3.3 V USB-UART adapter (CH340)
- [resistors](https://s.click.aliexpress.com/e/_c3o2QKSd) (1 kΩ between the adapter's TX and the board's SWire pad)
- [jumper wires](https://s.click.aliexpress.com/e/_c4W5qWTX)
- [clamp fixtures](https://de.aliexpress.com/item/1005004869027755.html?channel=twinner) to hold contacts on the board's programming pads
- [breadboard](https://de.aliexpress.com/item/1005006143786009.html?channel=twinner)

## How to do it

See **[INSTALLATION.md](INSTALLATION.md)**: wiring, backing up the stock firmware, building, flashing, Home Assistant setup, OTA updates and all settings. Hardware details are in [docs/HARDWARE.md](docs/HARDWARE.md).

## License

MIT, as the original Fortify firmware. SDK and source files keep their Telink notices.
