# snd-usb-audio and the MOTU 24Ao's 8-channel rotation: what the kernel code can and cannot do

Written 2026-11-02 10:36 PST [18:36 UTC]. Source reading only: no device step was run for this
note, and the pcaps in `rotation/pcap/` were not opened.

## Sources read

| Tree | Where | Notes |
|---|---|---|
| mainline v7.0 `sound/usb` | `rotation/src/v7.0/` (kernel.org `plain/sound/usb/<f>?h=v7.0`) | `endpoint.c` sha256 `bad3eb483239b362…` |
| Ubuntu `Ubuntu-7.0.0-42.42` `sound/usb` | `rotation/src/ubuntu-7.0.0-42.42/` (git.launchpad.net `~ubuntu-kernel/ubuntu/+source/linux/+git/resolute`, tag `Ubuntu-7.0.0-42.42`) | This is the kernel that runs on euclid. `endpoint.c` sha256 `ff901f2f114ad904…`. `quirks.h` could not be fetched (curl failed); it holds prototypes only |
| 6.18.48 | `/Volumes/Dock/iac-ear-run/motu-research/linux-6.18.48/sound/usb/` | Talos |

`endpoint.c` lines carry a prefix: **U:** Ubuntu 7.0.0-42, **M:** v7.0, **T:** 6.18.48. Unprefixed lines in other files are **v7.0**. `pcm.c`, `clock.c` and `implicit.c` are byte-identical in Ubuntu, except that the Ubuntu `clock.c` puts `prev_rate == rate` at line 622. `quirks.c` and `stream.c` are shifted by up to about 10 lines in Ubuntu: the MOTU vendor entry is at Ubuntu quirks.c:2442, and `SKIP_IFACE_SETUP` at Ubuntu stream.c:1250.

### How the three trees differ (only what touches streaming)

- **Ubuntu 7.0.0-42 vs v7.0** (stable backports, plus one Ubuntu-only flag):
  - Implicit-feedback packet count is clamped to the URB's size (U:495-498, U:1243-1246 `max_urb_packs`).
  - Each implicit-feedback packet size is clamped to `maxframesize` (U:1826-1830).
  - `prepare_inbound_urb` stops at the buffer size (U:388-389).
  - `packsize[]` is clamped *after* `maxframesize` is computed (U:1411-1416). v7.0 clamps it before (M:1382-1383 runs before M:1408). That is a v7.0 bug, but it only affects endpoints that size their own packets, which none of this device's do.
  - New flag `QUIRK_FLAG_IFB_SILENCE_ON_EMPTY` (U:1798, only on Behringer Flow 8, quirks.c:2308).
  - No MOTU change.
- **Ubuntu 7.0.0-42 vs 6.18.48**: the same streaming logic. The diff is cosmetic (`notify_xrun` returns a bool, `kzalloc_obj`, `max_urb_packs`), plus the prepare order: v7.0/Ubuntu run `select_mode_quirk`, `init_pitch`, `init_sample_rate` (U:1504-1512), while 6.18.48 runs pitch, rate, mode (T:1498-1506). `select_mode_quirk` only acts for `ITF_USB_DSD_DAC`, so this makes no difference here. So euclid (AMD) and Talos (Intel) run the same host algorithm. That fits the shift showing up on both.
- **Not in any of the three**: mainline commit `320e557` (2026-05-04, `kernel/commit-320e557.patch`). It extends the MicroBook "clock is invalid for over 2 s while switching rate" wait to **07fd:0005**. clock.c:215 still matches only `0x0004`.

## What the kernel does for this device (Ubuntu 7.0.0-42, quirk_flags `07fd:0005:!validate_rates|skip_iface_setup`)

- **Flags.** The built-in vendor entry `VENDOR_FLG(0x07fd, QUIRK_FLAG_VALIDATE_RATES)` sets `VALIDATE_RATES` (v7.0 quirks.c:2434-2435). The module string is parsed after the built-in table (card.c:721-735, quirks.c:2588-2660): `!validate_rates` clears it and `skip_iface_setup` adds bit 26. No other built-in flag, quirk-table entry (quirks-table.h only has 0x0001 Fastlane and 0x0004 MicroBook), implicit-fb table entry (implicit.c:67-72 only lists 0x0004), boot quirk (quirks.c:1655-1680), format quirk (quirks.c:1855-1886) or start quirk (quirks.c:1928-1955) matches 07fd:0005.
- **Implicit feedback.** It comes from the generic UAC2 rule (implicit.c:325-333). The OUT interface 1 alt 1 is async with one endpoint, so interface 2's IN endpoint 0x87 (usage: implicit feedback) becomes the playback's sync source. Capture has no sync endpoint (implicit.c:357-372 returns 0). Endpoint 0x87 is one shared `snd_usb_endpoint` object: it is capture's data endpoint and playback's sync source.
- **Frame size.** `cur_frame_bytes = physical_width(S24_3LE)=24 × 24 / 8 = 72` (U:838-839 / M:836-837). `stride = 72` (M:1121).
- **Packet sizes.** `maxpacksize = 2 × 936 = 1872` (stream.c `usb_endpoint_max_periodic_payload`), so `maxframesize = 1872/72 = 26` (U:1411). But the per-packet buffer is sized from 1.5 × the nominal rate, not from wMaxPacketSize: `freqmax = 9 frames/µframe`, giving `maxsize = curpacksize = 9 × 72 = 648 bytes` (U:1142-1178).
- **URBs.** Both directions use 8 packets per URB (1 ms) and `nurbs = MAX_URBS = 12` (U:1196-1207, card.h:6-9).
- **Harness.** It opens `hw:CARD=…,DEV=0` with `-f S24_3LE -c 24` (run-step:45, 137-138), so alsa-lib does no channel routing. It starts `arecord` first and `aplay` only once capture is RUNNING (run-step:167-173), so **capture starts first**.

## The central finding

**No code path in snd-usb-audio can put a 24-byte (one third of a frame) offset on the wire, or into the PCM buffer, for this device.**

- **Every OUT packet is whole frames.** Its length is `counts × stride` with `stride = 72` (pcm.c:1560-1561 for data, U:298-302 for silence). `counts` comes from `actual_length / sender->stride` of the matching IN packet (U:1828, integer division) or from the phase accumulator (U:163-180). Both give whole frames.
- **Every IN packet is cut to whole frames.** `frames = bytes / stride; bytes = frames × stride` (pcm.c:1346-1348); `txfr_quirk` (ALIGN_TRANSFER) is off. It is copied at the packet's own offset with `pkt_offset_adj = stream_offset_adj = 0` (pcm.c:1334-1345; stream.c:95-96; no MOTU case in quirks.c:1855-1886).
- **Every packet re-aligns.** The host starts each packet at byte 0 of a frame. So even a malformed packet from the device can only cause a glitch inside that packet, never an offset that lasts.
- **No channel reordering.** The kernel never reorders channels: the chmap is a read-only control (stream.c:119-240, built by `convert_chmap` at stream.c:240 from `bmChannelConfig`), and the data paths are plain `memcpy` (pcm.c:1373-1380, 1478-1489).

So a stable +8 that lasts the whole run means one of two things:

- (a) The bytes on the wire are already rotated, and the device's framing slipped.
- (b) The host's *timing or order* triggers a framing slip inside the device.

Both are device behaviour. What Linux can contribute is *when and how continuously* it sends OUT packets, which is what the ranked list below covers.

**The decisive trace test.** It needs no new device step, only the existing full-packet usbmon pcaps.

- In the first OUT (0x09) packets after `aplay` starts, find which tone sits in bytes 0-2 of each 72-byte frame.
- Do the same in IN (0x87) packets after the loopback is audible.
- **OUT shows source 1 at bytes 0-2, IN shows source 9 at bytes 0-2:** the host sent correct frames and the slip is inside the MOTU. Mechanisms 1-3 below are then the only host-side levers.
- **OUT already shows source 9 at bytes 0-2:** something in user space or the kernel is wrong, contrary to everything above. Re-read pcm.c:1473-1491 against the trace.

## Mechanisms, ranked by how well they explain (deterministic +8 on Linux, correct on Apple, ~1.8 s lost on 6 of 8 short starts)

### 1. The implicit-feedback OUT stream starts late, is paced by IN completions, and can run dry (most likely host-side trigger)

**Code**
- **The playback endpoint submits nothing at start.** U:1599-1606: `if (snd_usb_endpoint_implicit_feedback_sink(ep) && !(quirk_flags & QUIRK_FLAG_PLAYBACK_FIRST)) { … "No URB submission due to implicit fb sync"; goto fill_rest; }`. All 12 OUT URBs go to the ready list.
- **Each IN completion releases at most one OUT URB.** On every IN URB completion, `snd_usb_handle_sync_urb` (U:1763-1836) copies the IN packet sizes into the next-packet FIFO and calls `snd_usb_queue_pending_output_urbs` (U:468-537).
- **The OUT queue is about one IN URB deep.** In steady state the OUT stream is only about 1 ms (one 8-packet URB) ahead of the IN completion that released it. A late IN completion interrupt delays the next OUT submission one for one.
- **An empty IN URB releases nothing.** `if (bytes == 0 && …) return;` (U:1798). An IN URB whose packets are all zero-length releases no OUT URB, which leaves 8 microframes with no OUT packet.
- **Start order.** With capture first (the harness), `aplay`'s prepare runs `start_endpoints` (pcm.c:712-722). That sets `sync_source->sync_sink = ep` (U:1571-1572) while 0x87 is already streaming. The first OUT URB then waits for the next IN completion.
- **No `URB_ISO_ASAP`.** OUT URBs carry only `URB_NO_TRANSFER_DMA_MAP` (U:1264). How xHCI places a late or first isochronous URB is decided in xhci-ring.c (not read here).

**For this device:** the OUT stream begins one or more IN URBs after `SET_INTERFACE(1,1)`. It is continuous only as long as every IN URB completion is serviced within about 1 ms. Any IN URB that comes back empty leaves a gap.

**Why it fits:**
- The MOTU AVB family has a documented "channel hopping in blocks of 8 channels" failure: Drumfix issue 16 (LP32, outputs 1-8 appear at 17-24; `motu-research/full-Drumfix-16.md`).
- The RolandMueller1994 fork blames it on gaps in the isochronous OUT stream:
  - README: "time_silent … 2ms … XHCI … requires 2ms of queued data for continuous ISOC stream. In case you are still experiencing channel hopping …".
  - motu.c:518-533 resets the interface when an OUT URB's `start_frame` is later than expected ("dropouts in the iso stream … leads to channel hopping").
  - Its maintainer, in issue 2: frame_check "capture[s] gaps in the ISOC stream which causes the channel hopping".
- MOTU's own Mac driver schedules OUT on explicit frame numbers. snd-usb-audio never does.

**What it does not explain alone:** why the result is exactly one 8-channel step every time, rather than a random number of steps. A deterministic start pattern (a fixed lag between the device starting to read OUT and the first OUT packet) would explain it. Random gaps would explain the earlier "N→N but glitchy" run.

**Trace prediction (usbmon `S`/`C` on 0x09 and 0x87):**
- (i) The first OUT 0x09 submission comes ≥ 1 ms (≥ 8 microframes) after the first non-empty IN 0x87 completion following `SET_INTERFACE` (bRequest 0x0B, wValue 1, wIndex 1).
- (ii) On the lossy starts, a stretch of about 1.8 s of 0x87 completions with every iso descriptor `actual_length == 0` and no 0x09 submissions; OUT begins when IN carries data.
- (iii) Later, OUT URBs whose `start_frame` is not the previous one's `start_frame + 8`, or OUT iso descriptors with status -18 (EXDEV, missed service).

**Lever:**
- `quirk_flags=07fd:0005:!validate_rates|skip_iface_setup|playback_first` with **aplay started before arecord**. Then OUT URBs are submitted at start (silence sized by the phase accumulator, U:163-180, U:230-231) before IN starts (pcm.c:290-305 starts data then sync).
- `implicit_fb`-free behaviour is not reachable without code (`skip_implicit_fb` would remove the IN-to-OUT size match MOTU's own driver keeps).

### 2. Capture-first start order: `SET_INTERFACE` on the OUT interface while IN is already streaming

**Code**
- Capture's trigger START calls `start_endpoints` (pcm.c:1757-1761). Playback starts its endpoints in **prepare** (pcm.c:712-722), not in trigger.
- `snd_usb_endpoint_prepare` for the OUT endpoint deselects (U:1488-1489, no-op if alt already 0, U:921-922), then sets interface 1 alt 1 last, after the rate (UAC2 path, U:1516-1521).
- The clock is shared (`clock_ref`, one clock ID 1). So the playback prepare sends no rate request (`update_clock_ref_rate` and `clock->need_setup == false`, U:1315-1336, U:1420-1444).
- The sync endpoint is already prepared (`need_prepare == false`, U:1472).

**For this device:** the only control traffic at `aplay` start is `SET_INTERFACE(1,1)`, sent while iface 2 alt 1 has been streaming for as long as the harness's wait took. The device's OUT path is switched on mid-stream. Whatever phase its OUT deframer starts in is set by that moment, not by a joint start.

**Why it fits:** it is deterministic for a fixed harness order, and Linux-specific if Apple's class driver selects both alternate settings before streaming either (not verified). It does not explain the 1.8 s by itself.

**Trace prediction:**
- Between capture start and the first 0x09 packet there is exactly one control `SET_INTERFACE` with wIndex 1 and wValue 1, and no `SET_CUR`.
- On Apple (joule, if traced), both interfaces' `SET_INTERFACE` come back to back before the first data packet on either endpoint.

**Lever:** start `aplay` first, or open both from one process (as ear/PipeWire does). Talos already showed the shift with ear's order, so start order alone is not sufficient. That points back to #1.

### 3. The IN packet buffer is 648 bytes, but the device may send up to 1872 per microframe

**Code**
- `maxsize = ceil(1.5 × 6) × 72 = 648` (U:1142-1158).
- Every IN iso descriptor asks for `curpacksize = 648` (U:391; 6.18.48 and Ubuntu also stop at `buffer_size`, U:388-389).
- An OUT URB stops adding packets when `(frames + counts) × 72 > buffer_size = 8 × 648` (pcm.c:1556-1558).

**For this device:** if the MOTU sends a burst larger than 9 frames in one microframe (for example to drain its FIFO at start), xHCI truncates or flags the packet (babble or -EOVERFLOW). The OUT copy of that URB then has fewer frames or packets than the IN URB had, and the OUT stream runs short. That feeds #1. It cannot create a 24-byte shift by itself.

**Trace prediction:** 0x87 iso descriptors with `actual_length == 648` and status -75 (EOVERFLOW) or a babble error, and 0x09 URBs with `number_of_packets < 8`. Apple's pcap (if any) would show IN packets larger than 648.

### 4. Rate and clock requests at probe and at start (where the quirk flags act), and MOTU's more-than-2 s clock switch

**Code**
- `VALIDATE_RATES` walks every rate with `SET_CUR` and checks the alternate setting at probe (format.c:556-592). It is on by default for MOTU (quirks.c:2434).
- Without `SKIP_IFACE_SETUP`, probe sends `SET_INTERFACE(0)`, `init_pitch`, `init_sample_rate(rate_max)` and `SET_INTERFACE(alt)` (stream.c:1261-1269). That leaves the clock at `rate_max`.
- At start, `set_sample_rate_v2v3` only sends `SET_CUR` when `GET_CUR` differs from the wanted rate (clock.c:621-623).
- No MOTU-0x0005 clock-valid wait exists in v7.0 or Ubuntu (clock.c:215). Mainline `320e557` documents that 0x0005 reports the clock as invalid for more than 2 s after a switch.

**For this device with euclid's flags:** probe touches neither rate nor interface. A factory-reset unit at 48 kHz then gets no `SET_CUR` at start. So this explains the 1.8 s loss **only if** a `SET_CUR` (or a clock-invalid answer) shows up on the lossy starts. It does not explain +8.

**Trace prediction:**
- Lossy starts: a control `SET_CUR` (bmRequestType 0x21, bRequest 0x01, wValue 0x0100, wIndex 0x0100) or a `GET_CUR CLOCK_VALID` (0xA1/0x01, wValue 0x0200) answering 0, followed by about 1.8 s of empty IN.
- Clean starts: only `GET_CUR` (wValue 0x0100) answering `80 bb 00 00`.
- Stock flags (no `!validate_rates`, no `skip_iface_setup`): a run of `SET_CUR` for every table rate at probe.

### 5. Per-packet truncation of IN and the floor in OUT sizing

**Code:** pcm.c:1346-1348 (capture keeps whole frames, drops the tail), U:1828 (`actual_length / 72`). If the IN status is not 0, the OUT size is 0 and falls back to the phase accumulator (U:1825-1832, U:222-233).

**For this device:** if the MOTU ever sends an IN packet whose length is not a multiple of 72, Linux drops the tail of that packet only. The next packet is re-aligned at its own start. The result is a glitch, never a stable offset. If the device treated the IN stream as one continuous byte stream across packets, Linux would hear constant garbage, not a clean +8. Ranked low.

**Trace prediction:** 0x87 `actual_length % 72 != 0` (for example 456 = 6×72+24). Seeing it would count against the device's framing, not the host's.

### 6. Fixed offsets and the length-header quirk (ruled out)

- `pkt_offset_adj` and `stream_offset_adj` (pcm.c:1334, 1340-1345) are set only for E-Mu, MacroSilicon, Pioneer and the Mbox 3 (quirks.c:1859-1879). They are 0 here.
- `QUIRK_FLAG_TX_LENGTH` puts a 4-byte length at the start of each OUT packet (U:288-307, pcm.c:1494-1518). It is not set for MOTU, and it would add 4 bytes, not 24.
- **Trace prediction if active:** OUT lengths of `4 + n×72`. Not expected.

### 7. Channel maps and UAC2 cluster descriptors (ruled out)

- The chmap is built from `bmChannelConfig` by `convert_chmap` (stream.c:240; called from the UAC2 format parser, Ubuntu stream.c:775-857). It is exposed as a control and a TLV only.
- No kernel code path permutes samples. The harness uses `hw:`, so alsa-lib does no routing either.
- **Trace prediction:** OUT bytes 0-2 carry source 1 (the decisive test above).

### 8. The wrong frame size (ruled out)

- There is one alternate setting per interface, 24 channels with 3-byte subslots (descriptor bytes `02 01 03 18`). So the stride is 72.
- A 4-byte format would give 96-byte frames and a 32-byte slip, not 24.
- **Trace prediction:** OUT packet lengths are multiples of 72 (432 or 504 at 48 kHz).

### 9. The v7.0 `packsize` ordering bug (ruled out for euclid)

- In v7.0, `packsize[]` is clamped to a stale `maxframesize` (M:1382-1383 runs before M:1408). Ubuntu 7.0.0-42 (U:1414-1416) and 6.18.48 (T:1408) have the fix.
- `packsize[]` is only used by `next_packet_size()` for endpoints without a sync source (M:186-207). Neither of this device's endpoints is one.

### 10. Vendor interfaces 3-5 (out of scope for the host)

- Interfaces 4 and 5 reuse the endpoint addresses 0x09 and 0x87 (wMaxPacketSize 900 × 2). usbcore drops them as duplicates.
- snd-usb-audio only claims the interfaces the audio-control header lists.
- **Trace prediction:** no `SET_INTERFACE` with wIndex 3, 4 or 5 from Linux. Any 8-channel block structure comes from the device's own engine (900 bytes is not a whole number of 72-byte frames, so the vendor mode uses another frame size).

## Dependencies at a glance

| Depends on | Mechanisms |
|---|---|
| quirk_flags `validate_rates` / `skip_iface_setup` | #4 only (probe-time rate and interface traffic). Neither touches the streaming code |
| quirk_flags `playback_first` (not set now) | #1, #2: OUT URBs at start, before IN |
| other flags that could matter (not set) | `set_iface_first` (U:1490-1496, stream.c:1257), `iface_delay` (U:942-943), `force_iface_reset` and `iface_skip_close` (U:960-962, U:1695+), `align_transfer` (#5), `tx_length` (#6), `skip_implicit_fb` and `generic_implicit_fb` (implicit.c:353, 390) |
| implicit feedback handling | #1, #3, #5 (U:1599-1606, U:1763-1836, U:468-537) |
| packet sizing from the IN stream | #1, #3, #5 (U:222-233, U:1826-1830, pcm.c:1554-1558) |
| start order | #1, #2 (pcm.c:712-722 and 1757-1761, U:1488-1521, U:1571-1572) |
| the built-in quirk table for 0x07fd | only `VALIDATE_RATES` (#4). There are no entries for 0x0005 in quirks-table.h, implicit.c or clock.c |
| channel maps | none (#7) |

## Bottom line

Linux's code moves whole 72-byte frames in both directions and re-aligns at every packet. So the +8 rotation is a framing slip inside the MOTU, and the host can only trigger it through timing.

The best-supported trigger is #1:
- The implicit-feedback OUT stream starts only after IN data arrives.
- It is paced one IN URB at a time.
- It stalls whenever IN URBs come back empty.

The third-party MOTU AVB driver's maintainers name gaps in the OUT stream as the cause of 8-channel hopping. Their fix is OUT data queued ahead of time (`time_silent`) plus checks on frame continuity.

The existing pcaps can confirm the split without another device run:
- the payload test above (where source 1 sits in OUT packets and in IN packets);
- predictions 1(i)-(iii), 2 and 4 on the control and isochronous timing.
