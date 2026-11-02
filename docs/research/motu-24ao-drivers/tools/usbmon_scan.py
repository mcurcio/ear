#!/usr/bin/env python3
"""Quick scan: per device and endpoint, event counts and time range, for a usbmon mmapped pcap."""
import struct, sys, collections, datetime
HDR = struct.Struct('<QBBBBHbbqiiII8siiII')
def records(path):
    with open(path, 'rb') as f:
        g = f.read(24); magic, vmaj, vmin, tz, sig, snap, lt = struct.unpack('<IHHiIII', g)
        assert lt == 220, lt
        while True:
            rh = f.read(16)
            if len(rh) < 16: return
            ts, tu, incl, orig = struct.unpack('<IIII', rh)
            d = f.read(incl)
            yield ts + tu / 1e6, d
c = collections.Counter(); t = {}
for path in sys.argv[1:]:
    for ts, d in records(path):
        h = HDR.unpack_from(d)
        key = (h[5], h[4], h[3], h[2], chr(h[1]))
        c[key] += 1
        t.setdefault(key, [ts, ts])[1] = ts
for k in sorted(c):
    a, b = t[k]
    print(k, c[k], datetime.datetime.utcfromtimestamp(a).strftime('%T.%f'), datetime.datetime.utcfromtimestamp(b).strftime('%T.%f'))
