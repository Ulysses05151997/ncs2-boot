# ncs2-boot

Boot an Intel Neural Compute Stick 2 on modern Linux without OpenVINO.

Intel dropped NCS2 support after OpenVINO 2022.3, and the last working
runtime only supports Ubuntu 20.04 with Python 3.7–3.10. This tool
replaces the entire OpenVINO dependency for the boot step — a single C
program that uploads firmware to the NCS2 over USB using libusb.

## Build

Ubuntu / Debian:
```
sudo apt install libusb-1.0-0-dev
make
```

Arch Linux:
```
sudo pacman -S --needed gcc make libusb
make
```

## Usage

### Boot the NCS2

```
./ncs2_boot /path/to/usb-ma2x8x.mvcmd
```

The firmware file `usb-ma2x8x.mvcmd` comes from an OpenVINO 2022.3
installation. See [firmware/README.md](firmware/README.md) for
instructions on obtaining it.

### Run Inference

After booting:

```
./ncs2_infer model.blob [input.bin [output.bin]]
```

- `model.blob` — compiled model from OpenVINO's `compile_tool`
- `input.bin` — raw input tensor data (optional, uses zeros)
- `output.bin` — write raw output here (optional, prints to stdout)

Use `-v` for verbose XLink protocol trace.

To create a `.blob` file, use OpenVINO 2022.3's compile_tool:
```
compile_tool -m model.xml -d MYRIAD -o model.blob
```

### Quick test without a real model

`ncs2_mkblob` writes a tiny do-nothing blob that exercises the whole
pipeline (connect, capabilities, graph allocation, FIFOs, inference):

```
./ncs2_mkblob test.blob
./ncs2_infer -v test.blob
```

The 2-byte output is meaningless; the point is that every step
completes. The capabilities step also prints the device's memory
capacity.

### USB Permissions

Either run with `sudo`, or install the udev rules:

```
sudo cp udev/97-myriad-usbboot.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Then unplug and replug the NCS2.

## What It Does

**ncs2_boot:**
1. Finds the unbooted NCS2 on the USB bus (VID `03e7`, PID `2485`)
2. Opens the device and locates the bulk OUT endpoint
3. Sends the firmware file as a raw USB bulk transfer
4. Waits for the device to reboot and re-enumerate as PID `f63b`

**ncs2_infer:**
1. Connects to the booted NCS2 via XLink protocol over USB
2. Queries device capabilities (firmware version, memory, limits)
3. Loads a compiled model blob onto the device
4. Creates input/output FIFO buffers
5. Sends input data, triggers inference, reads results

## How It Works

The boot protocol is a raw firmware upload. The inference protocol
implements Intel's XLink and MVNC command layers over USB bulk
endpoints — 92-byte headers followed by data payloads.

See [docs/protocol.md](docs/protocol.md) for the full protocol
specification, reverse-engineered from the OpenVINO 2022.3 source.

## Hardware Requirements

- Any Linux machine with USB (tested on Ubuntu 24.04, and on Arch Linux
  on a 2019 MacBook Pro through a USB-C to USB-A adapter)
- Intel Neural Compute Stick 2
- The `usb-ma2x8x.mvcmd` firmware blob from OpenVINO 2022.3
- A compiled model `.blob` file (for inference)

## Troubleshooting

### The stick disappears after the firmware upload

Normal behavior: after the upload the stick disconnects (PID `2485`)
and comes back within a few seconds as PID `f63b`, usually as a
USB 3 device and often on a different USB bus.

On a 2019 MacBook Pro running Arch Linux, the stick disconnected and
never came back: nothing in `lsusb`, and nothing in `dmesg` after the
disconnect. The likely cause is that the booted stick reappears on a
USB controller that had nothing plugged into it, which Linux had put
to sleep. It booted after two changes:

1. Keeping the USB controllers awake (resets on reboot):
   ```
   for f in /sys/bus/usb/devices/usb*/power/control \
            /sys/bus/pci/drivers/xhci_hcd/*/power/control; do
     echo on | sudo tee "$f" >/dev/null
   done
   ```
2. Flipping the USB-C adapter over in the port.

Both were changed at once, so it is not yet proven which one was the
fix. Try step 1 first. Unplug and replug the stick before each attempt;
that returns it to the unbooted state.

### "No such device" messages at the end of a run

On the same machine, `ncs2_infer` finished and printed its output, then
logged a few "USB read timed out" / "No such device" messages: the
stick dropped off after the run. Every step reported before that had
completed. The cause has not been tracked down yet.

## License

MIT

## Author

Ulysses Isa
