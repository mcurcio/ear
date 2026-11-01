# MOTU Mac driver map: HAL plug-in and re-enumerator (static analysis)

Binaries read (arm64 slices, disassembled in `/Volumes/Dock/motu-mac-driver/scratch/`: `plugin_arm64.s`, `reenum_arm64.s`, `*_strings.txt`, `startaudio.txt`):
- HAL plug-in: `.../HAL/MOTUProAudioServerPlugin.driver/.../MOTUProAudioServerPlugin`, built from `IntegratedDrivers/AVB/Mac/Driver/User/Plug-In/SA_DeviceUSB.cpp`.
- Re-enumerator: `.../com.motu.driver.proaudio.reenumerator.xpc/.../com.motu.driver.proaudio.reenumerator`.

**Scope finding.** Neither binary touches USB directly. They make no alt-setting, isochronous, interrupt or control-transfer calls; the plug-in imports no IOUSB API at all. All USB-level work (interfaces, endpoints, packet sizing, feedback, the interrupt endpoint) lives in the dext, which this brief did not cover: `Applications/MOTU Pro Audio System Extension.app/Contents/Library/SystemExtensions/com.motu.driver.pro.audio.dext.dext/com.motu.driver.pro.audio.dext`. What the plug-in shows is the dext's contract: what it asks for, and what timing and position data the dext publishes.

## 0. How the plug-in talks to the dext

All CONFIRMED from `USBServiceClient::*`.

- **Opening it.** `IOServiceOpen` on the matched service (`USBServiceClient::Open`). `RegisterForNotifications` calls `IOConnectCallAsyncStructMethod(selector 3, 8-byte input)` on a CFMachPort.
- **Notifications.** The callback hands (type, length, data) to `SA_Device::DSNotificationCallback`. The value 0xA5A5A5A5 marks an empty message. The callback re-registers after every message (`UserClientCallBackProc` ends with a branch to `RegisterForNotifications`).
- **External-method selectors**, all through `IOConnectCallStructMethod`. The vtable at 0x10003cd60 confirms the method order.

| Selector | Method | Payload |
|---|---|---|
| 0 | `SetProperty(key, value)` | 8-byte input |
| 1 | `GetProperty(key)` | 4-byte input |
| 2 | `SendBusCommandSync(cmd byte, payload)` | 4096-byte struct, payload ≤ 0xFFC; logs "SendBusCommand cmd %d returned err" |
| 4 | `ForceStopAudio` | none |
| 5 | `allowMaxBusStall(bool)` | 1-byte input |

- **Property keys seen.**
  - 2: audio run state. `SA_DeviceUSB::StartAudio` sets it to 1 (`mov w1,#0x2; mov w2,#0x1`); `StopAudio` sets it to 0. `SA_Device::MaybeStopAudio` reads it back through `GetProperty` ("audio run state is kRun").
  - 9: maximum input bytes per microframe, set in `StartAudio` just before key 2.
  - 10 (0xA): "isocMode", read in `SA_Device::ProcessDataStoreChanges`.
  - 11 (0xB): set by `MaybeProcessPedalDS` (footswitch).
- **Shared memory.** `IOConnectMapMemory64` with memory type 1 (input ring, stored at +0x7cb0) and type 2 (output ring, stored at +0x7cb8). Both are mapped in `StartAudio` and unmapped in `StopAudio`. Type 0 is mapped by `GetTimestampStatusBuffer`, which the USB device path does not use (CONFIRMED: no call in `SA_DeviceUSB`).
- **Device configuration** (sample rate, host mode, footswitch) is not done with USB requests. It goes through the device's datastore (MOTU's web-API key/value tree, keys such as `avb/<uid>/cfg/0/current_sampling_rate`). `DataStoreService::Post` carries it over selector 2. How the dext moves those bytes to the device is not visible here.

## 1. Bring-up

### Re-enumerator (all CONFIRMED in the `Server::armTimer(bool)` lambda unless marked)

- **Launch.** launchd keeps it alive (plist: `RunAtLoad`, `KeepAlive`).
- **What it watches.** In `Server::Server()` it registers `IOServiceFirstPublish` and `IOServiceTerminate` notifications for `IOUserService` with `IOUserClass=com_motu_driver_proaudio_device` and bundle `com.motu.driver.pro.audio.dext`. On either event (`Server::PublishCallback` / `TerminateCallback`), and once at start-up, it posts a 1000 ms timer (`postTimed`, `mov w1,#0x3e8`).
- **When the timer fires:**
  1. It asks `SystemExtensionInstallChecker isInstalled` for the dext. If the dext is not installed it stops ("driver is not installed, bailing").
  2. It matches `IOUSBHostDevice` with `idVendor 0x7fd` / `idProduct 0x5` (`mov w9,#0x7fd; mov w8,#0x5`).
  3. It walks two levels of children looking for a name of 33 characters equal to `com_motu_driver_proaudio2_service` (`strlen` compared with 0x21, then a 33-byte compare).
  4. If that service is absent, it logs "found a MOTU device using Apple class driver". It then opens an `IOUSBDeviceInterface`, checks vendor and product again (vtable +0x68/+0x70), and calls `USBDeviceOpen` (+0x40).
  5. It then calls `USBDeviceReEnumerate` (vtable +0x128) whether the open succeeded or failed with `kIOReturnExclusiveAccess` (0xE00002C5). The options are `(open returned ExclusiveAccess) << 29`.
  6. Any other open failure increments a counter and re-arms the 1 s timer to retry.
- **What it does, in one line:** it detects the device bound to Apple's class driver instead of MOTU's dext and forces a USB re-enumeration so the dext can match. It sends no MOTU protocol to the device.
- INFERRED: bit 29 is IOKit's re-enumerate "release device" option. The SDK on disk has no USB headers to confirm the bit's name.

### Plug-in mode switch (CONFIRMED in `SA_Device::ProcessDataStoreChanges`)

When the datastore key `host/mode` changes (key compared as "host/mod"+'e'), the plug-in reads dext property 10 (isocMode). If `host/mode` equals "UAC":
- isocMode 2: it Posts `host/mode` = "USB2".
- isocMode 3: it Posts `host/mode` = "USB3".

So the Mac driver moves the device out of class-compliant (UAC) mode into MOTU's vendor mode. INFERRED: the device re-enumerates or reconfigures after this. The stock Linux driver leaves it in UAC mode, which is relevant to S1–S4.

### Linux snd-usb-motu, for comparison

- One vendor request: OUT | VENDOR | DEVICE, bRequest 0x01, wValue = interrupt interval in frames, wIndex 0, no data (motu-usb-audio.c:535-542).
- It does no host-mode switch.
- It matches only 07fd:0005 (motu-usb-audio.c:1282).

## 2. Clock and sample rate

- **How the rate is set** (CONFIRMED, `SA_Device::SetSampleRate`). Only 44100, 48000, 88200, 96000, 176400 and 192000 are accepted. The rate is applied by `DataStoreService::Post("avb/" + <service string> + "/cfg/0/current_sampling_rate", to_string(rate))`. No UAC clock request is involved.
- **Clock lock.** The datastore keys "clockLocked" and "dmaEnabled" set `fClockLocked` / `fDMALocked` and fire `Host_PropertiesChanged('goin')` (CONFIRMED in `ProcessDataStoreChanges`).
- **USB start does not wait for lock.** `SA_DeviceUSB::StartAudio` never reads `fClockLocked`; only `SA_DeviceTBolt::StartAudio` logs it (CONFIRMED by string cross-reference).
- INFERRED: lock waiting, if any, happens in the device or the dext.
- **After a rate change** (CONFIRMED, `SA_Device::PerformConfigChange`): `MaybeStopAudio`, then `ForceStopAudio` (selector 4), then up to 100 waits of 100 ms each (`cmp w22,#0x64`, `usleep(0x186a0)`), then a property-change notification.
- INFERRED: it waits for the device to report the new rate or configuration.

## 3. Stream start and stop

`SA_DeviceUSB::StartAudio` (CONFIRMED):
1. Asserts that `fActive.fInNumCh` and `fActive.fOutNumCh` are non-zero.
2. Maps the input ring (type 1) and the output ring (type 2).
3. Writes a "shred" pattern into the data area of each of the 20 input chunks: `memcpy` of 0x177000 bytes to chunk + 0x508, 20 times, from a template the constructor builds. Template byte i = ~(i mod 256): the constructor computes NOT(v) and 0xEF−v over 0..15 in 32-byte steps.
4. Zeroes a 16 MiB local float ring (`bzero 0x1010000`).
5. Computes:
   - `fBufferSyncOffsetSamples` = rate × 300 / 8000 (1800 at 48 kHz).
   - Frame size = channels × 3 bytes.
   - `fMaxIn_uFrameSizeBytes` = inCh × (7 if ≤48 kHz, 13 if ≤96 kHz, else 25) × 3, which is 504 at 24 ch / 48 kHz.
   - `fMaxSamplesPer_uFrame` = 7.
6. Marks the chunks: consumed count 0x9F for chunk 0 and 0xA0 for the others (one `stlr` per chunk).
7. `SetProperty(9, maxInBytes)`, then `SetProperty(2, 1)`. A failure logs "StartAudio failed!!!".
8. Adds a repeating WorkLoop timer with period 5 that runs `ChaseTimerProc` (`AddTimer(..., 5, 1)`). The unit is probably milliseconds (INFERRED).
9. Polls output chunk[index].timestamp every 125 µs (`usleep(0x7d)`) for up to 200 ms (0xBEBC200 ns). On timeout it logs "TIMEOUT STARTING OUTPUT" and stops.
10. When a timestamp appears, it computes the output frame count, seeds `FilterZHT`, and `mach_wait_until`s that timestamp. If no input was chased by then it logs "INPUT ERROR STARTING, CHASE FAIL" (it continues anyway). It then sets `fAudioStarted` and `EnableIO`.

The order of capture and playback endpoint start, and how the first output packets are primed, are decided in the dext.

`StopAudio` (CONFIRMED): `DisableIO`, remove the chase timer and wait for it to finish, clear the flags, `SetProperty(2, 0)`, release buffer 1, release buffer 2.

Linux comparison:
- Start: set the interrupt interval, alt 1 on interface 2 (record), then alt 1 on interface 1 (playback), then submit capture URBs (motu-usb-audio.c:599-605).
- Playback URBs go out only after the second capture completion (PRIME state, motu-usb-audio.c:520-522).
- Stop: interval 0, kill URBs, alt 0 (motu-usb-audio.c:626-629).

## 4. Isochronous framing, as the plug-in sees it

**Input ring** (type 1), CONFIRMED in `ChaseInput` and `StartAudio`:
- 20 chunks of 0x177538 bytes. The end bound is base + 0x1D52860, which is 20 × 0x177538.
- Each chunk holds 160 microframe slots (switch to the next chunk when offset + slotBytes ≥ 160 × slotBytes; 0xA0 compares). Each slot is `fMaxIn_uFrameSizeBytes` (504) long.
- 160 microframes × 125 µs = 20 ms per chunk. 20 chunks = 400 ms, which matches the static `kIsocRingBufferDuration` = 0x17D78400 ns (400 ms) in `ChaseTimerProc`.
- Chunk trailer fields: +0x177518 (plug-in's consume time), +0x177520 (dext fill time, INFERRED), +0x177528 (packets consumed, 0..160), +0x177530 (cached copy of +0x177520).

**How new input is found.** Not from a write index. For each frame in the slot (up to 7), the plug-in compares that frame's first 24 bytes with the shred template at the same offset. The first frame that still equals the template ends the packet. When a chunk is reached that is fully consumed and whose fill time is older than the consume time, the plug-in has caught up.

**Sample format in** (CONFIRMED by decoding the `tbl` masks at 0x100034ea0/eb0/ec0): packed 24-bit little-endian (S24_3LE). Bytes (0,1,2) become int32 bits 8..31, then `scvtf` and multiply by 2⁻³¹ (`movi v17, #0x30, lsl #24`).

**Sample format out** (CONFIRMED by tracing the masks at 0x100034ed0..f00): int32 bytes 1,2,3 of each sample, giving the same S24_3LE. Before packing, each sample is multiplied by a per-channel gain table at +0x74b0, clamped to ±2147483392.0, and rounded.

**Channel order and alignment.** There is no per-packet header or marker. The plug-in addresses data by whole frames:
- Input: slot + k × frameBytes.
- Output: writes at (sampleTime − chunkStartSample) × frameBytes + 0x508 (`WriteOutputData`).

So channel 0 is always at a frame boundary that the dext defines. The plug-in never checks or repairs a rotation (CONFIRMED: no such code).

INFERRED: one frame size (`fActive.fOutNumCh` × 3, field +0x268) serves both directions. Field names are uncertain; with 24 in and 24 out they agree.

Linux comparison: the identical shred pattern ~i (motu-usb-audio.c:201), and it compares whole 72-byte frames, up to NOM_SAMPLE_COUNT+1 frames per microframe (motu-usb-audio.c:224).

## 5. Rate matching and the clock

**Output.** The plug-in never chooses packet sizes. The dext publishes for each output chunk: +0x0 a mach timestamp, and +0x177508 / +0x177510 the start and end byte positions in a continuous output stream (CONFIRMED, used in `StartAudio`, `UpdateZeroTimes` and `WriteOutputData`). The plug-in treats CoreAudio sample time as (dext byte position ÷ frame bytes). If the requested sample is outside the current chunk it searches all 20 chunks; failing that it logs "CoreAudio fell behind, resynchronizing" and jumps.

**Zero timestamps** (`UpdateZeroTimes`, CONFIRMED). Walk the chunks while chunk[n+1].time > chunk[n].time. Whenever the chunk's end sample passes lastZero + period, the plug-in interpolates a host time: chunk time + (frames × 1e9 / rate) converted to mach ticks. It passes that to `FilterZHT`.
- Period (`UpdateZeroTimePeriod`): 16384 frames at ≤48 kHz, 32768 at ≤96 kHz, 65536 above.

**FilterZHT** (`SA_Device::FilterZHT`, CONFIRMED). The expected delta is period/rate. If the new delta is within ±100 µs of it (0x186A0 ns), the plug-in keeps a 10-entry running average (sum − average + delta, divided by 10) and advances the zero time by the average. Otherwise it takes the raw time.

**Input timing.** `ReadInputData` tries to lock the chase mutex and runs `ChaseInput` on every IO cycle. If the lock is busy it posts "chase lock contention detected" and copies anyway. It then copies from the local float ring at (sampleTime + offset) mod period. `ChaseTimerProc` chases only when IO has not chased within 200 ms (half of `kIsocRingBufferDuration`).

`ChaseInput` logs "!!! chased %llu sample frames, expected %llu += 1" when one packet held the expected count ± 2 or more. The expected counts are 6 / 11 / 12 / 22 / 24 frames for 48 / 88.2 / 96 / 176.4 / 192 kHz. 44.1 kHz falls to the default of 6.

INFERRED: feedback or implicit sizing of playback packets, and the timestamps, come from the dext. Possibly they use the same interval interrupt Linux uses, but that is not visible here.

Linux comparison:
- Playback packet length = the matching capture packet's actual length, i.e. implicit feedback (motu-usb-audio.c:503-511).
- Timing comes from interval interrupts with info 0x01 / attr 0x01 (motu-usb-audio.c:471).
- There is a manual `pb_adj` offset (motu-usb-audio.c:428-432).

## 6. Interrupt and status endpoint

None in either binary. The plug-in's only asynchronous channel is the user-client notification on selector 3, which carries datastore changes (`DSNotificationCallback`). Whether the dext reads an interrupt endpoint is NOT RUN (it is in the dext).

Linux comparison: interval interrupts (0x01/0x01) drive period timing; all other status messages are ignored (motu-usb-audio.c:461-475).

## 7. Errors and recovery (plug-in, CONFIRMED)

- **CoreAudio late:** search all chunks, then resynchronise by jumping (`WriteOutputData`).
- **Wrong frame count per packet:** logged only (`ChaseInput`).
- **No output timestamp within 200 ms:** stop ("TIMEOUT STARTING OUTPUT").
- **No input by the first timestamp:** logged ("INPUT ERROR STARTING, CHASE FAIL"), not fatal.
- **`MaybeStopAudio`:** re-reads run state (key 2). If `fAudioStarted` is 0 while the state is kRun, it resynchronises ("error detected... resynchronizing state") through `ForceStopAudio` (vtable +0x70).
- **Sleep:** `MaybeStopAudio`. **Wake:** restarts audio if `fStartCount` > 0.
- **Config change:** stop, `ForceStopAudio`, wait as in section 2.
- **`StartIO`:** waits on a condition variable with 1000 ms timeouts while the device is sleeping or reconfiguring.
- **`restartAudioQuietly`:** stop then start under both mutexes.
- INFERRED: `allowMaxBusStall(!flag)` (selector 5) is called in `Activate` and relates to bus stall tolerance in the dext.

## 8. Other load-bearing points

- Input detection by content (the shred pattern) means a short or missing packet can never shift channels: frames are counted, never bytes.
- The ring is 400 ms deep with a 200 ms watchdog. The output side runs on absolute sample positions defined by the dext.
- The mode switch out of UAC (section 1) and the forced re-enumeration away from Apple's class driver both show that MOTU never runs this device through the class-compliant path on macOS.

## OPEN QUESTIONS

1. Every USB-level fact the checklist asks for: interfaces and alt settings, endpoint numbers, vendor requests and their order, whether the dext uses implicit or explicit feedback, the interrupt messages, and how a stall is handled. These are in the dext binary named above, which was not mapped.
2. Whether posting `host/mode=USB2` makes the 24Ao re-enumerate, what it changes (descriptors, PID, clock source), and what the device's default mode is at power-on. This bears on S1–S4.
3. How the datastore `SendBusCommand` payloads reach the device: a control transfer or a bulk endpoint, and with what framing.
4. Exact meaning of the output chunk fields +0x177508 / +0x177510 / +0x0. They are read as start and end byte positions and a mach time; who writes them, and when, is a dext question.
5. Name of the `USBDeviceReEnumerate` option bit 29.
6. The unit of the `ChaseTimerProc` period of 5 (probably ms), and what `PerformConfigChange` waits for.
7. Fields +0x260 / +0x264 / +0x268 (in and out channel counts versus "calc" counts). Their naming cannot be settled from code paths where in = out = 24.