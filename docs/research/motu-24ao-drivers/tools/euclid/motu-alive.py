#!/usr/bin/python3 -I
"""One-shot liveness probe for the MOTU 24Ao (07fd:0005). Run on demand, never in a loop.

Sends two standard, read-only control requests on endpoint 0 through usbfs: GET_STATUS (device)
and GET_DESCRIPTOR (device, 18 bytes). Prints one line and exits 0 if the device answered both,
1 if it is absent or did not answer (the reason is on the line).
"""
import ctypes, fcntl, glob, os, struct, sys

VID, PID = "07fd", "0005"
TIMEOUT_MS = 1000


class CtrlTransfer(ctypes.Structure):
    _fields_ = [("bRequestType", ctypes.c_uint8), ("bRequest", ctypes.c_uint8),
                ("wValue", ctypes.c_uint16), ("wIndex", ctypes.c_uint16),
                ("wLength", ctypes.c_uint16), ("timeout", ctypes.c_uint32),
                ("data", ctypes.c_void_p)]


USBDEVFS_CONTROL = (3 << 30) | (ctypes.sizeof(CtrlTransfer) << 16) | (ord("U") << 8) | 0


def find():
    for d in glob.glob("/sys/bus/usb/devices/*"):
        try:
            if open(d + "/idVendor").read().strip() == VID and open(d + "/idProduct").read().strip() == PID:
                bus = int(open(d + "/busnum").read()); dev = int(open(d + "/devnum").read())
                return os.path.basename(d), "/dev/bus/usb/%03d/%03d" % (bus, dev)
        except OSError:
            pass
    return None, None


def control(fd, rtype, req, value, index, length):
    buf = ctypes.create_string_buffer(length)
    ct = CtrlTransfer(rtype, req, value, index, length, TIMEOUT_MS, ctypes.cast(buf, ctypes.c_void_p))
    n = fcntl.ioctl(fd, USBDEVFS_CONTROL, ct)
    return buf.raw[:n]


port, node = find()
if not node:
    print("DEAD absent: no 07fd:0005 on the USB bus"); sys.exit(1)
try:
    fd = os.open(node, os.O_RDWR)
except OSError as e:
    print("DEAD open %s (%s): %s" % (node, port, e)); sys.exit(1)
try:
    st = control(fd, 0x80, 0x00, 0, 0, 2)            # GET_STATUS, device
    dd = control(fd, 0x80, 0x06, 0x0100, 0, 18)      # GET_DESCRIPTOR, device
    vid, pid = struct.unpack_from("<HH", dd, 8)
    if (vid, pid) != (0x07fd, 0x0005):
        print("DEAD wrong descriptor %04x:%04x at %s" % (vid, pid, port)); sys.exit(1)
    print("ALIVE %s %s status=%s" % (port, node, st.hex()))
except OSError as e:
    print("DEAD control request at %s %s: %s" % (port, node, e)); sys.exit(1)
finally:
    os.close(fd)
