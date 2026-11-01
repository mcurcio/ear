# MOTU USB dext (07fd:0005): what it does on the USB bus

Everything below comes from static reading of the x86_64 slice. Disassembly was done with `otool -tV | c++filt`, which marks string literals. Both slices carry the same symbols. My working copies are in `/Volumes/Dock/motu-mac-driver/scratch/`. Two kinds of evidence carry most of the claims:
- the driver logs the source text of its own failed calls and assertions, so those strings quote MOTU's code;
- the instruction constants quoted below.

**The main finding is that the dext is only a transport.** It never sends a USB control request. `IOUSBHostDevice::DeviceRequest` is not among its imports (`nm -u` lists only Open, Close, SetConfiguration, CopyInterface, the interface iterators, Interface Open/Close/SelectAlternateSetting/GetFrameNumber/CopyPipe, and Pipe AsyncIO/IsochIO/Abort/GetDescriptors). CONFIRMED. Clock source, sample rate, lock and channel count are all set from user space: the HAL plug-in and the MOTUProAudioHTTPServer send their own commands, which the dext passes down a vendor bulk pipe.

## 1. Bring-up

- **Whole-device claim** (CONFIRMED). `usb_provider::initPriv` opens the whole device and calls `SetConfiguration(configDesc->bConfigurationValue, false)` (the call text is in the log string). Which configuration descriptor it picks goes through a virtual call I did not resolve.
- **No re-enumeration or reset** (CONFIRMED). There is no reset or re-enumerate import. Any mode switch belongs to the "reenumerator" daemon, which I did not read.
- **Command interface** (CONFIRMED). `initPriv` calls `FindInterface(class 0xFF, subclass 0x04, protocol 0x01)` (immediates `$0xff`, `$0x4`, `$0x1`), opens it with `SelectAlternateSetting(0)`, and finds a bulk IN and a bulk OUT pipe (`FindPipe(type 2, …)`).
  - It reads the IN pipe's endpoint descriptor ("max packet size (in) = %d") and keeps a pool of IN read requests of that size (a loop from 0x1228 to 0x1828 in 0x18-byte steps, which suggests 64 requests; INFERRED).
  - It keeps one OUT data request of 4096 bytes and one 4-byte "terminator" request.
  - The zero-length-packet mask `fTerminateOutCmdMask` is 0x3FF when a descriptor field read as a u16 is at least 0x201, otherwise 0x1FF (`cmpw $0x201; setae; shl 9; or 0x1ff`). The field is probably bcdUSB (INFERRED).
- **MIDI interface** (CONFIRMED). `FindInterface(class 1, subclass 3, …)` ("Found MIDI interface"), alt 0, bulk pipes. Every MIDI request is 4 bytes (`AsyncIO(…, 4, …)`), the USB-MIDI event-packet size.
- **Isochronous mode** (CONFIRMED). `fIsocMode` (ivar 0x1208) starts at 1, meaning UAC. If an interface with class 0xFF, subclass 0x04, protocol 0x02 exists, the mode becomes 2, or 3 when the device descriptor's bcdUSB is at least 0x300 (`cmpw $0x300,0x2(%rax); sbbl $-1`). The log line is "Setting fIsocMode to %d".
- **No commands at bring-up** (CONFIRMED). `usb_provider::HardwareInit` only sets flags (0x1874 and 0x2899). The commands the dext names itself (kCmdGetGuid, kCmdSetNotificationMasks, kCmdStart/StopMidiDMA) appear only in `tbt_provider` strings. GetGUID for USB probably comes from a string descriptor: `GetStringDescriptor`, `utf8_encodestr`, a lower-casing loop over 16 characters (INFERRED).

**The command protocol** runs on the bulk pipes, in `usb_provider::MaybeSendNextCommand`, `CmdReadComplete` and `CommandTimeoutTimerFired` (CONFIRMED).
- **Request layout:**
  - byte 0 is a token, `0x20 | (n+1 & 0x1F)` (`incb; andb $0x1f; orb $0x20`);
  - byte 1 is the command ID;
  - bytes 2–3 are the payload length (u16);
  - the payload follows, and the total is padded to 4 bytes (`add 7; and -4`), at most 0x1000.
- **Sending:** the OUT transfer has a 200 ms timeout (`$0xc8`). If the total is a multiple of the mask, a zero-length packet follows.
- **One command in flight:** commands wait in a queue and only one is outstanding. Its response timer is 2000 ms (`SetTimeout(2 * 1000)`, `$0x7d0`). On expiry the command completes with 0xE00002D6, the timeout code, and the next one is sent.
- **Response checks:**
  - the response's first u32 header must equal its trailing u32 ("Response header %08x != epilogue %08x");
  - the length sits in the header's high 16 bits and may be at most 4096;
  - the token must match the pending command's token ("Token doesn't match").
- **Device-initiated packets:** a set 0x40 bit marks an unsolicited write. An "HTTPProxyCommand" (constant 0x81) is forwarded to the user client. One more ID, 0x8F, gets special handling that I did not decode.
- **Where commands come from:** user clients only, through `userClient::SendBusCommandSync`. The client kinds are kNetIOUserHTTPServer, kNetIOUserAudioIO, kNetIOUserMidiIO and kNetIOUserTestClient.

## 2. Clock and sample rate

- **None of it is in the dext** (CONFIRMED). There are no control requests, no clock or rate strings, and no polling of clock validity. Rate, clock source and lock are handled through the bulk command channel by user space, most likely the AVB-style "HTTP datastore" passed through the 0x81 HTTP-proxy commands (INFERRED from names).
- **Stock-driver states the dext never reads.** The dext never reads the class-compliant (UAC2) clock-source entities that the Linux stock driver queries, so the state behind "clock source 1 is not valid" (S2) is never consulted at this layer. That is consistent with the Linux replacement's readme, which says the device must already be locked at the chosen rate before it is connected.

## 3. Starting and stopping streams

`StartAudioDMA` runs only when the HAL sets kAudioRunState through the audio user client (the string `result->fError = …getProvider()->StartAudioDMA()`). Its order (CONFIRMED):

1. `GetInterfacesAndIsocPipes(0xFF, 4, 2)` ("opened MOTU isoc interfaces and pipes"). If that fails, it falls back to `GetInterfacesAndIsocPipes(1, 2, 0x20)`: audio class, streaming subclass, protocol 0x20, the UAC2 version code ("opened UAC isoc interfaces and pipes").
   - For each matching interface, in iterator order, it calls Open and `SelectAlternateSetting(1)`, then finds an isochronous IN and OUT pipe.
   - There is no capture-before-playback rule at the alt-setting step: the order is the interface iteration order (INFERRED).
   - Nothing is sent to the device before or after the alt settings.
2. `usb_io_stream::Start`:
   - It takes a frame snapshot (USB frame number against host time) and reads the current frame number.
   - The first IN request is scheduled for current frame + 0x14 (20 frames). If `GetFrameNumber` fails it uses the constant 0x13.
   - The first OUT request is scheduled for first IN frame + 0x140 (320 frames).
   - It submits 4 IN requests. No OUT request is sent yet.
3. `SetMaxBusStall(5000)` (`$0x1388`), which only takes effect if the user client allowed it (flag 0x28A0). Then `fAudioDMAStarted` is set to 1.
4. **OUT priming** (CONFIRMED). Every completed IN request triggers one OUT request and one new IN request (`QueueIsocProc` calls `SubmitNextOutRequest(inReq)` then `SubmitNextInRequest()`). The first completed IN request is thrown away ("skipping first in request").
5. **Stopping** (`StopAudioDMA` and `usb_io_stream::Abort`, CONFIRMED):
   - lower the bus-stall limit;
   - stop the frame-snapshot thread and the event source;
   - abort the IN pipe synchronously (`Abort(1, 0xE00002EB)`), then the OUT pipe;
   - set the IN interface to alt 0 and close it ("Setting uac isoc in alternate to 0"), then the same for the OUT interface.
   - No command is sent on start or stop.

## 4. Isochronous framing

- **Requests** (CONFIRMED). A ring of 20 IN and 20 OUT requests (the index wraps at 0x13). Each request is 160 microframe entries (a loop from 0x20 to 0xF20 in 0x30-byte steps, two 24-byte `IOUSBIsochronousFrame` entries per step), which is 20 ms. Frame numbers are explicit and advance by 0x14 per request, never "as soon as possible".
- **IN size** (CONFIRMED). Each IN microframe's requestCount is `fMaxPacketSize` (ivar 0x177A58), set through `SetMaxPacketSize` from user space ("SetMaxPacketSize - %d"). The bytes per frame, channel count and sample format are therefore chosen by the HAL, not the dext.
- **Buffers** (CONFIRMED). Each request owns a 0x177000-byte data area (1,536,000 bytes, which is 160 × 9600) at block offset 0x508. The blocks sit in shared memory that the HAL maps (`GetBufferDesc`, `USBRequestBlock`, `kMaxRequestDataSizeBytes`).
- **Block header** (CONFIRMED). Each block's header carries:
  - the host time of its first frame, computed as (frame − snapshot frame) × period + snapshot time;
  - running byte totals at 0x177508 (start) and 0x177510 (end), kept for both IN and OUT.
- **Shred pattern** (CONFIRMED). The pattern is `~(k mod 256)` over the whole 0x177000 bytes: a loop seeded from `__const` bytes 00..0F, inverted with `pxor`. It is copied into an IN block before resubmission, and `shredPoll` only re-shreds a block once 0xA0 (160) frames of it have been consumed.
  - Linux `motu-usb-audio.c:198-211` uses the same per-byte pattern, repeated per 72-byte frame.
  - Linux uses it to find how far the device has written before the request completes (`motu-usb-audio.c:220-265`, `count_sample_frames` with `chase_pos`). The Mac HAL very likely does the same early read (INFERRED; the HAL was not read).
- **OUT data** is zeroed before each submit (`memset(block+0x508, 0, 0x177000)`). CONFIRMED.
- **Channel-0 alignment** (CONFIRMED). The dext adds no header, marker or per-packet check, and does nothing to establish, verify or recover channel alignment. Alignment holds only because every packet is a whole number of frames, which comes from the device's IN sizes copied to OUT (section 5). Sample format and channel order are not visible in the dext.
- **What this means for S1.** A rotation by 8 channels, if it does not come from the host, needs a device-side frame-start offset or a packet size that is not a whole multiple of the frame size. The binary cannot tell which (INFERRED).

## 5. Rate matching

- **Implicit feedback from capture only** (CONFIRMED in `SubmitNextOutRequest`). For each microframe i of the matching IN request, OUT requestCount[i] equals IN completeCount[i] (`movl -0x18(%rdx,%rdi)` copied into `-0x1c(%rsi,%rdi)`), and the running OUT byte total grows by the same amount.
- **No other mechanism** (CONFIRMED). There is no feedback endpoint (only isochronous IN and OUT pipes are looked up), no timer and no drift knob. Linux does the same: `motu-usb-audio.c:488-512` copies `actual_length` to the playback descriptor's length. Its `pb_adj` (`:428-432`) has no counterpart in the dext.
- **The 320 ms lead** (CONFIRMED). OUT frames sit 320 frames after the IN frames, so OUT packet sizes follow the capture pattern from 320 ms earlier, and OUT buffers are queued about 300 ms ahead.
- **Low latency must come from late writes** (INFERRED). Low latency then depends on the HAL writing samples into buffers that are already queued, just before the controller fetches them.
- **Clock mapping** (CONFIRMED structure, INFERRED purpose). The snapshot thread maps USB frames to host time. The starting period is 1,000,000 ns per frame (ivar 0x177A78 = `0xF4240`), "frame to host interpolation drifted by %llu usec" is logged, and the snapshot is retried within a 3,000,000-unit window (`$0x2dc6c0`) for up to 11 tries.

## 6. Interrupt and status endpoint

- **The USB path uses none** (CONFIRMED). No interrupt pipe is looked up and nothing parses interrupt data. The dext never sends Linux's vendor request 0x01 "interval" (`motu-usb-audio.c:535-547`) and never reads the 6-byte class interrupt messages (`:466-486`).
- **Device-to-host status** arrives only as unsolicited bulk command packets (0x40 flag, 0x81 forwarded to the user client).
- The counters gIntCount, gResponseIntCount and gStatusIntCount belong to the Thunderbolt provider.

## 7. Errors and recovery

- **IN completion** (CONFIRMED):
  - 0xE00002EB (aborted) returns without resubmitting.
  - 0, 0xE00002E7, 0xE00002ED and 0xE000400E are treated as data and queued.
  - Any other status is only logged ("IsocCompletionProc returned %08X") and not queued. Since IN and OUT are resubmitted only from that queue, the ring loses one slot each time (INFERRED from control flow).
  - "OH NO, bad index" guards the ring index.
- **OUT completion** (CONFIRMED). The handler is empty (push, pop, ret), so OUT status is never checked. Linux's `playback_complete_urb` is empty as well.
- **No stream restart** (CONFIRMED). There is no restart, underrun handling or overrun handling in the dext; recovery is left to the HAL stopping and starting DMA.
- **Commands** (CONFIRMED). A timeout gives 0xE00002D6 and the next command is sent. An oversized command gives 0xE00002C2. When offline, every queued command completes with 0xE00002D7 ("Completing command with kKernStatusOffline").
- **Unplug** (CONFIRMED). "Exiting quickly because device has disappeared". `HardwareStop` asserts that DMA is not running, then logs "Entering slave mode...".

## 8. Other things that look load-bearing

- **Exclusive ownership.** The dext opens the whole device, so no class driver coexists with it on macOS.
- **Bus-stall limit.** It requests a 5000 bus-stall limit while streaming, if allowed (`RequireMaxBusStall`).
- **Single queues.** All isochronous resubmission happens on one bus work queue, behind a gate (`with_gate_closed`), and commands are strictly one at a time.
- **Sleep and Wake.** The device's Sleep and Wake hooks pause and unpause; I did not decode them further.

## OPEN QUESTIONS

1. Does the 24Ao expose the vendor isochronous interface (class 0xFF, subclass 0x04, protocol 0x02)? That decides whether macOS streams through vendor mode (2) or UAC mode (1). Linux dmesg shows config 1 has at least interfaces 0–5. Answering it needs a full descriptor dump (`lsusb -v`).
2. Which configuration `SetConfiguration` selects (a virtual call I did not resolve).
3. `fMaxPacketSize`, the bytes per frame, the sample format, the channel order, and the rate or clock commands. All live in the HAL plug-in or the HTTP server, which I did not read. The next binary to read is `MOTUProAudioServerPlugin`.
4. The meaning of the 0x8F response handling and of the 0x40 flag beyond "unsolicited".
5. Whether the HAL reads IN data early through the shred pattern.
6. What the reenumerator daemon does, and whether it switches the device's configuration or mode.
7. The bus-stall value passed on stop. The stop call has no visible immediate; probably 0.

The S3 (ENXIO on first open after power-on) and S4 (firmware freezes) symptoms cannot be settled from the dext, since it issues no clock or rate operations.