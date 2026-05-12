# ncs2-boot

Boot an Intel Neural Compute Stick 2 on modern Linux without OpenVINO.

Intel dropped NCS2 support after OpenVINO 2022.3, and the last working
runtime only supports Ubuntu 20.04 with Python 3.7–3.10. This tool
replaces the entire OpenVINO dependency for the boot step — a single C
program that uploads firmware to the NCS2 over USB using libusb.

## Build

```
sudo apt install libusb-1.0-0-dev
make
```

## Usage

```
./ncs2_boot /path/to/usb-ma2x8x.mvcmd
```

The firmware file `usb-ma2x8x.mvcmd` comes from an OpenVINO 2022.3
installation. See [firmware/README.md](firmware/README.md) for
instructions on obtaining it.

### USB Permissions

Either run with `sudo`, or install the udev rules:

```
sudo cp udev/97-myriad-usbboot.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Then unplug and replug the NCS2.

## What It Does

1. Finds the unbooted NCS2 on the USB bus (VID `03e7`, PID `2485`)
2. Opens the device and locates the bulk OUT endpoint
3. Sends the firmware file as a raw USB bulk transfer
4. Waits for the device to reboot and re-enumerate as PID `f63b`

That's it. The Myriad X bootrom does the rest.

## How It Works

See [docs/protocol.md](docs/protocol.md) for a full description of the
USB boot protocol, reverse-engineered from the OpenVINO 2022.3 source.

## Hardware Requirements

- Any Linux machine with USB (tested on Ubuntu 24.04)
- Intel Neural Compute Stick 2
- The `usb-ma2x8x.mvcmd` firmware blob from OpenVINO 2022.3

## What This Does NOT Do

This tool boots the NCS2. It does not run inference. After boot, the
device speaks Intel's XLink protocol for loading models and executing
them. That is a separate (and much larger) project.

## License

MIT

## Author

Ulysses Isa
