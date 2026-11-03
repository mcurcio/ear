# Hardware observations, 2026-11-02: freezes, channel mapping, the fix

OBSERVED only: what the device, the hosts and the recordings showed. Times are US Pacific (PST,
UTC-8). What these facts suggest is in `../2-inferences/usb-tests-2026-11-02.md`. The agents'
full reports are in `../sources/2026-11-02/`; the tools are in `../tools/`.

The device: MOTU 24Ao, USB 07fd:0005, serial 0001f2fffe00f732, firmware 1.4.8+102192 (dated
06/29/26 in its settings store). Hosts:
- **euclid:** Ubuntu 26.04.1, kernel 7.0.0-42, AMD xHCI. Also a USB 2 (EHCI) port.
- **copernicus:** Mac Pro, macOS 12.7.6, Intel, with MOTU's driver 2.0.1+11ea3df24 installed.
- **joule:** Mac mini, Apple Silicon, macOS 26.5.2, with no MOTU driver, so Apple's class driver.

## Test setup

- **Loopback:** the owner set a digital loopback in the MOTU's routing, "From Computer" N to "To
  Computer" N, N = 1..24. The settings were reset at 05:3x; the loopback was set up again before
  the euclid tests.
- **Signal:** each test plays 24 sines at once, one frequency per channel (300 Hz to 12024 Hz,
  -12 dBFS), and records the 24 inputs.
- **Analysis:** `analyze` names the source heard on each capture channel every 250 ms. A
  frame-by-frame check fits each capture channel's own sine and counts samples more than 0.05 off.
- **Liveness, euclid:** `motu-alive`, a GET_STATUS and a GET_DESCRIPTOR on endpoint 0. The
  heartbeat runs it every 10 s, after the owner said "The heartbeat idea makes sense". A kernel-log
  watcher (`motu-watch`) runs beside it.
- **Liveness, copernicus:** a GET of the device's `apiversion` every 30 s, through MOTU's local web
  server (port 1280), which reaches the device over USB.

## Freezes

A freeze: the device stops answering on USB and, when looked at, its front panel is frozen too,
until a power-cycle. The panel was dead at the reset attempt (owner) and at 08:12 (owner).

| Host | Connected | Ethernet | Driver | Last answer | First failure |
|---|---|---|---|---|---|
| euclid | 05:16:25 | in | none | 05:16:33 | 06:00:07 (first check after that) |
| euclid (USB 3 port) | 05:00:34 | in | none | never enumerated: descriptor read -110/-71 | |
| copernicus | 04:51:06 | in | MOTU 2.0.1 | 04:51:08 (settings store read) | 04:54:15 (MOTU driver: "Command 80 ... timed out") |
| copernicus | 05:22:58 | in | MOTU | 05:28:01 | 05:28:32 |
| copernicus (after reset) | 05:36:38 | in | MOTU | 05:36:40 | 05:37:11 |
| copernicus | 05:39:25 | in | MOTU | 05:42:50 | 05:43:20 |
| copernicus (after onboarding) | 05:49:53 | in | MOTU | 06:02:00 | 06:02:30 |
| copernicus | 06:49:34 USB, Ethernet added 07:38 | in from 07:38 | MOTU | 08:12:02 | 08:12:32. `10.0.40.1` answered "Host is down" at 08:16:01 |
| euclid (USB 3 port 5-1) | 09:00:11 | in (by mistake) | none | 09:14:04 | 09:14:14 |

Runs with no freeze:

| Setup | Duration | Result |
|---|---|---|
| Nothing connected (no USB, no Ethernet) | 06:04 to 06:48 | Panel working at 06:19 and 06:48 (owner) |
| copernicus, USB only | 06:49:34 to 07:38 | 79+ heartbeats, no failure, no MOTU driver timeouts |
| euclid, USB only, Ethernet out | 09:32:02 to 13:19:51 (owner's power-off) | Every 10 s heartbeat answered. That covered about 60 driver loads and the runs below |
| euclid, USB only, after each of 3 power-cycles | 13:20:48, 13:39:28, 13:46:05 | Every heartbeat answered |

Other facts:
- **Freeze detection:** MOTU's driver on copernicus kept the device "alive" in CoreAudio while it
  was frozen. Only its own log ("Command 80 ... timed out! gIntCount 0") and the heartbeat showed it.
- **Mode switch:** on copernicus, at every connection MOTU's plug-in logged `host mode: UAC`, then
  switched the device to `host mode: USB2` (MOTU's own streaming mode).
- **192 kHz at power-on:** the MOTU came up at 192 kHz after power-cycles before the factory reset,
  and at 48 kHz after it.
- **The reset changed 361 settings:** rate 192000 to 48000; AVB input streams 0 to 8 and output
  streams 0 to 4; channel counts. See `../sources/2026-11-02/settings-diff-before-vs-after-reset.txt`.
- **Network:** with Ethernet in, the MOTU has address `10.0.40.1` (MAC 00:01:f2:00:f7:32) and
  advertises an `_http._tcp` service named "24Ao". The owner says it has OSC on port 9998.
- **No AES67:** the settings store has no AES67 or PTP keys.

## Channel mapping through the loopback (stock and candidate drivers)

| Host, driver, parameter | Order started | Starts | Capture channel N carried |
|---|---|---|---|
| joule, Apple class driver | duplex stream | 3 | source N, 3 of 3 |
| euclid, `snd-usb-audio` `quirk_flags=07fd:0005:!validate_rates\|skip_iface_setup` | capture first | 9 (one 10 min) | source N+8 (mod 24), 9 of 9 |
| same | capture first | 1 (5 s) | source N, with hundreds of discontinuities per second; a few frames shifted |
| same | playback first | 3 | 1 start N+8, 2 starts badly broken (worst-window SNR below 0 dB) |
| euclid, `snd-usb-audio` with no quirk flags | capture first | 1 | silence for 13 s (probe set 192 kHz, 1623 IN overflows -75), then source N |
| euclid, flags + `playback_first` | capture first | 3 + 15 + 3×2 power-cycles | source N, every start |
| same | playback first | 3 + 1 (10 min) + 15 + 3×1 power-cycles | source N, every start |
| same, + `iface_delay` | playback first | 3 | source N, every start |
| euclid, `snd-usb-motu` (d366122) | capture first | 1 | nothing: 99.4% zeros, the rest the driver's fill pattern |

- **With `playback_first`:** 49 of 49 starts gave source N on capture N. Frame-by-frame checks of
  48 of them (10 s each, and 9.6 million frames of the 10-minute run) found 0 samples off. Those
  49 starts were 40 with the driver reloaded and 9 straight after a MOTU power-on.
- **Start-up gap:** every start on every host lost about the first 2 s. On euclid the MOTU returned
  exact zeros for about 2.1 s after the first playback packet. On joule (Apple) the analyzer's
  start-up latency was 2.0 s.
- **`snd-usb-motu` on euclid's AMD xHCI:** "xhci_hcd ... ERROR unknown event type 37" every one or
  two periods (197 times), 94 "rec urb error". Playback URBs stopped reaching the bus after 0.8 s.

## USB traces (euclid, full usbmon, stock driver with the Talos flags)

From `../sources/2026-11-02/rotation-traces.md` and the analysis, re-decoded by two agents:
- **OUT (what Linux sends):** each 72-byte frame carries source k at bytes 3(k-1). Every OUT and IN
  packet is a whole number of 72-byte frames (432 or 360 bytes).
- **IN (what the MOTU returns):** in the +8 runs, slots 1..24 carry sources 9..24, 1..8. Each
  returned frame holds sources 9-24 of one frame and sources 1-8 of the next: the slip is 24 bytes
  in the byte stream.
- **Mixed states:** in two recordings, exactly 1 (or 2) frames in every 6 were shifted at first.
  At 48 kHz that is one frame position per 125 µs microframe. Within seconds every frame was shifted.
- **Start sequence (capture first):** at `aplay`'s start the only control request was
  SET_INTERFACE(1, 1), with the capture endpoint already streaming. Playback URBs then followed IN
  completions with about one 1 ms URB queued.

## Evening: the fix on the cluster, and EAR on hawking (OBSERVED)

- **Rollout** (mcurcio/iac PR 343, rolled by the owner from its branch):
  - `make talos+ NODE=fourier`, 15:58-16:03 PST.
  - Then `make talos+` for kepler, galileo, imes and hawking, 17:14-17:22 PST. Final gate ok, exit 0.
- **fourier, read back at 17:10 PST:** `MachineConfig persistent` and `v1alpha1` both set
  `module.snd_usb_audio.parameters.quirk_flags: 07fd:0005:!validate_rates|skip_iface_setup|playback_first`.
  `/sys/module/snd_usb_audio/parameters/quirk_flags` read that value followed by `(null)` slots.
- **The roll tool's sysfs gate during the fourier canary:** it printed "not in effect" every 16 s
  for 286 s, while the node already held the new value (read at 16:01 PST). During
  `apply-config --mode=try` the node lists two `MachineConfig` resources, `persistent` (old value)
  and `v1alpha1` (new). `talosctl get machineconfig` lists `persistent` first. Fixed in the same PR
  (`get machineconfig v1alpha1`). With the fix, each of the other four nodes was healthy straight
  after its apply.
- **The MOTU's cluster node is hawking.** The iac node inventory gives `motu-24ao` to hawking, only
  hawking has the label `node.iac/device.motu-24ao=true`, and the `ear` deployment selects it. Its
  pod ran on hawking.
- **The deployed EAR at 18:00 PST:**
  - Image: `ghcr.io/mcurcio/ear:sha-6f36d74`.
  - Arguments: `--config /etc/ear/ear.json`, read from ConfigMap `ear-config`, so edits stay in
    memory only.
  - Types available: `card`, `file` and `white`. No `graphFile.*` methods ("Method not found").
  - ConfigMap graph: one node, `motu`: `card D24Ao, playback 24, capture 0`. No routes. 48 kHz,
    block size 512, `SCHED_FIFO 83`.
- **That pod (started 17:18:30 PST, while the MOTU was on fourier):**
  - `/dev/snd` held only `timer`.
  - Log: `Cannot get card index for D24Ao`, `playback open failed: No such device`,
    `node motu failed engine-refused: the card failed: usage: node.name=<string>`.
  - `node.update motu` with capture 24: refused, `ports-changed motu`.
  - `node.update` with the same settings: answered `{}`, with no new log line and state still failed.
- **Not yet done:** the MOTU was moved to hawking at about 17:58 PST. EAR's pod has not been restarted
  since, so EAR has not opened the MOTU with the new flags. The kernel's view on hawking (cards,
  `stream0`, `quirk_flags`) was not read: two 1Password prompts timed out.
