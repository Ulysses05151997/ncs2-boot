# Firmware

This directory is where you place the NCS2 firmware blob.

The file you need is `usb-ma2x8x.mvcmd` from an OpenVINO 2022.3 installation.

## How to obtain it

1. Download the OpenVINO 2022.3.2 tarball for Ubuntu 20.04:

       wget https://storage.openvinotoolkit.org/repositories/openvino/packages/2022.3.2/linux/l_openvino_toolkit_ubuntu20_2022.3.2.9279.e2c7e4d7b4d_x86_64.tgz

2. Extract it:

       tar xzf l_openvino_toolkit_ubuntu20_2022.3.2.9279.e2c7e4d7b4d_x86_64.tgz

3. Copy the firmware file:

       cp l_openvino_toolkit_ubuntu20_*/runtime/lib/intel64/usb-ma2x8x.mvcmd firmware/

The firmware is Intel proprietary and is not included in this repository.
