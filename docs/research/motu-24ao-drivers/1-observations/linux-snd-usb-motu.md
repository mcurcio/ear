# Observations: snd-usb-motu (`motu-usb-audio.c`)

Source tree: `/Volumes/Dock/motu-mac-driver/snd-usb-motu`, commit d36612237c5b (provenance in `../README.md`),
`motu-usb-audio.c` 1295 lines, plus `readme.md`, `Makefile`, `build_and_load.sh`. Source of these bullets:
`../sources/map-linux-snd-usb-motu.md` (all 1295 lines read). Citations are `:<line>` in `motu-usb-audio.c`
unless another file is named. Interpretations are in `../2-inferences/linux.md`.

Tags: "(re-checked by verify-linux / verify-mac)", "(corrected by verify-<side>)", "(added by verify-<side>)",
"(mapper only; not re-checked)" as in `mac-dext.md`. "(also in the <name> map, cited :<lines>)" = a Mac map
quoted the same Linux fact, with the line range it gave. "(re-checked by the editor)" = the editor read the lines
again while writing these notes. Untagged bullets are the Linux mapper's reading (CONFIRMED in the map).

## 0. Scope and interfaces

- **L0.1** Interfaces used: 0 for control, with an interrupt endpoint (:757); 1 for playback and 2 for capture,
  each on alt setting 1 (:601-602, :662). Frames are interleaved 24 x 3 bytes.
- **L0.2** Vendor-specific parts: one vendor control request that programs a "period interrupt", and the
  interrupt message it produces (:532-547).
- **L0.3** No sample-rate request and no clock-validity request anywhere in the driver.
- **L0.4** `readme.md` says the device must already be locked at 48 kHz, set through its web UI over Ethernet,
  before it is connected (readme:27-30).

## 1. Bring-up

- **L1.1** Match table: `USB_DEVICE(0x07fd, 0x0005)` on every interface, no class or interface filter
  (:1281-1284; also in the HAL map, cited :1282). (re-checked by verify-linux and verify-mac.) The driver also
  binds the device's other interfaces as no-op probes (verify-mac: "interfaces 1-5").
- **L1.2** `build_and_load.sh` runs `rmmod snd-usb-audio`; nothing in the tree blacklists snd-usb-audio
  (re-checked by verify-mac).
- **L1.3** Probe runs once per interface. Only `ifnum == 0` does any work (:1163); other interfaces return 0
  ("Probe Success") having done nothing (:1246).
- **L1.4** For interface 0, in order:
  1. `snd_card_new` with id "motuusb" (:1164).
  2. Builds the shred pattern once (:1170).
  3. Looks up interfaces 1 and 2 and claims both with `usb_driver_claim_interface` (:1178-1194).
  4. Allocates the interrupt URBs on interface 0, alt 0, endpoint[0] (:757).
  5. Allocates the stream URBs from alt setting 1, endpoint[0] of each streaming interface (:662-663).
  6. Creates one PCM with one playback and one capture substream (:1133).
  7. Allocates an ordered workqueue (:1222) and the proc entry `pb_adj` (:1236).
  8. `snd_card_register` (:1238).
  9. Submits the 4 interrupt URBs (:1243). They run from probe until disconnect, whether or not anything is
     streaming.
- **L1.5** No `usb_set_configuration`, no reset, no vendor mode request, no host-mode switch (also in the HAL
  map). Probe leaves alt settings untouched; they are set only when streaming starts.
- **L1.6** The only control request in the driver (:532-547; also in the dext map, cited :535-547, and the HAL
  map, cited :535-542):

  | Field | Value |
  |---|---|
  | bmRequestType | 0x40 (out, vendor, device) |
  | bRequest | 0x01 |
  | wValue | interrupt interval in sample frames |
  | wIndex | 0 |
  | wLength | 0, no data stage |
  | timeout | 500 ms |

- **L1.7** It is sent at stream start with wValue = max(capture period, playback period) (:568-570, :600) and
  at stream stop with wValue = 0 (:621). A failure is only logged (:544-546).
- **L1.8** No UAC2 class requests at all: no SET_CUR or GET_CUR on a clock source, no CLOCK_VALID read.

## 2. Clock and sample rate

- **L2.1** `SAMPLE_RATE` is 48000 at compile time (:19). The ALSA hardware description offers one rate
  (`rate_min = rate_max = SAMPLE_RATE`, added by verify-linux), one format (S24_3LE) and exactly 24 channels
  (:134-151).
- **L2.2** No rate is ever sent; clock validity is never waited for, polled or checked.
- **L2.3** UAC2 class interrupt messages are parsed into a struct and discarded: `handle_status_interrupt` is
  an empty function (:456-464), its `dev_info` commented out (added by verify-linux).
- **L2.4** The per-microframe frame counter caps at 7 frames (:209, :224, `NOM_SAMPLE_COUNT+1`).
- **L2.5** A compile-time table maps 44.1 kHz to a nominal 6 frames (:31-32); with only 48 kHz built it is never
  used.
- **L2.6** `readme.md` says clock lock loss is not handled (readme:29); nothing in the code reacts to it.

## 3. Start and stop

- **L3.1** ALSA trigger only sets `enabled` and queues work (:919-930, :1053-1064). ALSA start and stop do not
  start or stop the USB streams.
- **L3.2** `start_streaming_endpoints` (:563-606): if streaming is already running (`streams_started` was 1) it
  returns, unless the requested interval differs from the current one; then it queues stop-now plus
  start-after-1 ms (:549-561, :573-577). (re-checked by the editor at :573-576.) This path runs only when a new
  stream starts with a different period, which `hw_params` already forbids for a mismatched second stream
  (corrected by verify-linux).
- **L3.3** Otherwise, in order (also in the HAL map, cited :599-605):
  1. Capture and playback `copy_pos` are set to `TOTAL_UFRAMES` (2048), the "not synced" marker (:583-590).
  2. All buffer lengths are zeroed and the shred pattern is written into the 512 capture microframe slots
     (:592-598).
  3. The vendor interval request is sent (:600).
  4. `usb_set_interface(2,1)`, capture (:601).
  5. `usb_set_interface(1,1)`, playback (:602).
  6. The 4 capture URBs are submitted (:604-605).
- **L3.4** No playback URB is submitted at that point. Every return value of `set_interface` and
  `usb_submit_urb` there is ignored.
- **L3.5** Playback priming in `capture_complete_urb` (:516-529; also in the HAL map, cited :520-522): 1st
  capture completion, state IDLE to PRIME, nothing submitted; 2nd completion, playback URBs 0 and 1 submitted
  together and state goes to SYNC (:519-523); each later completion submits exactly one playback URB.
- **L3.6** `playback_complete_urb` is empty (:480-486; also in the dext map). Playback URBs are submitted only
  from capture completions.
- **L3.7** `stop_streaming_endpoints` (:608-630; also in the HAL map, cited :626-629): zero `pb_adj_total`, send
  interval 0, kill capture and playback URBs (interleaved by index), `set_interface(2,0)`, then
  `set_interface(1,0)`.
- **L3.8** Stop is queued only when both streams are disabled, and only after 2000 ms (:807-821). A start cancels
  a pending stop (:799). Both directions always run together, even for a capture-only or playback-only user.

## 4. Isochronous framing

- **L4.1** 4 URBs per direction x 512 packets each, `interval = 1`, flags `URB_NO_TRANSFER_DMA_MAP |
  URB_ISO_ASAP` (:21-24, :698-703; flags re-checked by verify-linux and verify-mac).
- **L4.2** Packet slot stride is `wMaxPacketSize x mult` (:676).
- **L4.3** All 4 URBs of a direction share one coherent buffer of 512 slots (:687-699, :710-715):
  `transfer_buffer` is allocated only while it is NULL, and `bufs[i*512+f].data = transfer_buffer +
  max_packet_size*f` for every URB i (re-checked by verify-linux and verify-mac). The buffer is freed only
  through URB 0 (:644).
- **L4.4** `bufs[]` has 2048 entries (`TOTAL_UFRAMES`); its memory repeats every 512 microframes (64 ms). Lengths
  and positions are kept per 2048 index (:21-24).
- **L4.5** Format: S24_3LE, 24 channels, 72-byte frames (:25-27), interleaved in host order; the driver never
  reorders channels. Nominal 6 frames per microframe, at most 7 (:34, :209). No header or marker per packet or
  per frame.
- **L4.6** Capture length detection reads packet memory in place, before the URB completes, during each period
  interrupt, not `actual_length`:
  - every consumed slot is overwritten with a 7-frame shred pattern, byte i = ~i, repeated per 72-byte frame
    (:198-211, :342, :380; also in the dext map, cited :198-211, and the HAL map, cited :201; verify-mac cites
    the detection as :330-380);
  - `count_uframe_sample_frames` counts the 72-byte chunks from offset 0 that no longer equal the pattern
    (:220-229), comparing whole 72-byte frames, up to `NOM_SAMPLE_COUNT+1` per microframe (HAL map, :224);
  - `count_sample_frames` walks forward from `chase_pos`, the last place the device's writes were found
    (:236-266; dext map cites :220-265).
- **L4.7** Every copy writes whole frames at `copy_frame*72` from offset 0 of each packet (:276, :309, :368).
  Alignment of channel 0 is never checked or recovered.
- **L4.8** Playback packet byte length is copied from the capture packet's `actual_length` (:505-511).

## 5. Rate matching

- **L5.1** The capture URB completed on the k-th completion gives its per-packet lengths to playback URB k mod 4
  (:497-511), which is then submitted (:527). (also in the dext map, cited :488-512, and the HAL map, cited
  :503-511.)
- **L5.2** `urb->start_frame` is never read (re-checked by verify-linux and verify-mac).
- **L5.3** On the first period interrupt in SYNC: playback `copy_pos = chase_pos + 16 - 1024` (mod 2048)
  (:416-427; `PB_SAFETY_OFFSET` = 16, :28; 1024 = `UFRAMES_PER_URB*2`, verify-linux). Each period interrupt then
  writes one period from `copy_pos`, advancing by the mirrored lengths (:290-325).
- **L5.4** `pb_adj`: written through `/proc/asound/cardN/pb_adj` (:1097-1109). It shifts playback `copy_pos` by N
  microframes (not frames) once, at the next period interrupt, and accumulates into `pb_adj_total`, which the
  proc file reads back and which resets at stop (:428-433, :619; also in the HAL map, cited :428-432).
  `copy_frame` is not reset when it applies (:429-430).
- **L5.5** Period clock: the device's interval interrupt drives ALSA; each interrupt is one period, interval =
  period frames (:394, :568; HAL map cites :471). The ALSA position advances only in `copy_period_from_usb` and
  `copy_period_to_usb`, called from `handle_interval_interrupt` (added by verify-linux).
- **L5.6** Capture and playback must share one period size; `hw_params` rejects a mismatch (:867-874,
  :1001-1008). The pointer callbacks report only period boundaries, with `SNDRV_PCM_INFO_BATCH` set (:139, :189,
  :942, :1076).
- **L5.7** Capture startup: capture waits until `rx_frames` >= period + 16 frames (`REC_SAFETY_OFFSET`, in
  frames; the playback offset is in microframes), then discards whole microframes down to that margin
  (:398-415).

## 6. Interrupt and status

- **L6.1** 4 interrupt URBs, 6-byte buffers, `interval = 1`, on interface 0 alt 0 endpoint[0] (:753-780).
- **L6.2** Message `{u8 info; u8 attr; u32 frame}`, packed (:117-122). `info == 0x01 && attr == 0x01` is the
  period interrupt, handled by `handle_interval_interrupt` (:471-472). The `frame` field (:121) is never read
  (re-checked by verify-linux and verify-mac).
- **L6.3** Any other message is cast to the UAC2 class layout (info, attr, control number, control selector,
  interface, entity id; :124-132) and ignored (:456-464; HAL map cites :461-475).
- **L6.4** The interrupt completion resubmits unconditionally, even on error status (:477), with no backoff and
  no error count.

## 7. Errors and recovery

- **L7.1** Capture packet errors: the nominal 432 bytes is substituted as the playback length and
  `dev_info("rec urb error")` is printed per errored packet (:505-508).
- **L7.2** Capture and playback completions never check `urb->status`; capture resubmits regardless (:514) and
  playback errors are ignored (:480). `capture_complete_urb` does check each packet's status (:497-514; added by
  verify-mac; see L7.1).
- **L7.3** No `usb_submit_urb` result is checked anywhere (verify-mac lists :477, :514, :521-527, :605, :792).
  There is no watchdog; nothing calls `snd_pcm_stop_xrun` or detects stalled completions (added by
  verify-linux).
- **L7.4** Until capture syncs, `handle_interval_interrupt` returns early (:401), before playback sync and the
  playback copy.
- **L7.5** ALSA stop and start only toggle `enabled`; there is no USB restart. While a stream is disabled the
  handler still consumes capture (`sync_period_from_usb`) and writes silence for playback (:440-452). Playback
  copies `dma_area` regardless of the application's write position.
- **L7.6** The interrupt path uses `spin_trylock`. On contention capture is synced (`sync_period_from_usb`) and
  playback is muted (`mute_period_to_usb`, which zeroes it, memset at :278), without `period_elapsed`
  (:436-452), and the ALSA position loses that period (L5.5). (re-checked by the editor at :436-452; both
  verifiers confirm the mute.)
- **L7.7** A period-size change triggers a full stop and start (:573-576), on the path described in L3.2.
- **L7.8** No handling of device errors or clock loss.
- **L7.9** Disconnect of interface 0, in order (:1255-1278): `snd_card_disconnect`; clear the workqueue pointer
  under a lock; `cancel_delayed_work_sync` on both works; destroy the workqueue; free the URBs;
  `snd_card_free_when_closed`.
- **L7.10** Probe error path (:1249-1252): only `snd_card_free` (re-checked by the editor: `error_free:` then
  `snd_card_free(priv->card)`). It does not free the URBs, coherent buffers or workqueue and does not release
  interfaces 1 and 2. `snd_card_free` frees priv, so already-claimed interfaces keep a dangling priv as their
  interface data (corrected by verify-linux and verify-mac; the map said only that they stay bound).

## 8. Other

- **L8.1** The shred writes 504 bytes per slot (:209); there is no check that the max packet size is at least
  504.
- **L8.2** Maximum period is 2048 frames (about 341 microframes). Consumed slots are re-shredded in
  `copy_period_from_usb` and `sync_period_from_usb` (added by verify-linux). `start_streaming_endpoints` shreds
  only the first 512 slots (verify-linux).
- **L8.3** Chase: it steps back 2 microframes after the last filled slot (:254), and gives up after 33 empty
  slots without advancing `chase_pos` (:257-259).
- **L8.4** `pb_start_state`, `interrupt.interval`, the playback lengths and `pb_adj` are shared between URB
  completions, the workqueue and proc with no locks.
- **L8.5** `hw_params` adds no integer-periods constraint; the driver wraps at `period_count x period_frames`
  (:186-189).
- **L8.6** Alt 1 is assumed to exist (a NULL dereference otherwise, :662-663) and its format is never checked.
- **L8.7** `GFP_ATOMIC` submits are made from the workqueue (:605).
- **L8.8** Not present: rates other than 48 kHz (:19); mixer or ALSA controls; MIDI; clock source selection and
  lock handling; channel counts other than 24 (`readme.md` plans "64 channels"); explicit feedback; suspend and
  resume (no `.suspend` or `.resume` in the `usb_driver`, :1288-1293).
