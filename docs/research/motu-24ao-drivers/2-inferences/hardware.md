# Inferences from fourier's USB descriptors (2026-11-01)

INFERRED. Each item names the observations it rests on: `../hardware/observations-fourier.md`
(HW), the Mac observation files (D*, H*) and the Linux kernel 6.18.48 source kept at
`/Volumes/Dock/iac-ear-run/motu-research/linux-6.18.48/drivers/usb/core/config.c` (KC).

- **HI1. MOTU's vendor streaming interfaces are offered in the device's current mode.** Interfaces 4 and 5
  are class ff/04/02, the class the dext asks for first (GetInterfacesAndIsocPipes(0xff,4,2), see
  `../1-observations/mac-dext.md` and verify-mac on G1). Interface 3 is ff/04/01, the class of the dext's bulk
  command interface (FindInterface(0xff,4,1)). So, with this device in this state, MOTU's Mac driver would stream
  on interfaces 4 and 5, not on the class-compliant interfaces 1 and 2 that snd-usb-motu uses. No mode switch is
  needed for the interfaces to exist. Rests on: HW interface table; dext observations. Not settled: whether the
  device also needs `host/mode=USB2` (the HAL plug-in's datastore write) before it streams on them.
- **HI2. Both modes share the same two isochronous endpoints.** 0x09 (OUT) and 0x87 (IN) appear in interface 1 alt
  1 and interface 4 alt 1, and in interface 2 alt 1 and interface 5 alt 1. Only one interface pair can be active at a
  time. Rests on: HW interface table.
- **HI3. Linux's USB core removes the vendor interfaces' endpoints.** Kernel 6.18.48 skips an endpoint whose
  address another interface already uses (KC:259-285, :349-354, the "duplicate endpoint ... skipping" notice) and
  does not count it in bNumEndpoints (KC:366-367). fourier's log shows this for interface 4 alt 1 (0x09) and
  interface 5 alt 1 (0x87). So, as parsed by Linux, interfaces 4 and 5 have no endpoints in any alt setting. A Linux
  driver that wants MOTU's vendor streaming path cannot just select interface 4/5 alt 1 and submit to 0x09/0x87:
  the host controller is configured from the parsed alt setting, which has no endpoint (the host-controller part is
  INFERRED from general kernel behaviour, not read here). It would need a kernel change (for example a device quirk
  in `drivers/usb/core/config.c` that keeps these endpoints) or another way in. Rests on: HW kernel log; KC.
- **HI4. The vendor packet format differs from the class-compliant one.** UAC alt 1 packets are up to 936 bytes, which
  is 13 frames of 72 bytes (24 channels × 3 bytes). Vendor alt 1 packets are up to 900 bytes, which is not a whole
  number of 72-byte frames (12.5). So vendor-mode frames are either a different size (for example 75 bytes) or
  carry a header. Rests on: HW wMaxPacketSize values; the UAC channel and subslot bytes. Not settled by anything
  read so far; the dext's isochronous handling (`../1-observations/mac-dext.md`, section 4) is where to look.
- **HI5. The vendor endpoints declare no synchronisation type.** UAC endpoints are asynchronous, and the IN endpoint is
  implicit feedback (attributes 0x05 and 0x25); the vendor endpoints are plain isochronous (0x01). This fits the dext
  scheduling each transfer on an explicit frame number with a fixed IN to OUT offset (G3) rather than relying on
  USB synchronisation descriptors. Rests on: HW endpoint attributes; G3's observations.
- **HI6. The stock Linux driver uses only the class-compliant path.** snd-usb-audio is bound to interfaces 0 to 2;
  interfaces 3 to 5 have no driver. Rests on: HW sysfs driver links.
