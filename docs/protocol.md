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
reading results). XLink uses USB bulk transfers with a fixed-size
header followed by optional data payloads.

### USB Endpoints

The booted device exposes two bulk endpoints:

| Endpoint | Address | Direction       |
|----------|---------|-----------------|
| OUT      | 0x01    | Host → Device   |
| IN       | 0x81    | Device → Host   |

All data is sent in 1 MB chunks (`libusb_bulk_transfer` with
`max_packet = 1048576`).

### Wire Format

Every XLink message consists of a 92-byte header, optionally
followed by a data payload:

```
[xlink_header_t  — 92 bytes] [data payload — header.size bytes]
                               ^ only for WRITE_REQ type
```

The header structure:

```c
typedef struct {
    int32_t     id;                          //  4 bytes — event ID
    int32_t     type;                        //  4 bytes — event type enum
    char        stream_name[64];             // 64 bytes — stream name (CREATE only)
    uint32_t    stream_id;                   //  4 bytes — stream identifier
    uint32_t    size;                        //  4 bytes — payload size
    uint32_t    dropped;                     //  4 bytes — (unused, 0)
    uint32_t    can_be_served;               //  4 bytes — (unused, 0)
    uint32_t    flags;                       //  4 bytes — ack/nack bits
} xlink_header_t;                            // Total: 92 bytes
```

### Event Types

Request types (host → device):

| Value | Name               | Has Payload | Purpose                        |
|-------|--------------------|-------------|--------------------------------|
| 0     | WRITE_REQ          | Yes         | Send data to a stream          |
| 1     | READ_REQ           | No          | Notify device: data consumed   |
| 2     | READ_REL_REQ       | No          | Release device buffer          |
| 4     | CREATE_STREAM_REQ  | No          | Open a named stream            |
| 5     | CLOSE_STREAM_REQ   | No          | Close a stream                 |
| 6     | PING_REQ           | No          | Connection handshake           |
| 7     | RESET_REQ          | No          | Reset the device               |

Response types (device → host):

| Value | Name               | Has Payload | Purpose                        |
|-------|--------------------|-------------|--------------------------------|
| 10    | WRITE_RESP         | No          | Ack for WRITE_REQ              |
| 11    | READ_RESP          | No          | Ack for READ_REQ               |
| 12    | READ_REL_RESP      | No          | Ack for READ_REL_REQ           |
| 14    | CREATE_STREAM_RESP | No          | Stream created, ID assigned    |
| 15    | CLOSE_STREAM_RESP  | No          | Stream closed                  |
| 16    | PING_RESP          | No          | Connection confirmed           |
| 17    | RESET_RESP         | No          | Reset acknowledged             |

Note: The device also sends WRITE_REQ (type 0) to the host when
delivering response data.  The same type code is used in both
directions.

### Stream Operations

**Open a stream:**
```
Host → Device: CREATE_STREAM_REQ  {stream_name="name", size=buffer_size}
Device → Host: CREATE_STREAM_RESP {stream_id=assigned_id}
```

**Write data to a stream:**
```
Host → Device: WRITE_REQ {stream_id, size=N} + N bytes of data
Device → Host: WRITE_RESP {stream_id}
```

**Read data from a stream:**

The device writes response data proactively after processing a
command.  From the host's perspective:
```
Device → Host: WRITE_REQ {stream_id, size=N} + N bytes of data
Host → Device: WRITE_RESP {stream_id}  (ack)
Host → Device: READ_REQ {stream_id}    (consumed notification)
Device → Host: READ_RESP {stream_id}
Host → Device: READ_REL_REQ {stream_id} (release device buffer)
Device → Host: READ_REL_RESP {stream_id}
```

### Connection Sequence

After the device boots (PID changes to 0xF63B):

```
1. libusb_open(device)
2. libusb_detach_kernel_driver(handle, 0)
3. libusb_claim_interface(handle, 0)
4. Send PING_REQ → receive PING_RESP
5. Open "deviceMonitor" stream (write_size = 2000)
6. Send DEVICE_GET_CAPABILITIES → read deviceCapabilities_t
7. Open "graphMonitor" stream (write_size = 4096)
```

## MVNC Command Protocol

The device firmware implements a command protocol layered on top
of XLink streams.  Commands are sent as C structs on specific
named streams.

### Monitor Streams

| Stream Name     | Buffer Size | Purpose                            |
|-----------------|-------------|------------------------------------|
| deviceMonitor   | 2000        | Device-level commands              |
| graphMonitor    | 4096        | Graph and FIFO management          |
| graphBuffer*N*  | graph_size  | Model blob transfer                |
| FIFO*N*         | tensor_size | Input/output tensor data           |

### Device Commands (deviceMonitor)

```c
typedef struct {
    int32_t  type;    // deviceCommandType_t
    uint32_t arg;
} deviceCommand_t;     // 8 bytes
```

| Type | Name                 | Response                          |
|------|----------------------|-----------------------------------|
| 0    | GET_THERMAL_STATS    | Thermal data buffer               |
| 1    | GET_CAPABILITIES     | deviceCapabilities_t (32 bytes)   |
| 2    | GET_USED_MEMORY      | uint32_t                          |

**deviceCapabilities_t** (returned by GET_CAPABILITIES):

```c
typedef struct {
    uint32_t max_graphs;             // max simultaneous graphs
    uint32_t max_fifos;              // max FIFO buffers
    uint32_t max_memory;             // device memory in bytes
    uint32_t max_executors;          // max SHAVE executors
    uint32_t fw_version[4];          // firmware version
    uint32_t mv_tensor_version[2];   // tensor library version
} deviceCapabilities_t;              // 32 bytes
```

### Graph Commands (graphMonitor)

```c
typedef struct {
    int32_t  type;                    // graphCommandType_t
    uint32_t id;                      // graph ID
    char     stream_name[64];         // associated data stream
    uint32_t buff_id1;                // input FIFO ID (for TRIGGER)
    uint32_t buff_id2;                // output FIFO ID (for TRIGGER)
    uint32_t executors_number;        // SHAVE executors to use
} graphCMDCommand_t;                  // 80 bytes
```

| Type | Name                     | Purpose                         |
|------|--------------------------|---------------------------------|
| 0    | GRAPH_ALLOCATE_CMD       | Allocate graph on device        |
| 1    | GRAPH_DEALLOCATE_CMD     | Free graph                      |
| 2    | GRAPH_TRIGGER_CMD        | Run inference                   |
| 3    | GRAPH_VERIFY_CMD         | Verify graph header             |
| 4    | GRAPH_ALLOCATION_VERIFY  | Confirm allocation succeeded    |
| 5    | GRAPH_BUFFER_ALLOCATE    | Allocate a FIFO buffer          |
| 6    | GRAPH_BUFFER_DEALLOCATE  | Free a FIFO buffer              |

All graph commands return a 4-byte status code on graphMonitor:
0 = success (MVNCI_SUCCESS).

### FIFO Buffer Command

```c
typedef struct {
    int32_t  type;                    // GRAPH_BUFFER_ALLOCATE_CMD (5)
    uint32_t id;                      // FIFO ID
    char     name[64];                // stream name ("FIFO0", "FIFO1")
    uint32_t elem_cnt;                // number of elements (1)
    uint32_t n, c, w, h, total_size;  // tensor descriptor (20 bytes)
    uint8_t  read_channel;            // 1 for output FIFO
    uint8_t  write_channel;           // 1 for input FIFO
} bufferAllocateCommand_t;
```

## Graph Allocation Sequence

Loading a compiled model (.blob) onto the device:

```
1. Open "graphBuffer0" stream (write_size = blob_size)

2. Send GRAPH_VERIFY_CMD on graphMonitor
   - Write graph header (first 88 bytes of blob) on graphBuffer0
   - Read verification status from graphMonitor (expect 0)

3. Send GRAPH_ALLOCATE_CMD on graphMonitor
   - Write full blob on graphBuffer0
   - Read input tensor descriptor from graphBuffer0
   - Read output tensor descriptor from graphBuffer0
   - Read nstages (uint32_t) from graphBuffer0
   - Release all three reads
   - Read allocation status from graphMonitor (expect 0)

4. Send GRAPH_ALLOCATION_VERIFY_CMD on graphMonitor
   - Read verification status (expect 0)
```

The tensor descriptor returned by the device:

```c
typedef struct {
    uint32_t n;           // batch size
    uint32_t c;           // channels
    uint32_t w;           // width
    uint32_t h;           // height
    uint32_t total_size;  // total tensor size in bytes
} tensorDescriptor_t;     // 20 bytes
```

## FIFO Allocation

After graph allocation, create input and output FIFOs:

```
1. Open "FIFO0" stream (write_size = input_tensor.total_size)
   - Send GRAPH_BUFFER_ALLOCATE_CMD on graphMonitor
     (write_channel=1, read_channel=0 for input)
   - Read status (expect 0)

2. Open "FIFO1" stream (write_size = 8)
   - Send GRAPH_BUFFER_ALLOCATE_CMD on graphMonitor
     (write_channel=0, read_channel=1 for output)
   - Read status (expect 0)
```

## Inference Sequence

```
1. Write input tensor data to FIFO0 stream (XLink WRITE_REQ)

2. Send GRAPH_TRIGGER_CMD on graphMonitor
   - buff_id1 = input FIFO ID (0)
   - buff_id2 = output FIFO ID (1)
   - Read status (expect 0)

3. Read output tensor data from FIFO1 stream
   (device sends WRITE_REQ with result data)
```

## The .blob Model Format

The compiled model file (`.blob`) is produced by OpenVINO's
`compile_tool` or model optimizer.  It starts with a minimal
ELF header (52 bytes) followed by a blob header:

```c
typedef struct {
    uint32_t magic_number;
    uint32_t file_size;
    uint32_t blob_ver_major;
    uint32_t blob_ver_minor;
    uint32_t bss_mem_size;
    uint32_t mode;
    uint32_t stage_section_offset;
    uint32_t buffer_section_offset;
    uint32_t relocation_section_offset;
} blob_header_v2;   // 36 bytes
```

The graph header sent during GRAPH_VERIFY is the first 88 bytes
(ELF header + blob_header_v2).  The full blob is sent during
GRAPH_ALLOCATE.

The stage section contains:

```c
typedef struct {
    uint32_t stage_count;
    uint32_t stage_section_size;
    uint32_t input_size;
    uint32_t output_size;
    uint32_t batch_size;
} stage_section_header_v2;  // 20 bytes
```

To create a .blob file for the NCS2:
```
# Using OpenVINO 2022.3's compile_tool:
compile_tool -m model.xml -d MYRIAD -o model.blob
```

## Source References

All protocol details were extracted from the OpenVINO 2022.3 source:

- `src/plugins/intel_myriad/third_party/XLink/pc/protocols/usb_boot.c` — USB boot implementation
- `src/plugins/intel_myriad/third_party/XLink/pc/protocols/usb_boot.h` — Constants and PID definitions
- `src/plugins/intel_myriad/third_party/XLink/pc/PlatformDeviceControl.c` — Boot orchestration
- `src/plugins/intel_myriad/third_party/XLink/shared/src/XLinkDevice.c` — High-level boot API
- `src/plugins/intel_myriad/third_party/mvnc/src/mvnc_api.c` — Device open flow
