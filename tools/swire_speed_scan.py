"""Find the SWire speed setting of a halted TLSR825x (does not touch flash).

Tries every `-c` value (whole MHz) of pvvx's TLSR825xComFlasher, i.e. the SWS
divider (register 0x00b2) the flasher would set for it, and checks it by writing
and reading back test bytes in SRAM 0x840000 (harmless while the CPU is halted).
Recommends the value in the middle of the working range.

The chip must already be halted, e.g. by a flasher run with `-t` (activation)
during which the board was power-cycled; it stays halted while powered.

    uv run --with pyserial python tools/swire_speed_scan.py /dev/cu.usbserial-XXXX
"""
import argparse
import importlib.util
from pathlib import Path

import serial

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
parser.add_argument('port', help='USB-UART adapter wired to SWS (TX -> 1k -> SWS, RX -> SWS)')
parser.add_argument('--flasher', type=Path, default=ROOT / 'backup/TLSR825xComFlasher-macos.py',
                    help='pvvx TLSR825xComFlasher script providing the SWire helpers')
parser.add_argument('--baud', type=int, default=921600)
args = parser.parse_args()

spec = importlib.util.spec_from_file_location('flasher', args.flasher)
fl = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fl)


def flasher_div(mhz):
    """Divider the flasher sets for `-c mhz` (see set_sws_speed); it accepts at most 0x7f."""
    return round(mhz * 1e6 * 2 / args.baud)


s = serial.Serial(args.port, args.baud, timeout=0.1)
fl.drain_input(s)
mhz_values = [m for m in range(1, 100) if 0x10 <= flasher_div(m) <= 0x7f]
print(f'scanning -c {mhz_values[0]}..{mhz_values[-1]} on {args.port} at {args.baud} baud (CPU must already be halted)', flush=True)
ok = []
for mhz in mhz_values:
    div = flasher_div(mhz)
    fl.rd_sws_wr_addr_usbcom(s, 0x00b2, bytearray([div]))
    good = 0
    for val in (0x00, 0xff, 0x55, 0xaa, 0x0f, 0xf0, 0x5a, 0x81):
        fl.rd_sws_wr_addr_usbcom(s, 0x840000, bytearray([val]))
        r = fl.sws_read_data(s, 0x840000, 1)
        good += r is not None and r[0] == val
    if good == 8:
        ok.append(mhz)
    print(f'-c {mhz:2d} (div 0x{div:02x}): {good}/8', flush=True)
fl.rd_sws_wr_addr_usbcom(s, 0x00b2, bytearray([0x05]))

if not ok:
    raise SystemExit('RESULT: no working setting - is the chip halted (LED not blinking) and powered?')
print(f'RESULT: -c {ok[0]}..{ok[-1]} work; use -c {ok[len(ok) // 2]}')
