#!/usr/bin/env python3
"""Prepare pinned Fortify sources and build the Arteco raw-ADC port."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def run(*args, cwd=None):
    subprocess.run(args, cwd=cwd, check=True)


def gpio(value):
    if not re.fullmatch(r'P[A-D][0-7]', value):
        raise ValueError(f'Invalid GPIO: {value}')
    return value


def board_header(profile):
    button, led = gpio(profile['button']), gpio(profile['led'])
    drive, sense = gpio(profile['pwm_pin']), gpio(profile['adc_pin'])
    if len({button, led, drive, sense}) != 4:
        raise ValueError('Button, LED, probe drive and ADC must use distinct GPIOs')
    for key, prefix in [('pwm_function', 'AS_PWM'), ('pwm_id', 'PWM'), ('i2c_group', 'I2C_GPIO_GROUP_')]:
        if not re.fullmatch(prefix + r'[A-Z0-9_]+', profile[key]):
            raise ValueError(f'Invalid {key}: {profile[key]}')
    if profile['button_active'] not in (0, 1) or profile['led_active'] not in (0, 1):
        raise ValueError('Active levels must be 0 or 1')
    if not 1 <= profile['adc_channel'] <= 13:
        raise ValueError('Invalid ADC channel')
    return f'''/* Generated from the explicitly selected hardware profile. */
#ifndef BOARD_ARTECO_ZS304Z_H
#define BOARD_ARTECO_ZS304Z_H
#define ARTECO_PINMAP_VERIFIED {int(profile['verified'])}
#if !ARTECO_PINMAP_VERIFIED && !defined(ARTECO_COMPILE_CHECK)
#error "Arteco pin map is unverified; measure the board before a hardware build"
#endif
#define DEV_SERVICES (SERVICE_ZIGBEE | SERVICE_OTA | SERVICE_THS | SERVICE_PLM | SERVICE_LED)
#define BLE_MODEL_STR "ZS-304Z-Raw"
#define BLE_MAN_STR "Fortify"
#define ZCL_BASIC_MFG_NAME {{7,'F','o','r','t','i','f','y'}}
#define ZCL_BASIC_MODEL_ID {{11,'Z','S','-','3','0','4','Z','-','R','a','w'}}
#define USE_BATTERY BATTERY_2AAA
#define USE_DISPLAY 0
#define BUTTON1 GPIO_{button}
#define BUTTON1_ON {profile['button_active']}
#define {button}_FUNC AS_GPIO
#define {button}_OUTPUT_ENABLE 0
#define {button}_INPUT_ENABLE 1
#define PULL_WAKEUP_SRC_{button} {'PM_PIN_PULLUP_10K' if profile['button_active'] == 0 else 'PM_PIN_PULLDOWN_100K'}
#define I2C_GROUP {profile['i2c_group']}
#define USE_I2C_DRV I2C_DRV_HARD
#define I2C_CLOCK 400000
#define USE_SENSOR_CHT8305 0
#define USE_SENSOR_CHT8215 1
#define USE_SENSOR_AHT20_30 1
#define USE_SENSOR_SHT4X 1
#define USE_SENSOR_SHTC3 0
#define USE_SENSOR_SHT30 1
#define LED_ON {profile['led_active']}
#define LED_OFF {1 - profile['led_active']}
#define GPIO_LED GPIO_{led}
#define {led}_FUNC AS_GPIO
#define {led}_OUTPUT_ENABLE 1
#define {led}_INPUT_ENABLE 1
#define {led}_DATA_OUT LED_OFF
#define SHL_ADC_VBAT C5P
#define GPIO_VBAT GPIO_PC5
#define PC5_INPUT_ENABLE 0
#define PC5_DATA_OUT 1
#define PC5_OUTPUT_ENABLE 1
#define PC5_FUNC AS_GPIO
#define PWM_PIN GPIO_{drive}
#define AS_PWMx {profile['pwm_function']}
#define PWM_ID {profile['pwm_id']}
#define GPIO_RHI GPIO_{sense}
#define CHNL_RHI {profile['adc_channel']}
#if ZBHCI_UART
#error "UART pin assignments have not been validated for Arteco"
#endif
#if UART_PRINTF_MODE
#define DEBUG_INFO_TX_PIN GPIO_SWS
#endif
#endif
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument('--compile-check', action='store_true', help='ZG-303Z reference pins; no flashable outputs')
    group.add_argument('--profile', type=Path, help='Measured and verified Arteco hardware JSON')
    parser.add_argument('--source', type=Path, help='Local pinned pvvx clone to copy instead of fetching')
    parser.add_argument('--native', action='store_true', help='Build on Linux amd64 without Docker')
    args = parser.parse_args()
    profile_path = ROOT / 'hardware/compile-check.json' if args.compile_check else args.profile.resolve()
    profile = json.loads(profile_path.read_text())
    if not args.compile_check and not profile['verified']:
        parser.error('Arteco GPIOs are not verified. Complete hardware/arteco-zssf21.json first.')
    header = board_header(profile)
    lock = json.loads((ROOT / 'firmware/upstream.json').read_text())
    work = ROOT / 'build' / ('compile-check' if args.compile_check else 'arteco')
    work.mkdir(parents=True, exist_ok=True)
    source = work / 'ZigbeeTLc'
    if source.exists():
        parser.error(f'{source} already exists; choose a fresh build directory or remove only that generated directory')
    if args.source:
        run('git', 'clone', '--no-hardlinks', str(args.source.resolve()), str(source))
    else:
        run('git', 'clone', lock['repository'], str(source))
    run('git', 'checkout', lock['revision'], cwd=source)
    run('git', 'apply', '--check', str(ROOT / 'firmware/fortify_zg303z.patch'), cwd=source)
    for path in (ROOT / 'firmware/src').rglob('*'):
        if path.is_file():
            target = source / 'src' / path.relative_to(ROOT / 'firmware/src')
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
    cfg = source / 'src/app_cfg.h'
    cfg.write_text(cfg.read_text().replace('#elif BOARD == BOARD_ZG303Z', '#elif BOARD == BOARD_ARTECO_ZS304Z\n#include "board_arteco_zs304z.h"\n#elif BOARD == BOARD_ZG303Z'))
    (source / 'src/board_arteco_zs304z.h').write_text(header)
    script = '''set -eu
cd /work/build/REPLACE/ZigbeeTLc
tar -xjf tools/linux/tc32_gcc_v2.0.tar.bz2 -C tools/linux
python3 -m zipfile -e tools/SDK_z.zip SDK_z
make PYTHON=python3 PROJECT_NAME=ZS304Z BIN_PATH=./arteco-bin POJECT_DEF="-DBOARD=BOARD_ARTECO_ZS304Z CHECK_FLAG" pre-build
make -j2 PYTHON=python3 PROJECT_NAME=ZS304Z BIN_PATH=./arteco-bin POJECT_DEF="-DBOARD=BOARD_ARTECO_ZS304Z CHECK_FLAG" ZNAME=Fortify-Arteco-ZS304Z-Raw BUILD_TARGETS
tools/linux/tc32/bin/tc32-elf-nm --defined-only build/ZS304Z.elf > build/symbols.txt
tools/linux/tc32/bin/tc32-elf-objcopy -O binary -j .vectors build/ZS304Z.elf build/vectors.dat
'''.replace('REPLACE', work.name).replace('CHECK_FLAG', '-DARTECO_COMPILE_CHECK=1' if args.compile_check else '').replace('BUILD_TARGETS', 'build/ZS304Z.elf build/ZS304Z.lst sizedummy' if args.compile_check else 'all')
    if args.native:
        script = script.replace('/work', str(ROOT))
        run('sh', '-c', script)
    else:
        run('docker', 'build', '--platform', 'linux/amd64', '-t', 'fortify-arteco-build', '-f', str(ROOT/'tools/Dockerfile'), str(ROOT/'tools'))
        run('docker', 'run', '--rm', '--platform', 'linux/amd64', '-v', f'{ROOT}:/work', 'fortify-arteco-build', 'sh', '-c', script)
    symbols = {}
    for line in (source / 'build/symbols.txt').read_text().splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    vectors = (source / 'build/vectors.dat').read_bytes()
    word = lambda offset: int.from_bytes(vectors[offset:offset + 4], 'little')
    assert int.from_bytes(vectors[20:22], 'little') == 0xF304
    assert int.from_bytes(vectors[18:20], 'little') == 0x1141
    assert word(symbols['DAT0'] + 12) == 0x848000
    assert word(symbols['FLL_D'] + 8) == 0x848000
    assert symbols['_stack_end_'] == 0x848000, 'TLSR8251 has 32 KB SRAM'
    assert symbols['_ram_end_'] < symbols['_stack_end_']
    assert symbols['_bin_size_'] < profile['flash_bytes']
    artifacts = source / ('build' if args.compile_check else 'arteco-bin')
    metadata = {'board': profile['board'], 'chip': profile['chip'], 'pvvx_revision': lock['revision'], 'compile_check': args.compile_check, 'hardware_verified': profile['verified'], 'sram_bytes': 32768, 'used_sram_end': hex(symbols['_ram_end_']), 'image_bytes': symbols['_bin_size_'], 'file_version': hex(word(2)), 'image_type': hex(int.from_bytes(vectors[20:22], 'little')), 'model': 'ZS-304Z-Raw'}
    metadata['sha256'] = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in artifacts.iterdir() if p.is_file() and p.suffix in ('.bin', '.zigbee', '.elf', '.lst')}
    if args.compile_check:
        # Only ELF/listing targets were requested: no firmware/OTA images.
        metadata['flashable_artifacts_retained'] = False
    (work / 'build-info.json').write_text(json.dumps(metadata, indent=2) + '\n')
    print(json.dumps(metadata, indent=2))


if __name__ == '__main__':
    main()
