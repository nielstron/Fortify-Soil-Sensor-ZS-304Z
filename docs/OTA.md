# Arteco OTA channel (not yet released)

The Arteco target uses manufacturer code **0x1141**, private image type
**0xF304**, and model **ZS-304Z-Raw**. This separates it from pvvx board
images and original Fortify's 0xF32C channel.

`ota/index.json` is deliberately `[]`. There is no Arteco hardware-tested
release, and no original ZG-303Z binary belongs in this channel. Do not add
this fork as a Home Assistant OTA provider until the board has been tested.

After a verified pin map, original-flash backup and hardware validation:

1. Increase APP_BUILD in firmware/src/version_cfg.h for each release.
2. Build with the measured Arteco profile.
3. Check the OTA header manufacturer, image type, version, model and size.
4. Hardware-test before publishing the image and adding its SHA-512/index entry.

A stock Tuya image cannot be assumed to accept this OTA channel. Initial
conversion/recovery requires a separately validated programming procedure.
