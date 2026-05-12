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

- Any Linux machine with USB (tested on Ubuntu 24.04)
- Intel Neural Compute Stick 2
- The `usb-ma2x8x.mvcmd` firmware blob from OpenVINO 2022.3
- A compiled model `.blob` file (for inference)

## License

MIT

## Author

Ulysses Isa
