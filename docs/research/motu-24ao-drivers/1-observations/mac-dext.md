# Observations: MOTU's DriverKit driver (dext)

Binary: `com.motu.driver.pro.audio.dext` (provenance in `../README.md`). Source: `../sources/map-mac-dext.md`,
which read the x86_64 slice with `otool -tV | c++filt`; verify-mac re-read parts of the arm64 slice.

Only facts read in code, symbols or strings are here. Interpretations are in `../2-inferences/mac.md`.
Tags: "(re-checked by verify-mac)" = a verifier read the same thing again; "(corrected by verify-mac)" = the
verifier's corrected fact replaces the map's; "(mapper only; not re-checked)" = a verifier said it did not check it.
Every other bullet is the mapper's reading (marked CONFIRMED in the map) and was not re-checked by a verifier.

## 0. Scope and interfaces

- **D0.1** Imports from the IOUSBHost API (`nm -u`): device Open, Close, SetConfiguration, CopyInterface, the
  interface iterators; interface Open, Close, SelectAlternateSetting, GetFrameNumber, CopyPipe; pipe AsyncIO,
  IsochIO, Abort, GetDescriptors. `IOUSBHostDevice::DeviceRequest` is not imported. (re-checked by verify-mac
  on the arm64 slice: no DeviceRequest or ControlRequest import.)
- **D0.2** No USB control request is sent anywhere in the dext (no import for one, D0.1).
- **D0.3** The x86_64 and arm64 slices carry the same symbols.

## 1. Bring-up

- **D1.1** `usb_provider::initPriv` opens the whole device and calls `SetConfiguration` with the log text
  `ivars->fUSBDevice->SetConfiguration(configDesc.get()->bConfigurationValue, false)` (verify-mac's quotation;
  the map abbreviates it as `SetConfiguration(configDesc->bConfigurationValue, false)`). (re-checked by
  verify-mac: string present, `IOUSBHostDevice::SetConfiguration` imported.) Which configuration descriptor is
  picked goes through a virtual call the mapper did not resolve.
- **D1.2** No reset or re-enumerate function is imported.
- **D1.3** Command interface: `initPriv` calls `FindInterface(class 0xFF, subclass 0x04, protocol 0x01)`
  (immediates `$0xff`, `$0x4`, `$0x1`), opens it with `SelectAlternateSetting(0)`, and finds a bulk IN and a
  bulk OUT pipe with `FindPipe(type 2, ...)`. (re-checked by verify-mac: every `FindPipe` in `initPriv` asks for
  type 2, bulk.)
- **D1.4** It reads the bulk IN pipe's endpoint descriptor (string "max packet size (in) = %d") and keeps a pool
  of IN read requests of that size; the pool is built in a loop from offset 0x1228 to 0x1828 in 0x18-byte steps.
- **D1.5** It keeps one bulk OUT data request of 4096 bytes and one 4-byte "terminator" request.
- **D1.6** `fTerminateOutCmdMask` is 0x3FF when a descriptor field read as a u16 is at least 0x201, otherwise
  0x1FF (`cmpw $0x201; setae; shl 9; or 0x1ff`).
- **D1.7** MIDI interface: `FindInterface(class 1, subclass 3, ...)` (string "Found MIDI interface"), alt 0,
  bulk pipes. Every MIDI request is 4 bytes (`AsyncIO(..., 4, ...)`).
- **D1.8** `fIsocMode` (ivar 0x1208) starts at 1. `initPriv` calls `FindInterface(0xff, 4, 2)`; if that
  interface exists, `fIsocMode` becomes 2, or 3 when the device descriptor's bcdUSB is at least 0x300
  (x86_64: `cmpw $0x300,0x2(%rax); sbbl $-1`; arm64: `cmp w8,#0x2ff; cinc`). Log string "Setting fIsocMode to
  %d". (re-checked by verify-mac.)
- **D1.9** `usb_provider::HardwareInit` only sets flags (offsets 0x1874 and 0x2899); it sends no command.
- **D1.10** The command names the dext carries (kCmdGetGuid, kCmdSetNotificationMasks, kCmdStart/StopMidiDMA)
  appear only in `tbt_provider` strings.
- **D1.11** The USB provider calls `GetStringDescriptor`, `utf8_encodestr` and a lower-casing loop over 16
  characters.

### Bulk command protocol (in `usb_provider::MaybeSendNextCommand`, `CmdReadComplete`, `CommandTimeoutTimerFired`)

- **D1.12** Request layout: byte 0 is a token `0x20 | (n+1 & 0x1F)` (`incb; andb $0x1f; orb $0x20`); byte 1
  is the command ID; bytes 2-3 are the payload length (u16); the payload follows; the total is padded to 4 bytes
  (`add 7; and -4`), at most 0x1000.
- **D1.13** The bulk OUT transfer has a 200 ms timeout (`$0xc8`). When the total is a multiple of the mask
  (D1.6), a zero-length packet follows.
- **D1.14** Commands wait in a queue; one is outstanding at a time. Its response timer is 2000 ms
  (`SetTimeout(2 * 1000)`, `$0x7d0`). On expiry the command completes with 0xE00002D6 (the mapper names it the
  timeout code) and the next command is sent.
- **D1.15** Response checks: the first u32 (header) must equal the trailing u32 (string "Response header %08x
  != epilogue %08x"); the length is in the header's high 16 bits and may be at most 4096; the token must match
  the pending command's token (string "Token doesn't match").
- **D1.16** A set 0x40 bit marks an unsolicited, device-initiated write. An "HTTPProxyCommand" (constant 0x81)
  is forwarded to the user client. ID 0x8F gets special handling the mapper did not decode.
- **D1.17** Commands come only from user clients, through `userClient::SendBusCommandSync`. Client kinds:
  kNetIOUserHTTPServer, kNetIOUserAudioIO, kNetIOUserMidiIO, kNetIOUserTestClient.

## 2. Clock and sample rate

- **D2.1** No control requests (D0.1), no clock or rate strings, and no polling of clock validity.
- **D2.2** The dext never reads the class-compliant (UAC2) clock-source entities.

## 3. Start and stop

- **D3.1** `StartAudioDMA` runs when the HAL sets kAudioRunState through the audio user client (string
  `result->fError = ...getProvider()->StartAudioDMA()`).
- **D3.2** `StartAudioDMA` first calls `GetInterfacesAndIsocPipes(0xFF, 4, 2)` (log "opened MOTU isoc interfaces
  and pipes"). When that returns false it calls `GetInterfacesAndIsocPipes(1, 2, 0x20)` (log "opened UAC isoc
  interfaces and pipes"); the mapper reads 1/2/0x20 as audio class, streaming subclass, UAC2 protocol.
  (re-checked by verify-mac: arm64 `mov w0,#0xff / w1,#4 / w2,#2`; the function compares interface descriptor
  bytes +5/+6/+7, class, subclass and protocol; fallback on `cbz w0`.)
- **D3.3** For each matching interface, in iterator order, it calls Open and `SelectAlternateSetting(1)`, then
  finds an isochronous IN and OUT pipe.
- **D3.4** Nothing is sent to the device before or after the alt-setting changes.
- **D3.5** `usb_io_stream::Start` takes a frame snapshot (USB frame number against host time) and reads the
  current frame number. The first IN request is scheduled at current frame + 0x14 (20); if `GetFrameNumber`
  fails it uses the constant 0x13 (19). The first OUT request is scheduled at first IN frame + 0x140 (320).
  These are 1 ms frames: OUT starts 320 ms after IN, not 0x140 microframes (corrected by verify-mac; arm64
  `add x8,x8,#0x14 ... mov w9,#0x13; csel`, stored at +0x177a60; OUT stored at +0x177a68).
- **D3.6** `usb_io_stream::Start` submits 4 IN requests and no OUT request.
- **D3.7** Then `SetMaxBusStall(5000)` (`$0x1388`), only if the user client allowed it (flag 0x28A0), then
  `fAudioDMAStarted` = 1. `SetMaxBusStall` is the host-side `IOService::RequireMaxBusStall`, not a device
  request (added by verify-mac).
- **D3.8** OUT priming: `QueueIsocProc` calls `SubmitNextOutRequest(inReq)` then `SubmitNextInRequest()` for each
  completed IN request. The first completed IN request is discarded (string "skipping first in request").
- **D3.9** Stop (`StopAudioDMA`, `usb_io_stream::Abort`): lower the bus-stall limit; stop the frame-snapshot
  thread and the event source; abort the IN pipe synchronously (`Abort(1, 0xE00002EB)`), then the OUT pipe; set
  the IN interface to alt 0 and close it (string "Setting uac isoc in alternate to 0"), then the same for OUT.
  (re-checked by verify-mac: `StopAudioDMA` calls Abort, `SelectAlternateSetting(0)` and Close.)
- **D3.10** No command is sent on start or stop. The `StartAudioDMA` block calls only
  `GetInterfacesAndIsocPipes`, `usb_io_stream::Start` and `SetMaxBusStall(5000)` (re-checked by verify-mac).
- **D3.11** The bus-stall value passed on stop has no visible immediate.

## 4. Isochronous framing

- **D4.1** A ring of 20 IN and 20 OUT requests (index wraps at 0x13; verify-mac: compared with 0x13 and 0x14 in
  both submit paths). Each request has 160 microframe entries (loop 0x20 to 0xF20 in 0x30-byte steps, two
  24-byte `IOUSBIsochronousFrame` entries per step), 20 ms.
- **D4.2** Frame numbers are explicit and advance by 0x14 per request; never "as soon as possible".
  (re-checked by verify-mac: `SubmitNextInRequest` and `SubmitNextOutRequest` pass the stored frame number as
  x3 to `IOUSBHostPipe::IsochIO`, then add 0x14.)
- **D4.3** Each IN microframe's requestCount is `fMaxPacketSize` (ivar 0x177A58), set through `SetMaxPacketSize`
  from user space (string "SetMaxPacketSize - %d").
- **D4.4** Each request owns a 0x177000-byte data area (1,536,000 bytes, 160 x 9600) at block offset 0x508.
  The blocks sit in shared memory the HAL maps (`GetBufferDesc`, `USBRequestBlock`, `kMaxRequestDataSizeBytes`).
- **D4.5** Each block's header carries the host time of its first frame, computed as (frame - snapshot frame)
  x period + snapshot time, and running byte totals at 0x177508 (start) and 0x177510 (end), for IN and OUT.
- **D4.6** Shred pattern: byte k = `~(k mod 256)` over the whole 0x177000 bytes, built by a loop seeded from
  `__const` bytes 00..0F and inverted with `pxor`. It is copied into an IN block before resubmission
  (verify-mac: `memcpy` of 0x177000 in `SubmitNextInRequest`). `shredPoll` re-shreds a block only once 0xA0
  (160) of its frames are consumed (verify-mac: `SubmitNextInRequest` calls `shredPoll` and has
  `mov w9,#0xa0`).
- **D4.7** OUT data is zeroed before each submit: `memset(block+0x508, 0, 0x177000)` (x86_64); arm64
  `add x0,x8,#0x508; mov w1,#0x7000; movk #0x17,lsl16; bl _bzero` in `SubmitNextOutRequest`, before it fills
  the frame list and calls IsochIO (re-checked by verify-mac).
- **D4.8** The dext adds no header, marker or per-packet check, and has no code that establishes, verifies or
  recovers channel alignment. Sample format and channel order are not visible in the dext.

## 5. Rate matching

- **D5.1** In `SubmitNextOutRequest`, for each microframe i of the matching IN request, OUT requestCount[i] =
  IN completeCount[i] (`movl -0x18(%rdx,%rdi)` copied into `-0x1c(%rsi,%rdi)`); the running OUT byte total grows
  by the same amount.
- **D5.2** Only isochronous IN and OUT pipes are looked up (no feedback endpoint). No timer and no drift
  setting in the rate path.
- **D5.3** A snapshot thread maps USB frames to host time. Starting period 1,000,000 ns per frame (ivar
  0x177A78 = `0xF4240`). String "frame to host interpolation drifted by %llu usec". The snapshot is retried
  within a 3,000,000-unit window (`$0x2dc6c0`) for up to 11 tries (verify-mac: `DoGetFrameSnapshot` compares a
  retry count with 0xb).

## 6. Interrupt and status

- **D6.1** No interrupt pipe is looked up and nothing parses interrupt data on the USB path. The
  `IOInterruptDispatchSource` import belongs to the Thunderbolt provider (added by verify-mac).
- **D6.2** Device-to-host status on USB arrives only as unsolicited bulk command packets (0x40 flag; 0x81
  forwarded to the user client, D1.16).
- **D6.3** The counters gIntCount, gResponseIntCount and gStatusIntCount belong to the Thunderbolt provider.

## 7. Errors and recovery

- **D7.1** IN completion: 0xE00002EB (aborted) returns without resubmitting. 0, 0xE00002E7, 0xE00002ED and
  0xE000400E are treated as data and queued. Any other status is only logged ("IsocCompletionProc returned
  %08X") and not queued. IN and OUT are resubmitted only from that queue.
- **D7.2** String "OH NO, bad index" guards the ring index.
- **D7.3** OUT completion: the handler is empty (push, pop, ret); OUT status is never checked.
- **D7.4** No stream restart, underrun handling or overrun handling in the dext.
- **D7.5** Commands: a timeout gives 0xE00002D6 and the next command is sent; an oversized command gives
  0xE00002C2; when offline every queued command completes with 0xE00002D7 (string "Completing command with
  kKernStatusOffline").
- **D7.6** Unplug: string "Exiting quickly because device has disappeared". `HardwareStop` asserts that DMA is
  not running, then logs "Entering slave mode...".

## 8. Other

- **D8.1** While streaming it requests a 5000 bus-stall limit, if allowed (`RequireMaxBusStall`; see D3.7).
- **D8.2** All isochronous resubmission happens on one bus work queue, behind a gate (`with_gate_closed`).
  Commands are strictly one at a time (D1.14).
- **D8.3** The device's Sleep and Wake hooks pause and unpause; the mapper did not decode them further.
