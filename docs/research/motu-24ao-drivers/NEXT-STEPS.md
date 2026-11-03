# MOTU 24Ao on the cluster: where it stands (2026-11-02, 18:15 PST)

A cold start for whoever picks this up. Facts are in `hardware/observations-2026-11-02.md`; why
they matter is in `2-inferences/usb-tests-2026-11-02.md`.

## Done

- **The fix for the 8-channel shift:** snd-usb-audio
  `quirk_flags=07fd:0005:!validate_rates|skip_iface_setup|playback_first`. 49 of 49 loopback starts
  were correct on an Ubuntu host (euclid). It is in mcurcio/iac PR 343, rolled to all five Talos
  nodes and read back on fourier. The PR passes every check; the owner merges it.
- **The same PR fixes the roll tool's sysfs gate,** which read the old `persistent` config during
  `--mode=try`.
- **The MOTU's Ethernet stays unplugged.** Every freeze had it connected.
- **This research record:** mcurcio/ear PR 22.

## Next, in order

1. **Restart EAR's pod** so it gets the MOTU's device files: `kubectl rollout restart -n audio
   deploy/ear`. The MOTU is on hawking, its node in the iac inventory. A live cluster change: the
   owner runs it, or asks for it.
2. **Read the kernel's view on hawking** (10.0.0.8), read-only, one `op run` from the iac repo with
   `TALOS_CLIENT_CONFIGURATION`:
   - `/proc/asound/cards`: expect `D24Ao`
   - `/proc/asound/D24Ao/stream0`
   - `/sys/module/snd_usb_audio/parameters/quirk_flags`: expect the value above
   - `dmesg` for usb-audio
3. **Check EAR opens the card:** `graph.get` on port 7777 (`kubectl port-forward -n audio deploy/ear
   17777:7777`, then `scripts/rpc-call.mjs` with Node 24). The `motu` node should be `running`.
4. **Check the channel mapping through EAR,** in memory only (the deployed build reads a read-only
   ConfigMap, and refuses a channel-count change with `ports-changed`):
   - `node.remove motu`, then `node.add` it with `card D24Ao, playback 24, capture 24`.
   - Add a `white` node and two `file` nodes. Record `capture_1` and `capture_17` on one, and
     `capture_9` on the other.
   - Route the noise to `playback_1`, then `playback_9`, then `playback_17`. Change each file node's
     `path` between steps, so each step gets its own WAV.
   - Copy the WAVs out with `kubectl exec ... cat`, and check which channel carries the noise.
   - Correct: noise only on the matching capture channel. The old fault put it 8 channels away.
   - Clean up with `node.remove` of the test nodes, or a pod restart, which returns to the
     ConfigMap.
   - The MOTU's loopback ("From Computer" N to "To Computer" N) is still set in the device. Its
     output 1 plays noise during the test.
5. **Then:** decide whether the ConfigMap should enable capture, and deploy a current EAR image.
   The deployed `sha-6f36d74` predates `ear.conf`, the `rtp` type and `graphFile.*`.

## Loose ends outside the repos

- **euclid (Ubuntu, 10.0.0.5):** the systemd units `motu-heartbeat`, `motu-watch` and
  `motu-usbmon`, a full USB capture ring in `/var/lib/motu-test/usbmon-full`, and the drivers kept
  from loading on their own by `/etc/modprobe.d/motu-test.conf`. Also passwordless sudo
  (`/etc/sudoers.d/90-matt-nopasswd`) and the SSH key `~/.ssh/euclid` from the owner's Mac.
- **copernicus (Mac Pro, 10.0.0.3):** a `log stream` job (`com.claude.motu-log`), the test scripts
  in `~/motu`, passwordless sudo (`/private/etc/sudoers.d/90-matt-nopasswd`) and the SSH key
  `~/.ssh/copernicus`.
- **Working files** on `/Volumes/Dock/motu-mac-driver/`: the unpacked Mac driver, the pcaps and
  every run directory.
- **The iac worktree** `/Volumes/Dock/Home/Code/iac-worktrees/motu-playback-first`: remove it after
  PR 343 merges.
- **Not pursued:** why Ethernet triggers the freezes; controlling the MOTU over USB (interface 3,
  MOTU's HTTP-over-bulk protocol, `2-inferences/usb-tests-2026-11-02.md` UI7); `snd-usb-motu`.
