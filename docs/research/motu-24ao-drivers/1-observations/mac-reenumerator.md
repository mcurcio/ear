# Observations: MOTU's re-enumerator daemon

Binary: `com.motu.driver.proaudio.reenumerator` (provenance in `../README.md`). Source:
`../sources/map-mac-hal-reenumerator.md`, which read the arm64 slice. All bullets are in the
`Server::armTimer(bool)` lambda unless another function is named. Interpretations are in `../2-inferences/mac.md`.
Tags as in `mac-dext.md`.

## 0. Scope and interfaces

- **R0.1** It makes no alt-setting, isochronous, interrupt or control-transfer call. It sends no MOTU protocol to
  the device. Its only device-facing calls are `USBDeviceOpen` and `USBDeviceReEnumerate` (R1.5, R1.6).

## 1. Bring-up

- **R1.1** launchd keeps it alive (plist: `RunAtLoad`, `KeepAlive`).
- **R1.2** `Server::Server()` registers `IOServiceFirstPublish` and `IOServiceTerminate` notifications for
  `IOUserService` with `IOUserClass=com_motu_driver_proaudio_device` and bundle `com.motu.driver.pro.audio.dext`.
- **R1.3** On either event (`Server::PublishCallback` / `TerminateCallback`), and once at start-up, it posts a
  1000 ms timer (`postTimed`, `mov w1,#0x3e8`). (re-checked by verify-mac: `Server::armTimer` arms a 1000 ms
  timer, `mov w1,#0x3e8`.)
- **R1.4** When the timer fires:
  1. It asks `SystemExtensionInstallChecker isInstalled` for the dext; if not installed it stops (string "driver
     is not installed, bailing").
  2. It matches `IOUSBHostDevice` with `idVendor 0x7fd` / `idProduct 0x5` (`mov w9,#0x7fd; mov w8,#0x5`).
     (re-checked by verify-mac: compare with 0x5.)
  3. It walks two levels of children looking for a name of 33 characters equal to
     `com_motu_driver_proaudio2_service` (`strlen` compared with 0x21, then a 33-byte compare).
- **R1.5** If that service is absent it logs "found a MOTU device using Apple class driver" (re-checked by
  verify-mac), opens an `IOUSBDeviceInterface`, checks vendor and product again (vtable +0x68/+0x70) and calls
  `USBDeviceOpen` (+0x40).
- **R1.6** It then calls `USBDeviceReEnumerate` (vtable +0x128) when the open succeeded or failed with
  `kIOReturnExclusiveAccess` (0xE00002C5). The options are `(open returned ExclusiveAccess) << 29`.
  (`USBDeviceReEnumerate` call re-checked by verify-mac.)
- **R1.7** Any other open failure increments a counter and re-arms the 1 s timer.

## 2-8.

- The map records nothing else for the re-enumerator: no clock, stream, framing, rate, status or error handling.
