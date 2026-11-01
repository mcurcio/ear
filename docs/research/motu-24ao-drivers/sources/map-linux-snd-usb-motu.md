# snd-usb-motu (motu-usb-audio.c, commit d366122, 1295 lines): driver map

I read all 1295 lines, every function, plus `readme.md`, `Makefile` and `build_and_load.sh`. I did not execute anything; this is static analysis only. Citations are `motu-usb-audio.c:<line>`. CONFIRMED means I read it in the code. INFERRED means I reasoned it from structure or names. The brief's "~1000 lines" and commit "d36612237c5b" do not match the checkout: `git log` shows d366122 ("Update readme.md"), which may be the same commit abbreviated.

Two framing facts first:

- **It is not a different streaming format.** The driver reuses the USB Audio Class 2 (UAC2) interface layout: interface 0 for control (with an interrupt endpoint), interface 1 for playback and interface 2 for capture, each on alt setting 1, carrying plain interleaved 24 × 3-byte frames. Only two things are vendor-specific: one vendor control request that programs a "period interrupt", and the vendor interrupt message it produces. (CONFIRMED: :601-602, :662, :757, :532-547.)
- **It never touches the clock.** There is no sample-rate request and no clock-validity request anywhere. The readme says the device must already be locked at 48 kHz, set through its web UI over Ethernet, before it is connected (readme:27-30).

## 1. Bring-up

- **Device match (CONFIRMED).** It matches `USB_DEVICE(0x07fd, 0x0005)` on every interface (:1281-1284). There is no class or interface filter, so snd-usb-audio also matches the device. `build_and_load.sh` handles this by `rmmod snd-usb-audio`, and nothing blacklists it.
- **Probe (CONFIRMED).** Probe runs once per interface. Only `ifnum == 0` does any work (:1163). For any other interface it returns 0 ("Probe Success") having done nothing (:1246).
- **Interface 0 does, in order (CONFIRMED):**
  1. `snd_card_new` with id "motuusb" (:1164).
  2. Builds the shred pattern once (:1170).
  3. Looks up interfaces 1 and 2 and claims both with `usb_driver_claim_interface` (:1178-1194).
  4. Allocates the interrupt URBs on interface 0, alt 0, endpoint[0] (:757).
  5. Allocates the stream URBs from alt setting 1, endpoint[0] of each streaming interface (:662-663).
  6. Creates one PCM with one playback and one capture substream (:1133).
  7. Allocates an ordered workqueue (:1222) and the proc entry `pb_adj` (:1236).
  8. `snd_card_register` (:1238).
  9. Submits the 4 interrupt URBs (:1243). These run from probe until disconnect, whether or not anything is streaming.
- **No configuration change, re-enumeration or mode switch (CONFIRMED).** There is no `usb_set_configuration`, no reset and no vendor mode request. Probe leaves the alt settings untouched; they are set only when streaming starts.
- **Probe-order assumption (INFERRED).** It assumes interface 0 probes before 1 and 2. If 1 or 2 bound first, the stub return at :1246 would bind them with no data, and the claim at :1186/:1191 would fail with -EBUSY.
- **Only control request in the driver (CONFIRMED, :532-547):**

  | Field | Value |
  |---|---|
  | bmRequestType | 0x40 (out, vendor, device) |
  | bRequest | 0x01 |
  | wValue | interrupt interval in sample frames |
  | wIndex | 0 |
  | wLength | 0, no data stage |
  | timeout | 500 ms |

  It is sent at stream start with wValue = max(capture period, playback period) (:568-570, :600), and at stream stop with wValue = 0 (:621). A failure is only logged (:544-546). There are no UAC2 class requests at all: no SET_CUR or GET_CUR on a clock source, and no CLOCK_VALID read.

## 2. Clock and sample rate

- **Rate is fixed at compile time (CONFIRMED).** `SAMPLE_RATE` is 48000 (:19). The ALSA hardware description offers exactly one rate, one format (S24_3LE) and exactly 24 channels (:134-151).
- **No rate is ever sent and clock validity is never waited for, polled or checked (CONFIRMED by absence).** The UAC2 class interrupt messages, which carry clock-validity changes, are parsed into a struct and thrown away: the handler is an empty function (:456-464).
- **What this means for S2 and S3 (INFERRED).** Neither can occur in this driver's own code path: it never asks for a rate, so the stock driver's set-rate ENXIO cannot happen. The price is that a device which is unlocked, or at another rate, is streamed blindly. At 44.1 kHz the playback packets would mirror the device's 5-6 frames per microframe while ALSA believes 48 kHz. At 96 kHz the per-microframe frame counter caps at 7 frames (:209, :224, `NOM_SAMPLE_COUNT+1`), so capture would be miscounted.
- **Dead code (CONFIRMED).** The compile-time table maps 44.1 kHz to a nominal 6 frames (:31-32); with only 48 kHz built, it is never exercised.
- **Clock loss is unhandled.** The readme says so (readme:29), and nothing in the code reacts to it (CONFIRMED by absence).

## 3. Stream start and stop order

ALSA start and stop do not start or stop the USB streams. A trigger only sets `enabled` and queues work (:919-930, :1053-1064).

**Start work, `start_streaming_endpoints` (:563-606, CONFIRMED).**
- If streaming is already running (`streams_started` was 1) it returns, unless the requested interval differs from the current one. In that case it queues stop-now plus start-after-1 ms (:549-561, :573-577).
- Otherwise, in order:
  1. Resets positions: capture and playback `copy_pos` are set to `TOTAL_UFRAMES` (2048), which serves as the "not synced" marker (:583-590).
  2. Zeroes all buffer lengths, and writes the shred pattern into the 512 capture microframe slots (:592-598).
  3. Sends the vendor interval request (:600).
  4. `usb_set_interface(2,1)`, capture first (:601).
  5. `usb_set_interface(1,1)`, playback (:602).
  6. Submits the 4 capture URBs (:604-605).
- No playback URB is submitted at this point.
- Every return value is ignored here: `set_interface` and `usb_submit_urb` (CONFIRMED).

**Playback priming, in `capture_complete_urb` (:516-529, CONFIRMED).**
- 1st capture completion: state IDLE goes to PRIME, nothing is submitted.
- 2nd completion: playback URBs 0 and 1 are submitted together and the state goes to SYNC.
- Each later completion submits exactly one playback URB.
- Playback is therefore driven entirely by capture completions; `playback_complete_urb` is empty (:480-486).
- Primed contents: playback data is whatever is in the (zeroed, coherent) buffer until the period handler writes it, so silence (INFERRED).

**Stop work, `stop_streaming_endpoints` (:608-630, CONFIRMED).**
- Order: zero `pb_adj_total`, send interval 0, kill capture and playback URBs (interleaved by index), `set_interface(2,0)`, then `set_interface(1,0)`.
- Stop is queued only when both streams are disabled, and only after 2000 ms (:807-821). A start cancels a pending stop (:799).
- Both directions always run together, even for a capture-only or playback-only user.

## 4. Isochronous framing

**Buffers and URBs (CONFIRMED).**
- 4 URBs per direction × 512 packets each, `interval = 1` (every microframe), `URB_ISO_ASAP` (:21-24, :698-703).
- Packet slot stride is `wMaxPacketSize × mult` (:676).
- All 4 URBs of a direction share one coherent buffer of only 512 slots (:687-699, :710-715). The buffer is freed only through URB 0 (:644).
- So the driver's 2048-entry `bufs[]` index space aliases memory every 512 microframes. The effective audio ring is 512 microframes, which is 64 ms. The lengths, by contrast, are kept per 2048 index.

**Format (CONFIRMED).**
- S24_3LE, 24 channels, 72-byte frames (:25-27), interleaved in host order. The driver never reorders channels.
- Nominal 6 frames per microframe, at most 7 (:34, :209).
- No header or marker per packet or per frame.

**Capture length detection (CONFIRMED).**
- Capture does not read `actual_length` to find the audio. It reads the packet memory in place, before the URB completes, during each period interrupt:
  - Every consumed slot is overwritten with a 7-frame "shred" pattern, byte i = ~i (:198-211, :342, :380).
  - `count_uframe_sample_frames` counts the 72-byte chunks from offset 0 that no longer equal the pattern (:220-229).
  - `count_sample_frames` walks forward from `chase_pos`, the last place the device's writes were found (:236-266).
- So the period handler can deliver audio that arrived up to 64 ms before the URB completes.

**Channel-0 alignment (CONFIRMED by construction; never checked or recovered).**
- Every copy is whole frames written at `copy_frame*72` from offset 0 of each packet (:276, :309, :368).
- Playback packet byte length is copied from the capture packet's `actual_length` (:505-511).
- Alignment therefore holds only if every capture packet is a whole multiple of 72 bytes. If one is not, the playback packet carries the raw byte count while `bufs.length` is floored, so trailing stale bytes go out and the device sees a partial frame. Nothing detects this (INFERRED risk).
- Short of that, the worst desync here is a stale or duplicated whole frame, never a channel rotation (INFERRED). The driver has no rotation recovery.

## 5. Rate matching

- **Implicit feedback, mirrored packet by packet (CONFIRMED).** The capture URB completed on the k-th completion gives its per-packet lengths to playback URB k mod 4 (:497-511). That URB is then submitted (:527).
- **Lag and write lead (INFERRED from `ISO_ASAP` queueing).** Two playback URBs are always queued, so playback URB k transmits about two capture windows later (about 128 ms). That is why playback lengths are known before data is written ahead.
- **Playback write-position sync (CONFIRMED).** On the first period interrupt in SYNC: `copy_pos = chase_pos + 16 − 1024` (mod 2048), meaning two URBs back plus a 16-microframe (2 ms) safety lead (:416-427, `PB_SAFETY_OFFSET` :28). Each period interrupt then writes one period from `copy_pos`, advancing by the mirrored lengths (:290-325). In steady state the lead swings between about 16 microframes and 16 microframes plus one period.
- **What the 2 ms lead depends on (INFERRED).**
  - The lead assumes playback is transmitting exactly two URBs behind the capture position. The host controller's actual start delay for `ISO_ASAP` is never measured: `urb->start_frame` is never read.
  - If the period interrupt is handled more than about 2 ms late, playback sends stale audio. Because the buffer aliases, that stale audio is 64 ms old.
- **`pb_adj` (CONFIRMED).** It is written via `/proc/asound/cardN/pb_adj` (:1097-1109). It shifts playback `copy_pos` by N microframes (not frames) once, at the next period interrupt, and accumulates into `pb_adj_total`, which the proc file reads back and which resets at stop (:428-433, :619).
  - Despite the brief's "drift knob" wording, it corrects no drift: packet sizes already follow the device clock. Its purpose is most likely tuning the write lead or latency per host (INFERRED).
  - `copy_frame` is not reset when it applies (:429-430).
- **Period clock (CONFIRMED).** The device's interval interrupt drives ALSA. Each interrupt is exactly one period, because interval = period frames (:394, :568). Capture and playback must share one period size; `hw_params` rejects a mismatch (:867-874, :1001-1008). The pointer callbacks report only period boundaries, with `SNDRV_PCM_INFO_BATCH` set (:139, :189, :942, :1076).
- **Capture startup (CONFIRMED).** Capture waits until `rx_frames` ≥ period + 16 frames (`REC_SAFETY_OFFSET`, in frames, while the playback offset is in microframes), then discards whole microframes down to that margin (:398-415).

## 6. Interrupt endpoint

- **URBs (CONFIRMED).** 4 URBs, 6-byte buffers, `interval = 1`, on interface 0 alt 0 endpoint[0] (:753-780). On xHCI the descriptor's bInterval probably governs instead (INFERRED).
- **Message `{u8 info; u8 attr; u32 frame}`, packed (CONFIRMED, :117-122).**
  - `info == 0x01 && attr == 0x01` is the period interrupt, handled by `handle_interval_interrupt` (:471-472).
  - The `frame` field is never read (CONFIRMED).
  - INFERRED: this fits UAC2's interrupt data message, with bInfo bit 0 = vendor-specific and bAttribute 1 = CUR.
- **Any other message (CONFIRMED).** It is cast to the UAC2 class layout (info, attr, control number, control selector, interface, entity id; :124-132) and ignored (:456-464). That includes clock-validity changes.
- **Resubmission (CONFIRMED).** The completion resubmits unconditionally, even on error status (:477), with no backoff and no error count.

## 7. Errors and recovery

- **Capture packet errors (CONFIRMED).** The nominal 432 bytes is substituted as the playback length, and `dev_info("rec urb error")` is printed per errored packet (:505-508). That can be up to 512 prints per URB in atomic context, a log storm.
- **URB status (CONFIRMED).** Capture and playback completions never check `urb->status`. Capture resubmits blindly (:514); playback errors are ignored (:480).
- **Unchecked submits (CONFIRMED).** No `usb_submit_urb` result is checked anywhere. One lost capture URB stops its playback partner silently, and there is no watchdog. If a playback length ever exceeds the playback endpoint's max packet size, submission fails (INFERRED -EMSGSIZE) and playback stalls.
- **Startup stall (CONFIRMED).** Until capture syncs, `handle_interval_interrupt` returns early (:401). That also blocks playback sync and the playback copy, so with no capture data there is no audio either way.
- **ALSA underrun and overrun (CONFIRMED).**
  - No USB restart. ALSA stop and start only toggle `enabled`; while a stream is disabled, the handler still consumes capture (`sync_period_from_usb`) and writes silence for playback (:440-452).
  - So a PipeWire restart-on-underrun does not restart USB streaming (a contrast with S1 and S4, where every stream start is a risk).
  - Playback copies `dma_area` regardless of the application's write position; ALSA's own stop threshold judges the xrun.
- **Lock contention (CONFIRMED).** The interrupt path uses `spin_trylock`. On contention it syncs or mutes that period without `period_elapsed`, so the ALSA position silently loses a period (:436-452).
- **Period-size change while running (CONFIRMED).** It triggers a full stop and start (:573-576).
- **Device errors and clock loss: no handling.**
- **Unplug (CONFIRMED).** On disconnect of interface 0, in order (:1255-1278):
  1. `snd_card_disconnect`.
  2. Clear the workqueue pointer under a lock.
  3. `cancel_delayed_work_sync` on both works.
  4. Destroy the workqueue.
  5. Free the URBs.
  6. `snd_card_free_when_closed`.
- **Teardown gaps (INFERRED).** The stop sequence (interval 0, alt 0) never runs on rmmod with the device present. USB core's unbind likely resets alt settings, but the vendor interval request stays armed.
- **Probe error path leaks (CONFIRMED by absence).** It does not free URBs, coherent buffers or the workqueue, and does not release interfaces 1 and 2 (:1249-1252). They stay bound to this driver, so snd-usb-audio cannot take them.

## 8. Other load-bearing details

- **Possible buffer overrun (CONFIRMED arithmetic; maxp unknown).** The shred writes 504 bytes per slot (:209) and there is no check that max packet size is at least 504. If the device's capture maxp is below 504, the last slot overruns the coherent buffer.
- **Shred aliasing (CONFIRMED).** Shredding works only because consumed slots are re-shredded before the device rewrites them 64 ms later. That holds while period + margin < 512 microframes; the maximum period is 2048 frames, about 341 microframes.
- **Chase heuristics (CONFIRMED).**
  - It steps back 2 microframes after the last filled slot (:254). By my reading this double-counts one microframe per call, so `rx_frames` runs high. It only affects the startup discard, since `rx_frames` is unused after sync.
  - It gives up after 33 empty slots without advancing `chase_pos` (:257-259).
  - A real audio frame that exactly equals the shred pattern would be miscounted (negligible).
- **Unlocked shared state (CONFIRMED).** `pb_start_state`, `interrupt.interval`, the playback lengths and `pb_adj` are shared between URB completions, the workqueue and proc with no locks. That is probably safe only because one host controller gives back URBs serially (INFERRED).
- **No integer-periods constraint (INFERRED).** `hw_params` never adds one, while the driver wraps at `period_count × period_frames` (:186-189). A buffer that is not a whole number of periods would desync the position.
- **Alt 1 assumed (CONFIRMED).** Alt 1 is assumed to exist (NULL dereference otherwise, :662-663) and its format is never checked.
- **Atomic allocations from sleepable context (CONFIRMED).** `GFP_ATOMIC` submits from the workqueue (:605) are harmless.
- **What it leaves out:**
  - Rates other than 48 kHz, set only at compile time (:19).
  - Any mixer or ALSA controls.
  - MIDI.
  - Clock source selection and lock handling.
  - Channel counts other than 24 (the readme's planned "64 channels").
  - Explicit feedback.
  - Suspend and resume (no `.suspend` or `.resume` in the `usb_driver`, :1288-1293).

## OPEN QUESTIONS

1. The 24Ao's alt-setting-1 descriptors: playback and capture `wMaxPacketSize` and mult (needed to rule out the 504-byte shred overrun), the endpoint addresses, and whether a playback feedback endpoint exists that this driver ignores. No `lsusb -v` was on disk.
2. Whether the device ever sends a capture packet that is not a multiple of 72 bytes. That is the only path here to a channel rotation, and it is unchecked.
3. What the period-interrupt `frame` field (u32) means, and its byte order. The driver never reads it.
4. Whether the vendor interval request (0x40/0x01) also gates the device's streaming or clock, or only the interrupt. The driver always sends it before `set_interface`, and sends 0 before alt 0.
5. Whether keeping both directions running and avoiding USB restarts on ALSA restarts is what avoids S1 and S4. Static reading cannot show it.
6. The host controller's actual `ISO_ASAP` start latency against the assumed two-URB playback offset. This is probably what `pb_adj` compensates; the author's intent is not stated.
7. Whether the device needs its clock locked before enumeration (readme:29) or only before streaming starts.
8. Whether the double count in `count_sample_frames` (:254) is intended.

No scratch files were written; `/Volumes/Dock/motu-mac-driver/scratch/` exists but is empty.