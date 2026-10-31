---
name: ear-on-talos
description: Running EAR's run image on a Talos Linux Kubernetes node with a USB sound card (the owner's MOTU 24Ao): the usb-audio-drivers extension and loading snd_usb_audio, Pod Security and SYS_NICE, /dev/snd and containerd's device ownership, CAP_SYS_NICE for realtime priority, setting the MOTU's rate and channel count, finding the card id, the checks to run on the hardware, and getting FPP's RTP audio into the pod (a UDP Service for unicast, hostNetwork for multicast, PipeWire's module-rtp-sink as the sender). Use when deploying EAR to the cluster, when a card node fails on the hardware, when graph.get says "realtime": false in the pod, or when an rtp node in the pod is silent or stays starting.
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

```json
{ "name": "motu", "type": "card", "settings": { "card": "<card id>", "playback": 24 } }
```

`playback` and `capture` are channel counts (0 to 64, at least one above 0); the ports are
`playback_1`..`playback_N` (inputs) and `capture_1`..`capture_M` (outputs). The card's rate is
always the graph frequency (`graphFrequency` in `ear.json`), never a setting.

## 3. The MOTU's own settings

- Set the card's sample rate and USB channel count in MOTU's web app, to the graph frequency
  and the channel count the node asks for. EAR cannot change them.
- The kernel validates every rate a MOTU (USB vendor 0x07fd) reports
  (`QUIRK_FLAG_VALIDATE_RATES`, `sound/usb/quirks.c`), so a graph frequency the card is not set
  to fails the card when it opens: the node is `failed engine-refused: <detail>`, and every other
  node keeps running on the dummy driver. Fix the rate, then `node.update` the card node (no
  retry happens by itself).
- Class-compliant mode offers 24 channels per direction (the kernel's `stream0` for the card,
  2026-10-31).

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
  privileged container.
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
- `ear.json` mounted at `/etc/ear/ear.json` (the image's default `--config`); port 7777 exposed.
  A browser reaching it by a DNS name needs `--allowed-host <name>` in the container's args
  (`control-port` skill). Args replace the image's default command, so they must also carry
  `--config /etc/ear/ear.json --recordings-dir /var/lib/ear/recordings`.
- Recordings: `file` nodes write only beneath `/var/lib/ear/recordings` (their `path` is relative
  to it); mount a volume there writable by uid 10001 (for example `fsGroup: 10001`) to keep them.

## 5. Checks on the hardware

1. Realtime: `ear`'s boot line says `engine: realtime SCHED_FIFO <n>`, and `graph.get` has
   `"realtime": true` (`node scripts/rpc-call.mjs ws://<node>:7777/rpc graph.get`). The image
   smoke proved this in a container with `--cap-add SYS_NICE`; the pod is not proven.
2. The card drives: the card node is `running`, its routes are `linked`, and audio routed to
   `motu:playback_<n>` comes out of that output only. CI checks the same path with a test ALSA
   card (alsa-lib's file and null PCMs), not the MOTU.
3. A wrong rate fails only the card node (section 3), and unplugging the card fails it too while
   the rest keeps running (no hotplug: plug it back and `node.update`).
4. Latency (R4, the owner: "under 10ms is obviously ideal"): one cycle is `blockSize /
   graphFrequency` (256 at 48 kHz is 5.3 ms). One way to measure the round trip: a loopback
   cable from a MOTU output to an input it captures (the 24Ao is mostly outputs, so which input
   is **[check]**), a capture card node and a `file` node recording both; EAR has no latency
   tool yet, so the delay is read from the recording **[not designed yet]**.

## 6. FPP's audio over RTP

The owner runs FPP (Falcon Player) in the same cluster and wants its audio as a source; what FPP
sends (format, rate, channels, address) is not known yet (`docs/design/rtp.md` section 1), so
everything here is **[check]**. An `rtp` node (`AGENTS.md`, graph file) receives one stream:

```json
{ "name": "fpp", "type": "rtp",
  "settings": { "address": "0.0.0.0", "port": 5004, "format": "L16", "channels": 2 } }
```

`address` is `0.0.0.0` (any sender, unicast) or an IPv4 multicast group; a unicast address is
refused (`pipewire-engine` skill, "RTP receivers"). `rate` defaults to the graph frequency and a
sender at another rate is resampled; about 100 ms of latency is accepted for this source (the
owner, 2026-10-30: "If 100ms is the best rep can do, then so be it").

- **Unicast (the simple way).** Use `"address": "0.0.0.0"`, declare the port in the pod
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
