# MOTU 24Ao on euclid: Linux snd-usb-audio traces, driver load to steady streaming (2026-11-02)

Observations only. Anything marked **Inference** is not directly seen in a trace.
Times are UTC with US Pacific (PST, UTC-8 on this date) in brackets where it matters.
Device: 07fd:0005 at usb 5-1 (bus 5, device 9), high speed, AMD xHCI 0000:00:10.0, Ubuntu 26.04 kernel
7.0.0-42-generic. Ethernet stated unplugged (not checked by this step). Kernel source citations are
to the 6.18.48 tree at `/Volumes/Dock/iac-ear-run/motu-research/linux-6.18.48/` (euclid runs 7.0.0;
the cited lines were not re-checked against 7.0).

## Runs

| Run | Driver | run dir (copy) | Result |
|---|---|---|---|
| 1 | `run-step stock-quirk 15` (`quirk_flags=07fd:0005:!validate_rates\|skip_iface_setup`) | `runs/20261102T182649Z-stock-quirk-15s/` | all checks passed; analyze: **rotation +8** (capture N carries source N+8), first ~1.8 s lost |
| 2 | `run-step stock 15` (no quirk flags) | `runs/20261102T182732Z-stock-15s/` | **FAILED**: motu-watch tripped on `xhci_hcd 0000:00:10.0: WARN: buffer overrun event for slot 1 ep 14 on endpoint` at 18:27:39.395351 [10:27:39.395]; analyze (run offline on the Mac afterwards): **identity**, but all channels silent for the first 13.16 s of the recording |
| 2b | second `run-step stock 15` | — | **NOT RUN**: run 2 failed, so the binding rule stopped all device work |

State left on euclid after run 2 (nothing touched after the failure, per the rule): `/var/lib/motu-test/DEAD`
present, `motu-watch` failed (exited on the trip), `motu-heartbeat` and `motu-usbmon` active,
`snd_usb_audio` still loaded, card D24Ao still present. aplay and arecord ran to completion on their own
(OUT stopped 18:27:58.35, IN 18:28:00.04). One `sudo motu-alive` was run by this step at about 18:28:10
after the failure (ALIVE status=0000); the heartbeat's own probes continued every 10 s.

xHCI "ep 14" is the endpoint index (DCI - 1); DCI 15 = endpoint 7 IN = **0x87**, the capture endpoint.
The message comes from `drivers/usb/host/xhci-ring.c:2771-2774` (`COMP_ISOCH_BUFFER_OVERRUN`), which
`xhci-ring.c:2461-2462` reports to the URB as packet status `-EOVERFLOW` (-75). 1623 such warnings were
logged, 18:27:39.395 to 18:27:40.197 (log timestamps; the pcap's -75 packets end at 18:27:39.597, see below).

## Files

- `pcap/run1-stock-quirk_run2-stock-start.usb.pcap00` (200 MB): ring file 00, 18:25:13 to 18:27:41.16; holds all of run 1 and run 2 up to 18:27:41.
- `pcap/run2-stock-failed.usb.pcap01` (168 MB): ring file 01, the rest of run 2 and the heartbeat until the copy.
- `tools/usbmon_decode.py`: the decoder (usbmon mmapped pcap, linktype 220; header layout in its docstring). `tools/usbmon_scan.py`: per-endpoint event counts.
- `decoded/run1-stock-quirk/`, `decoded/run2-stock/`: `control.txt` (every control transfer in order: submit and complete time, pcap frame numbers, bmRequestType, bRequest, wValue, wIndex, wLength, status, actual, data, meaning), `iso-0x87.txt` / `iso-0x09.txt` (first 40 URBs and the last, per packet `requested/actual:status@offset`), `iso-*-anomalies.txt` (every packet with status != 0 or length != 432), `interrupt.txt` (ep 0x81), `summary.json` (counts, distributions, placement fits).
- `runs/…`: each run dir as run-step left it (run.log, checks.log, dmesg.txt, proc snapshots, tones.json, rec.wav, aplay/arecord -v output); run 2's `rec.summary.txt`/`rec.analysis.json` were produced on the Mac.

Reproduce: `python3 tools/usbmon_decode.py --bus 5 --dev 9 --from 18:26:53 --to 18:27:20 --tones runs/20261102T182649Z-stock-quirk-15s/tones.json --out decoded/run1-stock-quirk pcap/run1-stock-quirk_run2-stock-start.usb.pcap00` (run 2: `--from 18:27:36 --to 18:28:30`, both pcaps).

Control requests `GET_STATUS` + `GET_DESCRIPTOR(device)` pairs in `control.txt` are motu-alive probes
(run-step's checks and the 10 s heartbeat), not the driver.

## Run 1: stock-quirk (UTC [Pacific])

| Phase | Time | Evidence |
|---|---|---|
| pre-load check | 18:26:53.788 [10:26:53.788] | ctrl #0-1 (pcap00 #45-48) |
| module load | 18:26:53.849 modprobe; 53.885 `mc: Linux media interface`; 53.972 `registered new interface driver snd-usb-audio` | run.log, dmesg.txt |
| card probe on the wire | 18:26:53.949 - 53.971 [10:26:53.949 - .971] | ctrl #2-10: string descriptors 2, 1; `GET RANGE` clock 1 (2 then 74 bytes); `SET_INTERFACE 1/0` (#6, pcap00 #57); `GET RANGE` again; `SET_INTERFACE 2/0` (#9, #63); string 5 ("Internal Clock"). The alt 0 resets are `sound/usb/card.c:320`; `stream.c:1252` skips the per-interface rate init. No SET CUR. |
| interrupt URB ep 0x81 submitted | 18:26:53.9715 | never completed until unload (-108 at 18:27:16.355) |
| capture open/hw_params/prepare (wire side) | 18:26:54.888 - 54.898 [10:26:54.888] | ctrl #15 `GET CUR` clock valid = 01; #16 `GET CUR` freq = 0x0000bb80 (48000); #17 valid = 01; #18 `SET_INTERFACE 2/1` 54.895964 (pcap00 #82). arecord started 54.859 (run.log). |
| capture start: first IN URBs | 18:26:54.901792 [10:26:54.901792] | 12 URBs submitted in 0.08 ms (pcap00 #86-97), 8 packets each, 648 B buffers |
| playback hw_params/prepare (wire side) | 18:26:54.962215 | ctrl #20 `SET_INTERFACE 1/1` (pcap00 #218); no clock requests. aplay started 54.930. |
| playback start: first OUT URB | 18:26:54.967754 [10:26:54.967754] | pcap00 #233; then one OUT URB submitted per IN completion |
| first non-zero OUT audio | 18:26:55.470862 | OUT URB 502 packet 0 |
| first non-zero IN audio | 18:26:57.061807 [10:26:57.061807] | IN URB 2159 packet 4; IN was exact zeros for the 2.160 s before |
| steady state | IN URB 1 (18:26:54.903736) to URB 20750 (18:27:15.652864); OUT URB 0 (54.968775) to URB 18751 (18:27:13.720071) | every packet 432 B (or 360 B, see below), status 0 |
| playback stop | last OUT complete 18:27:13.720071; `SET_INTERFACE 1/0` 13.721721 (pcap00 #75252) | |
| capture stop | IN 12 URBs -104 (ECONNRESET), 90 packets -18 (EXDEV), 18:27:15.653782-.653870; `SET_INTERFACE 2/0` 15.655704 (#79130) | |
| unload | modprobe -r 18:27:16.327; deregister 16.355; ep 0x81 URB -108 16.3547 | |

## Run 2: stock (no quirk flags)

| Phase | Time | Evidence |
|---|---|---|
| pre-load check | 18:27:36.096 - 36.680 | ctrl #0-3 |
| module load | 18:27:36.738 modprobe; 36.774 `mc: ...`; 37.699 `registered new interface driver` | run.log, dmesg.txt |
| card probe, interface 1 | 18:27:36.838 - 37.671 | #4-5 strings; #6-7 GET RANGE; #8 `SET_INTERFACE 1/0` (`stream.c:1255`); #9 valid = 01; #10 freq = 48000; **#11 `SET CUR` freq = 0x0002ee00 = 192000 Hz** (`stream.c:1259`, `snd_usb_init_sample_rate(..., fp->rate_max)`), submitted 36.856337, **completed 37.661266 (805 ms)**; #12 freq = 192000; #13 **valid = 00**; #14 `SET_INTERFACE 1/1` (`stream.c:1261`); #15 `SET_INTERFACE 1/0` (`card.c:320`) |
| card probe, interface 2 | 18:27:37.671 - 37.694 | #16-17 GET RANGE; #18 `SET_INTERFACE 2/0`; #19 valid = 00 -> kernel `usb 5-1: clock source 1 is not valid, cannot use` 37.684367 (`clock.c:317`); #20 freq = 192000 (equal to rate_max, so no SET); #21 valid = 00; #22 `SET_INTERFACE 2/1`; #23 `SET_INTERFACE 2/0` |
| UAC2 interrupts on ep 0x81 | 18:27:37.696903 - 47.454492 | 366 completions, each 6 bytes `00 01 00 01 00 01` (bInfo 0, bAttribute 1 = CUR, wValue 0x0100 = CS 1 SAM_FREQ, wIndex 0x0100 = entity 1), one about every 24 ms for 9.8 s. None in run 1. |
| capture hw_params/prepare (wire side) | 18:27:38.574 - 39.392 | #29 valid = 00 -> kernel `clock source 1 is not valid` 38.579365; #30 freq = 192000; **#31 `SET CUR` freq = 48000, submitted 38.579570, completed 39.384556 (805 ms)**; #32 freq read back = **192000**; #33 valid = 01; #34 `SET_INTERFACE 2/1` 39.390152 (pcap00 #79298) |
| capture start: first IN URBs | 18:27:39.393157 [10:27:39.393157] | 12 URBs, 648 B buffers |
| IN overflow | IN URB 0: 8 packets 0 B status 0 (39.394184); **URBs 1-203: every packet status -75 (EOVERFLOW), 648 B** (18:27:39.395182 - 39.597168) | 1623 packets = the 1623 kernel warnings |
| playback start | `SET_INTERFACE 1/1` 18:27:39.465296 (#35); first OUT URB 39.597199 (pcap00 #79733) | the last -75 IN packet completes at 39.597168, 31 us before the first OUT submit |
| first non-zero OUT audio | 18:27:40.098288 | OUT URB 500 packet 0 |
| first non-zero IN audio | **18:27:52.640240** [10:27:52.640240] | IN URB 13246 packet 3; IN was exact zeros for 13.25 s after the IN start (12.54 s after OUT audio began), 5.19 s after the last interrupt |
| steady state | IN URB 204 (39.598184) to 20649 (18:28:00.043255); OUT URB 0 (39.598172) to 18749 (18:27:58.347650) | |
| playback stop | `SET_INTERFACE 1/0` 18:27:58.349754 (pcap01 #155390) | |
| capture stop | 12 URBs -104, 90 packets -18; `SET_INTERFACE 2/0` 18:28:00.045762 (#158792) | |
| unload | not done (stopped by the failure rule) | |

## Isochronous endpoints (both runs)

- Every URB on both endpoints has 8 packets (one per microframe; start_frame advances 8 per URB).
- IN 0x87: 12 URBs queued at start; packet buffers 648 B (9 frames of 72 B). OUT 0x09: one URB submitted as each IN URB completes.
- Packet length distribution (bytes):

| Run | Endpoint | lengths (count) | multiple of 72 |
|---|---|---|---|
| 1 | OUT 0x09 | 432 (150013), 360 (3) | all |
| 1 | IN 0x87 | 432 (166006), 360 (5), 0 (93) | all |
| 2 | OUT 0x09 | 432 (149996), 360 (4) | all |
| 2 | IN 0x87 | 432 (163572), 360 (4), 648 (1623, all status -75), 0 (97) | all |

  The IN 0-length packets are the first URB's first packets (run 1: 4, status 0; run 2: all 8, status 0) and 89 of the
  90 packets cancelled at stop (status -18; one -18 packet per run reports 432 B, e.g. run 1 URB 20762 packet 6).
  The 360 B IN packets: run 1 URB 22/0 (18:26:54.924743, before OUT started), 4940/3, 9872/0, 14813/3, 19763/7.
- A 360 B (5-frame) IN packet is followed about 1.26 ms later by a 360 B OUT packet at the same packet
  index (run 1: IN URB 4940/3 at 18:26:59.842785 -> OUT URB 4875/3 at 59.844045; IN 9872/0 -> OUT 9807/0;
  IN 14813/3 -> OUT 14748/3). The OUT packet sizes copy the IN packet sizes, about every 4.94 s.
- Each run's OUT stream and IN stream total a whole number of 72 B frames (stream bytes mod 72 = 0).

## Audio placement on the wire

Method: the payload of every packet, concatenated per endpoint (IN: only status 0 packets, at the
descriptor offsets usbmon gives), cut into 72 B frames from the first byte of the first packet. For
each byte offset 0..71, 480 consecutive frames are read as S24_3LE and least-squares fitted to all
24 planned tones (tones.json); the tone with the largest amplitude and the residual share are kept. A
real slot shows amplitude 0.2512 (-12 dBFS) and residual about 0; offsets not on a 3-byte boundary
show residual near 1. Done once per second through each stream.

**OUT 0x09 (what Linux sends), both runs:** tone 1 (300 Hz, source channel 1) sits at bytes 0-2 of
every 72 B frame, and tone k at bytes 3(k-1)..3(k-1)+2, for all 24 tones, in every 1 s check
(run 1: 15 checks 18:26:58.97 - 18:27:12.97; run 2: 15 checks). The first non-zero OUT packet (run 1:
URB 502 packet 0, 18:26:55.470862; run 2: URB 500 packet 0, 18:27:40.098288) is 432 B with non-zero
bytes only at packet offsets 72, 144, 216, 288, 360, 361, i.e. byte 0 (and 1) of frames 1-5: the
fade-in of source 1's identification burst, in slot 1.

**IN 0x87 (what the MOTU returns):**
- Run 1 (quirk): byte 0 of each frame carries **tone 9**; tone 1 sits at bytes 48-50 (slot 17); all 24 slots fit
  cleanly (capture slot N carries source N+8 mod 24) in every check, 18:26:58.90 - 18:27:12.90. The
  first non-zero IN packet (URB 2159 packet 4, 18:26:57.061807) is non-zero only at bytes 21-23 of each
  frame (slot 8), carrying **tone 16**: source 16's identification burst, which Linux put on the OUT
  wire at about 18:26:56.968 (OUT stream frame 96000 from the first OUT packet; **Inference** from the
  plan's slot timing). Sources 1-15's bursts produced only zeros on IN. So the +8 shift is already in the
  IN bytes the device sends, from its first non-zero sample, while the OUT bytes are in the right place.
- Run 2 (stock): byte 0 carries **tone 1**, tone k at bytes 3(k-1) (identity), in every check from
  18:27:53.60 to 18:27:57.60 (the IN stream is zeros before 18:27:52.64). The first non-zero IN packet
  (URB 13246 packet 3) is already all 24 tones at full level, residual 0.0, slot N = source N.

## Differences between the two starts (observed)

| | Run 1 stock-quirk | Run 2 stock |
|---|---|---|
| SET CUR at probe | none | 192000 Hz (805 ms to complete) |
| clock valid at capture prepare | 01 | 00 until the SET CUR 48000 (805 ms), then 01; freq still reads 192000 |
| ep 0x81 interrupts | none | 366 SAM_FREQ notifications, 37.697 - 47.454 |
| IN before OUT starts | 66 ms, 432 B packets after the first 4 empty ones | 204 ms, 203 URBs of -75 overflow (device sent more than 648 B per microframe) |
| IN silence after start | 2.160 s | 13.25 s |
| IN slot order once audio appears | source N+8 in slot N | source N in slot N |

**Inference (not verified):** 648 B is 9 frames per microframe; at 192 kHz the device would send 24
frames (1728 B) per microframe, which would overflow every 648 B buffer, as seen in run 2 until the OUT
stream started. The device may have still been clocked at 192 kHz then, and the 9.8 s of SAM_FREQ
interrupts and the 13 s of IN silence may be the device settling to 48 kHz. The identity order in run 2
against +8 in run 1 suggests the device's IN slot order is set by how its clock/stream start happened,
not by Linux's buffer handling; one run of each is too few to say which condition decides it.

## Harness notes (findings)

- `run-step` records `usbmon-first.txt`/`usbmon-last.txt` and prints "current usbmon pcap" from
  `/var/lib/motu-test/usbmon/`, but the running `motu-usbmon` unit writes to
  `/var/lib/motu-test/usbmon-full/usb.pcapNN`; both runs' files name a stale `usbmon/usb-b.pcap07` (18:25:13).
- The ring is 20 x 200 MB and each 15 s run writes about 185 MB, so the ring holds about 21 runs.
- Run 2's IN overflow is in the first 0.2 s of a stock (no-quirk) start; the watcher treats it as a failure,
  so a second stock run needs the owner's decision.
