# Gaps between MOTU's Mac driver and snd-usb-motu

> **Everything in this file is COMPARISON or judgement, not observation.** It is one agent's comparison
> (`sources/compare-gaps.json`) and two skeptics' verdicts (`sources/verify-linux-side.json`,
> `sources/verify-mac-side.json`). The comparer's and verifiers' text is copied verbatim; only the headings,
> the status line and the editor's notes (marked "Editor") are added. The facts each side rests on are in
> `../1-observations/`; the interpretations in `../2-inferences/`. Symptoms S1-S4 are defined in `../README.md`.

Status rule: **survived** = neither verifier refuted it; **disputed** = one did; **refuted** = both did.

| Gap | Title (short) | Confidence (comparer) | verify-linux | verify-mac | Status |
|---|---|---|---|---|---|
| G1 | Mac takes the device out of class-compliant (UAC) mode and streams on MOTU's vendor isochronous interface | medium | refuted | not refuted | **disputed** |
| G2 | Mac sends no vendor control request at stream start or stop | medium | refuted | refuted | **refuted** |
| G3 | Mac schedules isochronous transfers on explicit frame numbers with a fixed IN to OUT offset | high | not refuted | not refuted | **survived** |
| G4 | Mac zeroes every OUT buffer before it is submitted | high | not refuted | not refuted | **survived** |
| G5 | Mac derives timing and a watchdog from USB frames | high | not refuted | not refuted | **survived** |
| G6 | Mac owns the whole device exclusively and evicts the class driver | medium | not refuted | not refuted | **survived** |
| G7 | Mac sets the sample rate and clock explicitly through the device datastore and waits for reconfiguration | medium | not refuted | not refuted | **survived** |
| G8 | Mac's ring is 400 ms deep with independent buffers | medium | not refuted | not refuted | **survived** |

## G1. Mac takes the device out of class-compliant (UAC) mode and streams on MOTU's vendor isochronous interface; snd-usb-motu stays in UAC mode on the UAC streaming interfaces

- **Status: disputed** (verify-linux refuted: true; verify-mac refuted: false)
- **Area:** 1 Bring-up / 3 Stream start
- **Confidence (comparer):** medium

**Mac behaviour.** The HAL plug-in rewrites the device datastore key host/mode from "UAC" to "USB2" (or "USB3"), chosen by the dext's isocMode property. The dext first looks for a vendor isochronous interface (class 0xFF, subclass 0x04, protocol 0x02) and uses the UAC2 streaming interfaces (1/2/0x20) only as a fallback. The re-enumerator forces a re-enumeration whenever Apple's class driver holds 07fd:0005. MOTU never runs this device through the class-compliant path on macOS.

**Mac evidence.** CONFIRMED: SA_Device::ProcessDataStoreChanges compares the key with "host/mod"+'e', reads dext property 10 (isocMode) and posts host/mode="USB2" when isocMode is 2 and "USB3" when it is 3. CONFIRMED: usb_provider sets fIsocMode=2/3 when an interface with immediates $0xff,$0x4,$0x2 exists, and logs "Setting fIsocMode to %d". CONFIRMED: StartAudioDMA calls GetInterfacesAndIsocPipes(0xFF,4,2) first and falls back to (1,2,0x20) ("opened MOTU isoc interfaces and pipes" / "opened UAC isoc interfaces and pipes"). CONFIRMED: the re-enumerator calls Server::armTimer -> USBDeviceReEnumerate after "found a MOTU device using Apple class driver".

**Linux behaviour.** No mode switch and no vendor interface. It streams on interface 2 alt 1 (capture) and interface 1 alt 1 (playback), the UAC2 layout, and uses UAC2-style interrupt messages on interface 0. Linux dmesg shows the 24Ao's config 1 has interfaces 4 and 5 with altsetting 1 reusing endpoints 0x09 and 0x87 ("duplicate endpoint"). That fits a second, vendor-mode pair of streaming interfaces on the same endpoints (INFERRED; no lsusb -v).

**Linux evidence.** motu-usb-audio.c:601-602 (usb_set_interface(2,1), (1,1)); :662-663 alt 1 endpoint[0]; :1163-1194 claims only interfaces 1 and 2; no set_configuration or mode request anywhere (CONFIRMED by absence). The dmesg lines in /Volumes/Dock/iac-ear-run/motu-quirks/*.log: "config 1 interface 4 altsetting 1 has a duplicate endpoint with address 0x9" and "interface 5 ... 0x87".

**Symptom link.** Possibly S1 and S4. MOTU's known-good path never exercises the device's UAC streaming firmware, and the stock driver's rotation and freezes happened in UAC mode. snd-usb-motu stays on that same UAC firmware path, so any firmware fault specific to UAC mode stays in reach. S2 and S3 belong to the UAC clock entities, which vendor mode would not use.

**Proposed fix.** Find out from a descriptor dump whether interfaces 4 and 5 (or another one) are class 0xFF/0x04/0x02. If they are, set host/mode=USB2 in the device datastore, either once through the device's web API (the same key tree the Mac uses) or from the driver over the vendor bulk command interface (0xFF/0x04/0x01), and stream on the vendor interfaces. Keep the UAC interfaces only as a fallback.

**verify-linux (refuted: true), verbatim:**

> Linux side holds: motu-usb-audio.c claims only interfaces 1 and 2 (probe, usb_ifnum_to_if(dev,1/2) + usb_driver_claim_interface), streams on alt 1 via usb_set_interface(2,1)/(1,1) in start_streaming_endpoints, and sends no mode or set_configuration request (CONFIRMED by absence). But the gap does not hold as a defect. (1) The claim that MOTU never uses the class-compliant path on macOS is contradicted by the dext itself: its strings include both "opened MOTU isoc interfaces and pipes" and "opened UAC isoc interfaces and pipes" (CONFIRMED string), so MOTU's own driver supports UAC streaming as a fallback. (2) Nobody has shown that the 24Ao has a 0xFF/0x04/0x02 interface at all. The duplicate-endpoint dmesg lines for interfaces 4/5 show that some extra interfaces share 0x09/0x87, not their class (INFERRED). If they already appear in the current mode, the host/mode switch may not even be what exposes them. (3) The S1/S4 link rests on snd-usb-audio behaviour: snd-usb-motu was written by a MOTU engineer for interfaces 1/2 with MOTU's vendor interval interrupt and shred-pattern counting, so it is not the stock UAC path, and no S1 rotation has been seen with it. (4) The fix is not actionable as written: writing the datastore over the vendor bulk interface needs a protocol nobody has documented. Keep it as an open research item: run lsusb -v to get interface 4/5 class/subclass/protocol. It is not a confirmed gap.

**verify-mac (refuted: false), verbatim:**

> Mac side holds, with two corrections. CONFIRMED: in the StartAudioDMA block, usb_provider::StartAudioDMA calls GetInterfacesAndIsocPipes(0xff,4,2) first (mov w0,#0xff / w1,#4 / w2,#2). That function compares interface descriptor bytes +5/+6/+7, which are class, subclass and protocol. When the call returns false (cbz w0) the code falls back to (1,2,0x20). The log strings are 'opened MOTU isoc interfaces and pipes' and 'opened UAC isoc interfaces and pipes'. CONFIRMED: usb_provider::initPriv calls FindInterface(0xff,4,2). If it is found, fIsocMode (+0x1208) is set to 2 when bcdUSB<=0x2ff and to 3 otherwise (cmp w8,#0x2ff; cinc). CONFIRMED: in SA_Device::ProcessDataStoreChanges, when host/mode equals "UAC" the code posts host/mode="USB2" if isocMode==2 or "USB3" if isocMode==3 (DataStoreService::Post), and posts nothing for any other isocMode. I did not verify the 'dext property 10' detail. CORRECTIONS: (a) The switch happens only when the device's current descriptors already contain a 0xFF/4/2 interface, and the dext has a complete UAC fallback path. So 'MOTU never runs this device class-compliant' overstates the code: the code supports both paths. (b) The re-enumerator only evicts Apple's class driver ('found a MOTU device using Apple class driver', USBDeviceReEnumerate, 1000 ms re-arm). That is about which driver owns the device, not about switching it to vendor mode. Linux side CONFIRMED by absence: there is no mode request, and the driver uses interface 1 and 2 alt 1 (motu-usb-audio.c:601-602, 662-663). However, the driver already speaks MOTU's vendor protocol on those interfaces: a vendor interval request (:532-547), vendor interrupts (:466-472) and shred-pattern length detection (:330-380), the same idea as the dext's shredPoll. The dmesg duplicate-endpoint lines show that interfaces 4 and 5 reuse the same endpoints 0x09 and 0x87, so both modes would stream on the same endpoints. That interfaces 4 and 5 are 0xFF/4/2 is INFERRED. The link to S1 and S4 (a firmware fault specific to UAC mode) is INFERRED with no evidence: S1 and S4 were seen with snd-usb-audio, not with snd-usb-motu. The gap is real, but its value depends on the descriptor dump, which is the right first step of the proposed fix.

*Editor: both verifiers correct the Mac-side summary (the dext has a full UAC fallback; the re-enumerator only evicts Apple's class driver and does not switch mode) and the symptom link (S1 and S4 were seen under snd-usb-audio, not snd-usb-motu). See README, Corrections C2-C4.*

## G2. Mac sends no vendor control request at stream start or stop; snd-usb-motu sends vendor request 0x01 (interval) before every alt-setting change

- **Status: refuted** (verify-linux refuted: true; verify-mac refuted: true)
- **Area:** 3 Stream start/stop, 6 Interrupt endpoint
- **Confidence (comparer):** medium

**Mac behaviour.** The dext issues no USB control request of any kind; the only device-facing actions at start and stop are alt-setting changes and isochronous I/O. Timing comes from the USB frame number and host time, not from a device interrupt.

**Mac evidence.** CONFIRMED: IOUSBHostDevice::DeviceRequest is absent from the dext's imports (nm -u). No interrupt pipe is looked up in usb_provider. StartAudioDMA and StopAudioDMA send nothing besides SelectAlternateSetting and the pipe I/O.

**Linux behaviour.** At every stream start it sends bmRequestType 0x40, bRequest 0x01, wValue = period frames, then alt 1 on interface 2, then on interface 1. At stop it sends wValue 0, then alt 0. A period-size change while streaming does stop plus start. Period timing depends entirely on the device's interrupt (info 0x01, attr 0x01) on interface 0.

**Linux evidence.** motu-usb-audio.c:532-547 (request), :568-570 and :600 (sent before set_interface), :621 (wValue 0 at stop), :573-577 (stop/start on interval change), :471-472 (period interrupt drives the PCM).

**Symptom link.** S4 (freezes around stream starts) and long-run reliability. The vendor interval command is extra firmware work at the exact moment the stock driver saw freezes, and the Mac never uses it. Linux also has no period clock without these interrupts (see G5). Whether the request is safe is INFERRED either way, since the Mac shows a design that avoids it, not that it is harmful.

**Proposed fix.** Drive the PCM period from isochronous completions and the frame counter instead of the vendor period interrupt, and stop sending the 0x01 request (or send it once, not at every start and stop). At minimum, never re-send it while streams run (no stop/start on a period change).

**verify-linux (refuted: true), verbatim:**

> The difference is real. On Linux, set_interrupt_interval sends bmRequestType 0x40, bRequest 0x01, wValue=period, timeout 500 ms. start_streaming_endpoints calls it before usb_set_interface(2,1),(1,1), and stop_streaming_endpoints sends interval 0 (CONFIRMED). On the Mac, nm -u of the arm64 dext imports SetConfiguration and SelectAlternateSetting but no DeviceRequest/ControlRequest (CONFIRMED). But nothing ties the request to harm. S4 (the freezes) happened under snd-usb-audio, which never sends vendor request 0x01, so the request cannot explain S4. In snd-usb-motu the request is load-bearing: it is what makes the device emit the info=0x01/attr=0x01 interrupt that handle_interval_interrupt uses as the period clock. Removing it means redesigning period timing. With 512-microframe (64 ms) URBs, capture completions are far too coarse to drive periods of 32-2048 frames without also shrinking the URBs. The restart-on-period-change path (start_streaming_endpoints: interval != priv->interrupt.interval -> restart_streaming_endpoints) only runs when a new stream starts with a different period, which hw_params already forbids for a mismatched second stream. This is a design difference, not a gap with evidence. The real part, no liveness check, is covered by G5.

**verify-mac (refuted: true), verbatim:**

> The Mac facts hold: the dext imports no IOUSBHostDevice::DeviceRequest (CONFIRMED with nm -u). Every FindPipe in usb_provider::initPriv asks for bulk endpoints (type 2); there is no interrupt pipe, and the IOInterruptDispatchSource import belongs to the Thunderbolt provider. The StartAudioDMA block calls only GetInterfacesAndIsocPipes (which opens the interface and calls SelectAlternateSetting(1)), usb_io_stream::Start and SetMaxBusStall(5000). SetMaxBusStall is a host-side IOService::RequireMaxBusStall, not a device request. StopAudioDMA calls Abort, SelectAlternateSetting(0) and Close. One caveat: the dext does talk to the device over a bulk command interface (FindInterface(0xff,4,1); SendBusCommandSync/ASync; HTTP proxy), so 'no device-facing commands' holds only for the start and stop paths. The gap still does not stand on its own. Its only symptom claim, that vendor request 0x01 contributes to S4, is contradicted by the record: S4 happened under stock snd-usb-audio, which never sends request 0x01. Nothing shows the request is harmful, and it comes from MOTU's own author (motu-usb-audio.c:532-547). The proposed fix, dropping the request and timing from isochronous completions, is G5's fix, and doing only the first half would remove snd-usb-motu's only period clock (:394, :471). Fold this into G5.

*Editor: both verifiers fold the real part (no liveness check) into G5. See README, Corrections C4-C6.*

## G3. Mac schedules isochronous transfers on explicit frame numbers with a fixed IN to OUT offset; snd-usb-motu uses URB_ISO_ASAP and assumes the playback lag

- **Status: survived** (verify-linux refuted: false; verify-mac refuted: false)
- **Area:** 4 Isochronous framing / 5 Rate matching
- **Confidence (comparer):** high

**Mac behaviour.** usb_io_stream::Start reads the current frame. The first IN is scheduled at current+0x14 (or 0x13 if GetFrameNumber fails), and the first OUT at first IN frame + 0x140. Each request carries an explicit frame number that advances by 0x14, never as soon as possible. The dext publishes, per OUT chunk, the host time and the start and end byte positions, and the HAL writes output at absolute sample positions taken from them (WriteOutputData). The relation between capture packet sizes and playback position is therefore exact and known.

**Mac evidence.** CONFIRMED: usb_io_stream::Start immediates 0x14 and 0x140, and explicit frame numbers in the request ring. CONFIRMED: chunk fields +0x0, +0x177508 and +0x177510 are used by SA_DeviceUSB::StartAudio, UpdateZeroTimes and WriteOutputData.

**Linux behaviour.** All stream URBs use URB_ISO_ASAP. urb->start_frame is never read. The playback write position is set to chase_pos+16-1024 microframes, assuming playback runs exactly two URBs (1024 microframes) behind capture. Any difference is corrected only by the manual pb_adj knob.

**Linux evidence.** motu-usb-audio.c:698 (URB_NO_TRANSFER_DMA_MAP | URB_ISO_ASAP); :416-427 (copy_pos = chase_pos + 16 - 1024); :428-433 (pb_adj); :519-523 (two playback URBs submitted at once on the second capture completion). CONFIRMED.

**Symptom link.** Long-run reliability and latency. If the host controller starts the ASAP playback URBs at a different frame than assumed, the 2 ms write lead is wrong: early or stale audio, or a whole-frame shift. This is not an S1-style channel rotation; pb_adj exists to paper over it.

**Proposed fix.** Read the frame number at start and submit capture and playback URBs at explicit start_frame values with a fixed offset (no ISO_ASAP). Or read urb->start_frame back after submission and derive the playback write position from the real frame numbers. Then pb_adj is no longer needed.

**verify-linux (refuted: false), verbatim:**

> CONFIRMED on Linux. alloc_stream_urbs sets transfer_flags = URB_NO_TRANSFER_DMA_MAP | URB_ISO_ASAP for every stream URB, and urb->start_frame is never read anywhere. handle_interval_interrupt sets pb copy_pos = chase_pos + PB_SAFETY_OFFSET(16) - UFRAMES_PER_URB*2 (1024). That assumes playback transmits exactly 1024 microframes behind capture, because capture_complete_urb submits pb URBs 0 and 1 together on the second capture completion. The resulting write lead is only 16 microframes (2 ms) ahead of transmission, so any ASAP start slop in the host controller lands directly as a wrong lead. The proc knob pb_adj exists only to shift copy_pos by hand, which suggests the author saw this vary. The fix is plausible in Linux: xHCI honours an explicit urb->start_frame when URB_ISO_ASAP is clear, and it writes back the scheduled start_frame on submission, so either reading it back or scheduling explicitly works with usb_get_current_frame_number(). Caveat: once explicit scheduling is used, a missed start frame returns -EXDEV/-EFBIG and must be handled (ties to G5). I did not verify the Mac-side constants 0x14/0x140 myself.

**verify-mac (refuted: false), verbatim:**

> CONFIRMED on the Mac. In the usb_io_stream::Start block, after IOUSBHostInterface::GetFrameNumber the code stores 'add x8,x8,#0x14 ... mov w9,#0x13; csel' as the first IN frame (+0x177a60), meaning current+20, or a literal 19 if the call fails. It stores the first OUT frame as IN+0x140 (+0x177a68). Frame numbers are 1 ms frames: one request holds 160 microframes, which is 20 frames. So OUT starts 320 ms after IN, not '0x140 microframes'. SubmitNextInRequest and SubmitNextOutRequest pass the stored frame number as x3 to IOUSBHostPipe::IsochIO and then add 0x14, so scheduling is explicit, never as soon as possible. I did not re-read the chunk timestamp fields used by the HAL. CONFIRMED on Linux: URB_ISO_ASAP (motu-usb-audio.c:698), start_frame is never read for stream URBs, the write position is chase+16-1024 (:416-427), the pb_adj knob exists (:428-433), and two playback URBs are submitted at the second capture completion (:519-523). One correction to the harm. Because playback URB 0 is submitted after capture URB 2 has started, the playback start delay d is at least 0, so the write lead is 16+d microframes and never negative. The first start therefore causes a fixed, unknown offset of d×6 samples between capture and playback, not early or stale audio. The real exposure is later in the run: if a capture completion runs late, the as-soon-as-possible resubmit (:527) starts playback at a later frame, which permanently shifts the playback-to-capture offset in the middle of a stream. Explicit frame numbers stop that, as on the Mac.

*Editor: verify-mac corrects the harm (the write lead is 16+d microframes, never negative; the exposure is a mid-stream shift after a late capture completion) and makes the unit of 0x140 explicit (1 ms frames, 320 ms). See README, Corrections C1 and C7.*

## G4. Mac zeroes every OUT buffer before it is submitted; snd-usb-motu leaves old audio in the playback ring, so a late write replays audio from 64 ms earlier

- **Status: survived** (verify-linux refuted: false; verify-mac refuted: false)
- **Area:** 4 Isochronous framing / 7 Errors
- **Confidence (comparer):** high

**Mac behaviour.** Before each OUT submit the dext clears the request's data area, so a microframe the HAL did not write in time goes out as silence.

**Mac evidence.** CONFIRMED (dext map): memset(block+0x508, 0, 0x177000) before each OUT submit in the SubmitNextOutRequest path.

**Linux behaviour.** The playback ring is 512 microframes (64 ms) of one coherent buffer shared by the four URBs and is never cleared after use. Data is written only by copy_period_to_usb or mute_period_to_usb from the period interrupt. If the interrupt is late, skipped (spin_trylock contention) or not yet synced, the URB re-sends whatever that slot held 64 ms earlier.

**Linux evidence.** motu-usb-audio.c:687-699 and :710-715 (one shared 512-slot buffer); :290-325 (copy, no clear); :278 (zeroing only in the mute path); :436-452 (trylock contention skips the period); :401 (early return before sync). CONFIRMED.

**Symptom link.** Long-run reliability: audible 64 ms repeats or glitches under scheduling load instead of a clean dropout. Not S1-S4 directly.

**Proposed fix.** Clear (or mute) each playback URB's slots in capture_complete_urb before resubmitting it, after the period handler has had its chance to write. Or clear slots once transmitted, so a missed write yields silence, as the Mac does.

**verify-linux (refuted: false), verbatim:**

> The gap holds on Linux, but two parts of its evidence and fix are wrong. CONFIRMED: playback slot memory is never cleared after transmission. playback_complete_urb is empty, and slots are written only by copy_period_to_usb or mute_period_to_usb from the interval interrupt. Since bufs[] aliases every 512 slots (one transfer_buffer per stream, offset = max_packet_size*f independent of i, in alloc_stream_urbs), an unwritten slot re-sends audio from 64 ms earlier. The write lead is only 2 ms (see G3), so a late interrupt, or one lost to a non-zero status in interrupt_complete_urb, leaves the first slots stale. A lost interrupt also leaves copy_pos one period behind for good. Correction 1: lock contention does not skip the period. In handle_interval_interrupt, a failed spin_trylock falls to the else branch, which calls mute_period_to_usb and writes zeros. The real triggers are a late or missed interrupt, and the early return during capture startup sync. Correction 2: the first proposed fix is wrong. capture_complete_urb resubmits the pb URB whose slots the period handler has just filled, so clearing them there erases valid audio. The second option, clearing once transmitted in playback_complete_urb, breaks under the 512-slot aliasing, because the next in-flight URB shares that memory. So clear-after-transmit needs G8's per-URB buffers first; with those in place it is sound.

**verify-mac (refuted: false), verbatim:**

> CONFIRMED on the Mac. usb_io_stream::SubmitNextOutRequest calls _bzero(block+0x508, 0x177000) ('add x0,x8,#0x508; mov w1,#0x7000; movk #0x17,lsl16; bl _bzero') before it fills the frame list and calls IsochIO. OUT is scheduled 320 ms ahead (see G3), so the HAL writes into an already-cleared request, and a late write goes out as silence. Linux is mostly right but has one factual error. On trylock contention the driver does not skip the period: the else branch calls mute_period_to_usb, which zeroes it (motu-usb-audio.c:443-452, memset at :278). The real route to stale data is a late, lost or skipped period interrupt (:466-472). copy_pos then lags, and those slots go out with whatever the shared 512-slot buffer held 64 ms earlier (:687-699, 710-715; copy_period_to_usb :290-325 never clears). The proposed fix as written is wrong. Clearing a playback URB's slots in capture_complete_urb before it is resubmitted would wipe the first 16+d microframes the period handler has already written there. Clearing in playback completion would hit memory that the next URB, sharing the same buffer, is already using. A clean fix needs per-URB buffers (G8), or zeroing a span just behind the write position.

*Editor: both verifiers say trylock contention mutes the period rather than skipping it, and that the first proposed fix is wrong. See README, Corrections C8 and C9.*

## G5. Mac derives timing and a watchdog from USB frames; snd-usb-motu depends solely on the device's period interrupt and has no watchdog

- **Status: survived** (verify-linux refuted: false; verify-mac refuted: false)
- **Area:** 5 Rate matching / 7 Errors and recovery
- **Confidence (comparer):** high

**Mac behaviour.** The dext keeps a USB-frame to host-time snapshot (period 1,000,000 ns per frame, drift logging, up to 11 retries). The HAL builds zero timestamps from chunk times (UpdateZeroTimes, FilterZHT with a ±100 µs gate and a 10-entry average). A 5 ms chase timer keeps reading input whenever IO has not run for 200 ms, and start fails cleanly when no output timestamp appears within 200 ms ("TIMEOUT STARTING OUTPUT"). When CoreAudio falls behind it resynchronises ("CoreAudio fell behind, resynchronizing"), and MaybeStopAudio repairs run-state mismatches.

**Mac evidence.** CONFIRMED in SA_DeviceUSB::StartAudio (usleep 0x7d loop, 0xBEBC200 ns limit, AddTimer(...,5,1)), SA_Device::FilterZHT (0x186A0), WriteOutputData, MaybeStopAudio; dext snapshot constants 0xF4240 and 0x2dc6c0.

**Linux behaviour.** The ALSA position advances only from vendor interval interrupts on interface 0. The interrupt frame field is ignored. URB statuses and every usb_submit_urb result are unchecked. A single lost capture URB silently stops its playback partner. Nothing detects that interrupts or completions have stopped, so the stream just stalls, and ALSA sees only a timeout.

**Linux evidence.** motu-usb-audio.c:466-477 (interrupt handling, unconditional resubmit), :394 and :471 (period from interrupt), :514 and :519-527 (unchecked submits), :480-486 (empty playback completion). CONFIRMED.

**Symptom link.** Long-run reliability and S4 recovery. After a firmware freeze Linux has no detection or restart, while the Mac at least fails or resyncs cleanly. Neither side restarts the stream inside the kernel driver; the Mac's resync lives in the HAL.

**Proposed fix.** Check usb_submit_urb results and URB status. Add a completion-driven liveness check (no capture completion within N ms, or no interrupt) that reports an xrun or disconnects the PCM so ALSA or PipeWire can restart. Ideally take period timing from capture completions and frame numbers rather than the vendor interrupt.

**verify-linux (refuted: false), verbatim:**

> CONFIRMED on Linux. The ALSA position (inc_frame_position/hw_ptr) advances only in copy_period_from_usb and copy_period_to_usb, which run from handle_interval_interrupt. interrupt_complete_urb ignores motu_interrupt_msg.frame and resubmits unconditionally. Every usb_submit_urb result goes unchecked: in capture_complete_urb, start_streaming_endpoints and start_interrupt_urbs. Per-packet capture errors only log "rec urb error" and substitute the nominal length. playback_complete_urb is empty. If a capture resubmit fails, the round-robin pb resubmission in capture_complete_urb (indexed by urb_idx) slows down and playback starves. Nothing calls snd_pcm_stop_xrun or detects stalled completions, so ALSA only notices through its own period wait timeout (the PCM is BATCH with no other pointer source). The fix is plausible and idiomatic: check submit returns and urb->status; on a stall (missed completions or interrupts beyond N ms, tracked from completions themselves rather than a free-running timer) call snd_pcm_stop_xrun on the running substreams so PipeWire restarts. Taking period timing from completions is a bigger redesign (see G2).

**verify-mac (refuted: false), verbatim:**

> CONFIRMED on the Mac. SA_DeviceUSB::StartAudio calls WorkLoop::AddTimer(...,5,...), runs a wait loop of mach_absolute_time and usleep(0x7d), and logs 'TIMEOUT STARTING OUTPUT' and 'INPUT ERROR STARTING, CHASE FAIL'. It calls UpdateZeroTimes, FilterZHT and ChaseInput. SA_Device::FilterZHT uses the constant 0x186A0 (100000). The string 'CoreAudio fell behind, resynchronizing' is present. usb_io_stream::DoGetFrameSnapshot compares a retry count with 0xb. I did not decode the exact 200 ms limit: the limit register comes from 0x118EDC0 plus a value, in mach ticks. CONFIRMED on Linux: the PCM position advances only in handle_interval_interrupt (motu-usb-audio.c:394, 466-472). motu_interrupt_msg.frame (:121) is never read. No usb_submit_urb result is checked (:477, 514, 521-527, 605, 792). capture_complete_urb checks the status of each packet but not urb->status (:497-514). Playback URBs are resubmitted only from capture completions (:519-527), so one capture completion that fails to resubmit stops both directions. Nothing watches for liveness. The S4 recovery point stands: neither side restarts the stream in the kernel, but the Mac HAL times out or resynchronises cleanly, while Linux stalls until ALSA's own timeout.

*Editor: the verifiers differ on what one failed capture resubmit does (verify-mac: both directions stop; verify-linux: playback slows and starves). See README, Correction C14.*

## G6. Mac owns the whole device exclusively and evicts the class driver; snd-usb-motu can lose a bind race with snd-usb-audio

- **Status: survived** (verify-linux refuted: false; verify-mac refuted: false)
- **Area:** 1 Bring-up
- **Confidence (comparer):** medium

**Mac behaviour.** The dext opens the whole device and calls SetConfiguration. The re-enumerator retries every 1 s and forces a re-enumeration whenever com_motu_driver_proaudio2_service is missing and Apple's class driver holds the device.

**Mac evidence.** CONFIRMED: usb_provider::initPriv (Open plus the SetConfiguration log text). CONFIRMED: re-enumerator Server::armTimer lambda (VID 0x7fd, PID 0x5, a 33-byte name compare, USBDeviceReEnumerate, 1000 ms re-arm).

**Linux behaviour.** It matches 07fd:0005 per interface with no class filter and does work only on interface 0. It claims 1 and 2 with usb_driver_claim_interface, and the result is not acted on beyond failing probe. Nothing blacklists snd-usb-audio (the build script just rmmods it). If interface 1 or 2 probes or binds to snd-usb-audio first, the claim fails with -EBUSY. The probe error path does not release what it took.

**Linux evidence.** motu-usb-audio.c:1281-1284 (match), :1163 and :1246 (only ifnum 0 does work), :1178-1194 (claims), :1249-1252 (error path leaks). CONFIRMED. Probe-order risk INFERRED.

**Symptom link.** Reliability at boot and replug. A host that loads snd-usb-audio (as the Talos node did) gives a half-bound device that brings S2 and S3 back.

**Proposed fix.** Ship a modprobe blacklist or a device-specific quirk telling snd-usb-audio to ignore 07fd:0005 (or match by vendor interface class only). Make probe robust to interface order, and release claimed interfaces and buffers on failure.

**verify-linux (refuted: false), verbatim:**

> CONFIRMED. motu_usb_ids matches only USB_DEVICE(0x07fd,0x0005) with no interface-class filter. motu_usb_audio_probe does work only when ifnum==0, and claims interfaces 1 and 2 with usb_driver_claim_interface. snd-usb-audio matches by audio interface class, and the USB core tries drivers in registration order, not by match specificity. So with both modules loaded, whichever probes interface 0 first takes the device, and if snd-usb-audio already holds 1 or 2 the claim fails with -EBUSY (probe-order risk INFERRED). The error path (error_free: snd_card_free) never releases interfaces 1/2 already claimed, which then keep a dangling priv as intfdata. It also never frees interrupt URBs or coherent buffers allocated before the failure, because snd_card_free does not touch them: a leak, plus a stale claim until replug. The fix is clear and standard in Linux: blacklist snd-usb-audio for this VID:PID (modprobe.d, or a quirk entry with QUIRK_IGNORE_INTERFACE in snd-usb-audio's quirks table), release claimed interfaces (usb_driver_release_interface) and free URBs on probe failure. I did not verify the Mac re-enumerator details.

**verify-mac (refuted: false), verbatim:**

> CONFIRMED on the Mac. The dext has the string 'ivars->fUSBDevice->SetConfiguration(configDesc.get()->bConfigurationValue, false)' and imports IOUSBHostDevice::SetConfiguration. The re-enumerator's Server::armTimer arms a 1000 ms timer (mov w1,#0x3e8), compares with 0x5 (the product id), logs 'found a MOTU device using Apple class driver', and calls USBDeviceReEnumerate. CONFIRMED on Linux: USB_DEVICE(0x07fd,0x0005) with no class filter (motu-usb-audio.c:1281-1284), so the driver also binds interfaces 1-5 as empty no-op probes. Only ifnum 0 does any work (:1163). Interfaces 1 and 2 are claimed with usb_driver_claim_interface (:1186-1194). build_and_load.sh only rmmods snd-usb-audio; there is no blacklist. The error path calls snd_card_free, which frees priv (the card's private data) without usb_driver_release_interface. Interfaces already claimed then keep a dangling priv as interface data, a use-after-free risk on disconnect, which is worse than a leak. The probe-order race is INFERRED: the USB core normally probes interfaces in descriptor order, so the realistic risk is snd-usb-audio binding first (the Talos node loads it), not a self-race. The symptom link (S2 and S3 return under snd-usb-audio) is plausible.

*Editor: both verifiers say the probe error path leaves a dangling priv as interface data (verify-mac: a use-after-free risk), and verify-mac narrows the probe-order risk to snd-usb-audio binding first. See README, Corrections C10 and C11.*

## G7. Mac sets the sample rate and clock explicitly through the device datastore and waits for reconfiguration; snd-usb-motu never sets or checks the clock

- **Status: survived** (verify-linux refuted: false; verify-mac refuted: false)
- **Area:** 2 Clock and sample rate
- **Confidence (comparer):** medium

**Mac behaviour.** SetSampleRate posts avb/<uid>/cfg/0/current_sampling_rate. After a config change it stops audio, calls ForceStopAudio and waits up to 100 × 100 ms. clockLocked and dmaEnabled from the datastore are tracked. USB StartAudio itself does not wait for lock.

**Mac evidence.** CONFIRMED: SA_Device::SetSampleRate (DataStoreService::Post), SA_Device::PerformConfigChange (cmp w22,#0x64; usleep 0x186a0), ProcessDataStoreChanges (clockLocked, dmaEnabled -> fClockLocked).

**Linux behaviour.** There is no rate or clock request; the rate is fixed at 48000 at compile time. The device must already be locked at 48 kHz (set over Ethernet) before connect. Clock-validity interrupts are parsed and discarded, and a device at another rate is streamed blindly.

**Linux evidence.** motu-usb-audio.c:19 (SAMPLE_RATE), :134-151 (one rate), :456-464 (status interrupt ignored); readme:27-30.

**Symptom link.** S2 and S3. snd-usb-motu avoids both by never touching the UAC clock entities, which matches the Mac. But it gives no check that the device really runs at 48 kHz, and no reaction to clock loss.

**Proposed fix.** At start, read the device's datastore rate and lock state (the same keys, through the vendor bulk command interface or by documenting the web API) and refuse or warn when it is not 48 kHz and locked. Act on clock-change notifications by stopping the stream with an error instead of ignoring them.

**verify-linux (refuted: false), verbatim:**

> CONFIRMED on Linux. SAMPLE_RATE is a compile-time 48000, and snd_motu_hw has rate_min = rate_max = SAMPLE_RATE. There is no rate or clock request anywhere. handle_status_interrupt is an empty body (its dev_info is commented out), so class status and clock-change interrupts are discarded. The readme says outright that it "Does not handle clock lock loss" and that the device must be locked at the rate before connecting. So a device at another rate, or one that loses lock, is streamed blindly. The S2/S3 reasoning is fair: snd-usb-motu never touches the UAC clock entities. The proposed fix is weaker than it needs to be. The datastore over the vendor bulk interface is an undocumented protocol. A simpler route that works in a kernel driver: act on the status interrupt, since class_interrupt_msg already decodes cs/id, and on a clock-validity or sampling-frequency change call snd_pcm_stop_xrun or stop the streams. At start, optionally read clock validity and rate with a standard UAC2 GET_CUR on the clock source, which is read-only and is the very check that produced S2's message under snd-usb-audio. I did not verify the Mac SetSampleRate/PerformConfigChange details.

**verify-mac (refuted: false), verbatim:**

> CONFIRMED on the Mac. SA_Device::SetSampleRate calls DataStoreService::Post. ProcessDataStoreChanges references 'cfg/0/current_sampling_rate', 'fClockLocked %s', 'fDMALocked %s' and 'fConfiguredRate %d'. SA_Device::PerformConfigChange calls MaybeStopAudio, then loops with 'cmp w22,#0x64' around usleep(0x186a0), which is up to 100 × 100 ms. CONFIRMED on Linux: SAMPLE_RATE is fixed (motu-usb-audio.c:19), the hardware advertises one rate (:134-151), handle_status_interrupt is empty (:456-464), and readme.md says the device must already be locked at 48 kHz before connecting. Avoiding the UAC clock entities, and so S2 and S3, matches the Mac. The gap is the missing rate and lock check and the missing reaction to clock loss. The fix needs the datastore transport: the Mac reaches it through the dext's bulk command interface 0xFF/4/1 (SendBusCommand, the HTTP proxy) or over the network. That transport is INFERRED for Linux use and not yet specified.

*Editor: verify-linux proposes a different fix (act on the status interrupt; optionally a read-only UAC2 GET_CUR); verify-mac says the datastore transport for Linux is not yet specified. See README, Correction C13.*

## G8. Mac's ring is 400 ms deep with independent buffers; snd-usb-motu's ring aliases every 64 ms

- **Status: survived** (verify-linux refuted: false; verify-mac refuted: false)
- **Area:** 4 Isochronous framing
- **Confidence (comparer):** medium

**Mac behaviour.** 20 requests × 160 microframes, each with its own 0x177000-byte area (400 ms, kIsocRingBufferDuration 0x17D78400 ns). The shred pattern is re-applied only after 160 frames of a chunk have been consumed.

**Mac evidence.** CONFIRMED: dext ring index wraps at 0x13, request data size 0x177000; HAL ChaseTimerProc constant 0x17D78400; shredPoll threshold 0xA0.

**Linux behaviour.** 4 URBs × 512 packets index 2048 slots, but all URBs of a direction share one 512-slot buffer, so memory aliases every 512 microframes (64 ms) while lengths are tracked per 2048 slots. Shred correctness depends on consumed slots being re-shredded before the device writes them again 64 ms later.

**Linux evidence.** motu-usb-audio.c:21-24, :644, :676, :687-699, :710-715. CONFIRMED (the map's reading).

**Symptom link.** Long-run reliability: with large periods or a late handler, stale or mis-detected capture (the shred count is wrong) and the replay described in G4. Not S1.

**Proposed fix.** Give each URB its own buffer (or index the coherent buffer by the full 2048-slot space), so the ring depth matches the length bookkeeping.

**verify-linux (refuted: false), verbatim:**

> CONFIRMED, though the aliasing looks deliberate and the risk is a timing margin, not a logic bug. alloc_stream_urbs allocates one coherent transfer_buffer per stream (only while transfer_buffer is NULL) and maps bufs[i*512+f].data = transfer_buffer + max_packet_size*f for every URB i. All four URBs of a direction therefore share 512 slots of memory, while lengths and positions run over 2048 indices (TOTAL_UFRAMES). start_streaming_endpoints shreds only the first 512 slots, which fits a deliberate design. The bookkeeping is consistent: iso_frame_desc lengths are per URB, and shred is re-applied on consumption in copy_period_from_usb and sync_period_from_usb. With the largest period (2048 frames, about 42.7 ms) plus REC_SAFETY_OFFSET, capture consumption has about 20 ms of slack before the device overwrites a slot; beyond that, the shred count and data go wrong. On the playback side, the aliasing is what blocks a clean clear-after-transmit fix for G4. Per-URB buffers (4 x 512 x max_packet_size, about 1.2 MB per direction at 24ch x 3 B x 7 frames per microframe, roughly 504-byte packets, small) are a straightforward and plausible fix. Severity is lower than G3 to G6.

**verify-mac (refuted: false), verbatim:**

> CONFIRMED on the Mac. Each request has its own data block, 0x177000 bytes, cleared with _bzero in SubmitNextOutRequest and copied with memcpy of 0x177000 in SubmitNextInRequest. The ring index is compared with 0x13 and 0x14 in both submit paths, giving 20 requests × 20 frames = 400 ms. SubmitNextInRequest calls shredPoll and contains 'mov w9,#0xa0' (160). I did not re-read the HAL's 0x17D78400 constant. CONFIRMED on Linux: one coherent buffer of max_packet_size × 512 per direction is shared by all 4 URBs. bufs[i*512+f].data points into the same 512 slots for every i (motu-usb-audio.c:676-715), while lengths and positions run over 2048 slots (:21-24). Memory therefore repeats every 64 ms. The impact is narrower than stated. Capture is safe while the handler reads within about 64 ms of arrival, and the largest period is 2048 frames (42.7 ms) plus a 16-microframe offset. That leaves roughly 20 ms of margin with large periods, so a late handler corrupts capture and its shred count. On playback this aliasing is what makes a missed write replay 64 ms-old audio (G4), and it is also why G4 cannot be fixed cleanly without per-URB buffers. Keep this together with G4.

*Editor: both verifiers narrow the impact to a timing margin of about 20 ms at the largest period. See README, Correction C12.*

## Matches (verbatim from compare-gaps.json)

- Implicit feedback: both copy each capture microframe's actual or complete length into the matching playback microframe (dext SubmitNextOutRequest; motu-usb-audio.c:497-511).
- Both detect capture fill by content with the same shred pattern (byte i = ~i) and count whole 72-byte frames (HAL ChaseInput / dext shred; motu-usb-audio.c:198-266).
- Sample format S24_3LE, 24 channels, 72-byte frames, no per-packet header; channel 0 is aligned only by whole-frame packets, and neither side checks or repairs a rotation.
- Neither sends a UAC2 clock-source or sample-rate class request, so neither reads the 'clock source not valid' state behind S2 and S3.
- Start order: alt settings first, capture submitted first, playback primed only after capture completions (the dext discards the first IN; Linux PRIME state); both run capture and playback together.
- OUT completion status is ignored on both sides (empty dext handler; motu-usb-audio.c:480-486).
- Stop: abort URBs, then alt 0 on both interfaces; neither sends a device command on stop beyond Linux's interval 0.
- Neither kernel-level driver restarts streaming itself on errors; recovery is left to the layer above (the HAL on Mac).
- Pure feature differences (MIDI, other rates, mixer/gain, bus-stall hint, sleep/wake): not reliability gaps for this 48 kHz, 24-channel use, apart from no suspend/resume on Linux.

*Editor: the second match says the HAL counts "whole 72-byte frames". The HAL map says `ChaseInput` compares each frame's first 24 bytes with the template ([H4.5](../1-observations/mac-hal-plugin.md#4-isochronous-framing)); Linux compares whole 72-byte frames ([L4.5](../1-observations/linux-snd-usb-motu.md#4-isochronous-framing)). Neither verifier checked the matches.*

## Unresolved (verbatim from compare-gaps.json)

- Whether the 24Ao's interfaces 4 and 5 (alt 1, sharing endpoints 0x09 and 0x87) are the vendor isochronous interface class 0xFF/0x04/0x02, and whether the 0xFF/0x04/0x01 bulk command interface exists: needs lsusb -v.
- What posting host/mode=USB2 does on the 24Ao (re-enumeration, new descriptors, a different clock path) and whether the mode persists across power cycles: needs a USB capture on the Mac or a hardware test.
- Whether streaming in vendor mode avoids S1's rotation by 8 and S4's freezes: hardware test.
- Whether the vendor request 0x40/0x01 (interval) is used by any MOTU host driver, and whether it contributes to firmware freezes: a USB capture of the Mac driver would show it absent or present at the bus level.
- The capture and playback wMaxPacketSize at alt 1 (the 504-byte shred overrun risk on Linux) and whether the device ever sends a packet that is not a multiple of 72 bytes.
- The bulk command opcodes the HAL uses for datastore get and set (SendBusCommandSync cmd byte and the 0x81 HTTP-proxy framing), needed to set host/mode or read the rate from Linux.
- The real ISO_ASAP start latency on the Talos xHCI against snd-usb-motu's assumed 1024-microframe playback lag.

## Update from fourier's descriptors (2026-11-01, added after the run)

COMPARISON, by the orchestrating session after reading the MOTU's USB descriptors on fourier
(`../hardware/observations-fourier.md`; inferences HI1 to HI6 in `../2-inferences/hardware.md`). The
verifiers above did not see this.

- **G1: the descriptor question is answered.** verify-linux-side refuted G1 partly because "nobody has
  shown that the 24Ao has a 0xFF/0x04/0x02 interface at all". It has two: interfaces 4 and 5 are
  ff/04/02, with alt 1 on endpoints 0x09 and 0x87, and interface 3 is the ff/04/01 bulk command
  interface. They exist in the device's current (class-compliant) mode. So MOTU's Mac driver, which
  opens ff/04/02 first, streams on interfaces 4 and 5 on this device, while snd-usb-motu streams on
  the class-compliant interfaces 1 and 2. The remaining objections still stand: S1 and S4 were seen
  under snd-usb-audio, not under snd-usb-motu, and no link from the class-compliant path to the
  rotation is shown.
- **New obstacle for a vendor-mode Linux driver:** Linux's USB core drops the vendor interfaces'
  endpoints as duplicates (HI3). Taking MOTU's path on Linux needs a kernel change, not only a
  driver.
- **The vendor packet format is unknown** (900-byte packets are not a whole number of 72-byte frames,
  HI4), so the Mac dext's isochronous handling has to be read for the frame layout before any Linux
  code could use those interfaces.
