---
name: ear-on-talos
description: Running EAR's run image on a Talos Linux Kubernetes node with a USB sound card (the owner's MOTU 24Ao): the usb-audio-drivers extension and loading snd_usb_audio, Pod Security and SYS_NICE, /dev/snd and containerd's device ownership, CAP_SYS_NICE for realtime priority, the writable graph file volume and seeding ear.conf, setting the MOTU's rate and channel count, its Linux quirks (quirk flags, the playback channels rotated by 8, freezes), finding the card id, the checks to run on the hardware, and getting FPP's RTP audio into the pod (a UDP Service for unicast, hostNetwork for multicast, PipeWire's module-rtp-sink as the sender). Use when deploying EAR to the cluster, when a card node fails on the hardware, when graph.get says "realtime": false in the pod, or when an rtp node in the pod is silent or stays starting.
---

# EAR on Talos with a MOTU 24Ao

EAR has run on the owner's node (hawking) since 2026-10-31; the rest is what the design (`docs/design/batch2.md`
sections 5 and 7) and the sources below say the node and pod must give. Each step marked
**[check]** is still to be confirmed on the cluster; correct this skill when it is.

The owner (2026-10-30): "Motu 24ao. Talos 1.13.x I think maybe 1.14.x". The Talos nodes' CPU is
not known yet, so the image is published for amd64 only (`ghcr.io/mcurcio/ear`, section 7 of
the design page). The package is public: anonymous pulls work, so the cluster needs no pull
secret.

## 1. The node: the USB audio driver

The MOTU 24Ao is USB class compliant: the kernel's `snd-usb-audio` drives it; no MOTU driver
exists or is needed, in the image or on the node. The Talos kernel builds it as a module
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
`playback_1`..`playback_N` (inputs) and `capture_1`..`capture_M` (outputs). The card's rate is
always the graph frequency (`graph_frequency` in `ear.conf`), never a setting.

## 3. The MOTU's own settings

- Set the card's sample rate and USB channel count in MOTU's web app, to the graph frequency
  and the channel count the node asks for. EAR cannot change them.
- The kernel validates every rate a MOTU (USB vendor 0x07fd) reports
  (`QUIRK_FLAG_VALIDATE_RATES`, `sound/usb/quirks.c`; the quirk flags below turn it off), so a graph frequency the card is not set
  to fails the card when it opens: the node is `failed engine-refused: <detail>`, and every other
  node keeps running on the dummy driver. Fix the rate, then `node.update` the card node (no
  retry happens by itself; issue #19). A `node.update` on the card node has also hung until the
  pod was restarted (issue #18).
- Power-cycled, the MOTU comes up at 192 kHz unless its rate was set on the device, so check the
  rate after every power-cycle. With the quirk flags below it stays at the rate set (48 kHz seen,
  2026-11-01).
- Quirk flags: `snd_usb_audio` parameter `quirk_flags=07fd:0005:!validate_rates|skip_iface_setup`
  (the owner's iac repo sets it) removed the kernel's "clock source 1 is not valid" at connect.
  The kernel reads the flags only when it probes the card, so they take effect after a MOTU
  power-cycle or re-plug; a MOTU already attached when the node boots did not get them (seen on
  hawking, kernel 6.18.48, MOTU firmware 1.4.8+102192). The mainline fix that waits for a valid
  clock on 07fd:0005 (commit 320e55722ca4) is not in 6.18.y.
- Class-compliant mode offers 24 channels per direction (the kernel's `stream0` for the card,
  2026-10-31), through one playback altsetting only (interface 1 alt 1, 24 channels
  `S24_3LE`); interfaces 3 to 5 are vendor-only, no use to ALSA.
- The 24-channel playback frame came out rotated by 8 (2026-11-01): EAR's `playback_N` was heard
  on MOTU output N-8 (mod 24), so `playback_1` on output 17, `playback_6` on 22, `playback_19` on
  11. EAR's own channel mapping is correct; the shift is below it (driver, USB or the MOTU's
  firmware), a known MOTU AVB issue (Drumfix `motu-avb-usb` issue 16, PipeWire issue 2768).
  Check the map after every restart of the card (section 5).

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
  the MOTU comes on, restart the pod (`kubectl rollout restart -n audio deploy/ear`); `node.update`
  alone cannot see a card the container never got.
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

## 5. Checks on the hardware

1. Realtime: `ear`'s boot line says `engine: realtime SCHED_FIFO <n>`, and `graph.get` has
   `"realtime": true` (`node scripts/rpc-call.mjs ws://<node>:7777/rpc graph.get`). The image
   smoke proved this in a container with `--cap-add SYS_NICE`; the pod is not proven.
2. The card drives: the card node is `running`, its routes are `linked`, and audio routed to
   `motu:playback_<n>` comes out of that output only. Play into `playback_1` and listen on
   output 1: hearing it on output 17 is the rotation in section 3. CI checks the same path with
   a test ALSA card (alsa-lib's file and null PCMs), not the MOTU. `running` alone is not proof
   the card is there: a USB card that vanished left the node `running` (issue #17).
3. A wrong rate fails only the card node (section 3), and unplugging the card or switching it
   off fails it too while the rest keeps running. There is no hotplug, and in a pod the card's
   device files are fixed when the container starts: plug it back or switch it on, then restart
   the pod (`kubectl rollout restart -n audio deploy/ear`), not just `node.update`.
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
5. Latency (R4, the owner: "under 10ms is obviously ideal"): one cycle is `block_size /
   graph_frequency` (256 at 48 kHz is 5.3 ms). One way to measure the round trip: a loopback
   cable from a MOTU output to an input it captures (the 24Ao is mostly outputs, so which input
   is **[check]**), a capture card node and a `file` node recording both; EAR has no latency
   tool yet, so the delay is read from the recording **[not designed yet]**.

## 6. FPP's audio over RTP

The owner runs FPP (Falcon Player) in the same cluster and wants its audio as a source; what FPP
sends (format, rate, channels, address) is not known yet (`docs/design/rtp.md` section 1), so
everything here is **[check]**. An `rtp` node (`AGENTS.md`, graph file) receives one stream:

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
  does not matter. The image's `EXPOSE` names only 7777; that is a label, not a limit. A sender
  outside the cluster needs a NodePort or LoadBalancer Service, still UDP.
- **Multicast.** A Service carries unicast only, and pod networks usually do not carry multicast
  between pods. A multicast `address` needs the pod on the node's own network
  (`hostNetwork: true`) with FPP on the same network segment (PipeWire's sender uses TTL 1 by
  default, `net.ttl`). Baseline Pod Security forbids `hostNetwork`, so the ValidatingAdmissionPolicy
  of section 4 must allow it too, and port 7777 and the RTP port are then the node's own ports.
  Not tested anywhere yet, not even in CI.
- **A busy port.** If another program on the same network (the node's, with `hostNetwork`)
  already holds the UDP port, the node stays `starting` with silent outputs, and only PipeWire's
  log says why (`bind() failed: Address already in use`). Pick another port.
- **The sender: FPP's own PipeWire, `libpipewire-module-rtp-sink`** (if FPP's audio runs through
  PipeWire **[check]**). Its arguments must match the node: `destination.ip` = the Service's
  address (its default, `224.0.0.56`, is multicast), `destination.port` = the node's `port` (its
  default is a random port from 46000), `audio.format` = `S16BE` for `L16` (its default) or
  `S24BE` for `L24`, `audio.channels` (default 2) and `audio.rate` (default 48000) as the node's
  `channels` and `rate`. Leave `net.mtu` at 1280 or below: ear's receiver reads at most 1280
  bytes a packet and garbles longer ones; the sender sizes its packets to its own `net.mtu`. Its
  SSRC changes when it restarts; ear hears it again. Prefer an IP literal for `destination.ip`
  (a name is looked up while the sender's loop waits).
- **No login.** Anyone who can reach the UDP port can play audio into the graph, as anyone who
  reaches port 7777 can edit it.
- **Checks.** The rtp node is `running` and its routes `linked`; `levels.subscribe` shows
  non-zero peaks on `fpp:channel_1` while FPP plays (with no packets the node is still `running`,
  with peak 0: ear has no "receiving" status yet). Short gaps of silence come from packets lost
  or out of order: the receiver drops its buffer on any sequence break and refills it
  (`pipewire-engine` skill).
