---
name: ear-on-talos
description: Running EAR's run image on a Talos Linux Kubernetes node with a USB sound card (the owner's MOTU 24Ao): loading snd-usb-audio in the machine config, the privileged namespace, /dev/snd and the audio group, CAP_SYS_NICE for realtime priority, setting the MOTU's rate and channel count, finding the card id, and the checks to run on the hardware. Use when deploying EAR to the cluster, when a card node fails on the hardware, or when graph.get says "realtime": false in the pod.
---

# EAR on Talos with a MOTU 24Ao

Nothing here has run on the hardware yet: it is what the design (`docs/design/batch2.md`
sections 5 and 7) and the sources below say the node and pod must give. Each step marked
**[check]** is still to be confirmed on the cluster; correct this skill when it is.

The owner (2026-10-30): "Motu 24ao. Talos 1.13.x I think maybe 1.14.x". The Talos nodes' CPU is
not known yet, so the image is published for amd64 only (`ghcr.io/mcurcio/ear`, section 7 of
the design page; it is private until the owner makes it public, so the cluster needs a pull
secret).

## 1. The node: the USB audio driver

The MOTU 24Ao is USB class compliant: the kernel's `snd-usb-audio` drives it; no MOTU driver
exists or is needed, in the image or on the node. Talos builds `snd-usb-audio` as a module from
1.12 (`CONFIG_SND_USB_AUDIO=m` in siderolabs/pkgs `kernel/build/config-amd64` on release-1.12,
1.13 and 1.14; not set on 1.11). Talos loads only the modules its machine config names, so add
to each audio node's machine config:

```yaml
machine:
  kernel:
    modules:
      - name: snd-usb-audio
```

Then the card appears in `/proc/asound/cards` **[check]**: `talosctl -n <node> read
/proc/asound/cards`.

## 2. The card id

A `card` node names its card only by its ALSA card id: the name in brackets in
`/proc/asound/cards` (letters, digits, `-` and `_`, at most 15 characters; `CardId` in
`src/core/sound_card.cpp`), never an index or an ALSA device string. EAR opens
`hw:CARD=<id>,DEV=0`. The MOTU's id is whatever the kernel gives it **[check]**.

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
- Users report that class-compliant mode offers at most 24 channels per direction **[check]**.

## 4. The namespace and the pod

- Talos enforces Pod Security "baseline" by default, which forbids adding SYS_NICE and mounting
  host devices, so label EAR's namespace `pod-security.kubernetes.io/enforce: privileged`. Cost:
  that widens what a compromised EAR could do (the card setting takes only a card id, and the
  control port checks Host, for this reason). Port 7777 has no login.
- `/dev/snd` reaches the container through a device plugin such as squat/generic-device-plugin
  (a device group with path `/dev/snd`, then a resource limit on that group in the pod), or a
  privileged container.
- The image runs as uid 10001, which must be in the group that owns `/dev/snd/*` on the node:
  add that group's id to `securityContext.supplementalGroups` **[check: read it with `talosctl
  -n <node> list --long /dev/snd`]**.
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
