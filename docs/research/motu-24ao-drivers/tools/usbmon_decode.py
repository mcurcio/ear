#!/usr/bin/env python3
"""usbmon_decode: decode a usbmon mmapped pcap (linktype 220, DLT_USB_LINUX_MMAPPED) for one device.

  usbmon_decode.py --bus 5 --dev 9 --from HH:MM:SS[.f] --to HH:MM:SS[.f] --tones tones.json --out DIR PCAP...

Writes DIR/control.txt (every control transfer, in order), DIR/iso-<ep>.txt (first 40 URBs, per
packet; plus summaries), DIR/interrupt.txt, DIR/audio.txt (audio placement analysis), DIR/summary.json.
Times are the pcap record times (UTC; Pacific = UTC-8 on this date).
usbmon header (64 bytes, little-endian): id u64, type u8 ('S','C','E'), xfer_type u8 (0 iso,
1 intr, 2 ctrl, 3 bulk), epnum u8 (bit 7 = IN), devnum u8, busnum u16, flag_setup s8, flag_data
s8, ts_sec s64, ts_usec s32, status s32, length u32, len_cap u32, setup[8] | (error_count s32,
numdesc s32), interval s32, start_frame s32, xfer_flags u32, ndesc u32; then ndesc iso
descriptors (status s32, offset u32, len u32, pad u32), then the captured data.
"""
import argparse, collections, datetime, json, os, struct, sys
import numpy as np

HDR = struct.Struct('<QBBBBHbbqiiII8siiII')
ISO = struct.Struct('<iIII')
REQ = {0: 'GET_STATUS', 1: 'CLEAR_FEATURE', 3: 'SET_FEATURE', 5: 'SET_ADDRESS', 6: 'GET_DESCRIPTOR',
       7: 'SET_DESCRIPTOR', 8: 'GET_CONFIGURATION', 9: 'SET_CONFIGURATION', 10: 'GET_INTERFACE',
       11: 'SET_INTERFACE', 12: 'SYNCH_FRAME'}
UAC2 = {1: 'CUR', 2: 'RANGE', 3: 'MEM'}


def utc(ts):
    return datetime.datetime.fromtimestamp(ts, datetime.timezone.utc).strftime('%H:%M:%S.%f')


def pac(ts):
    return datetime.datetime.fromtimestamp(ts - 8 * 3600, datetime.timezone.utc).strftime('%H:%M:%S.%f')


def tparse(s, day):
    h, m, sec = s.split(':')
    return day + int(h) * 3600 + int(m) * 60 + float(sec)


def records(paths):
    n = 0
    for path in paths:
        with open(path, 'rb') as f:
            magic, vmaj, vmin, tz, sig, snap, lt = struct.unpack('<IHHiIII', f.read(24))
            assert lt == 220, 'linktype %d is not 220' % lt
            while True:
                rh = f.read(16)
                if len(rh) < 16:
                    break
                ts, tu, incl, orig = struct.unpack('<IIII', rh)
                d = f.read(incl)
                n += 1
                yield os.path.basename(path), n, ts + tu / 1e6, d, orig


def parse(d):
    h = HDR.unpack_from(d)
    e = dict(id=h[0], type=chr(h[1]), xt=h[2], ep=h[3], dev=h[4], bus=h[5], flag_setup=h[6], flag_data=h[7],
             status=h[10], length=h[11], len_cap=h[12], interval=h[14], start_frame=h[15], xfer_flags=h[16], ndesc=h[17])
    if h[2] == 0:
        e['err_count'], e['numdesc'] = struct.unpack('<ii', h[13])
    else:
        e['setup'] = h[13]
    off = 64
    descs = []
    for i in range(h[17]):
        descs.append(ISO.unpack_from(d, off)[:3]); off += 16
    e['descs'] = descs
    e['data'] = d[off:]
    return e


def describe_ctrl(setup, data_out):
    bmrt, breq, wv, wi, wl = struct.unpack('<BBHHH', setup)
    typ = (bmrt >> 5) & 3
    rcpt = bmrt & 31
    dirn = 'IN' if bmrt & 0x80 else 'OUT'
    if typ == 0:
        name = REQ.get(breq, 'std?%d' % breq)
        if breq == 6:
            name += ' type=%d idx=%d' % (wv >> 8, wv & 255)
        if breq == 11:
            name += ' intf=%d alt=%d' % (wi, wv)
    elif typ == 1:
        name = 'class %s %s CS=0x%02x CN=%d entity=%d intf/ep=%d' % (
            'GET' if dirn == 'IN' else 'SET', UAC2.get(breq, '0x%02x' % breq), wv >> 8, wv & 255, wi >> 8, wi & 255)
        if rcpt == 2:
            name = 'class(ep) %s %s CS=0x%02x ep=0x%02x' % ('GET' if dirn == 'IN' else 'SET', UAC2.get(breq, '0x%02x' % breq), wv >> 8, wi)
    else:
        name = 'vendor req=0x%02x' % breq
    return bmrt, breq, wv, wi, wl, name


def s24(buf, off, n, stride=72):
    """n little-endian signed 24-bit samples at off, off+stride, ..."""
    b = np.frombuffer(buf, dtype=np.uint8)
    idx = off + stride * np.arange(n)
    v = b[idx].astype(np.int32) | (b[idx + 1].astype(np.int32) << 8) | (b[idx + 2].astype(np.int32) << 16)
    v = np.where(v >= 1 << 23, v - (1 << 24), v)
    return v / float(1 << 23)


def fit_tones(x, freqs, rate=48000):
    """Amplitude of each tone (least-squares with sin/cos per freq, jointly) and the residual share."""
    n = np.arange(len(x))
    cols = []
    for f in freqs:
        cols += [np.sin(2 * np.pi * f * n / rate), np.cos(2 * np.pi * f * n / rate)]
    A = np.stack(cols + [np.ones(len(x))], 1)
    c, *_ = np.linalg.lstsq(A, x, rcond=None)
    amps = np.hypot(c[0:-1:2], c[1:-1:2])
    res = x - A @ c
    e = float(np.sum(x * x))
    return amps, (float(np.sum(res * res)) / e if e > 0 else 1.0), float(np.sqrt(np.mean(x * x)))


def identify(stream, base, nframes, freqs):
    """For each byte offset 0..71 of the 72-byte frame: best tone, its amplitude, residual share."""
    out = []
    for o in range(72):
        x = s24(stream, base + o, nframes)
        amps, resid, rms = fit_tones(x, freqs)
        k = int(np.argmax(amps))
        out.append(dict(offset=o, tone=k + 1, amp=float(amps[k]), resid=resid, rms=rms))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--bus', type=int, required=True)
    ap.add_argument('--dev', type=int, required=True)
    ap.add_argument('--from', dest='t0', required=True)
    ap.add_argument('--to', dest='t1', required=True)
    ap.add_argument('--day', default='2026-11-02')
    ap.add_argument('--tones', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--out-ep', type=lambda s: int(s, 0), default=0x09)
    ap.add_argument('--in-ep', type=lambda s: int(s, 0), default=0x87)
    ap.add_argument('pcaps', nargs='+')
    a = ap.parse_args()
    day = datetime.datetime.strptime(a.day, '%Y-%m-%d').replace(tzinfo=datetime.timezone.utc).timestamp()
    t0, t1 = tparse(a.t0, day), tparse(a.t1, day)
    plan = json.load(open(a.tones))
    freqs = plan['freqs_hz']
    os.makedirs(a.out, exist_ok=True)

    pend = {}
    ctrl, intr = [], []
    urbs = {a.out_ep: [], a.in_ep | 0x80: []}
    for fname, frame, ts, d, orig in records(a.pcaps):
        if ts < t0 or ts > t1:
            continue
        e = parse(d)
        if e['bus'] != a.bus or e['dev'] != a.dev:
            continue
        e['ts'], e['frame'], e['file'] = ts, frame, fname
        if e['type'] == 'S':
            pend[e['id']] = e
            continue
        s = pend.pop(e['id'], None)
        if e['xt'] == 2:
            ctrl.append((s, e))
        elif e['xt'] == 1:
            intr.append((s, e))
        elif e['xt'] == 0 and e['ep'] in urbs:
            urbs[e['ep']].append((s, e))
    unmatched = [(v['ts'], v['ep'], v['xt']) for v in pend.values()]

    summ = dict(window_utc=[utc(t0), utc(t1)], unmatched_submits=[(utc(t), ep, xt) for t, ep, xt in unmatched])
    # ---------------------------------------------------------------- control
    with open(os.path.join(a.out, 'control.txt'), 'w') as f:
        f.write('# n  submit(UTC) [Pacific]  complete(UTC)  pcapS/pcapC  bmRequestType bRequest wValue wIndex wLength  status actual  data  -- meaning\n')
        for i, (s, c) in enumerate(ctrl):
            if s is None:
                f.write('%3d (no submit) C %s\n' % (i, utc(c['ts']))); continue
            bmrt, breq, wv, wi, wl, name = describe_ctrl(s['setup'], s['data'])
            data = s['data'] if not bmrt & 0x80 else c['data']
            f.write('%3d %s [%s] %s  %s#%d/#%d  0x%02x 0x%02x 0x%04x 0x%04x %4d  st=%d act=%d  %s  -- %s\n' % (
                i, utc(s['ts']), pac(s['ts']), utc(c['ts']), s['file'], s['frame'], c['frame'],
                bmrt, breq, wv, wi, wl, c['status'], c['length'], data.hex() or '-', name))
    summ['control_count'] = len(ctrl)
    summ['set_interface'] = [dict(t_utc=utc(s['ts']), t_pac=pac(s['ts']), frame=s['frame'], intf=struct.unpack('<BBHHH', s['setup'])[3],
                                  alt=struct.unpack('<BBHHH', s['setup'])[2], status=c['status'])
                             for s, c in ctrl if s and s['setup'][1] == 11 and s['setup'][0] == 1]
    with open(os.path.join(a.out, 'interrupt.txt'), 'w') as f:
        for s, c in intr:
            f.write('%s %s S#%s C#%d st=%d len=%d data=%s\n' % (utc(c['ts']), 'ep0x%02x' % c['ep'], s['frame'] if s else '-', c['frame'],
                                                              c['status'], c['length'], c['data'].hex()))
    summ['interrupt'] = dict(count=len(intr), statuses=dict(collections.Counter(c['status'] for _, c in intr)),
                             first=utc(intr[0][1]['ts']) if intr else None, last=utc(intr[-1][1]['ts']) if intr else None)

    # ---------------------------------------------------------------- iso
    for ep, lst in urbs.items():
        isin = bool(ep & 0x80)
        name = 'iso-0x%02x' % ep
        info = dict(urbs=len(lst))
        if not lst:
            summ[name] = info; continue
        lens = collections.Counter()
        pst = collections.Counter()
        npk = collections.Counter()
        ust = collections.Counter()
        stream = bytearray()
        pkts = []   # (urb index, packet index, stream offset, length, status, ts)
        with open(os.path.join(a.out, name + '.txt'), 'w') as f:
            f.write('# URB n: submit/complete UTC, pcap frames, start_frame S/C, number_of_packets, urb status, error_count; then per packet: len(requested)/actual status offset\n')
            for i, (s, c) in enumerate(lst):
                descs_c = c['descs']; descs_s = s['descs'] if s else []
                npk[c['ndesc']] += 1
                ust[c['status']] += 1
                src = c if isin else s
                for j, (st, off, ln) in enumerate(descs_c):
                    alen = ln if isin else (descs_s[j][2] if s else ln)
                    pst[st] += 1
                    if isin:
                        if st != 0:
                            alen_data = b''
                        else:
                            alen_data = src['data'][off:off + ln]
                        lens[ln] += 1
                    else:
                        so, sl = descs_s[j][1], descs_s[j][2]
                        alen_data = s['data'][so:so + sl]
                        lens[sl] += 1
                    pkts.append((i, j, len(stream), len(alen_data), st, c['ts']))
                    stream += alen_data
                if i < 40 or i == len(lst) - 1:
                    f.write('URB %d: S %s [%s] C %s  S#%s C#%d  sf=%s/%d  npk=%d st=%d err=%s len=%d cap=%d\n' % (
                        i, utc(s['ts']) if s else '-', pac(s['ts']) if s else '-', utc(c['ts']), s['frame'] if s else '-', c['frame'],
                        s['start_frame'] if s else '-', c['start_frame'], c['ndesc'], c['status'], c.get('err_count'), c['length'], c['len_cap']))
                    f.write('   ' + ' '.join('%d/%d:%d@%d' % ((descs_s[j][2] if s else -1), ln, st, off) for j, (st, off, ln) in enumerate(descs_c)) + '\n')
        info.update(first_submit_utc=utc(lst[0][0]['ts']) if lst[0][0] else None, first_complete_utc=utc(lst[0][1]['ts']),
                    last_complete_utc=utc(lst[-1][1]['ts']), packets_per_urb=dict(npk), urb_status=dict(ust),
                    packet_status=dict(pst), packet_len_bytes=dict(sorted(lens.items())),
                    lens_multiple_of_72={str(k): k % 72 == 0 for k in sorted(lens)},
                    stream_bytes=len(stream), stream_bytes_mod_72=len(stream) % 72)
        # first packet with non-zero audio
        b = np.frombuffer(bytes(stream), dtype=np.uint8)
        nz = np.flatnonzero(b)
        if len(nz):
            p0 = nz[0]
            pk = next(p for p in pkts if p[2] <= p0 < p[2] + p[3])
            seg = b[pk[2]:pk[2] + pk[3]]
            offs = np.flatnonzero(seg)
            info['first_nonzero'] = dict(urb=pk[0], packet=pk[1], t_utc=utc(pk[5]), t_pac=pac(pk[5]), stream_byte=int(p0),
                                         stream_byte_mod_72=int(p0 % 72), packet_len=pk[3],
                                         nonzero_byte_offsets_in_packet=sorted(set(int(o) for o in offs))[:48],
                                         nonzero_offsets_mod_72=sorted(set(int(o % 72) for o in offs)),
                                         packet_hex=bytes(seg[:min(len(seg), 216)]).hex())
            # placement of the first identification burst: tone 1 alone (preamble slot 0)
            nf = 600
            base = int(p0 - p0 % 72) + 72 * 40   # some frames after onset, inside the burst
            if base + 72 * nf <= len(b):
                ident = identify(bytes(stream), base, nf, freqs)
                live = [r for r in ident if r['rms'] > 1e-4]
                info['first_burst'] = dict(stream_byte=base, t_note='first non-zero + 40 frames',
                                           offsets_with_signal=[(r['offset'], r['tone'], round(r['amp'], 4), round(r['resid'], 4)) for r in live])
        # steady section: placement every second
        st_rows = []
        nf = 480
        total = len(stream) // 72
        for k in range(0, total - nf, 48000):
            base = k * 72
            ident = identify(bytes(stream), base, nf, freqs)
            good = [r for r in ident if r['resid'] < 0.05 and r['amp'] > 0.1]
            if len(good) >= 20:
                pk = next(p for p in pkts if p[2] <= base < p[2] + max(p[3], 1))
                st_rows.append(dict(stream_frame=k, t_utc=utc(pk[5]), urb=pk[0],
                                    good_offsets=[(r['offset'], r['tone']) for r in good],
                                    tone_at_byte0=ident[0]['tone'], byte0_resid=round(ident[0]['resid'], 4),
                                    tone1_offset=[r['offset'] for r in good if r['tone'] == 1]))
        info['steady_placement'] = st_rows
        # every packet whose length is not the modal length or whose status is not 0
        modal = lens.most_common(1)[0][0]
        anom = [p for p in pkts if p[4] != 0 or (p[3] != modal)]
        with open(os.path.join(a.out, name + '-anomalies.txt'), 'w') as f:
            f.write('# packets with status != 0 or length != %d (modal); urb packet stream_offset length status complete_utc\n' % modal)
            for p in anom:
                f.write('%d %d %d %d %d %s\n' % (p[0], p[1], p[2], p[3], p[4], utc(p[5])))
        info['modal_packet_len'] = modal
        info['anomalous_packets'] = len(anom)
        # steady state: the longest run of URBs whose every packet has the modal length and status 0
        okv = [c['status'] == 0 and all(d[0] == 0 and (d[2] if isin else (s['descs'][j][2] if s else d[2])) in (modal, modal - 72) for j, d in enumerate(c['descs'])) for s, c in lst]
        best = (0, -1); i = 0
        while i < len(okv):
            if okv[i]:
                j = i
                while j + 1 < len(okv) and okv[j + 1]:
                    j += 1
                if j - i > best[1] - best[0]:
                    best = (i, j)
                i = j + 1
            else:
                i += 1
        if best[1] >= 0:
            f0, l0 = lst[best[0]], lst[best[1]]
            info['steady_urbs'] = dict(rule='longest run of URBs with every packet %d or %d B and status 0' % (modal, modal - 72),
                                       first=dict(urb=best[0], submit_utc=utc(f0[0]['ts']) if f0[0] else None, complete_utc=utc(f0[1]['ts']), pcap_frames=[f0[0]['frame'] if f0[0] else None, f0[1]['frame']], file=f0[1]['file']),
                                       last=dict(urb=best[1], submit_utc=utc(l0[0]['ts']) if l0[0] else None, complete_utc=utc(l0[1]['ts']), pcap_frames=[l0[0]['frame'] if l0[0] else None, l0[1]['frame']], file=l0[1]['file']))
        # urb-level steady state: from the last URB with any non-zero packet status (+1) to the end
        bad = [i for i, (s, c) in enumerate(lst) if c['status'] != 0 or any(d[0] != 0 for d in c['descs'])]
        info['urbs_with_packet_errors'] = len(bad)
        info['first_error_urbs'] = [(i, utc(lst[i][1]['ts']), [d[0] for d in lst[i][1]['descs']]) for i in bad[:10]]
        info['last_error_urb'] = (bad[-1], utc(lst[bad[-1]][1]['ts'])) if bad else None
        summ[name] = info
        np.save(os.path.join(a.out, name + '-stream.npy'), np.frombuffer(bytes(stream), dtype=np.uint8)) if os.environ.get('SAVE_STREAM') else None
    json.dump(summ, open(os.path.join(a.out, 'summary.json'), 'w'), indent=1, default=str)
    print(json.dumps({k: v for k, v in summ.items() if k not in ('iso-0x09', 'iso-0x87')}, indent=1, default=str)[:4000])


main()
