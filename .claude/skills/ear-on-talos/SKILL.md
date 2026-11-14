---
name: ear-on-talos
description: Running EAR's run image on a Talos Linux Kubernetes node with a USB sound card (the owner's MOTU 24Ao): the usb-audio-drivers extension and loading snd_usb_audio, Pod Security and SYS_NICE, /dev/snd and containerd's device ownership, CAP_SYS_NICE for realtime priority, the writable graph file volume and seeding ear.conf, setting the MOTU's rate and channel count, its Linux quirks (quirk flags, the playback channels rotated by 8, freezes), finding the card id, the checks to run on the hardware, getting FPP's RTP audio into the pod (a UDP Service for unicast, hostNetwork for multicast, PipeWire's module-rtp-sink as the sender), and a sendspin node in the pod (its key file under fsGroup, dialling, listening and mDNS). Use when deploying EAR to the cluster, when a card node fails on the hardware, when graph.get says "realtime": false in the pod, when an rtp node in the pod is silent, stays starting (configuring) or fails "never ran", or when a sendspin node in the pod is not found or loses its pairing, or when upgrading the volume's ear.conf past the graph { } and rate-unit change (the pod runs no nodes, or exits on --graph-frequency).
---

# EAR on Talos with a MOTU 24Ao

EAR has run on the owner's node (hawking) since 2026-10-31; the rest is what the docs (`docs/engine.md#sound-cards`,
`docs/engine.md#realtime-priority`) and the sources below say the node and pod must give. Each step marked
**[check]** is still to be confirmed on the cluster; correct this skill when it is.

The owner (2026-10-30): "Motu 24ao. Talos 1.13.x I think maybe 1.14.x". The Talos nodes' CPU is
not known yet, so the image is published for amd64 only (`ghcr.io/mcurcio/ear`;
`docs/architecture.md#the-container-image`). The package is public: anonymous pulls work, so the cluster needs no pull
secret.

## 1. The node: the USB audio driver

The MOTU 24Ao is USB class compliant: the kernel's `snd-usb-audio` drives it, with no driver in
the image or on the node. MOTU ships its own driver only for macOS, and that driver streams on the
MOTU's vendor interfaces, not the class-compliant ones; the comparison with the unofficial Linux
driver `snd-usb-motu` is in `docs/research/motu-24ao-drivers/`. The Talos kernel builds it as a module
(`CONFIG_SND_USB_AUDIO=m` in siderolabs/pkgs `kernel/build/config-amd64`), but Talos's own image
carries only the modules in `hack/modules-amd64.txt`, which has no sound modules (checked at
v1.13.10). The ALSA modules (`snd`, `snd-pcm`, `snd-timer`, `snd-usb-audio` and their helpers)
ship only in the **`siderolabs/usb-audio-drivers` system extension**, so:

1. Add `usb-audio-drivers` to the node's extensions (its Image Factory installer). That changes
   the install image, so it lands through an OS upgrade, not a config apply.
2. Load the module in the machine config. `/proc/modules` lists it as `snd_usb_audio` (kmod
   treats `-` and `_` alike), so a check that compares names must normalise them:

```yaml
machine:
  kernel:
    modules:
      - name: snd_usb_audio
```

Then the card appears in `/proc/asound/cards` (seen 2026-10-31): `talosctl -n <node> read
/proc/asound/cards`. Until a sound module loads there is no `/dev/snd` at all, so a pod that
asks a device plugin for it stays Pending.

## 2. The card id

A `card` node names its card only by its ALSA card id: the name in brackets in
`/proc/asound/cards` (letters, digits, `-` and `_`, at most 15 characters; `CardId` in
`src/core/sound_card.cpp`), never an index or an ALSA device string. EAR opens
`hw:CARD=<id>,DEV=0`. The MOTU 24Ao's id is `D24Ao` (seen on the owner's node 2026-10-31:
`MOTU 24Ao at usb-0000:00:14.0-7, high speed`, 24 channels each way, 44.1 to 192 kHz).

```conf
node card motu { card <card id>; playback 24; }
```

`playback` and `capture` are channel counts (0 to 64, at least one above 0); the ports are
`play.1`..`play.N` (inputs) and `capture.1`..`capture.M` (outputs). The card's rate is
always the graph frequency (`graph { rate 48khz; ... }` in `ear.conf`), never a setting.

## 3. The MOTU's own settings

- Set the card's sample rate and USB channel count in MOTU's web app, to the graph frequency
  and the channel count the node asks for. EAR cannot change them.
- The kernel validates every rate a MOTU (USB vendor 0x07fd) reports
  (`QUIRK_FLAG_VALIDATE_RATES`, `sound/usb/quirks.c`; the quirk flags below turn it off), so a graph frequency the card is not set
  to fails the card when it opens: the node is `failed engine-refused: <detail>`, and every other
  node keeps running on ear's internal clock, which drives the graph whether or not a card works. Fix the rate, then retry the card node with `node.restart` (or `node.start`:
  `node scripts/rpc-call.mjs ws://<address>:7777/rpc node.restart '{"name":"motu"}' --until 'motu running'`), which opens the
  card afresh; no retry happens by itself (slice 8). An update with the same settings changes nothing. A
  `node.update` on the card node has hung until the pod was restarted (issue #18): if a restart hangs the
  same way, ear's main-loop watchdog now says so (`ear.main-loop-stalled: no main-loop heartbeat for <ms>
  ms, in <call> motu`), which tells a card blocking ALSA on the main loop from a quiet ear (ALSA's
  open, drop and close run there; opening cards off the main loop is a later slice).
- Power-cycled, the MOTU comes up at 192 kHz unless its rate was set on the device, so check the
  rate after every power-cycle. With the quirk flags below it stays at the rate set (48 kHz seen,
  2026-11-01).
- Quirk flags: `snd_usb_audio` parameter
  `quirk_flags=07fd:0005:!validate_rates|skip_iface_setup|playback_first` (the owner's iac repo
  sets it; `playback_first` added 2026-11-02, mcurcio/iac PR 343). The first two removed the
  kernel's "clock source 1 is not valid" at connect; `playback_first` stops the 8-channel shift
  (below).
  The kernel reads the flags only when it probes the card, so they take effect after a MOTU
  power-cycle or re-plug; a MOTU already attached when the node boots did not get them (seen on
  hawking, kernel 6.18.48, MOTU firmware 1.4.8+102192). The mainline fix that waits for a valid
  clock on 07fd:0005 (commit 320e55722ca4) is not in 6.18.y.
- Class-compliant mode offers 24 channels per direction (the kernel's `stream0` for the card,
  2026-10-31), through one playback altsetting only (interface 1 alt 1, 24 channels
  `S24_3LE`); interfaces 3 to 5 are vendor-only (3: bulk commands, 4 and 5: MOTU's own streaming
  on the same endpoints, whose endpoints Linux drops as duplicates), no use to ALSA. The
  descriptors are in `docs/research/motu-24ao-drivers/hardware/`.
- The 24-channel playback frame came out rotated by 8 (2026-11-01): EAR's `playback_N` was heard
  on MOTU output N-8 (mod 24), so `playback_1` on output 17, `playback_6` on 22, `playback_19` on
  11. EAR's own channel mapping is correct. Linux sends every channel in the right place; the MOTU
  loses its frame boundary depending on how its playback stream starts (2026-11-02, USB traces).
  (`playback_<n>` is the port's name then; it is `play.<n>` since 2026-11-06.)
  The fix is the `playback_first` quirk flag above: with it, 49 of 49 starts mapped channel N to N
  through a loopback on an Ubuntu host (AMD xHCI, kernel 7.0), 9 of them after a MOTU power-on;
  without it, 0 of 12 were clean. It is not yet confirmed on a Talos node or through EAR, so check
  the map after every restart of the card (section 5). Record:
  `docs/research/motu-24ao-drivers/hardware/observations-2026-11-02.md`.
- Each start of the card's streams loses about 2 s of audio: the MOTU returns silence that long
  (Apple's driver too). In EAR it happens when a `card` node starts or restarts, not per sound.
- Keep the MOTU's Ethernet port unplugged. On 2026-11-02 the whole unit (USB and front panel)
  froze minutes to about 45 minutes after connecting, every time Ethernet was in, on Linux and
  macOS hosts alike. With Ethernet out it ran over 3 hours and about 60 driver loads without a
  freeze. A frozen MOTU still looks attached on USB and needs a power-cycle.

## 4. The namespace and the pod

- Talos enforces Pod Security "baseline" by default, which forbids adding SYS_NICE (it is not in
  baseline's allowed capabilities). Pod Security never sees device-plugin devices, so SYS_NICE is
  the only gap. An admission policy can only refuse, never admit, so the namespace must enforce
  `privileged`; on its own that lets anything that can patch a deployment there get root on the
  node. The narrower way (what the owner's iac repo does) is `privileged` enforced, `baseline`
  audited and warned, and a ValidatingAdmissionPolicy on the namespace that re-imposes baseline
  plus SYS_NICE; it holds only while the policy and its binding exist. Port 7777 has no login.
- `/dev/snd` reaches the container through a device plugin such as squat/generic-device-plugin
  (a device group with path `/dev/snd`, then a resource limit on that group in the pod), or a
  privileged container. Do not make the pod request the card's own USB device (a USB group for
  07fd:0005): the plugin advertises it only while the MOTU is on, so with it off the pod stays
  Pending and a deploy waiting on it fails. Every node that loads `snd_usb_audio` has `/dev/snd`
  (`snd-timer` makes `/dev/snd/timer` with no card), so the owner's iac repo puts the pod on the
  MOTU's node by a node label (`nodeSelector` `node.iac/device.motu-24ao: "true"`; its node
  inventory sets the label on the node the card is attached to) and requests only the `/dev/snd`
  group. With the MOTU off, EAR still starts: the card node is `failed engine-refused` and the
  control port answers. A container sees only the device files present when it starts, so after
  the MOTU comes on, restart the pod (`kubectl rollout restart -n audio deploy/ear`); no `node.update`
  or `node.restart` can see a card the container never got. To keep the card off across restarts
  (the MOTU switched off for a season), Disable its node (`node.disable`, or `disabled true;` in its
  braces in `ear.conf`): a disabled node opens nothing and fails nothing at boot. `node.stop` is
  silence for now only: it keeps the card open and is forgotten when the pod restarts.
- Out of the box `/dev/snd/*` is `root:root 0600` on the node (seen on the owner's node
  2026-10-31): the kernel gives sound devices no mode. Talos runs udevd with
  `--resolve-names=never`, so systemd's stock `SUBSYSTEM=="sound", GROUP="audio"` rule (a named
  group) does nothing, and so does a numeric one: Talos has no `/etc/group`, udevd fails to
  resolve even `GROUP="2029"` (its log: `Failed to resolve group '2029', ignoring: Unknown
  group`) and drops the whole rule line, `MODE` included (seen on the owner's node 2026-10-31).
  No udev group rule can work on Talos; only a world-open `MODE="0666"` would, so do not use
  udev. Instead turn on
  containerd's CRI option `device_ownership_from_security_context` on every node: a device a
  device plugin gives a container is then created in it owned by the container's `runAsUser`
  and `runAsGroup` (each only when set and non-zero), with the host's mode, so the owner's `rw`
  is enough. On Talos it is one `machine.files` entry, applied only on a reboot:

```yaml
machine:
  files:
    - path: /etc/cri/conf.d/20-customization.part
      op: create
      content: |
        [plugins."io.containerd.cri.v1.runtime"]
          device_ownership_from_security_context = true
```

  Then set `securityContext.runAsUser: 10001` and `runAsGroup: 10001` in the pod (the image's
  `ear` user), plus `runAsNonRoot: true`; no supplemental group. Without `runAsUser` the kubelet
  passes the image's numeric user but never its group, so set both. The option does not touch a
  privileged container's devices or a hostPath mount of `/dev`. Check it with `talosctl -n
  <node> read /etc/cri/conf.d/cri.toml` (the key under `io.containerd.cri.v1.runtime`), then
  `ls -ln /dev/snd` in the container shows `10001 10001` **[check]**.
- A wrong owner shows up as ALSA's `Cannot get card index for <id>` and `playback open failed:
  No such device`, not as a permission error: alsa-lib cannot open the card's control device.
- Realtime priority: `securityContext.capabilities.add: [SYS_NICE]`, and leave
  `allowPrivilegeEscalation` unset or true. The binary carries CAP_SYS_NICE as a permitted-only
  file capability that EAR makes effective at boot; `allowPrivilegeEscalation: false` sets
  no_new_privs, which ignores file capabilities, and a capability added without the file
  capability never reaches a non-root process (`pipewire-engine` skill, "Realtime priority").
- The graph file: `ear.conf` lives in the graph file folder, `/var/lib/ear/config` (the image's
  default `--config-dir`), and ear writes every accepted editor edit back into it. So mount a
  writable volume there that uid 10001 can write (a PersistentVolumeClaim with `fsGroup: 10001`,
  for example): a ConfigMap mount is read-only, so edits would run but never be saved, and a
  restart would lose them. ear writes by renaming a new file over `ear.conf` inside the folder,
  so the folder itself must be writable, not just the file, and `ear.conf` must be a regular
  file, not a symlink (a ConfigMap's files are symlinks, so mounting one into the folder does not
  work either).
- `fsGroup` and the `sendspin` key files: `sendspin-<node>.key` lives in the same folder, and holds
  a private key and pairing secrets. `fsGroup` widens every file to 0660 on every mount. ear narrows
  it back to 0600 at each node's start, on its own, so this is no failure. Between the mount and that start, and for a key file
  whose node was removed (`node.remove` leaves it), it stays 0660: the group, 10001, is ear's. To
  wipe an identity, delete the file and restart the node.
- Seeding `ear.conf`: either an initContainer that copies it from a ConfigMap into the volume
  only when it is missing (`[ -e /cfg/ear.conf ] || cp /seed/ear.conf /cfg/`, so later edits are
  kept), or nothing: a first boot without `ear.conf` runs no nodes, and the editor's text pane
  writes one with Save. Copying the ConfigMap over it on every start throws away every edit
  made in the editor. The folder may not be, or be inside, the recordings folder (exit 1).
- Port 7777 exposed. A browser reaching it by a DNS name needs `--allowed-host <name>` in the
  container's args (`control-port` skill). Args replace the image's default command, so they
  must also carry `--config-dir /var/lib/ear/config --recordings-dir /var/lib/ear/recordings`.
- Stopping: ear writes the last accepted version of `ear.conf` as it stops, after the
  recordings are finished, and waits on the disk with no deadline: a hung volume delays stop
  without limit on ear's side, until Kubernetes kills the pod after
  `terminationGracePeriodSeconds` and the unwritten edits are lost. A failed final
  write is printed (`graph-file write-failed <detail>`) and reaches only the container's log:
  check `kubectl logs --previous` after a restart if an edit seems lost.
- Recordings: `file` nodes write only beneath `/var/lib/ear/recordings` (their `path` is relative
  to it); mount a volume there writable by uid 10001 (for example `fsGroup: 10001`) to keep them.

### Upgrading past the `graph { }` change

The release that moved the graph's settings into a `graph { }` block and put units on every rate
(`docs/graph-file.md#directives`, `#rates`) refuses older files and keeps no compatibility. The
volume's `ear.conf` is seeded only when missing and ear has rewritten it since, so it is still in
the old form after the image is upgraded: edit it once, by hand.

Before:

```conf
graph_frequency 48000;
block_size 256;
node rtp fpp { address "0.0.0.0"; port 5004; format "L24"; channels 2; rate 48000; }
```

After:

```conf
graph {
    rate 48khz;
    block_size 256;
}
node rtp fpp { address "0.0.0.0"; port 5004; format "L24"; channels 2; rate 48khz; }
```

`44100` is `44.1khz`, `96000` is `96khz`; `48000hz` also works. Every `rtp` node's `rate` needs
its unit; an `rtp` node without `rate` needs nothing. In the same rollout, replace
`--graph-frequency 48000` in the container's args, if it is there, with `--rate 48khz`.

Three ways to make the edit, from simplest:

1. **The editor's text pane.** ear stays up on an old file (below), so open port 7777, fix the
   text and Save (`graphFile.set` writes the volume's file). If the file's rate or block size
   differs from the flags the engine took, the pane says it applies at restart: restart the pod
   (`kubectl rollout restart -n audio deploy/ear`).
2. **kubectl, through the running container.** The run image has `sh`, `cat` and `mv` but no
   editor, so edit a copy locally and write it back by renaming inside the folder (ear reads a
   renamed file once, whole; never truncate `ear.conf` in place):

   ```bash
   kubectl exec -n audio deploy/ear -- cat /var/lib/ear/config/ear.conf > ear.conf
   # edit ear.conf as above
   kubectl exec -i -n audio deploy/ear -- sh -c \
     'cat > /var/lib/ear/config/ear.conf.new && mv /var/lib/ear/config/ear.conf.new /var/lib/ear/config/ear.conf' < ear.conf
   kubectl rollout restart -n audio deploy/ear   # only when the rate or block size changed
   ```

3. **Re-seed.** Put the new-form file in the seed ConfigMap, delete the volume's copy
   (`kubectl exec -n audio deploy/ear -- rm /var/lib/ear/config/ear.conf`) and restart the pod, so
   the initContainer copies it again. This throws away every edit made in the editor since the
   first seed.

**If the step is skipped**, the pod still starts and ear stays up, but runs no nodes: the file has
errors, so nothing applies. `kubectl logs -n audio deploy/ear` shows the refusals, one record each,
for example:

```
graph-file error 1:1 ear/unknown-directive: graph_frequency is no longer a directive: graph settings moved: write graph { rate 48khz; block_size 256; }
graph-file error 2:1 ear/unknown-directive: block_size is no longer a directive: graph settings moved: write graph { rate 48khz; block_size 256; }
graph-file error 3:77 ear/bad-rate: rate: 48000 has no unit: write 48000hz or 48khz
```

The engine falls back to `--rate` and `--block-size` (48khz and 256 unless the args say
otherwise), `graph.get` lists no nodes, and the editor's pane shows the same diagnostics. Args that
still carry `--graph-frequency` are worse: ear exits 1 at once with `ear: --graph-frequency is gone:
write --rate with a unit, such as --rate 48khz`, and the pod goes into CrashLoopBackOff until the
args change.

## 5. Checks on the hardware

1. Realtime: `ear`'s boot line says `engine: realtime SCHED_FIFO <n>`, and `graph.get` has
   `"realtime": true` (`node scripts/rpc-call.mjs ws://<node>:7777/rpc graph.get`). The image
   smoke proved this in a container with `--cap-add SYS_NICE`; the pod is not proven.
2. The card follows: the card node is `running`, its routes are `linked`, and audio routed to
   `motu:play.<n>` comes out of that output only. Play into `play.1` and listen on
   output 1: hearing it on output 17 is the rotation in section 3. CI checks the same path with
   a test ALSA card (alsa-lib's file and null PCMs), not the MOTU. `running` alone is not proof
   the card is there: a USB card that vanished left the node `running` (issue #17).
3. A wrong rate fails only the card node (section 3), and unplugging the card or switching it
   off fails it too while the rest keeps running. There is no hotplug, and in a pod the card's
   device files are fixed when the container starts: plug it back or switch it on, then restart
   the pod (`kubectl rollout restart -n audio deploy/ear`), not just `node.update` or `node.restart`.
   After the node reboots, the MOTU may fail USB enumeration (`-71` in the kernel log) until it
   is power-cycled; nothing in the node's roll notices the card is missing.
4. The MOTU freezes on Linux (2026-10-31 to 2026-11-01, cause unknown): front panel and web
   app (http://10.0.40.1) dead, only a power-cycle recovers it. Seen with EAR playing; with USB
   connected and idle, no EAR, after 35 s to 2.5 min; and with USB unplugged (power and network
   only) after about 12 min, 18 min at 48 kHz, 2 min at 44.1 kHz under heavy polling of its web
   app, and 32 min with only pings. The owner ran it on a Mac on the same network "for months at
   a time without issue"; the next test is the Mac over USB for about an hour. To notice a
   freeze, run a watcher that pings the MOTU and exits after 2 missed pings (EAR does not notice:
   check 2).
5. Latency (the owner: "under 10ms is obviously ideal"): one cycle is `block_size /
   rate` (256 at 48 kHz is 5.3 ms). One way to measure the round trip: a loopback
   cable from a MOTU output to an input it captures (the 24Ao is mostly outputs, so which input
   is **[check]**), a capture card node and a `file` node recording both; EAR has no latency
   tool yet, so the delay is read from the recording **[not designed yet]**.

6. **[check]** A `sendspin` node's sync on the MOTU. Since slice 7 the graph always runs on ear's
   internal clock and the card follows it (`docs/engine.md#the-internal-clock`), so the
   protocol's 1 ms bound at the block holds as on the dummy driver; CI proves it through a
   following test card, dated by that card's latency (the test card is clockless, so it shows no
   drift). The node dates what it played by the latency downstream of its `from` outputs, which
   the card block carries back from the MOTU's playback node (one quantum plus its headroom); no
   real card has reported it yet. On the hardware: Music Assistant playing, how far the node's
   `from.*` outputs sit from what the library's time filter predicts, no repeated corrections in
   ear's log, and past the block: a test tone on another Sendspin device and on the MOTU, recorded
   together, within the protocol's bound once `output_delay` is 0. NOT RUN.
7. **[check]** The MOTU as a follower (slice 7; NOT RUN). The card no longer drives: PipeWire's ALSA
   node runs a delay-locked loop on its buffer against the internal clock and steers the adapter's
   resampler, so every MOTU channel is resampled all the time. Check, and write down what you see:
   - Drift tracking: the rate settles within a few seconds of the card starting and stays there.
     With `PIPEWIRE_DEBUG="2,spa.alsa:4"` (in the pod's env) the ALSA node logs its rate match and
     delay; the correction should stay within the MOTU's crystal's tolerance (tens of ppm), not
     climb.
   - Follower resyncs at a 256-frame quantum: a USB card reports its pointer at URB granularity, so
     the follower's "delay too big" resync can fire and click. Count them in the log over an hour of
     playback. If they come, raise `api.alsa.headroom` (frames of extra buffer) or the period size
     (`api.alsa.period-size`) on the card's adapter; both are adapter properties, so a change is a
     code change to `AlsaCardBody` (`sound_cards.cpp`), not a graph setting yet.
   - The quantum limit at the block size (`docs/engine.md#the-quantum-guard`; CI checks only the
     test card): the MOTU's buffer must be what it was before the guard. Run with
     `PIPEWIRE_DEBUG="2,spa.alsa:4"` and read the playback and capture lines `buffer frames ...,
     period frames ..., periods ... batch:1`: a batch card made with `api.alsa.period-num` gets
     `min(hardware maximum, 32768)` frames at a 256 block in half-block periods, never 1024. Compare
     them, and the xruns and follower resyncs over an hour, with an image before the guard; the
     playback node's reported latency (one quantum plus headroom) should not change. If it does not
     hold, the fallback the design names is the limit back to `max(block, 8192)` with the clock
     properties still refused. NOT RUN.
   - The card's readiness deadline: the MOTU must open, run and link within 10 s of its node's
     configure (`docs/engine.md#readiness-deadlines`), or the node fails `engine-refused: the card
     never started: <id>`. Time it from ear's records on a cold start. NOT RUN.
   - CPU: 24 resampled channels at 48 kHz on the node's CPU (`top -H` in the pod: the data thread),
     with and without the card linked.
   - Playback and capture of one card follow with two loops: their relative latency jitters by
     tens of samples; measure it with the loopback of check 5 if a round trip matters.
   - The host clock: the internal clock tracks `CLOCK_MONOTONIC_RAW` through a loop that corrects
     at most 1 ms a cycle, so a host whose NTP slews `CLOCK_MONOTONIC` hard is followed only up to
     that. Fallback if it is not: bound slewing in the node's chrony (`maxslewrate 500`, in ppm;
     Talos sets time sync in its machine config, `machine.time`), so no follower loop has to chase
     chrony's default of up to 83,333 ppm.
   - A frozen MOTU (check 4) no longer stalls the graph: the blocks keep the internal clock. Nothing
     in the graph shows a frozen follower yet (the follower watchdog is slice 8), so watch the
     card's own state: `cat /proc/asound/<card>/pcm0p/sub0/status` on the node, a `hw_ptr` that
     stops moving while `state: RUNNING`.
   - A wedged MOTU and ear's main loop: record whether a frozen or unplugged MOTU blocks
     `node.stop`, `node.restart` or the pod's stop. If ALSA's drop or close blocks, ear's log shows
     `ear.main-loop-stalled ... in destroy engine companion motu` (or `make engine companion motu`) and no
     `ear.main-loop-resumed`; if the data loop stops answering, `ear.barrier-overdue` and then
     `ear.data-loop-wedged`. Either answers the open question of `docs/design/cards-and-clock.md`
     (whether a wedged card blocks those calls), which decides how cards are opened off the main
     loop.

## 6. FPP's audio over RTP

The owner runs FPP (Falcon Player) in the same cluster and wants its audio as a source; what FPP
sends (format, rate, channels, address) is not known yet (`docs/node-types.md#rtp`), so
everything here is **[check]**. An `rtp` node (`docs/node-types.md#rtp`) receives one stream:

```conf
node rtp fpp { address "0.0.0.0"; port 5004; format "L16"; channels 2; }
```

`address` is `0.0.0.0` (any sender, unicast) or an IPv4 multicast group; a unicast address is
refused (`pipewire-engine` skill, "RTP receivers"). `rate` defaults to the graph frequency and a
sender at another rate is resampled; about 100 ms of latency is accepted for this source (the
owner, 2026-10-30: "If 100ms is the best rep can do, then so be it").

- **Unicast (the simple way).** Use `address "0.0.0.0";`, declare the port in the pod
  (`containerPort: 5004, protocol: UDP`) and put a Service in front of it with a UDP port whose
  `targetPort` is the node's `port`. FPP sends to the Service's address. The Service may rewrite
  the sender's address and port on the way (kube-proxy NAT); `0.0.0.0` hears any sender, so that
  does not matter. The image's `EXPOSE` names 7777 and 8928; that is a label, not a limit. A sender
  outside the cluster needs a NodePort or LoadBalancer Service, still UDP.
- **Multicast.** A Service carries unicast only, and pod networks usually do not carry multicast
  between pods. A multicast `address` needs the pod on the node's own network
  (`hostNetwork: true`) with FPP on the same network segment (PipeWire's sender uses TTL 1 by
  default, `net.ttl`). Baseline Pod Security forbids `hostNetwork`, so the ValidatingAdmissionPolicy
  of section 4 must allow it too, and port 7777 and the RTP port are then the node's own ports.
  On the node's network another program may listen to the same group and port (a monitor, VLC):
  one that binds with `SO_REUSEADDR` shares the port and the node still starts; one that holds it
  alone fails the node `start-failed: cannot bind UDP port <port>: Address already in use`. Two
  `rtp` nodes on one port are refused whatever their groups. Not tested anywhere yet, not even in
  CI.
- **A busy port.** If another program on the same network (the node's, with `hostNetwork`)
  already holds the UDP port (on `0.0.0.0`, any holder; on a group, one not sharing it by
  `SO_REUSEADDR`), the node fails at once, `start-failed: cannot bind UDP port <port>:
  Address already in use`; a second `rtp` node on it fails `... held by node <n>`. Pick another
  port, or free it and `node.restart` the node. A program that takes the port in the moment
  between ear's check and PipeWire's bind leaves the node `configuring` waiting for `fpp.rtp`,
  then failing after 5 s `engine-refused: the RTP receiver never ran (UDP port <port> held?)`.
- **The sender: FPP's own PipeWire, `libpipewire-module-rtp-sink`** (if FPP's audio runs through
  PipeWire **[check]**). Its arguments must match the node: `destination.ip` = the Service's
  address (its default, `224.0.0.56`, is multicast), `destination.port` = the node's `port` (its
  default is a random port from 46000), `audio.format` = `S16BE` for `L16` (its default) or
  `S24BE` for `L24`, `audio.channels` (default 2) and `audio.rate` (default 48000) as the node's
  `channels` and `rate` (in `ear.conf` the rate takes its unit: `rate 48khz;`). Leave `net.mtu` at 1280 or below: ear's receiver reads at most 1280
  bytes a packet and garbles longer ones; the sender sizes its packets to its own `net.mtu`. Its
  SSRC changes when it restarts; ear hears it again. Prefer an IP literal for `destination.ip`
  (a name is looked up while the sender's loop waits).
- **No login.** Anyone who can reach the UDP port can play audio into the graph, as anyone who
  reaches port 7777 can edit it.
- **Checks.** The rtp node is `running` and its routes `linked`; `levels.subscribe` shows
  non-zero peaks on `fpp:stream.1` while FPP plays (with no packets the node is still `running`,
  with peak 0: ear has no "receiving" status yet). Short gaps of silence come from packets lost
  or out of order: the receiver drops its buffer on any sequence break and refills it
  (`pipewire-engine` skill).

## 7. A `sendspin` node in the pod

Music Assistant's audio (`sendspin` skill). All **[check]**: not run in a pod yet.

- **Dialling is the simple one.** `node sendspin ma { server "ws://<music assistant>:8927/sendspin"; }`
  only needs the pod to reach that address. A host name is looked up from inside the pod.
- **Listening** needs the server to reach the pod's port, 8928 by default (the image exposes it).
  **A pod on an overlay network cannot advertise**: mDNS goes no further than its own network, so
  Music Assistant will not find it. Either give the pod `hostNetwork: true` (as for multicast RTP,
  section 6: the policy of section 4 must allow it), or give Music Assistant the URL by hand,
  `ws://<address>:<port>/sendspin`. `says advertise-failed <detail>` means no mDNS socket opened;
  no such line does not mean anybody heard it.
- **With `hostNetwork`:** 7777, 8928 and mDNS's 5353 (UDP, shared, `SO_REUSEPORT`) are the node's own, on
  every one of its interfaces, since a listening node has no address setting: filter 8928 and 5353 to the
  network Music Assistant is on. A peer on 8928 can still hold the node from an unpaired server by finishing the published handshake, though no longer with silent sockets, and a flood can keep servers out while it lasts (`sendspin` skill), so
  on a network you do not trust, dial instead of listening.
  The node advertises on one interface, the one the kernel routes 224.0.0.251 through, and the
  address of that interface, read once when the sendspin node starts, so on a machine with several
  networks check that route, and restart the sendspin node if the address changes. Music Assistant
  has to be on that same network segment to hear it: on an overlay itself it hears nothing, and
  needs the URL by hand. A rolling update's new pod would find these ports held by the old one, so
  use the `Recreate` strategy.
- **Give the pod a memory limit.** One connection holds at most about 1 MiB before it proves anything, and a flood runs up to 64 sockets and threads at once, but the
  library's parsers run in the process with every audio node; a limit keeps whatever else goes wrong in this pod.
- **Pair once.** The node prints `says pairing-code <123-456>` when Music Assistant starts an attempt;
  the code reaches only standard output (`kubectl logs -n audio deploy/ear | grep says`), not the
  editor. Type it there. The pairing lives in the key file, so the graph file volume must persist
  (section 4), or every restart asks again.
- **Stop.** The node's goodbye runs on its own thread and is short, but an inbound half-open socket can
  take up to 3 s; it all has to fit `terminationGracePeriodSeconds` with the last `ear.conf` write.
