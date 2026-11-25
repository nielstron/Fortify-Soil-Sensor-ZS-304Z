"""Live web dashboard for the --hw-test image.

Reads the test firmware's GPIO-printf output (SWS pad, 1 Mbaud) from the SWire
USB-UART adapter and serves it at http://localhost:<http>/ with live updates (SSE).

    uv run --with pyserial python tools/hwtest_dashboard.py --port /dev/cu.usbserial-2110
"""
import argparse
import collections
import json
import queue
import re
import threading
import time
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path

import serial

ADC_VREF_MV = 1175  # pvvx adc_drv.c: mV = counts * 1175 / 4096 (counts = sum of 4 samples)
SAMPLE_RE = re.compile(
    r'#(?P<n>\d+) btn=(?P<btn>\d) probe_adc=(?P<probe>\d+) ref_adc=(?P<ref>\d+) '
    r'light_mv=(?P<light>\d+) vbat_mv=(?P<vbat>\d+) T=(?P<t>-?\d+) RH=(?P<rh>-?\d+)')
SETTLE_RE = re.compile(r'#(?P<n>\d+) settle(?P<points>(?: \d+:\d+:\d+)+)')
HTML = Path(__file__).with_name('hwtest_dashboard.html')

history = collections.deque(maxlen=7200)   # ~2 h at 1 sample/s
sweeps = collections.deque(maxlen=600)     # settle-time sweeps, one per loop
log = collections.deque(maxlen=200)        # firmware messages that are not samples (banner, button held)
clients = []
lock = threading.Lock()


def stock_percent(probe_mv, vbat_mv):
    """Stock Arteco conversion (disassembled at 0xe616): end points scale with battery."""
    vmin = 900 * vbat_mv / 995 - 248   # dry
    vmax = 800 * vbat_mv / 995 - 347   # wet
    if probe_mv > vmin:
        return 0.0, vmin, vmax
    if probe_mv < vmax:
        return 100.0, vmin, vmax
    return 100 * (vmin - probe_mv) / (vmin - vmax), vmin, vmax


def parse(line):
    m = SAMPLE_RE.search(line)
    if not m:
        return None
    g = {k: int(v) for k, v in m.groupdict().items()}
    probe_mv = g['probe'] * ADC_VREF_MV / 4096
    pct, vmin, vmax = stock_percent(probe_mv, g['vbat'])
    return {
        'ts': time.time(), 'n': g['n'], 'button': g['btn'],
        'probe_adc': g['probe'], 'ref_adc': g['ref'],
        'probe_mv': round(probe_mv, 1), 'ref_mv': round(g['ref'] * ADC_VREF_MV / 4096, 1),
        'light_mv': g['light'], 'vbat_mv': g['vbat'],
        'temp_c': g['t'] / 100, 'rh': g['rh'] / 100,
        'stock_pct': round(pct, 1), 'stock_dry_mv': round(vmin, 1), 'stock_wet_mv': round(vmax, 1),
    }


def parse_sweep(line):
    m = SETTLE_RE.search(line)
    if not m:
        return None
    points = []
    for p in m.group('points').split():
        ms, probe, ref = (int(v) for v in p.split(':'))
        points.append({'ms': ms, 'probe_mv': round(probe * ADC_VREF_MV / 4096, 1),
                       'ref_mv': round(ref * ADC_VREF_MV / 4096, 1)})
    return {'ts': time.time(), 'n': int(m.group('n')), 'points': points}


def broadcast(kind, payload):
    msg = f'event: {kind}\ndata: {json.dumps(payload)}\n\n'.encode()
    with lock:
        for q in clients:
            q.put(msg)


def reader(port):
    s = serial.Serial(port, 1_000_000, timeout=0.5)
    s.reset_input_buffer()
    while True:
        raw = s.readline()
        if not raw:
            continue
        line = raw.decode(errors='replace').strip()
        if not line:
            continue
        sample = parse(line)
        sweep = None if sample else parse_sweep(line)
        entry = {'ts': time.time(), 'line': line}
        with lock:
            if sample:
                history.append(sample)
            elif sweep:
                sweeps.append(sweep)
            else:
                log.append(entry)
        if sample:
            broadcast('sample', sample)
        elif sweep:
            broadcast('sweep', sweep)
        else:
            broadcast('log', entry)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def send_body(self, body, ctype):
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/':
            self.send_body(HTML.read_bytes(), 'text/html; charset=utf-8')
        elif self.path == '/api/history':
            with lock:
                body = json.dumps({'samples': list(history), 'log': list(log), 'sweeps': list(sweeps)}).encode()
            self.send_body(body, 'application/json')
        elif self.path == '/events':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-store')
            self.end_headers()
            q = queue.Queue()
            with lock:
                clients.append(q)
            try:
                while True:
                    try:
                        self.wfile.write(q.get(timeout=15))
                    except queue.Empty:
                        self.wfile.write(b': keepalive\n\n')
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            finally:
                with lock:
                    clients.remove(q)
        else:
            self.send_error(404)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', default='/dev/cu.usbserial-2110', help='USB-UART adapter wired to SWS')
    ap.add_argument('--http', type=int, default=8765, help='HTTP port')
    args = ap.parse_args()
    threading.Thread(target=reader, args=(args.port,), daemon=True).start()
    print(f'Dashboard on http://localhost:{args.http}/ reading {args.port}', flush=True)
    ThreadingHTTPServer(('127.0.0.1', args.http), Handler).serve_forever()


if __name__ == '__main__':
    main()
