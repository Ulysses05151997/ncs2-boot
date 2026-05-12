# NCS2 USB Boot Protocol

Reverse-engineered from the OpenVINO 2022.3 source code
(`src/plugins/intel_myriad/third_party/XLink/pc/protocols/usb_boot.c`
and related files).

## Overview

The Intel Neural Compute Stick 2 (NCS2) uses the Intel Myriad X (MA2485)
Vision Processing Unit. When plugged into USB, the Myriad X has no
persistent firmware — its internal bootrom waits for firmware to be
delivered over USB. Intel's OpenVINO runtime handles this transparently,
but the protocol is simple enough to implement independently.

## USB Device States

The NCS2 presents two different USB identities depending on its state:

| State    | Vendor ID | Product ID | Description                          |
|----------|-----------|------------|--------------------------------------|
| Unbooted | 0x03E7    | 0x2485     | Bootrom waiting for firmware         |
| Booted   | 0x03E7    | 0xF63B     | Firmware running, ready for commands |

When the device is first plugged in, it appears as `03e7:2485`. After
firmware is uploaded and boots successfully, the device disconnects from
the USB bus and re-enumerates as `03e7:f63b`.

A third PID exists (`0xF63C`) for an alternate "VSC mode" build, but
the standard firmware uses `0xF63B`.

The older NCS1 (Myriad 2) used PID `0x2150` in its unbooted state.

## Boot Sequence

### 1. Initialize libusb

Standard `libusb_init()`. No special context needed.

### 2. Find the unbooted device

Enumerate USB devices and match `VID=0x03E7, PID=0x2485`.

### 3. Open and configure

```c
libusb_open(dev, &handle);
libusb_set_configuration(handle, 1);
libusb_claim_interface(handle, 0);
```

Configuration 1, Interface 0. This is the only configuration the
bootrom exposes.

### 4. Locate the BULK OUT endpoint

Read the config descriptor and iterate endpoints. The bootrom
exposes a single BULK OUT endpoint (typically `0x01`). The BULK IN
endpoint is not used during boot.

```c
// Pseudo-code
for each endpoint in interface 0:
    if type == BULK and direction == OUT:
        use this endpoint
```

### 5. Determine chunk size

The transfer chunk size depends on USB speed:

| USB Version | bcdUSB    | Chunk Size |
|-------------|-----------|------------|
| USB 1.x     | < 0x0200  | 64 bytes   |
| USB 2.0+    | >= 0x0200 | 1,048,576 bytes (1 MB) |

Check `bcdUSB` from the device descriptor to determine which to use.
The NCS2 is a USB 2.0 device, so you will almost always use 1 MB chunks.

### 6. Send the firmware

The firmware file (`.mvcmd` format) is sent as a raw binary blob via
USB bulk transfer. No framing, no headers, no handshaking — just write
the bytes sequentially.

```c
while (bytes_remaining > 0) {
    chunk = min(bytes_remaining, chunk_size);
    libusb_bulk_transfer(handle, endpoint, data + offset, chunk,
                         &transferred, timeout_ms);
    offset += transferred;
    bytes_remaining -= transferred;
}
```

The write timeout in OpenVINO is 2000ms per chunk.

### 7. Release and wait

After the final chunk is sent, release the interface and close the
handle. The device will disconnect from the USB bus as the Myriad X
bootrom hands control to the uploaded firmware.

```c
libusb_release_interface(handle, 0);
libusb_close(handle);
```

### 8. Verify boot

Poll the USB bus for the booted device identity (`03e7:f63b`). The
device typically re-enumerates within 1-3 seconds. OpenVINO waits
up to 5 seconds.

## The .mvcmd Firmware Format

The firmware file (`usb-ma2x8x.mvcmd`) is approximately 2.3 MB.
It begins with the ASCII magic `MA2x` followed by packed ELF segments
for the Myriad X processors.

Internal structure (from hex inspection):

```
Offset 0x00: "MA2x"             — Magic identifier
Offset 0x04: ...                — Header fields (sizes, load addresses)
Offset 0x0E: "ddrInitMvcmd.elf" — DDR initialization code
              ...                — Additional ELF segments
              ...                — SHAVE processor code
              ...                — Leon OS/RT processor code
```

The bootrom parses this format internally. The host simply sends
the raw bytes — it does not need to understand the .mvcmd structure.

## USB Permissions

On Linux, raw USB access requires either root privileges or udev rules.
The standard rule shipped with OpenVINO:

```
SUBSYSTEM=="usb", ATTRS{idProduct}=="2485", ATTRS{idVendor}=="03e7", \
    GROUP="users", MODE="0660", ENV{ID_MM_DEVICE_IGNORE}="1"
SUBSYSTEM=="usb", ATTRS{idProduct}=="f63b", ATTRS{idVendor}=="03e7", \
    GROUP="users", MODE="0660", ENV{ID_MM_DEVICE_IGNORE}="1"
```

`ID_MM_DEVICE_IGNORE` prevents ModemManager from probing the device.

Install to `/etc/udev/rules.d/97-myriad-usbboot.rules` and reload:

```
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## Post-Boot Communication (XLink Protocol)

After boot, the device runs Intel's XLink protocol over USB for
all subsequent communication (loading models, running inference,
reading results). XLink uses USB bulk transfers on named "channels"
with a header-based multiplexing scheme.

Documenting the full XLink protocol is beyond the scope of this
project — it would be needed for running inference, but not for
booting the device.

## Source References

All protocol details were extracted from the OpenVINO 2022.3 source:

- `src/plugins/intel_myriad/third_party/XLink/pc/protocols/usb_boot.c` — USB boot implementation
- `src/plugins/intel_myriad/third_party/XLink/pc/protocols/usb_boot.h` — Constants and PID definitions
- `src/plugins/intel_myriad/third_party/XLink/pc/PlatformDeviceControl.c` — Boot orchestration
- `src/plugins/intel_myriad/third_party/XLink/shared/src/XLinkDevice.c` — High-level boot API
- `src/plugins/intel_myriad/third_party/mvnc/src/mvnc_api.c` — Device open flow
