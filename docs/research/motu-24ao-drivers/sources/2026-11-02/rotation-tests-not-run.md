# Quirk-flag candidate tests on euclid: NOT RUN (safety pre-condition failed)

Checked 2026-11-02 10:55 PST [18:55 UTC]. No device step was taken: no motu-alive by me, no
module load or unload, no PCM opened, the harness not changed.

## Why nothing ran

The binding rule says: before every device step, no DEAD, all three units active. Neither holds.

- `/var/lib/motu-test/DEAD` exists (written 10:27:39 PST [18:27:39 UTC]):
  `xhci_hcd 0000:00:10.0: WARN: buffer overrun event for slot 1 ep 14 on endpoint`
- `motu-watch` is `failed` since 18:27:39 UTC (it tripped on that line, exit 1).
  `motu-heartbeat` and `motu-usbmon` are active; heartbeat reports ALIVE every 10 s up to 18:55:47 UTC.
- That trip ended run `/var/lib/motu-test/runs/20261102T182732Z-stock-15s` (the plain `stock` run,
  run 2 of the rotation analysis): its `FAILED` reads `stream: waitev exit 10: DEAD: ...`. As
  designed, run-step then touched nothing more, so the state was left as it failed:
  - `snd_usb_audio` is still loaded (no quirk_flags: the plain `stock` load), card 2 `[D24Ao]`
    "MOTU 24Ao at usb-0000:00:10.0-1, high speed" is present. No aplay/arecord process remains.
  - Kernel log before the trip: `usb 5-1: clock source 1 is not valid, cannot use` at 18:27:37.684
    and 18:27:38.579 UTC (probe without `!validate_rates|skip_iface_setup`).
- The MOTU is on the bus at 5-1, 480 Mb/s (enumerated 18:24:54.978 UTC as device 9). enp1s0 is UP:
  that is euclid's own NIC; the MOTU's Ethernet state was not checked (not visible from the host).

## What it needs before any candidate runs (the owner's call)

1. Decide the DEAD trip is understood (the expected 192 kHz/no-quirk IN overflow of the `stock`
   run, rotation analysis section 5(a)) and clear it: remove `/var/lib/motu-test/DEAD`.
2. Unload `snd_usb_audio` (`sudo modprobe -r snd_usb_audio`), so run-step's pre-flight passes.
3. Restart `motu-watch` (it is a transient `systemd-run` unit, so it must be started again the
   way it was first started).
4. Then run-step needs one adaptation, recorded here when made: a way to pass the quirk string
   (`QUIRK`), and, for candidates 1 and 3, playback started before capture (an order option;
   this goes beyond "only the modprobe string" in the brief, since those candidates need it).

## Candidates (all NOT RUN)

| # | quirk_flags (07fd:0005) | Order | Starts | Mapping | Start-up gap | Events | Health |
|---|---|---|---|---|---|---|---|
| 1 | `!validate_rates\|skip_iface_setup\|playback_first` | aplay first | 0/3 | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| 2 | `!validate_rates\|skip_iface_setup\|playback_first` | arecord first | 0/3 | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| 3 | `!validate_rates\|skip_iface_setup` | aplay first | 0/3 | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
| 4 | `!validate_rates\|skip_iface_setup\|playback_first\|iface_delay` | aplay first | 0/3 | NOT RUN | NOT RUN | NOT RUN | NOT RUN |
