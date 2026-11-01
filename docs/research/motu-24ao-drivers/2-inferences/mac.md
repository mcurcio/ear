# Inferences: MOTU's Mac driver (dext, HAL plug-in, re-enumerator)

Every claim here is INFERRED: reasoned from names, structure or partial reads, or an interpretation of observed
facts. Each one names the observations it rests on (IDs link to `../1-observations/`). Where a verifier or another
map bears on it, that is noted. Sources: `../sources/map-mac-dext.md` (MI-D) and
`../sources/map-mac-hal-reenumerator.md` (MI-H, MI-R). Cross-driver remarks the maps made are grouped at the end
of each part as COMPARISON.

Link keys: D = [mac-dext.md](../1-observations/mac-dext.md), H = [mac-hal-plugin.md](../1-observations/mac-hal-plugin.md),
R = [mac-reenumerator.md](../1-observations/mac-reenumerator.md), L = [linux-snd-usb-motu.md](../1-observations/linux-snd-usb-motu.md).

## Part 1: the dext

- **MI-D1** The dext is only a transport: clock source, sample rate, lock and channel count are all set from user
  space (the HAL plug-in and the MOTUProAudioHTTPServer send their own commands, which the dext passes down the
  vendor bulk pipe). Rests on [D0.1, D0.2](../1-observations/mac-dext.md#0-scope-and-interfaces),
  [D1.17](../1-observations/mac-dext.md#1-bring-up), [D2.1](../1-observations/mac-dext.md#2-clock-and-sample-rate);
  supported by [H0.7, H2.1](../1-observations/mac-hal-plugin.md#2-clock-and-sample-rate).
- **MI-D2** Rate, clock source and lock are most likely handled through the AVB-style "HTTP datastore" carried
  by the 0x81 HTTP-proxy commands (from names). Rests on [D1.16](../1-observations/mac-dext.md#1-bring-up),
  [D2.1](../1-observations/mac-dext.md#2-clock-and-sample-rate); the HAL map observes the datastore keys and
  selector 2 ([H0.7](../1-observations/mac-hal-plugin.md#0-scope-and-interfaces)) but not how the dext moves
  the bytes.
- **MI-D3** Any mode switch belongs to the re-enumerator daemon. Rests on
  [D1.2](../1-observations/mac-dext.md#1-bring-up). **Corrected:** the re-enumerator only evicts Apple's class
  driver and does not switch mode (verify-mac G1; [R0.1, R1.5, R1.6](../1-observations/mac-reenumerator.md#1-bring-up));
  the mode post is in the HAL plug-in ([H1.1](../1-observations/mac-hal-plugin.md#1-bring-up)).
- **MI-D4** The bulk IN pool holds 64 requests (loop 0x1228 to 0x1828 in 0x18 steps). Rests on
  [D1.4](../1-observations/mac-dext.md#1-bring-up).
- **MI-D5** The u16 field compared with 0x201 for the zero-length-packet mask is probably bcdUSB. Rests on
  [D1.6](../1-observations/mac-dext.md#1-bring-up).
- **MI-D6** The 4-byte MIDI request is the USB-MIDI event-packet size. Rests on
  [D1.7](../1-observations/mac-dext.md#1-bring-up).
- **MI-D7** `fIsocMode` 1 means UAC mode (2 and 3: MOTU vendor mode over USB 2 and USB 3). Rests on
  [D1.8](../1-observations/mac-dext.md#1-bring-up), [D3.2](../1-observations/mac-dext.md#3-start-and-stop),
  [H1.1](../1-observations/mac-hal-plugin.md#1-bring-up).
- **MI-D8** GetGUID for USB probably comes from a string descriptor. Rests on
  [D1.10, D1.11](../1-observations/mac-dext.md#1-bring-up).
- **MI-D9** There is no capture-before-playback rule at the alt-setting step; the order is the interface
  iteration order. Rests on [D3.3](../1-observations/mac-dext.md#3-start-and-stop).
- **MI-D10** Bytes per frame, channel count and sample format are chosen by the HAL, not the dext. Rests on
  [D4.3, D4.8](../1-observations/mac-dext.md#4-isochronous-framing); consistent with
  [H0.5 key 9, H3.1 steps 5 and 7](../1-observations/mac-hal-plugin.md#3-start-and-stop).
- **MI-D11** The Mac HAL very likely reads IN data early through the shred pattern, as Linux does. Rests on
  [D4.6](../1-observations/mac-dext.md#4-isochronous-framing). **Answered by the HAL map:** the plug-in does find
  new input by comparing against the shred template ([H4.5](../1-observations/mac-hal-plugin.md#4-isochronous-framing)).
- **MI-D12** Channel-0 alignment holds only because every packet is a whole number of frames, which comes from
  the device's IN sizes copied to OUT. Rests on [D4.8](../1-observations/mac-dext.md#4-isochronous-framing),
  [D5.1](../1-observations/mac-dext.md#5-rate-matching).
- **MI-D13** For S1: a rotation by 8 channels, if it does not come from the host, needs a device-side frame-start
  offset or a packet size that is not a whole multiple of the frame size; the binary cannot tell which. Rests on
  [D4.8](../1-observations/mac-dext.md#4-isochronous-framing), [D5.1](../1-observations/mac-dext.md#5-rate-matching).
- **MI-D14** OUT packet sizes follow the capture pattern from 320 ms earlier, and OUT buffers are queued about
  300 ms ahead. Rests on [D3.5, D3.8](../1-observations/mac-dext.md#3-start-and-stop),
  [D5.1](../1-observations/mac-dext.md#5-rate-matching). (The map tagged the "320 ms lead" CONFIRMED; it is a
  derivation from these three facts. verify-mac confirms the 320 ms unit.)
- **MI-D15** Low latency therefore depends on the HAL writing samples into buffers that are already queued, just
  before the controller fetches them. Rests on MI-D14 and [D4.7](../1-observations/mac-dext.md#4-isochronous-framing).
- **MI-D16** The purpose of the frame snapshot thread is to map USB frames to host time for timestamps (the map:
  "CONFIRMED structure, INFERRED purpose"). Rests on [D5.3](../1-observations/mac-dext.md#5-rate-matching),
  [D4.5](../1-observations/mac-dext.md#4-isochronous-framing).
- **MI-D17** An IN completion with an unlisted status costs the ring one slot for good, since IN and OUT are
  resubmitted only from the queue it skips. Rests on [D7.1](../1-observations/mac-dext.md#7-errors-and-recovery).
- **MI-D18** Recovery is left to the HAL stopping and starting DMA. Rests on
  [D7.4](../1-observations/mac-dext.md#7-errors-and-recovery); consistent with
  [H7.5, H7.9](../1-observations/mac-hal-plugin.md#7-errors-and-recovery).
- **MI-D19** No class driver coexists with the dext on macOS, since it opens the whole device. Rests on
  [D1.1](../1-observations/mac-dext.md#1-bring-up).
- **MI-D20** The bus-stall value passed on stop is probably 0. Rests on
  [D3.11](../1-observations/mac-dext.md#3-start-and-stop).
- **MI-D21** S3 (ENXIO on first open after power-on) and S4 (firmware freezes) cannot be settled from the dext,
  since it issues no clock or rate operations. Rests on [D2.1](../1-observations/mac-dext.md#2-clock-and-sample-rate).

### Cross-driver remarks in the dext map (COMPARISON)

- **MC-D1** The dext never reads the UAC2 clock-source entities the Linux stock driver queries, so the state
  behind "clock source 1 is not valid" (S2) is never consulted at this layer. That is consistent with the
  snd-usb-motu readme's rule that the device must be locked at the chosen rate before it is connected. Rests on
  [D2.2](../1-observations/mac-dext.md#2-clock-and-sample-rate), [L0.4](../1-observations/linux-snd-usb-motu.md#0-scope-and-interfaces).
- **MC-D2** Linux uses the same per-byte shred pattern and uses it the same way, to find how far the device has
  written before the request completes. Rests on [D4.6](../1-observations/mac-dext.md#4-isochronous-framing),
  [L4.6](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing).
- **MC-D3** Linux does the same implicit feedback (copies `actual_length` to the playback length); its `pb_adj`
  has no counterpart in the dext. Rests on [D5.1, D5.2](../1-observations/mac-dext.md#5-rate-matching),
  [L5.1, L5.4](../1-observations/linux-snd-usb-motu.md#5-rate-matching).
- **MC-D4** The dext never sends Linux's vendor request 0x01 "interval" (the dext map cites :535-547) and never
  reads the 6-byte class interrupt messages (cited :466-486). Rests on [D0.1](../1-observations/mac-dext.md#0-scope-and-interfaces),
  [D6.1](../1-observations/mac-dext.md#6-interrupt-and-status), [L1.6](../1-observations/linux-snd-usb-motu.md#1-bring-up),
  [L6.1-L6.3](../1-observations/linux-snd-usb-motu.md#6-interrupt-and-status).
- **MC-D5** Linux's `playback_complete_urb` is empty, as the dext's OUT completion is. Rests on
  [D7.3](../1-observations/mac-dext.md#7-errors-and-recovery), [L3.6](../1-observations/linux-snd-usb-motu.md#3-start-and-stop).

### Open questions from the dext map (verbatim in substance; editor's cross-references in brackets)

1. Does the 24Ao expose the vendor isochronous interface (class 0xFF, subclass 0x04, protocol 0x02)? That decides
   whether macOS streams through vendor mode (2) or UAC mode (1). Linux dmesg shows config 1 has at least
   interfaces 0-5. Answering it needs a full descriptor dump (`lsusb -v`).
2. Which configuration `SetConfiguration` selects (a virtual call not resolved).
3. `fMaxPacketSize`, the bytes per frame, the sample format, the channel order, and the rate or clock commands.
   All live in the HAL plug-in or the HTTP server; the next binary to read is `MOTUProAudioServerPlugin`.
   [Partly answered by the HAL map: key 9 carries max input bytes per microframe, 504 at 24 ch / 48 kHz (H0.5,
   H3.1); format S24_3LE (H4.6, H4.7); rate through the datastore (H2.1). The command opcodes stay open.]
4. The meaning of the 0x8F response handling and of the 0x40 flag beyond "unsolicited".
5. Whether the HAL reads IN data early through the shred pattern. [Answered: yes, H4.5.]
6. What the reenumerator daemon does, and whether it switches the device's configuration or mode. [Answered by
   the HAL map and verify-mac: it forces re-enumeration away from Apple's class driver; it does not switch mode
   (R1.5, R1.6).]
7. The bus-stall value passed on stop (no visible immediate; probably 0, MI-D20).

## Part 2: the HAL plug-in

- **MI-H1** The datastore is MOTU's web-API key/value tree. Rests on
  [H0.7](../1-observations/mac-hal-plugin.md#0-scope-and-interfaces) (key names such as `avb/<uid>/cfg/...`).
- **MI-H2** All USB-level work (interfaces, endpoints, packet sizing, feedback, the interrupt endpoint) lives in
  the dext; the plug-in shows the dext's contract. Rests on [H0.1](../1-observations/mac-hal-plugin.md#0-scope-and-interfaces);
  borne out by the dext map (D3, D4, D5).
- **MI-H3** The Mac driver moves the device out of class-compliant (UAC) mode into MOTU's vendor mode. Rests on
  [H1.1](../1-observations/mac-hal-plugin.md#1-bring-up). **Corrected by verify-mac (G1):** the post happens only
  when isocMode is 2 or 3, which the dext sets only when the device's current descriptors already contain a
  0xFF/4/2 interface ([D1.8](../1-observations/mac-dext.md#1-bring-up)), and the dext has a full UAC fallback
  ([D3.2](../1-observations/mac-dext.md#3-start-and-stop)).
- **MI-H4** The device re-enumerates or reconfigures after `host/mode` is posted. Rests on
  [H1.1](../1-observations/mac-hal-plugin.md#1-bring-up). Unverified (see open question 2).
- **MI-H5** Lock waiting on the USB path, if any, happens in the device or the dext. Rests on
  [H2.3](../1-observations/mac-hal-plugin.md#2-clock-and-sample-rate). The dext map observes no clock polling in
  the dext ([D2.1](../1-observations/mac-dext.md#2-clock-and-sample-rate)).
- **MI-H6** `PerformConfigChange` waits for the device to report the new rate or configuration. Rests on
  [H2.4](../1-observations/mac-hal-plugin.md#2-clock-and-sample-rate).
- **MI-H7** The `ChaseTimerProc` period of 5 is probably milliseconds. Rests on
  [H3.1 step 8](../1-observations/mac-hal-plugin.md#3-start-and-stop).
- **MI-H8** Chunk field +0x177520 is the dext's fill time. Rests on
  [H4.4, H4.5](../1-observations/mac-hal-plugin.md#4-isochronous-framing).
- **MI-H9** Channel 0 is always at a frame boundary that the dext defines. Rests on
  [H4.8](../1-observations/mac-hal-plugin.md#4-isochronous-framing).
- **MI-H10** One frame size (`fActive.fOutNumCh` x 3, field +0x268) serves both directions; the field names are
  uncertain, and with 24 in and 24 out they agree. Rests on
  [H3.1 step 5](../1-observations/mac-hal-plugin.md#3-start-and-stop),
  [H4.8](../1-observations/mac-hal-plugin.md#4-isochronous-framing).
- **MI-H11** Feedback or implicit sizing of playback packets, and the timestamps, come from the dext; possibly
  through the same interval interrupt Linux uses. Rests on [H5.1](../1-observations/mac-hal-plugin.md#5-rate-matching).
  **The first half is observed in the dext map** ([D5.1](../1-observations/mac-dext.md#5-rate-matching),
  [D4.5](../1-observations/mac-dext.md#4-isochronous-framing)); **the second half is contradicted** by it: the dext
  looks up no interrupt pipe ([D6.1](../1-observations/mac-dext.md#6-interrupt-and-status); verify-mac G2).
- **MI-H12** `allowMaxBusStall(!flag)` (selector 5) is called in `Activate` and relates to bus-stall tolerance in
  the dext. Rests on [H0.4](../1-observations/mac-hal-plugin.md#0-scope-and-interfaces); compare
  [D3.7, D8.1](../1-observations/mac-dext.md#3-start-and-stop) (the 0x28A0 "allowed" flag).
- **MI-H13** Input detection by content means a short or missing packet can never shift channels: frames are
  counted, never bytes. Rests on [H4.5, H4.8](../1-observations/mac-hal-plugin.md#4-isochronous-framing).
- **MI-H14** MOTU never runs this device through the class-compliant path on macOS (from the mode post and the
  forced re-enumeration). Rests on [H1.1](../1-observations/mac-hal-plugin.md#1-bring-up),
  [R1.5, R1.6](../1-observations/mac-reenumerator.md#1-bring-up). **Corrected by verify-mac and verify-linux
  (G1):** overstated; the dext has a complete UAC fallback ([D3.2](../1-observations/mac-dext.md#3-start-and-stop)),
  and the re-enumerator is about which driver owns the device, not its mode.

### Cross-driver remarks in the HAL map (COMPARISON)

- **MC-H1** The stock Linux driver leaves the device in UAC mode, which is relevant to S1-S4. Rests on
  [H1.1](../1-observations/mac-hal-plugin.md#1-bring-up). Both verifiers note S1 and S4 were seen under
  snd-usb-audio, and that the link to a UAC-mode firmware fault has no evidence (verify-mac G1, verify-linux G1).
- **MC-H2** Linux uses the identical shred pattern but compares whole 72-byte frames, where the plug-in compares
  each frame's first 24 bytes. Rests on [H4.5](../1-observations/mac-hal-plugin.md#4-isochronous-framing),
  [L4.6](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing).
- **MC-H3** Linux takes timing from interval interrupts and has a manual `pb_adj` offset; the plug-in takes it
  from dext-published chunk times. Rests on [H5.1, H5.3](../1-observations/mac-hal-plugin.md#5-rate-matching),
  [L5.4, L5.5](../1-observations/linux-snd-usb-motu.md#5-rate-matching).

### Open questions from the HAL map (verbatim in substance; editor's cross-references in brackets)

1. Every USB-level fact: interfaces and alt settings, endpoint numbers, vendor requests and their order, implicit
   or explicit feedback, the interrupt messages, how a stall is handled. These are in the dext. [Largely answered
   by the dext map: D3.2-D3.10, D5.1-D5.2, D6.1, D0.1. Endpoint numbers and stall handling stay open.]
2. Whether posting `host/mode=USB2` makes the 24Ao re-enumerate, what it changes (descriptors, PID, clock
   source), and the device's default mode at power-on. Bears on S1-S4.
3. How the datastore `SendBusCommand` payloads reach the device: a control transfer or a bulk endpoint, and with
   what framing. [Partly answered by the dext map: no control transfers exist (D0.1); commands go over the
   0xFF/4/1 bulk pipes with the D1.12 framing. The datastore opcodes stay open.]
4. Exact meaning of output chunk fields +0x177508 / +0x177510 / +0x0, and who writes them, when. [The dext map
   says the dext writes the first-frame host time and running byte totals there (D4.5).]
5. Name of the `USBDeviceReEnumerate` option bit 29 (MI-R2).
6. The unit of the `ChaseTimerProc` period of 5 (MI-H7), and what `PerformConfigChange` waits for (MI-H6).
7. Fields +0x260 / +0x264 / +0x268 (in and out channel counts versus "calc" counts); naming cannot be settled
   where in = out = 24.

The HAL map also marked "whether the dext reads an interrupt endpoint" as NOT RUN (outside its scope). [Answered
by the dext map: it looks up no interrupt pipe, D6.1; re-checked by verify-mac.]

## Part 3: the re-enumerator

- **MI-R1** Purpose: it detects the device bound to Apple's class driver instead of MOTU's dext and forces a USB
  re-enumeration so the dext can match. Rests on [R1.2-R1.7](../1-observations/mac-reenumerator.md#1-bring-up).
  verify-mac (G1): this is about which driver owns the device, not about switching it to vendor mode.
- **MI-R2** Option bit 29 is IOKit's re-enumerate "release device" option. Rests on
  [R1.6](../1-observations/mac-reenumerator.md#1-bring-up). The SDK on disk has no USB headers to confirm the
  bit's name.
