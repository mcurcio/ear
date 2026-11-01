# Hardware observations: the MOTU 24Ao on fourier (2026-11-01)

OBSERVED only. Read from the Talos node fourier (10.0.0.6, Talos v1.13.10, kernel 6.18.48-talos) with
`talosctl read` (read-only), at about 21:14 UTC. Inferences from these facts are in
`../2-inferences/hardware.md`; what they change in the comparison is in `../3-comparison/gaps.md`
(section "Update from fourier's descriptors").

Files in this folder:

| File | What |
|---|---|
| `fourier-07fd-0005-descriptors.bin` | raw bytes of `/sys/bus/usb/devices/1-5/descriptors` (309 bytes: device + configuration descriptors) |
| `fourier-07fd-0005-descriptors-decoded.txt` | the same, decoded by a small Python script (field offsets per the USB 2.0 spec) |
| `fourier-07fd-0005-sysfs.txt` | sysfs attributes of the device and of each interface's current alternate setting, and the bound driver |
| `fourier-asound-cards.txt` | `/proc/asound/cards` |
| `fourier-dmesg-usb.txt` | kernel log lines about USB audio, the MOTU and duplicate endpoints |
| `fourier-usb-devices-ls.txt` | `ls /sys/bus/usb/devices` (truncated by talosctl's column format) |

## Device

- USB port `1-5` on fourier, high speed (480 Mb/s). idVendor 07fd, idProduct 0005, bcdDevice 0200,
  manufacturer "MOTU", product "24Ao", bcdUSB 0200, device class ef/02/01 (interface association).
- One configuration (value 1), six interfaces (0 to 5), self-powered (attributes 0x40).
- ALSA card 0 is `D24Ao`, "USB-Audio - 24Ao", driven by `snd-usb-audio`.

## Interfaces (all alternate settings, from the raw descriptors)

| Interface | Alt | Class/Subclass/Protocol | Endpoints | Bound driver now |
|---|---|---|---|---|
| 0 | 0 | 01/01/20 (UAC2 audio control) | 0x81 interrupt IN, 6 bytes, bInterval 1 | snd-usb-audio |
| 1 | 0 | 01/02/20 (UAC2 streaming) | none | snd-usb-audio |
| 1 | 1 | 01/02/20 | 0x09 isochronous OUT, attributes 0x05 (asynchronous, data), wMaxPacketSize 0x0ba8 = 936 bytes × 2 per microframe | |
| 2 | 0 | 01/02/20 | none | snd-usb-audio |
| 2 | 1 | 01/02/20 | 0x87 isochronous IN, attributes 0x25 (asynchronous, implicit feedback data), wMaxPacketSize 0x0ba8 = 936 bytes × 2 | |
| 3 | 0 | ff/04/01 (vendor) | 0x83 bulk IN 512, 0x04 bulk OUT 512 | none |
| 4 | 0 | ff/04/02 (vendor) | none | none |
| 4 | 1 | ff/04/02 | 0x09 isochronous OUT, attributes 0x01 (no sync type, data), wMaxPacketSize 0x0b84 = 900 bytes × 2 | |
| 5 | 0 | ff/04/02 (vendor) | none | none |
| 5 | 1 | ff/04/02 | 0x87 isochronous IN, attributes 0x01 (no sync type, data), wMaxPacketSize 0x0b84 = 900 bytes × 2 | |

- Interface association: interfaces 0 to 2, class 01/00/20. Interfaces 3 to 5 are outside it.
- The UAC2 streaming interfaces' class-specific descriptors give 0x18 = 24 channels and
  3-byte subslots (bytes `02 01 03 18`).
- The audio control interface has a clock source (subtype 0x0a, ID 0x01) and two input
  terminals and two output terminals (subtypes 0x02 and 0x03), as raw bytes in the decoded file.

## Kernel log

- `usb 1-5: config 1 interface 4 altsetting 1 has a duplicate endpoint with address 0x9, skipping`
- `usb 1-5: config 1 interface 5 altsetting 1 has a duplicate endpoint with address 0x87, skipping`
- Both at 2026-11-01T21:02:42Z (the last USB enumeration before the read).
