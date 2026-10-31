---
name: ear-on-talos
description: Running EAR's run image on a Talos Linux Kubernetes node with a USB sound card (the owner's MOTU 24Ao): the usb-audio-drivers extension and loading snd_usb_audio, Pod Security and SYS_NICE, /dev/snd and its udev group, CAP_SYS_NICE for realtime priority, the writable graph file volume and seeding ear.conf, setting the MOTU's rate and channel count, finding the card id, and the checks to run on the hardware. Use when deploying EAR to the cluster, when a card node fails on the hardware, or when graph.get says "realtime": false in the pod.
---

# EAR on Talos with a MOTU 24Ao

Nothing here has run on the hardware yet: it is what the design (`docs/design/batch2.md`
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

Then the card appears in `/proc/asound/cards` **[check]**: `talosctl -n <node> read
/proc/asound/cards`. Until a sound module loads there is no `/dev/snd` at all, so a pod that
asks a device plugin for it stays Pending.

## 2. The card id

A `card` node names its card only by its ALSA card id: the name in brackets in
`/proc/asound/cards` (letters, digits, `-` and `_`, at most 15 characters; `CardId` in
`src/core/sound_card.cpp`), never an index or an ALSA device string. EAR opens
`hw:CARD=<id>,DEV=0`. The MOTU's id is whatever the kernel gives it **[check]**.

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
  (`QUIRK_FLAG_VALIDATE_RATES`, `sound/usb/quirks.c`), so a graph frequency the card is not set
  to fails the card when it opens: the node is `failed engine-refused: <detail>`, and every other
  node keeps running on the dummy driver. Fix the rate, then `node.update` the card node (no
  retry happens by itself).
- Users report that class-compliant mode offers at most 24 channels per direction **[check]**.

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
- The image runs as uid 10001, which must be in the group that owns `/dev/snd/*` on the node.
  Out of the box no such group exists: the kernel gives sound devices no mode (root:root 0600),
  and Talos runs udevd with `--resolve-names=never`, which ignores systemd's stock
  `SUBSYSTEM=="sound", GROUP="audio"` rule (a named group). containerd copies the host's owner
  and mode into the container. So give the node a udev rule with a numeric group, through
  `machine.udev.rules` (applied only on a reboot), for example `SUBSYSTEM=="sound",
  GROUP="2029", MODE="0660"`, and put the same gid in `securityContext.supplementalGroups`.
  Check it with `talosctl -n <node> list --long /dev/snd`.
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
   `motu:playback_<n>` comes out of that output only. CI checks the same path with a test ALSA
   card (alsa-lib's file and null PCMs), not the MOTU.
3. A wrong rate fails only the card node (section 3), and unplugging the card fails it too while
   the rest keeps running (no hotplug: plug it back and `node.update`).
4. Latency (R4, the owner: "under 10ms is obviously ideal"): one cycle is `blockSize /
   graphFrequency` (256 at 48 kHz is 5.3 ms). One way to measure the round trip: a loopback
   cable from a MOTU output to an input it captures (the 24Ao is mostly outputs, so which input
   is **[check]**), a capture card node and a `file` node recording both; EAR has no latency
   tool yet, so the delay is read from the recording **[not designed yet]**.
