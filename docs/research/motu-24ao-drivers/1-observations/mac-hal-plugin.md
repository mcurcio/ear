# Observations: MOTU's CoreAudio HAL plug-in

Binary: `MOTUProAudioServerPlugin` (provenance in `../README.md`). Source: `../sources/map-mac-hal-reenumerator.md`,
which read the arm64 slice. The binary's own strings name its source file
`IntegratedDrivers/AVB/Mac/Driver/User/Plug-In/SA_DeviceUSB.cpp`.

Only facts read in code, symbols or strings are here. Interpretations are in `../2-inferences/mac.md`.
Tags as in `mac-dext.md`. Untagged bullets are the mapper's reading (CONFIRMED in the map), not re-checked.
Linux facts the map quoted for comparison are in `linux-snd-usb-motu.md`.

## 0. Scope and interfaces

- **H0.1** The plug-in imports no IOUSB API. It makes no alt-setting, isochronous, interrupt or control-transfer
  call.
- **H0.2** Opening the dext: `IOServiceOpen` on the matched service (`USBServiceClient::Open`).
  `RegisterForNotifications` calls `IOConnectCallAsyncStructMethod(selector 3, 8-byte input)` on a CFMachPort.
- **H0.3** Notifications: the callback hands (type, length, data) to `SA_Device::DSNotificationCallback`.
  0xA5A5A5A5 marks an empty message. `UserClientCallBackProc` ends with a branch to `RegisterForNotifications`,
  re-registering after every message.
- **H0.4** External-method selectors, all through `IOConnectCallStructMethod`; the vtable at 0x10003cd60 has
  the methods in this order:

  | Selector | Method | Payload |
  |---|---|---|
  | 0 | `SetProperty(key, value)` | 8-byte input |
  | 1 | `GetProperty(key)` | 4-byte input |
  | 2 | `SendBusCommandSync(cmd byte, payload)` | 4096-byte struct, payload at most 0xFFC; logs "SendBusCommand cmd %d returned err" |
  | 4 | `ForceStopAudio` | none |
  | 5 | `allowMaxBusStall(bool)` | 1-byte input |

- **H0.5** Property keys seen:
  - 2, audio run state. `SA_DeviceUSB::StartAudio` sets it to 1 (`mov w1,#0x2; mov w2,#0x1`); `StopAudio` sets
    it to 0; `SA_Device::MaybeStopAudio` reads it back through `GetProperty` (string "audio run state is kRun").
  - 9, maximum input bytes per microframe, set in `StartAudio` just before key 2.
  - 10 (0xA), "isocMode", read in `SA_Device::ProcessDataStoreChanges` (mapper only; not re-checked: verify-mac
    did not verify the "property 10" detail).
  - 11 (0xB), set by `MaybeProcessPedalDS`.
- **H0.6** Shared memory: `IOConnectMapMemory64` with memory type 1 (input ring, stored at +0x7cb0) and type 2
  (output ring, stored at +0x7cb8), both mapped in `StartAudio` and unmapped in `StopAudio`. Type 0 is mapped by
  `GetTimestampStatusBuffer`, which nothing in `SA_DeviceUSB` calls.
- **H0.7** Sample rate, host mode and footswitch are set through the device's datastore (keys such as
  `avb/<uid>/cfg/0/current_sampling_rate`), not with USB requests. `DataStoreService::Post` carries them over
  selector 2.

## 1. Bring-up

- **H1.1** In `SA_Device::ProcessDataStoreChanges`, when the datastore key `host/mode` changes (the key is
  compared as "host/mod" + 'e'), the plug-in reads dext property 10 (isocMode). When `host/mode` equals "UAC" it
  posts `host/mode` = "USB2" if isocMode is 2 and "USB3" if isocMode is 3 (`DataStoreService::Post`), and posts
  nothing for any other isocMode. (re-checked by verify-mac, except the "property 10" number.)

## 2. Clock and sample rate

- **H2.1** `SA_Device::SetSampleRate` accepts only 44100, 48000, 88200, 96000, 176400 and 192000. It applies the
  rate with `DataStoreService::Post("avb/" + <service string> + "/cfg/0/current_sampling_rate",
  to_string(rate))`. No UAC clock request is involved. (re-checked by verify-mac: `SetSampleRate` calls
  `DataStoreService::Post`.)
- **H2.2** In `ProcessDataStoreChanges`, the datastore keys "clockLocked" and "dmaEnabled" set `fClockLocked` /
  `fDMALocked` and fire `Host_PropertiesChanged('goin')`. (re-checked by verify-mac: the function references
  "cfg/0/current_sampling_rate", "fClockLocked %s", "fDMALocked %s" and "fConfiguredRate %d".)
- **H2.3** `SA_DeviceUSB::StartAudio` never reads `fClockLocked`; only `SA_DeviceTBolt::StartAudio` logs it
  (string cross-reference).
- **H2.4** `SA_Device::PerformConfigChange`: `MaybeStopAudio`, then `ForceStopAudio` (selector 4), then up to
  100 waits of 100 ms (`cmp w22,#0x64`, `usleep(0x186a0)`), then a property-change notification. (re-checked by
  verify-mac.)

## 3. Start and stop

- **H3.1** `SA_DeviceUSB::StartAudio`, in order:
  1. Asserts that `fActive.fInNumCh` and `fActive.fOutNumCh` are non-zero.
  2. Maps the input ring (type 1) and the output ring (type 2).
  3. Writes a "shred" pattern into the data area of each of the 20 input chunks: `memcpy` of 0x177000 bytes to
     chunk + 0x508, 20 times, from a template the constructor builds. Template byte i = ~(i mod 256): the
     constructor computes NOT(v) and 0xEF-v over 0..15 in 32-byte steps.
  4. Zeroes a 16 MiB local float ring (`bzero 0x1010000`).
  5. Computes `fBufferSyncOffsetSamples` = rate x 300 / 8000 (1800 at 48 kHz); frame size = channels x 3
     bytes; `fMaxIn_uFrameSizeBytes` = inCh x (7 if at most 48 kHz, 13 if at most 96 kHz, else 25) x 3 (504
     at 24 channels, 48 kHz); `fMaxSamplesPer_uFrame` = 7.
  6. Marks the chunks: consumed count 0x9F for chunk 0 and 0xA0 for the others (one `stlr` per chunk).
  7. `SetProperty(9, maxInBytes)`, then `SetProperty(2, 1)`; a failure logs "StartAudio failed!!!".
  8. Adds a repeating WorkLoop timer with period 5 that runs `ChaseTimerProc` (`AddTimer(..., 5, 1)`).
     (re-checked by verify-mac: `WorkLoop::AddTimer(...,5,...)`.)
  9. Polls output chunk[index].timestamp every 125 us (`usleep(0x7d)`) for up to 200 ms (0xBEBC200 ns); on
     timeout logs "TIMEOUT STARTING OUTPUT" and stops. (The usleep loop and the string re-checked by
     verify-mac; the 200 ms limit is mapper only: verify-mac could not decode it, the limit register coming
     from 0x118EDC0 plus a value, in mach ticks.)
  10. When a timestamp appears, computes the output frame count, seeds `FilterZHT`, and `mach_wait_until`s that
      timestamp. If no input was chased by then it logs "INPUT ERROR STARTING, CHASE FAIL" and continues. It
      then sets `fAudioStarted` and calls `EnableIO`.
- **H3.2** The plug-in has no code that orders capture and playback endpoint start or primes output packets.
- **H3.3** `StopAudio`: `DisableIO`, remove the chase timer and wait for it to finish, clear the flags,
  `SetProperty(2, 0)`, release buffer 1, release buffer 2.

## 4. Isochronous framing

- **H4.1** Input ring (type 1; `ChaseInput`, `StartAudio`): 20 chunks of 0x177538 bytes; the end bound is base
  + 0x1D52860 = 20 x 0x177538.
- **H4.2** Each chunk holds 160 microframe slots (switch to the next chunk when offset + slotBytes >= 160 x
  slotBytes; 0xA0 compares). Each slot is `fMaxIn_uFrameSizeBytes` (504) long.
- **H4.3** 160 microframes x 125 us = 20 ms per chunk; 20 chunks = 400 ms, equal to the static
  `kIsocRingBufferDuration` = 0x17D78400 ns (400 ms) in `ChaseTimerProc` (constant: mapper only; not
  re-checked, verify-mac did not re-read it).
- **H4.4** Chunk trailer fields: +0x177518 the plug-in's consume time; +0x177520 a time field (meaning in
  `../2-inferences/mac.md`); +0x177528 packets consumed, 0..160; +0x177530 a cached copy of +0x177520.
- **H4.5** New input is found by content, not a write index. For each frame in a slot (up to 7) the plug-in
  compares that frame's first 24 bytes with the shred template at the same offset; the first frame still equal
  to the template ends the packet. It has caught up when it reaches a chunk that is fully consumed and whose fill
  time is older than its consume time.
- **H4.6** Input sample format (decoding the `tbl` masks at 0x100034ea0/eb0/ec0): packed 24-bit little-endian
  (S24_3LE). Bytes (0,1,2) become int32 bits 8..31, then `scvtf` and multiply by 2^-31
  (`movi v17, #0x30, lsl #24`).
- **H4.7** Output sample format (masks at 0x100034ed0..f00): int32 bytes 1, 2, 3 of each sample, S24_3LE.
  Before packing, each sample is multiplied by a per-channel gain table at +0x74b0, clamped to +/-2147483392.0,
  and rounded.
- **H4.8** No per-packet header or marker. Data is addressed by whole frames: input at slot + k x frameBytes;
  output at (sampleTime - chunkStartSample) x frameBytes + 0x508 (`WriteOutputData`). The plug-in has no code
  that checks or repairs a channel rotation. (Which channel-count field sets frameBytes in each direction is an
  inference: `../2-inferences/mac.md`, MI-H10.)

## 5. Rate matching

- **H5.1** The plug-in never chooses packet sizes. Per output chunk it reads +0x0 (a mach timestamp) and
  +0x177508 / +0x177510 (start and end byte positions in a continuous output stream), in `StartAudio`,
  `UpdateZeroTimes` and `WriteOutputData` (mapper only; not re-checked: verify-mac did not re-read the chunk
  timestamp fields). It takes CoreAudio sample time as byte position / frame bytes.
- **H5.2** In `WriteOutputData`, a requested sample outside the current chunk triggers a search of all 20 chunks;
  failing that it logs "CoreAudio fell behind, resynchronizing" and jumps (string re-checked by verify-mac).
- **H5.3** `UpdateZeroTimes`: walk the chunks while chunk[n+1].time > chunk[n].time; whenever a chunk's end
  sample passes lastZero + period, interpolate a host time as chunk time + (frames x 1e9 / rate), converted to
  mach ticks, and pass it to `FilterZHT`. Period (`UpdateZeroTimePeriod`): 16384 frames at up to 48 kHz, 32768
  at up to 96 kHz, 65536 above.
- **H5.4** `SA_Device::FilterZHT`: expected delta = period / rate. If the new delta is within +/-100 us of it
  (0x186A0 ns; constant re-checked by verify-mac), keep a 10-entry running average (sum - average + delta,
  divided by 10) and advance the zero time by the average; otherwise take the raw time.
- **H5.5** `ReadInputData` tries to lock the chase mutex and runs `ChaseInput` on every IO cycle; if the lock is
  busy it posts "chase lock contention detected" and copies anyway. It copies from the local float ring at
  (sampleTime + offset) mod period.
- **H5.6** `ChaseTimerProc` chases only when IO has not chased within 200 ms (half of `kIsocRingBufferDuration`).
- **H5.7** `ChaseInput` logs "!!! chased %llu sample frames, expected %llu += 1" when one packet held the
  expected count +/- 2 or more. Expected counts: 6 / 11 / 12 / 22 / 24 frames at 48 / 88.2 / 96 / 176.4 / 192
  kHz; 44.1 kHz falls to the default of 6.

## 6. Interrupt and status

- **H6.1** No interrupt endpoint use in the plug-in. Its only asynchronous channel is the user-client
  notification on selector 3, which carries datastore changes (`DSNotificationCallback`).

## 7. Errors and recovery

- **H7.1** CoreAudio late: search all chunks, then resynchronise by jumping (`WriteOutputData`; H5.2).
- **H7.2** Wrong frame count per packet: logged only (`ChaseInput`; H5.7).
- **H7.3** No output timestamp within 200 ms: stop ("TIMEOUT STARTING OUTPUT"; H3.1 step 9).
- **H7.4** No input by the first timestamp: logged ("INPUT ERROR STARTING, CHASE FAIL"), not fatal.
- **H7.5** `MaybeStopAudio` re-reads run state (key 2). If `fAudioStarted` is 0 while the state is kRun, it
  resynchronises ("error detected... resynchronizing state") through `ForceStopAudio` (vtable +0x70).
- **H7.6** Sleep: `MaybeStopAudio`. Wake: restarts audio if `fStartCount` > 0.
- **H7.7** Config change: stop, `ForceStopAudio`, wait (H2.4).
- **H7.8** `StartIO` waits on a condition variable with 1000 ms timeouts while the device is sleeping or
  reconfiguring.
- **H7.9** `restartAudioQuietly`: stop then start under both mutexes.

## 8. Other

- **H8.1** The input ring is 400 ms deep (H4.3) with a 200 ms chase watchdog (H5.6). The output side runs on
  absolute byte positions published per chunk (H5.1).
