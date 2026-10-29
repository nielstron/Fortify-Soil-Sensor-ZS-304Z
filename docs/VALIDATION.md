# Validation — 2026-10-29

This is a software-validated bring-up port, not hardware validation.

- Full Telink TC32 compile and link succeeded in an amd64 Ubuntu 22.04 container on an Apple Silicon Mac.
- All source paths were prepared from pinned pvvx revision `bda98388738e35897bfe230b74211bcecb4dc039` and the original Fortify overlay.
- The original Fortify patch applies cleanly to that pinned base.
- The linked image footprint is 133,280 bytes, below 512 KB flash.
- TLSR8251 linker/startup stack end is `0x848000` (32 KB SRAM), with the linker RAM reservation ending at `0x845f00`.
- Runtime pin assignments were **reference ZG-303Z pins solely for compilation**, not measured Arteco pins. Compile-check mode produces no flash/OTA images.
- The shipped Arteco hardware profile is rejected for hardware builds while `verified` is false.
- Two ZHA contract tests passed against `zha-quirks==2.3.0`, `zigpy==2.3.0`: raw-ADC attributes/configuration and identity isolation from stock Arteco, AOYAN and original Fortify.
- The Arteco OTA index is empty. No firmware was flashed and no running Home Assistant integration/configuration was changed by this fork task.

Remaining work: header continuity, GPIO and analog-circuit mapping, original-flash backup, hardware acquisition tests, sensor-specific calibration and power/radio validation. The retained upstream adaptive radio/parent-rejoin logic has not been retested on this board.
