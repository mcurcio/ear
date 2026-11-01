# MOTU 24Ao: MOTU's Mac driver compared with snd-usb-motu

## What this is

Notes from a reverse-engineering run on 2026-11-01. The aim: compare MOTU's Mac driver, which works with the
device, against the Linux driver snd-usb-motu, to find what the Linux side lacks for a MOTU 24Ao (USB vendor:product
07fd:0005, firmware 1.4.8).

The notes keep three kinds of statement apart:

| Label | Meaning | Where |
|---|---|---|
| OBSERVED | Read directly in code, a symbol or a string, with the evidence beside it | `1-observations/` |
| INFERRED | Reasoned from names, structure or partial reads; each names the observations it rests on | `2-inferences/` |
| COMPARISON | A judgement across the two drivers, or between a driver and the symptoms | `3-comparison/`, and marked COMPARISON where a map made one |

## Method

- Static analysis only. Nothing from the MOTU package was run, and snd-usb-motu was not built or loaded.
- Tools: `nm`, `c++filt`, `strings`, `otool`, `llvm-objdump`.
- Three mappers, each answering one checklist (1 bring-up, 2 clock and rate, 3 start and stop, 4 framing, 5 rate
  matching, 6 interrupt and status, 7 errors, 8 other):
  - the dext (`sources/map-mac-dext.md`): read the x86_64 slice with `otool -tV | c++filt`, which marks string
    literals; it notes both slices carry the same symbols. Its main evidence: the dext logs the source text of
    its own failed calls and assertions, and the instruction constants;
  - the HAL plug-in and re-enumerator (`sources/map-mac-hal-reenumerator.md`): read the arm64 slices;
  - snd-usb-motu (`sources/map-linux-snd-usb-motu.md`): read all 1295 lines of `motu-usb-audio.c`, plus
    `readme.md`, `Makefile` and `build_and_load.sh`.
  The Mac mappers' working copies (disassembly, strings; for the HAL map `plugin_arm64.s`, `reenum_arm64.s`,
  `*_strings.txt`, `startaudio.txt`) are in `/Volumes/Dock/motu-mac-driver/scratch/`; the
  Linux mapper wrote none (it found that folder empty at the time).
- One comparer (`sources/compare-gaps.json`) listed gaps G1-G8, matches and unresolved points.
- Two verifiers, one per side (`sources/verify-linux-side.json`, `sources/verify-mac-side.json`), each tried to
  refute every gap, re-read code to do so, and corrected some map facts.
- This editor sorted every map claim into `1-observations/` or `2-inferences/` (or split it across both), copied
  the comparison and verdicts into `3-comparison/gaps.md`, computed the provenance below, and re-read
  `motu-usb-audio.c` lines 436-453, 563-578 and 1246-1253 to settle corrections C5, C8 and C10. Nothing else
  was re-checked by the editor. The files in `sources/` are unchanged.

## Provenance

Computed by the editor on 2026-11-01 (`shasum -a 256`, `PlistBuddy`, `git log`).

| Item | Value |
|---|---|
| Installer | `/Users/matt/Downloads/MOTU Pro Audio Installer (Dext b72db03fb).pkg`, sha256 `939740ab77bef014a76b11a2ae1018d2a10097ef1e6ffed8371b98728b4948bb` |
| Expanded payload | `/Volumes/Dock/motu-mac-driver/pkg/MOTU_Pro_Audio_Drivers.pkg/Payload/` |
| dext | `Applications/MOTU Pro Audio System Extension.app/Contents/Library/SystemExtensions/com.motu.driver.pro.audio.dext.dext/com.motu.driver.pro.audio.dext`; universal (x86_64, arm64); sha256 `8ce4f43b0e3b72b4279bf8be4b17a3b767fab31095bdc39b57583327b94c1b6d`; bundle `com.motu.driver.pro.audio.dext`, CFBundleShortVersionString `2.0.2 b72db03fb`, CFBundleVersion `2.0.2 b72db03fb` |
| HAL plug-in | `Library/Audio/Plug-Ins/HAL/MOTUProAudioServerPlugin.driver/Contents/MacOS/MOTUProAudioServerPlugin`; universal (x86_64, arm64); sha256 `2f2874d742d01672e9baf92a2417070666c3d3cf766802eaee80d2b060b11ab6`; bundle `com.motu.proaudio.audioServerPlugin`, CFBundleShortVersionString `2.0.2+b72db03fb`, CFBundleVersion `2.0.2+b72db03fb` |
| Re-enumerator | `Library/Application Support/MOTU/ProAudio/com.motu.driver.proaudio.reenumerator.xpc/Contents/MacOS/com.motu.driver.proaudio.reenumerator`; universal (x86_64, arm64); sha256 `0711a05a70e0227c69288fea8a9487aa140f1fefea1f77aba9b679fc8c2cf635`; bundle `com.motu.driver.proaudio.reenumerator`, CFBundleShortVersionString `1.0`, CFBundleVersion `1` |
| snd-usb-motu | `/Volumes/Dock/motu-mac-driver/snd-usb-motu`, origin `https://github.com/dylan-motu/snd-usb-motu.git`, HEAD `d36612237c5bf057f3832bc65ca1c25d01059ade` ("Update readme.md"), author and commit date 2026-09-30 13:50:23 -0400, clean working tree; `motu-usb-audio.c` 1295 lines |
| llvm-objdump | `/Library/Developer/CommandLineTools/usr/bin/llvm-objdump --version`: "Apple LLVM version 21.0.0, Optimized build" (otool: cctools-1040) |

The sha256 values are of the universal files; the mappers read single slices of them.

## The owner's observed symptoms

The owner's observations on 2026-10-31, from the "The problem" paragraph of
`/Volumes/Dock/iac-ear-run/BRIEF-motu-quirks.md`. EAR ran on Talos node `hawking` with the 24Ao (ALSA card
`D24Ao`). The brief lists them unnumbered; S1-S4 are the labels the maps and the comparison use.

- **S1** "the 24-channel playback frame rotated by 8 (EAR port N on MOTU output N-8 mod 24, confirmed with port 1
  on output 17)".
- **S2** "clock source 1 is not valid, cannot use" at every USB connect.
- **S3** "EAR's first open after power-on failing with ENXIO".
- **S4** "two MOTU firmware freezes around stream starts".

That paragraph does not name the kernel driver in use. Both verifiers state S1 and S4 were seen under the stock
snd-usb-audio, not snd-usb-motu (verify-linux G1, verify-mac G1 and G2).

## Files

| File | What |
|---|---|
| `README.md` | This file |
| `1-observations/mac-dext.md` | OBSERVED: the dext (IDs D0.1-D8.3) |
| `1-observations/mac-hal-plugin.md` | OBSERVED: the HAL plug-in (H0.1-H8.1) |
| `1-observations/mac-reenumerator.md` | OBSERVED: the re-enumerator daemon (R0.1-R1.7) |
| `1-observations/linux-snd-usb-motu.md` | OBSERVED: snd-usb-motu (L0.1-L8.8) |
| `2-inferences/mac.md` | INFERRED, Mac: MI-D*, MI-H*, MI-R*, cross-driver remarks MC-*, the two Mac maps' open questions |
| `2-inferences/linux.md` | INFERRED, Linux: LI1-LI30 and the Linux map's open questions |
| `hardware/observations-fourier.md` | OBSERVED: the MOTU's USB descriptors, interfaces and kernel log read from the Talos node fourier on 2026-11-01, with the raw files beside it |
| `2-inferences/hardware.md` | INFERRED from those descriptors (HI1-HI6) |
| `3-comparison/gaps.md` | COMPARISON: G1-G8 with both verdicts verbatim and a status each; matches; unresolved |
| `sources/` | The run's raw output, unedited |

Gap status (`3-comparison/gaps.md`): G1 disputed (verify-linux refuted it); G2 refuted (both); G3-G8 survived.

## Corrections

Every place a verifier corrected a mapper or the comparer, and which source file it corrects. The corrected fact
is what the observation and inference files state.

- **C1** OUT is scheduled 0x140 *frames* after IN: 1 ms frames, so 320 ms, not 0x140 microframes (verify-mac G3).
  Corrects `compare-gaps.json` G3, which gave 0x140 with no unit. The dext map already said 320 frames and 320 ms;
  no source file actually says "microframes". See D3.5.
- **C2** The dext switches to vendor streaming only when the device's current descriptors already contain a
  0xFF/4/2 interface, and it has a complete UAC fallback; "MOTU never runs this device class-compliant" overstates
  the code (verify-mac G1; verify-linux G1 makes the same point from the "opened UAC isoc interfaces and pipes"
  string). Corrects `compare-gaps.json` G1 (Mac behaviour) and `map-mac-hal-reenumerator.md` sections 1 ("So the
  Mac driver moves the device out of ... UAC mode") and 8. See MI-H3, MI-H14.
- **C3** The re-enumerator only evicts Apple's class driver (which driver owns the device); it does not switch
  the device's mode (verify-mac G1). Corrects `compare-gaps.json` G1 (Mac behaviour),
  `map-mac-hal-reenumerator.md` section 8, and `map-mac-dext.md` section 1 ("Any mode switch belongs to the
  'reenumerator' daemon"). See MI-D3, MI-R1.
- **C4** S1 and S4 were seen under snd-usb-audio, not snd-usb-motu; snd-usb-motu is not the stock UAC path
  (verify-linux G1, verify-mac G1). S4 cannot be explained by vendor request 0x01, which snd-usb-audio never sends
  (verify-linux G2, verify-mac G2). Corrects the symptom links of `compare-gaps.json` G1 and G2 and
  `map-mac-hal-reenumerator.md` section 1 ("relevant to S1-S4"). See MC-H1.
- **C5** The stop-and-restart on a period change runs only when a new stream starts with a different period,
  which `hw_params` already forbids for a mismatched second stream (verify-linux G2). Corrects
  `compare-gaps.json` G2 ("A period-size change while streaming does stop plus start") and the scope of
  `map-linux-snd-usb-motu.md` section 7 ("Period-size change while running"). See L3.2, L7.7.
- **C6** "No device-facing commands" in the dext holds only for the start and stop paths: the dext does talk to
  the device over the 0xFF/4/1 bulk command interface; `SetMaxBusStall` is a host-side call, not a device request
  (verify-mac G2). Corrects `compare-gaps.json` G2 (Mac behaviour). See D3.7, D1.12-D1.17.
- **C7** On Linux the playback write lead is 16+d microframes and never negative; the first start gives a fixed,
  unknown offset of d x 6 samples, not early or stale audio; the real exposure is a late capture completion
  shifting the offset mid-stream (verify-mac G3). Corrects `compare-gaps.json` G3 (symptom link) and the
  inference in `map-linux-snd-usb-motu.md` section 5 ("What the 2 ms lead depends on"). See LI13.
- **C8** On Linux, `spin_trylock` contention mutes the period (playback: `mute_period_to_usb` writes zeros;
  capture: `sync_period_from_usb`); it does not skip it (verify-linux G4, verify-mac G4; editor re-read
  :436-452). Corrects `compare-gaps.json` G4 (Linux behaviour and evidence: "trylock contention skips the
  period"). `map-linux-snd-usb-motu.md` section 7 already said "syncs or mutes". See L7.6.
- **C9** G4's proposed fix is wrong both ways: clearing in `capture_complete_urb` erases audio the period handler
  just wrote; clearing on playback completion hits memory the next URB shares; a clean fix needs per-URB buffers
  (G8) first, or zeroing a span just behind the write position (verify-linux G4, verify-mac G4). Corrects
  `compare-gaps.json` G4 (proposed fix).
- **C10** The probe error path frees priv (`snd_card_free`) without releasing interfaces 1 and 2, which keep a
  dangling priv as interface data: a use-after-free risk on disconnect (verify-mac G6), and interrupt URBs and
  coherent buffers leak (verify-linux G6). Corrects `map-linux-snd-usb-motu.md` section 7 ("They stay bound to
  this driver, so snd-usb-audio cannot take them") and `compare-gaps.json` G6 (Linux behaviour). See L7.10, LI23.
- **C11** The USB core normally probes interfaces in descriptor order, so the realistic bind risk is
  snd-usb-audio binding first, not a self-race (verify-mac G6); verify-linux G6 adds that drivers are tried in
  registration order. Corrects `map-linux-snd-usb-motu.md` section 1 ("Probe-order assumption") and
  `compare-gaps.json` G6. See LI2, LI3.
- **C12** The Linux 512-slot aliasing looks deliberate (start shreds only the first 512 slots); the risk is a
  timing margin of about 20 ms at the largest period, not a logic bug (verify-linux G8; verify-mac G8: "impact is
  narrower than stated"). Corrects `compare-gaps.json` G8 (symptom link). See L8.2, LI24.
- **C13** G7's fix: the datastore over the vendor bulk interface is an undocumented protocol; acting on the
  status interrupt, and optionally a read-only UAC2 GET_CUR on the clock source, works in a kernel driver
  (verify-linux G7); the datastore transport is not yet specified for Linux (verify-mac G7). Corrects
  `compare-gaps.json` G7 (proposed fix).
- **C14** What one failed capture resubmit does on Linux: verify-mac G5 says both directions stop; verify-linux G5
  says playback resubmission slows and playback starves. Both refine `map-linux-snd-usb-motu.md` section 7 ("One
  lost capture URB stops its playback partner silently") and `compare-gaps.json` G5; they do not agree with each
  other. See LI19.
- **C15** `capture_complete_urb` does check each packet's status, though not `urb->status` (verify-mac G5).
  Refines `map-linux-snd-usb-motu.md` section 7 ("URB status"). See L7.2.

Not corrections, but marked in the observation files as "(mapper only; not re-checked)": the 200 ms output-start
limit (verify-mac could not decode it, H3.1 step 9), dext property 10 (H0.5, H1.1), `kIsocRingBufferDuration`
0x17D78400 (H4.3), the HAL's chunk timestamp fields (H5.1). verify-linux did not check the Mac constants 0x14 and
0x140; verify-mac did (D3.5).

### Inconsistencies the editor found (no verifier raised them)

- **E1** `map-mac-hal-reenumerator.md`'s scope finding says "Neither binary touches USB directly", but its own
  section 1 has the re-enumerator calling `USBDeviceOpen` and `USBDeviceReEnumerate`. R0.1 states the precise
  version.
- **E2** `compare-gaps.json` "Matches" says both sides "count whole 72-byte frames"; the HAL map says the
  plug-in compares each frame's first 24 bytes (H4.5). Noted under the matches in `3-comparison/gaps.md`.
- **E3** `map-mac-hal-reenumerator.md` section 5 guesses the dext may use "the same interval interrupt Linux
  uses"; the dext map shows it looks up no interrupt pipe (D6.1). See MI-H11.
- **E4** `map-mac-dext.md` section 4 leaves "the Mac HAL very likely does the same early read" as INFERRED; the
  HAL map observes it (H4.5). See MI-D11.
- **E5** `map-linux-snd-usb-motu.md` doubted that its checkout d366122 is the brief's "d36612237c5b"; it is: HEAD
  is `d36612237c5bf057f3832bc65ca1c25d01059ade`. The same map notes the brief's "~1000 lines" does not match
  the file's 1295 lines.
- **E6** The maps cite the same Linux facts with different line ranges: the vendor request :532-547 (Linux map),
  :535-547 (dext map), :535-542 (HAL map); implicit feedback :497-511, :488-512, :503-511; the match table
  :1281-1284 and :1282. The observation file keeps the Linux map's ranges and notes the others.
