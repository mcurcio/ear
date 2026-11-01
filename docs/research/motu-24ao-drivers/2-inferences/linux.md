# Inferences: snd-usb-motu

Every claim here is INFERRED: reasoned from structure or names, or an interpretation of observed code. Each names
the observations it rests on in [linux-snd-usb-motu.md](../1-observations/linux-snd-usb-motu.md). Source:
`../sources/map-linux-snd-usb-motu.md`, with the verifiers' corrections noted. Cross-driver or symptom remarks
are marked COMPARISON.

Link key: `#0-scope-and-interfaces`, `#1-bring-up`, `#2-clock-and-sample-rate`, `#3-start-and-stop`,
`#4-isochronous-framing`, `#5-rate-matching`, `#6-interrupt-and-status`, `#7-errors-and-recovery`, `#8-other`.

## Bring-up

- **LI1** It is not a different streaming format: the driver reuses the USB Audio Class 2 interface layout
  (control on 0, playback on 1, capture on 2, alt 1), with only the interval request and its interrupt being
  vendor-specific. Rests on [L0.1, L0.2](../1-observations/linux-snd-usb-motu.md#0-scope-and-interfaces).
  verify-mac (G1) reads the same code as already speaking MOTU's vendor protocol on those interfaces (the vendor
  interval request, vendor interrupts, shred-pattern length detection).
- **LI2** snd-usb-audio also matches the device, since the match has no class or interface filter. Rests on
  [L1.1, L1.2](../1-observations/linux-snd-usb-motu.md#1-bring-up). verify-linux (G6): snd-usb-audio matches by
  audio interface class, and the USB core tries drivers in registration order, not by match specificity.
- **LI3** Probe-order assumption: the driver assumes interface 0 probes before 1 and 2; if 1 or 2 bound first,
  the stub return at :1246 would bind them with no data, and the claim at :1186/:1191 would fail with -EBUSY.
  Rests on [L1.3, L1.4](../1-observations/linux-snd-usb-motu.md#1-bring-up). **Corrected by verify-mac (G6):**
  the USB core normally probes interfaces in descriptor order, so the realistic risk is snd-usb-audio binding
  first (the Talos node loads it), not a self-race.

## Clock and sample rate

- **LI4** (COMPARISON with symptoms) S2 and S3 cannot occur in this driver's own code path: it never asks for a
  rate, so the stock driver's set-rate ENXIO cannot happen. Rests on
  [L0.3, L1.8, L2.2](../1-observations/linux-snd-usb-motu.md#2-clock-and-sample-rate).
- **LI5** The price: a device that is unlocked, or at another rate, is streamed blindly. At 44.1 kHz the playback
  packets would mirror the device's 5-6 frames per microframe while ALSA believes 48 kHz. At 96 kHz the
  per-microframe counter's cap of 7 frames would miscount capture. Rests on
  [L2.1, L2.4](../1-observations/linux-snd-usb-motu.md#2-clock-and-sample-rate),
  [L5.1](../1-observations/linux-snd-usb-motu.md#5-rate-matching).

## Start and stop

- **LI6** Primed playback contents are silence: playback data is whatever is in the (zeroed, coherent) buffer
  until the period handler writes it. Rests on [L3.3 step 2, L3.5](../1-observations/linux-snd-usb-motu.md#3-start-and-stop).

## Isochronous framing

- **LI7** The effective audio ring is 512 microframes, 64 ms, while lengths are kept per 2048 index. Rests on
  [L4.3, L4.4](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing).
- **LI8** The period handler can deliver audio that arrived up to 64 ms before its URB completes. Rests on
  [L4.6](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing), LI7.
- **LI9** Channel-0 alignment holds only if every capture packet is a whole multiple of 72 bytes. If one is not,
  the playback packet carries the raw byte count while `bufs.length` is floored, so trailing stale bytes go out
  and the device sees a partial frame; nothing detects this. Rests on
  [L4.7, L4.8](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing).
- **LI10** Short of that, the worst desync is a stale or duplicated whole frame, never a channel rotation; the
  driver has no rotation recovery. Rests on [L4.7](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing).

## Rate matching

- **LI11** Lag and write lead: two playback URBs are always queued, so playback URB k transmits about two capture
  windows (about 128 ms) later, which is why playback lengths are known before data is written ahead. Rests on
  [L3.5](../1-observations/linux-snd-usb-motu.md#3-start-and-stop),
  [L4.1](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing),
  [L5.1](../1-observations/linux-snd-usb-motu.md#5-rate-matching).
- **LI12** In steady state the write lead swings between about 16 microframes and 16 microframes plus one period.
  Rests on [L5.3](../1-observations/linux-snd-usb-motu.md#5-rate-matching).
- **LI13** The 2 ms lead assumes playback transmits exactly two URBs behind the capture position; the host
  controller's real `ISO_ASAP` start delay is never measured. If the period interrupt is handled more than about
  2 ms late, playback sends stale audio, 64 ms old because the buffer aliases. Rests on
  [L5.2, L5.3](../1-observations/linux-snd-usb-motu.md#5-rate-matching), LI7.
  **Corrected by verify-mac (G3):** the playback start delay d is at least 0, so the lead is 16+d microframes and
  never negative; the first start gives a fixed, unknown offset of d x 6 samples, not early or stale audio; the
  real exposure is a late capture completion whose as-soon-as-possible resubmit (:527) shifts the
  playback-to-capture offset mid-stream. verify-linux (G4) adds: a late interrupt, or one lost to a non-zero
  status in `interrupt_complete_urb`, leaves the first slots stale, and a lost interrupt leaves `copy_pos` one
  period behind for good.
- **LI14** Despite the brief's "drift knob" wording, `pb_adj` corrects no drift (packet sizes already follow the device clock); its purpose is most likely
  tuning the write lead or latency per host. Rests on [L5.1, L5.4](../1-observations/linux-snd-usb-motu.md#5-rate-matching).
  verify-linux (G3): it "exists only to shift copy_pos by hand, which suggests the author saw this vary".

## Interrupt and status

- **LI15** On xHCI the interrupt endpoint descriptor's bInterval probably governs instead of `interval = 1`.
  Rests on [L6.1](../1-observations/linux-snd-usb-motu.md#6-interrupt-and-status).
- **LI16** The 6-byte message fits UAC2's interrupt data message, with bInfo bit 0 = vendor-specific and
  bAttribute 1 = CUR. Rests on [L6.2](../1-observations/linux-snd-usb-motu.md#6-interrupt-and-status).
- **LI17** The ignored class messages include clock-validity changes. Rests on
  [L6.3](../1-observations/linux-snd-usb-motu.md#6-interrupt-and-status), [L2.3](../1-observations/linux-snd-usb-motu.md#2-clock-and-sample-rate).

## Errors and recovery

- **LI18** Packet errors can print up to 512 lines per URB in atomic context, a log storm. Rests on
  [L7.1](../1-observations/linux-snd-usb-motu.md#7-errors-and-recovery), [L4.1](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing).
- **LI19** One lost capture URB stops its playback partner silently; a playback length above the playback
  endpoint's max packet size would fail submission (-EMSGSIZE) and stall playback. Rests on
  [L7.2, L7.3](../1-observations/linux-snd-usb-motu.md#7-errors-and-recovery),
  [L3.6](../1-observations/linux-snd-usb-motu.md#3-start-and-stop). The verifiers differ on the first part:
  verify-mac (G5) "one capture completion that fails to resubmit stops both directions"; verify-linux (G5) "the
  round-robin pb resubmission ... slows down and playback starves".
- **LI20** With no capture data there is no audio either way, since the early return also blocks playback sync
  and the playback copy. Rests on [L7.4](../1-observations/linux-snd-usb-motu.md#7-errors-and-recovery).
- **LI21** (COMPARISON with symptoms) A PipeWire restart-on-underrun does not restart USB streaming, a contrast
  with S1 and S4, where every stream start is a risk; ALSA's own stop threshold judges the xrun. Rests on
  [L7.5](../1-observations/linux-snd-usb-motu.md#7-errors-and-recovery), [L3.1](../1-observations/linux-snd-usb-motu.md#3-start-and-stop).
- **LI22** Teardown gaps: the stop sequence (interval 0, alt 0) never runs on rmmod with the device present. USB
  core's unbind likely resets alt settings, but the vendor interval request stays armed. Rests on
  [L7.9](../1-observations/linux-snd-usb-motu.md#7-errors-and-recovery), [L3.7](../1-observations/linux-snd-usb-motu.md#3-start-and-stop).
- **LI23** After a probe failure interfaces 1 and 2 stay bound to this driver, so snd-usb-audio cannot take them.
  Rests on [L7.10](../1-observations/linux-snd-usb-motu.md#7-errors-and-recovery). **Corrected by both
  verifiers (G6):** they keep a dangling priv as interface data; verify-mac calls it a use-after-free risk on
  disconnect, "worse than a leak"; verify-linux adds a stale claim until replug.

## Other

- **LI24** Shred aliasing works only because consumed slots are re-shredded before the device rewrites them 64 ms
  later; that holds while period + margin < 512 microframes. Rests on
  [L4.4](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing), [L8.2](../1-observations/linux-snd-usb-motu.md#8-other).
  verify-linux and verify-mac (G8): with the largest period there is about 20 ms of slack; the aliasing looks
  deliberate and the risk is a timing margin, not a logic bug.
- **LI25** If the device's capture max packet size is below 504, the last slot's shred overruns the coherent
  buffer. Rests on [L8.1](../1-observations/linux-snd-usb-motu.md#8-other) (the map: "CONFIRMED arithmetic;
  maxp unknown").
- **LI26** Stepping back 2 microframes (:254) double-counts one microframe per call, so `rx_frames` runs high;
  this affects only the startup discard, since `rx_frames` is unused after sync. Rests on
  [L8.3](../1-observations/linux-snd-usb-motu.md#8-other), [L5.7](../1-observations/linux-snd-usb-motu.md#5-rate-matching).
- **LI27** A real audio frame exactly equal to the shred pattern would be miscounted (negligible). Rests on
  [L4.6](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing).
- **LI28** The unlocked shared state is probably safe only because one host controller gives back URBs serially.
  Rests on [L8.4](../1-observations/linux-snd-usb-motu.md#8-other).
- **LI29** A buffer that is not a whole number of periods would desync the position. Rests on
  [L8.5](../1-observations/linux-snd-usb-motu.md#8-other).
- **LI30** The `GFP_ATOMIC` submits from sleepable context are harmless. Rests on
  [L8.7](../1-observations/linux-snd-usb-motu.md#8-other).

## Open questions from the Linux map

1. The 24Ao's alt-setting-1 descriptors: playback and capture `wMaxPacketSize` and mult (to rule out the 504-byte
   shred overrun), the endpoint addresses, and whether a playback feedback endpoint exists that this driver
   ignores. No `lsusb -v` was on disk.
2. Whether the device ever sends a capture packet that is not a multiple of 72 bytes. That is the only path here
   to a channel rotation, and it is unchecked.
3. What the period interrupt's `frame` field (u32) means, and its byte order. The driver never reads it.
4. Whether the vendor interval request (0x40/0x01) also gates the device's streaming or clock, or only the
   interrupt. The driver always sends it before `set_interface`, and sends 0 before alt 0.
5. Whether keeping both directions running and avoiding USB restarts on ALSA restarts is what avoids S1 and S4.
   Static reading cannot show it.
6. The host controller's real `ISO_ASAP` start latency against the assumed two-URB playback offset; probably
   what `pb_adj` compensates; the author's intent is not stated.
7. Whether the device needs its clock locked before enumeration (readme:29) or only before streaming starts.
8. Whether the double count in `count_sample_frames` (:254) is intended.
