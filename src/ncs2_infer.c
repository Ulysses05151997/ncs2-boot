/*
 * ncs2_infer.c — Run inference on Intel Neural Compute Stick 2
 *                without OpenVINO.
 *
 * Author:  Ulysses Isa
 * License: MIT
 *
 * After ncs2_boot uploads firmware and the device re-enumerates as
 * PID 0xF63B, this tool implements Intel's XLink protocol to:
 *   1. Connect to the booted device
 *   2. Load a compiled model (.blob)
 *   3. Send input data and run inference
 *   4. Read the output
 *
 * The XLink protocol was reverse-engineered from the OpenVINO 2022.3
 * source code.  See docs/protocol.md for full details.
 *
 * Every byte on the wire goes through two USB bulk endpoints:
 *   OUT 0x01 (host → device)   IN 0x81 (device → host)
 * Each message is a 92-byte xlink_header_t, optionally followed by a
 * data payload (only for XLINK_WRITE_REQ).
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

/* ------------------------------------------------------------------ */
/*  Constants                                                          */
/* ------------------------------------------------------------------ */

#define NCS2_VID            0x03E7
#define NCS2_PID_BOOTED     0xF63B

#define USB_EP_OUT          0x01
#define USB_EP_IN           0x81
#define USB_TIMEOUT_MS      10000
#define USB_CHUNK           (1024 * 1024)

#define MAX_STREAM_NAME     64
#define CONFIG_STREAM_SIZE  2000
#define GRAPH_STREAM_SIZE   4096
#define MAX_PENDING         16

static int verbose = 0;

/* ------------------------------------------------------------------ */
/*  XLink protocol types (must match device firmware)                  */
/* ------------------------------------------------------------------ */

enum {
    XLINK_WRITE_REQ          = 0,
    XLINK_READ_REQ           = 1,
    XLINK_READ_REL_REQ       = 2,
    XLINK_READ_REL_SPEC_REQ  = 3,
    XLINK_CREATE_STREAM_REQ  = 4,
    XLINK_CLOSE_STREAM_REQ   = 5,
    XLINK_PING_REQ           = 6,
    XLINK_RESET_REQ          = 7,
    XLINK_WRITE_RESP         = 10,
    XLINK_READ_RESP          = 11,
    XLINK_READ_REL_RESP      = 12,
    XLINK_READ_REL_SPEC_RESP = 13,
    XLINK_CREATE_STREAM_RESP = 14,
    XLINK_CLOSE_STREAM_RESP  = 15,
    XLINK_PING_RESP          = 16,
    XLINK_RESET_RESP         = 17,
};

typedef struct {
    int32_t     id;
    int32_t     type;
    char        stream_name[MAX_STREAM_NAME];
    uint32_t    stream_id;
    uint32_t    size;
    uint32_t    dropped;
    uint32_t    can_be_served;
    uint32_t    flags;
} xlink_header_t;

_Static_assert(sizeof(xlink_header_t) == 92, "header must be 92 bytes");

/* ------------------------------------------------------------------ */
/*  MVNC command types (device-side command protocol)                  */
/* ------------------------------------------------------------------ */

enum {
    GRAPH_ALLOCATE_CMD          = 0,
    GRAPH_DEALLOCATE_CMD        = 1,
    GRAPH_TRIGGER_CMD           = 2,
    GRAPH_VERIFY_CMD            = 3,
    GRAPH_ALLOCATION_VERIFY_CMD = 4,
    GRAPH_BUFFER_ALLOCATE_CMD   = 5,
    GRAPH_BUFFER_DEALLOCATE_CMD = 6,
};

enum {
    DEVICE_GET_THERMAL_STATS = 0,
    DEVICE_GET_CAPABILITIES  = 1,
    DEVICE_GET_USED_MEMORY   = 2,
};

typedef struct {
    int32_t  type;
    uint32_t id;
    char     stream_name[MAX_STREAM_NAME];
    uint32_t buff_id1;
    uint32_t buff_id2;
    uint32_t executors_number;
} graph_cmd_t;

typedef struct {
    int32_t  type;
    uint32_t id;
    char     name[MAX_STREAM_NAME];
    uint32_t elem_cnt;
    uint32_t n, c, w, h, total_size;   /* tensorDescriptor_t */
    uint8_t  read_channel;
    uint8_t  write_channel;
} buffer_alloc_cmd_t;

typedef struct {
    int32_t  type;
    uint32_t arg;
} device_cmd_t;

typedef struct {
    uint32_t max_graphs;
    uint32_t max_fifos;
    uint32_t max_memory;
    uint32_t max_executors;
    uint32_t fw_version[4];
    uint32_t mv_tensor_version[2];
} device_caps_t;

typedef struct {
    uint32_t n, c, w, h, total_size;
} tensor_desc_t;

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */

static libusb_context       *usb_ctx;
static libusb_device_handle *usb_dev;
static int32_t               next_event_id = 1;

/* Stream ID tracking — we need to assign IDs when the device creates
 * streams on our side.  IDs the device assigned to OUR streams start
 * at 0.  IDs we assign to DEVICE-created streams start at 0 too, but
 * we match by name to reuse existing IDs. */
#define MAX_STREAMS 16
static struct {
    char     name[MAX_STREAM_NAME];
    uint32_t id;
    int      used;
} stream_table[MAX_STREAMS];
static uint32_t next_local_stream_id = 0;

static uint32_t get_or_assign_stream_id(const char *name)
{
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (stream_table[i].used &&
            strncmp(stream_table[i].name, name, MAX_STREAM_NAME) == 0)
            return stream_table[i].id;
    }
    for (int i = 0; i < MAX_STREAMS; i++) {
        if (!stream_table[i].used) {
            snprintf(stream_table[i].name, MAX_STREAM_NAME, "%s", name);
            stream_table[i].id   = next_local_stream_id++;
            stream_table[i].used = 1;
            return stream_table[i].id;
        }
    }
    return next_local_stream_id++;
}

/* Pending event buffer — events we read from USB but weren't expecting */
typedef struct {
    xlink_header_t hdr;
    uint8_t       *data;
    int            valid;
} pending_event_t;

static pending_event_t pending[MAX_PENDING];

static const char *type_name(int type)
{
    switch (type) {
    case XLINK_WRITE_REQ:          return "WRITE_REQ";
    case XLINK_READ_REQ:           return "READ_REQ";
    case XLINK_READ_REL_REQ:       return "READ_REL_REQ";
    case XLINK_CREATE_STREAM_REQ:  return "CREATE_STREAM_REQ";
    case XLINK_CLOSE_STREAM_REQ:   return "CLOSE_STREAM_REQ";
    case XLINK_PING_REQ:           return "PING_REQ";
    case XLINK_RESET_REQ:          return "RESET_REQ";
    case XLINK_WRITE_RESP:         return "WRITE_RESP";
    case XLINK_READ_RESP:          return "READ_RESP";
    case XLINK_READ_REL_RESP:      return "READ_REL_RESP";
    case XLINK_CREATE_STREAM_RESP: return "CREATE_STREAM_RESP";
    case XLINK_CLOSE_STREAM_RESP:  return "CLOSE_STREAM_RESP";
    case XLINK_PING_RESP:          return "PING_RESP";
    case XLINK_RESET_RESP:         return "RESET_RESP";
    default:                       return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/*  USB I/O — chunked bulk transfers                                   */
/* ------------------------------------------------------------------ */

static int usb_write(const void *buf, int len)
{
    const uint8_t *p = buf;
    while (len > 0) {
        int chunk = (len > USB_CHUNK) ? USB_CHUNK : len;
        int transferred;
        int rc = libusb_bulk_transfer(usb_dev, USB_EP_OUT,
                                       (uint8_t *)p, chunk,
                                       &transferred, USB_TIMEOUT_MS);
        if (rc != 0) {
            fprintf(stderr, "USB write failed: %s\n", libusb_strerror(rc));
            return -1;
        }
        p   += transferred;
        len -= transferred;
    }
    return 0;
}

static int usb_read(void *buf, int len)
{
    uint8_t *p = buf;
    while (len > 0) {
        int chunk = (len > USB_CHUNK) ? USB_CHUNK : len;
        int transferred = 0;
        int rc = libusb_bulk_transfer(usb_dev, USB_EP_IN,
                                       p, chunk,
                                       &transferred, USB_TIMEOUT_MS);
        if (transferred > 0) {
            p   += transferred;
            len -= transferred;
        }
        if (rc != 0 && rc != LIBUSB_ERROR_TIMEOUT) {
            fprintf(stderr, "USB read failed: %s\n", libusb_strerror(rc));
            return -1;
        }
        if (rc == LIBUSB_ERROR_TIMEOUT && transferred == 0) {
            fprintf(stderr, "USB read timed out\n");
            return -1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  XLink event I/O                                                    */
/* ------------------------------------------------------------------ */

static int xlink_send(int type, uint32_t stream_id, const char *name,
                      uint32_t size, const void *data)
{
    xlink_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.id        = next_event_id++;
    hdr.type      = type;
    hdr.stream_id = stream_id;
    hdr.size      = size;

    if (name)
        strncpy(hdr.stream_name, name, MAX_STREAM_NAME - 1);

    if (verbose)
        printf("  >> %s  stream=%u size=%u id=%d\n",
               type_name(type), stream_id, size, hdr.id);

    if (usb_write(&hdr, sizeof(hdr)) < 0)
        return -1;

    if (type == XLINK_WRITE_REQ && size > 0 && data) {
        if (usb_write(data, (int)size) < 0)
            return -1;
    }

    return 0;
}

static int xlink_send_resp(int type, const xlink_header_t *orig)
{
    xlink_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.id        = orig->id;
    hdr.type      = type;
    hdr.stream_id = orig->stream_id;
    memcpy(hdr.stream_name, orig->stream_name, MAX_STREAM_NAME);

    if (verbose)
        printf("  >> %s  stream=%u (ack id=%d)\n",
               type_name(type), orig->stream_id, orig->id);

    return usb_write(&hdr, sizeof(hdr));
}

/* Read one event from USB.  If it's WRITE_REQ, also reads the payload. */
static int xlink_recv_raw(xlink_header_t *hdr, uint8_t **data_out)
{
    if (data_out) *data_out = NULL;

    if (usb_read(hdr, sizeof(*hdr)) < 0)
        return -1;

    if (verbose) {
        printf("  << %s  stream=%u size=%u id=%d",
               type_name(hdr->type), hdr->stream_id, hdr->size, hdr->id);
        if (hdr->stream_name[0] && hdr->type == XLINK_CREATE_STREAM_REQ)
            printf("  name=\"%.*s\"", MAX_STREAM_NAME, hdr->stream_name);
        printf("\n");
    }

    if (hdr->type == XLINK_WRITE_REQ && hdr->size > 0) {
        uint8_t *buf = malloc(hdr->size);
        if (!buf) {
            fprintf(stderr, "Out of memory for %u byte payload\n", hdr->size);
            return -1;
        }
        if (usb_read(buf, (int)hdr->size) < 0) {
            free(buf);
            return -1;
        }
        if (data_out)
            *data_out = buf;
        else
            free(buf);
    }

    return 0;
}

/*
 * Wait for a specific event type (optionally on a specific stream).
 * While waiting, handle any other events:
 *   - WRITE_REQ from device: read payload, buffer it, send WRITE_RESP
 *   - Other types: buffer them
 * If want_data is non-NULL and the matched event is WRITE_REQ,
 * the payload pointer is stored there (caller must free).
 */
static int xlink_wait(int want_type, int want_stream,
                      xlink_header_t *out_hdr, uint8_t **want_data)
{
    /* Check pending buffer first */
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!pending[i].valid)
            continue;
        if (pending[i].hdr.type == want_type &&
            (want_stream < 0 ||
             pending[i].hdr.stream_id == (uint32_t)want_stream)) {
            *out_hdr = pending[i].hdr;
            if (want_data)
                *want_data = pending[i].data;
            else
                free(pending[i].data);
            pending[i].valid = 0;
            return 0;
        }
    }

    /* Read from USB until we get what we want */
    for (int tries = 0; tries < 200; tries++) {
        xlink_header_t hdr;
        uint8_t *data = NULL;

        if (xlink_recv_raw(&hdr, &data) < 0)
            return -1;

        /* Is this what we wanted? */
        if (hdr.type == want_type &&
            (want_stream < 0 ||
             hdr.stream_id == (uint32_t)want_stream)) {
            *out_hdr = hdr;
            if (want_data)
                *want_data = data;
            else
                free(data);
            return 0;
        }

        /* Not what we wanted — handle it */
        if (hdr.type == XLINK_CREATE_STREAM_REQ) {
            /* Device is creating a stream on our side — assign ID and respond */
            uint32_t assigned = get_or_assign_stream_id(hdr.stream_name);
            if (verbose)
                printf("  ** Device creating stream: \"%.*s\" → id=%u\n",
                       MAX_STREAM_NAME, hdr.stream_name, assigned);
            xlink_header_t resp;
            memset(&resp, 0, sizeof(resp));
            resp.id        = hdr.id;
            resp.type      = XLINK_CREATE_STREAM_RESP;
            resp.stream_id = assigned;
            resp.size      = hdr.size;
            memcpy(resp.stream_name, hdr.stream_name, MAX_STREAM_NAME);
            if (verbose)
                printf("  >> CREATE_STREAM_RESP  stream=%u (ack id=%d)\n",
                       assigned, hdr.id);
            usb_write(&resp, sizeof(resp));
            free(data);
        } else if (hdr.type == XLINK_WRITE_REQ) {
            /* Device sent data we're not expecting yet — buffer it.
             * Do NOT ack here; the consumer (xlink_read_data) will ack
             * when it retrieves this event from the pending buffer. */
            for (int i = 0; i < MAX_PENDING; i++) {
                if (!pending[i].valid) {
                    pending[i].hdr   = hdr;
                    pending[i].data  = data;
                    pending[i].valid = 1;
                    data = NULL;
                    break;
                }
            }
            free(data);
        } else if (hdr.type == XLINK_READ_REQ) {
            /* Device is reading from a stream — respond */
            xlink_send_resp(XLINK_READ_RESP, &hdr);
            free(data);
        } else if (hdr.type == XLINK_READ_REL_REQ) {
            xlink_send_resp(XLINK_READ_REL_RESP, &hdr);
            free(data);
        } else {
            /* Some other event — just consume it */
            free(data);
        }
    }

    fprintf(stderr, "Timeout waiting for %s on stream %d\n",
            type_name(want_type), want_stream);
    return -1;
}

/* ------------------------------------------------------------------ */
/*  XLink stream operations                                            */
/* ------------------------------------------------------------------ */

static int xlink_ping_once(void)
{
    if (xlink_send(XLINK_PING_REQ, 0, NULL, 0, NULL) < 0)
        return -1;

    xlink_header_t resp;
    return xlink_wait(XLINK_PING_RESP, -1, &resp, NULL);
}

static int xlink_ping(void)
{
    for (int attempt = 0; attempt < 5; attempt++) {
        if (xlink_ping_once() == 0)
            return 0;
        if (verbose)
            printf("  PING attempt %d failed, retrying...\n", attempt + 1);
        usleep(500000);
    }
    return -1;
}

static int xlink_open_stream(const char *name, uint32_t write_size,
                              uint32_t *out_stream_id)
{
    uint32_t local_id = get_or_assign_stream_id(name);

    if (xlink_send(XLINK_CREATE_STREAM_REQ, local_id, name, write_size, NULL) < 0)
        return -1;

    xlink_header_t resp;
    if (xlink_wait(XLINK_CREATE_STREAM_RESP, -1, &resp, NULL) < 0)
        return -1;

    *out_stream_id = local_id;
    return 0;
}

/* Write data to a stream.  Sends WRITE_REQ + payload, waits for WRITE_RESP. */
static int xlink_write_data(uint32_t stream_id, const void *data, uint32_t size)
{
    if (xlink_send(XLINK_WRITE_REQ, stream_id, NULL, size, data) < 0)
        return -1;

    xlink_header_t resp;
    return xlink_wait(XLINK_WRITE_RESP, (int)stream_id, &resp, NULL);
}

/*
 * Read data that the device has written to a stream.
 * The device sends WRITE_REQ + payload; we ACK with WRITE_RESP,
 * then send READ_REQ and wait for READ_RESP.
 */
static int xlink_read_data(uint32_t stream_id, void *buf, uint32_t max_size,
                            uint32_t *actual_size)
{
    xlink_header_t hdr;
    uint8_t *data = NULL;

    if (xlink_wait(XLINK_WRITE_REQ, (int)stream_id, &hdr, &data) < 0)
        return -1;

    /* ACK the device's write */
    xlink_send_resp(XLINK_WRITE_RESP, &hdr);

    uint32_t sz = hdr.size;
    if (sz > max_size) sz = max_size;
    if (data) {
        memcpy(buf, data, sz);
        free(data);
    }
    if (actual_size) *actual_size = hdr.size;

    /* Tell device we consumed the data */
    xlink_send(XLINK_READ_REQ, stream_id, NULL, 0, NULL);
    xlink_header_t read_resp;
    xlink_wait(XLINK_READ_RESP, (int)stream_id, &read_resp, NULL);

    return 0;
}

static int xlink_release(uint32_t stream_id)
{
    xlink_send(XLINK_READ_REL_REQ, stream_id, NULL, 0, NULL);
    xlink_header_t resp;
    return xlink_wait(XLINK_READ_REL_RESP, (int)stream_id, &resp, NULL);
}

static int __attribute__((unused)) xlink_close_stream(uint32_t stream_id)
{
    xlink_send(XLINK_CLOSE_STREAM_REQ, stream_id, NULL, 0, NULL);
    xlink_header_t resp;
    return xlink_wait(XLINK_CLOSE_STREAM_RESP, (int)stream_id, &resp, NULL);
}

static int __attribute__((unused)) xlink_reset(void)
{
    xlink_send(XLINK_RESET_REQ, 0, NULL, 0, NULL);
    xlink_header_t resp;
    return xlink_wait(XLINK_RESET_RESP, -1, &resp, NULL);
}

/* ------------------------------------------------------------------ */
/*  MVNC operations — command/response on monitor streams              */
/* ------------------------------------------------------------------ */

/*
 * Send a command on graphMonitor, read one-int response.
 * Returns: 0 on success (device returned MVNCI_SUCCESS), -1 on error.
 */
static int __attribute__((unused)) graph_monitor_cmd(uint32_t gm_stream,
                              const void *cmd, uint32_t cmd_size)
{
    if (xlink_write_data(gm_stream, cmd, cmd_size) < 0) {
        fprintf(stderr, "Failed to send graph command\n");
        return -1;
    }

    int32_t status = -1;
    uint32_t actual;
    if (xlink_read_data(gm_stream, &status, sizeof(status), &actual) < 0) {
        fprintf(stderr, "Failed to read graph command response\n");
        return -1;
    }
    xlink_release(gm_stream);

    if (status != 0) {
        fprintf(stderr, "Device returned error %d for graph command\n", status);
        return -1;
    }
    return 0;
}

static int ncs2_get_capabilities(uint32_t dm_stream, device_caps_t *caps)
{
    device_cmd_t cmd = { .type = DEVICE_GET_CAPABILITIES, .arg = 0 };

    if (xlink_write_data(dm_stream, &cmd, sizeof(cmd)) < 0)
        return -1;

    uint32_t actual;
    if (xlink_read_data(dm_stream, caps, sizeof(*caps), &actual) < 0)
        return -1;

    xlink_release(dm_stream);

    if (actual != sizeof(*caps)) {
        fprintf(stderr, "Unexpected capabilities size: %u (expected %zu)\n",
                actual, sizeof(*caps));
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Graph allocation                                                   */
/* ------------------------------------------------------------------ */

static int ncs2_graph_allocate(uint32_t gm_stream,
                                const uint8_t *blob, uint32_t blob_size,
                                uint32_t header_size,
                                uint32_t *out_graph_stream,
                                tensor_desc_t *in_desc,
                                tensor_desc_t *out_desc,
                                uint32_t *nstages)
{
    uint32_t graph_id = 0;
    uint32_t gb_stream;
    char gb_name[MAX_STREAM_NAME];
    snprintf(gb_name, sizeof(gb_name), "graphBuffer%u", graph_id);

    printf("  Opening stream: %s (size=%u)\n", gb_name, blob_size);
    if (xlink_open_stream(gb_name, blob_size, &gb_stream) < 0)
        return -1;

    /* Step 1: GRAPH_VERIFY_CMD — send header portion */
    printf("  Sending GRAPH_VERIFY_CMD\n");
    graph_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = GRAPH_VERIFY_CMD;
    cmd.id   = graph_id;
    cmd.executors_number = 1;
    snprintf(cmd.stream_name, MAX_STREAM_NAME, "%s", gb_name);

    if (xlink_write_data(gm_stream, &cmd, sizeof(cmd)) < 0)
        return -1;

    /* Send graph header on graphBuffer stream */
    if (xlink_write_data(gb_stream, blob, header_size) < 0)
        return -1;

    /* Check verification response */
    int32_t status = -1;
    uint32_t actual;
    if (xlink_read_data(gm_stream, &status, sizeof(status), &actual) < 0)
        return -1;
    xlink_release(gm_stream);

    if (status != 0) {
        fprintf(stderr, "Graph header verification failed (status=%d)\n", status);
        return -1;
    }
    printf("  Graph header verified\n");

    /* Step 2: GRAPH_ALLOCATE_CMD — send full blob */
    printf("  Sending GRAPH_ALLOCATE_CMD (%u bytes)\n", blob_size);
    cmd.type = GRAPH_ALLOCATE_CMD;
    if (xlink_write_data(gm_stream, &cmd, sizeof(cmd)) < 0)
        return -1;

    if (xlink_write_data(gb_stream, blob, blob_size) < 0)
        return -1;

    printf("  Reading tensor descriptors\n");

    /* Read input tensor descriptor */
    if (xlink_read_data(gb_stream, in_desc, sizeof(*in_desc), &actual) < 0)
        return -1;

    /* Read output tensor descriptor */
    if (xlink_read_data(gb_stream, out_desc, sizeof(*out_desc), &actual) < 0)
        return -1;

    /* Read nstages */
    if (xlink_read_data(gb_stream, nstages, sizeof(*nstages), &actual) < 0)
        return -1;

    /* Release all three reads */
    xlink_release(gb_stream);
    xlink_release(gb_stream);
    xlink_release(gb_stream);

    /* Check allocation response on graphMonitor */
    status = -1;
    if (xlink_read_data(gm_stream, &status, sizeof(status), &actual) < 0)
        return -1;
    xlink_release(gm_stream);

    if (status != 0) {
        fprintf(stderr, "Graph allocation rejected (status=%d)\n", status);
        return -1;
    }

    /* Step 3: GRAPH_ALLOCATION_VERIFY_CMD */
    printf("  Sending GRAPH_ALLOCATION_VERIFY_CMD\n");
    cmd.type = GRAPH_ALLOCATION_VERIFY_CMD;
    if (xlink_write_data(gm_stream, &cmd, sizeof(cmd)) < 0)
        return -1;

    status = -1;
    if (xlink_read_data(gm_stream, &status, sizeof(status), &actual) < 0)
        return -1;
    xlink_release(gm_stream);

    if (status != 0) {
        fprintf(stderr, "Graph allocation verify failed (status=%d)\n", status);
        return -1;
    }

    *out_graph_stream = gb_stream;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  FIFO allocation                                                    */
/* ------------------------------------------------------------------ */

static int ncs2_fifo_allocate(uint32_t gm_stream, uint32_t fifo_id,
                               const tensor_desc_t *desc, int is_input,
                               uint32_t *out_stream)
{
    char name[MAX_STREAM_NAME];
    snprintf(name, sizeof(name), "FIFO%u", fifo_id);

    uint32_t write_size = is_input ? desc->total_size : 8;

    printf("  Allocating %s FIFO: %s (size=%u)\n",
           is_input ? "input" : "output", name, write_size);

    uint32_t stream_id;
    if (xlink_open_stream(name, write_size, &stream_id) < 0)
        return -1;

    buffer_alloc_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type          = GRAPH_BUFFER_ALLOCATE_CMD;
    cmd.id            = fifo_id;
    cmd.elem_cnt      = 1;
    cmd.n             = desc->n;
    cmd.c             = desc->c;
    cmd.w             = desc->w;
    cmd.h             = desc->h;
    cmd.total_size    = desc->total_size;
    cmd.write_channel = is_input ? 1 : 0;
    cmd.read_channel  = is_input ? 0 : 1;
    snprintf(cmd.name, MAX_STREAM_NAME, "%s", name);

    if (xlink_write_data(gm_stream, &cmd, sizeof(cmd)) < 0)
        return -1;

    int32_t status = -1;
    uint32_t actual;
    if (xlink_read_data(gm_stream, &status, sizeof(status), &actual) < 0)
        return -1;
    xlink_release(gm_stream);

    if (status != 0) {
        fprintf(stderr, "FIFO %s allocation failed (status=%d)\n", name, status);
        return -1;
    }

    *out_stream = stream_id;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Inference trigger                                                  */
/* ------------------------------------------------------------------ */

static int ncs2_trigger_inference(uint32_t gm_stream, uint32_t graph_id,
                                   uint32_t in_fifo_id, uint32_t out_fifo_id)
{
    graph_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type     = GRAPH_TRIGGER_CMD;
    cmd.id       = graph_id;
    cmd.buff_id1 = in_fifo_id;
    cmd.buff_id2 = out_fifo_id;

    if (xlink_write_data(gm_stream, &cmd, sizeof(cmd)) < 0)
        return -1;

    int32_t status = -1;
    uint32_t actual;
    if (xlink_read_data(gm_stream, &status, sizeof(status), &actual) < 0)
        return -1;
    xlink_release(gm_stream);

    if (status != 0) {
        fprintf(stderr, "Inference trigger failed (status=%d)\n", status);
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  File I/O helpers                                                   */
/* ------------------------------------------------------------------ */

static uint8_t *read_file(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) {
        fprintf(stderr, "%s: empty or unreadable\n", path);
        fclose(f);
        return NULL;
    }

    uint8_t *buf = malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "%s: read error\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size_out = (size_t)sz;
    return buf;
}

/*
 * Compute graph header size from a .blob file.
 * OpenVINO blobs start with an ELF header (52 bytes on Myriad X)
 * followed by a blob_header_v2 (36 bytes).
 */
static uint32_t get_blob_header_size(const uint8_t *blob, size_t blob_size)
{
    /* ELF header (52 bytes) + mv_blob_header (80 bytes) = 132 bytes */
    if (blob_size >= 132 && blob[0] == 0x7f &&
        blob[1] == 'E' && blob[2] == 'L' && blob[3] == 'F') {
        return 132;
    }
    return (blob_size > 132) ? 132 : (uint32_t)blob_size;
}

/* ------------------------------------------------------------------ */
/*  Device connection                                                  */
/* ------------------------------------------------------------------ */

static libusb_device_handle *find_booted_ncs2(void)
{
    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(usb_ctx, &devs);
    if (cnt < 0) return NULL;

    libusb_device_handle *handle = NULL;
    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) < 0)
            continue;
        if (desc.idVendor == NCS2_VID && desc.idProduct == NCS2_PID_BOOTED) {
            if (libusb_open(devs[i], &handle) < 0)
                handle = NULL;
            break;
        }
    }
    libusb_free_device_list(devs, 1);
    return handle;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

static void print_tensor(const char *label, const tensor_desc_t *d)
{
    printf("  %s: n=%u c=%u h=%u w=%u total=%u bytes\n",
           label, d->n, d->c, d->h, d->w, d->total_size);
}

static void print_output(const uint8_t *data, uint32_t size)
{
    /* Print as floats if size is a multiple of 4 */
    if (size % 4 == 0) {
        uint32_t n = size / 4;
        const float *fp = (const float *)data;

        printf("\n  Output (%u values):\n", n);

        /* Find top-5 */
        if (n > 1) {
            int top[5] = {0};
            int ntop = (n < 5) ? (int)n : 5;
            for (uint32_t i = 0; i < n; i++) {
                for (int j = 0; j < ntop; j++) {
                    if (fp[i] > fp[top[j]]) {
                        for (int k = ntop - 1; k > j; k--)
                            top[k] = top[k-1];
                        top[j] = (int)i;
                        break;
                    }
                }
            }
            printf("  Top-%d:\n", ntop);
            for (int j = 0; j < ntop; j++)
                printf("    [%d] = %f\n", top[j], fp[top[j]]);
        }

        /* First 20 values */
        uint32_t show = (n < 20) ? n : 20;
        printf("  First %u values:\n    ", show);
        for (uint32_t i = 0; i < show; i++)
            printf("%.4f ", fp[i]);
        printf("\n");
    } else {
        printf("  Output (%u bytes, hex):\n    ", size);
        uint32_t show = (size < 64) ? size : 64;
        for (uint32_t i = 0; i < show; i++)
            printf("%02x ", data[i]);
        if (size > 64) printf("...");
        printf("\n");
    }
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: %s [options] <model.blob> [input.bin [output.bin]]\n"
            "       %s --reset\n\n"
            "Run inference on a booted Intel NCS2 without OpenVINO.\n"
            "The device must already be booted (use ncs2_boot first).\n\n"
            "  model.blob   Compiled model from OpenVINO compile_tool\n"
            "  input.bin    Raw input tensor (optional — uses zeros)\n"
            "  output.bin   Write raw output here (optional — prints)\n\n"
            "Options:\n"
            "  -v           Verbose (show XLink protocol traffic)\n"
            "  --reset      Reset device to unbooted state and exit\n",
            argv[0], argv[0]);
        return 1;
    }

    /* Parse arguments */
    int argi = 1;
    int reset_mode = 0;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "-v") == 0)
            verbose = 1;
        else if (strcmp(argv[argi], "--reset") == 0)
            reset_mode = 1;
        argi++;
    }

    if (!reset_mode && argi >= argc) {
        fprintf(stderr, "Missing model.blob argument\n");
        return 1;
    }

    const char *blob_path   = reset_mode ? NULL : argv[argi++];
    const char *input_path  = (!reset_mode && argi < argc) ? argv[argi++] : NULL;
    const char *output_path = (!reset_mode && argi < argc) ? argv[argi++] : NULL;

    uint8_t *blob = NULL;
    uint8_t *input_data = NULL;

    /* Connect to booted NCS2 */
    printf(reset_mode ? "Connecting to booted NCS2\n"
                      : "[1/6] Connecting to booted NCS2\n");

    int rc = libusb_init(&usb_ctx);
    if (rc < 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_strerror(rc));
        return 1;
    }

    usb_dev = find_booted_ncs2();
    if (!usb_dev) {
        fprintf(stderr, "  No booted NCS2 found (PID=0x%04X).\n"
                        "  Run ncs2_boot first.\n", NCS2_PID_BOOTED);
        libusb_exit(usb_ctx);
        return 1;
    }
    printf("  Found booted NCS2\n");

    libusb_detach_kernel_driver(usb_dev, 0);
    rc = libusb_claim_interface(usb_dev, 0);
    if (rc < 0) {
        fprintf(stderr, "claim_interface: %s\n", libusb_strerror(rc));
        libusb_close(usb_dev); libusb_exit(usb_ctx);
        return 1;
    }

    /* Flush any stale data from previous sessions */
    {
        uint8_t flush_buf[512];
        int flush_xfer;
        for (int i = 0; i < 32; i++) {
            int frc = libusb_bulk_transfer(usb_dev, USB_EP_IN, flush_buf,
                                            (int)sizeof(flush_buf),
                                            &flush_xfer, 100);
            if (frc == LIBUSB_ERROR_TIMEOUT)
                break;
            if (frc != 0)
                break;
            if (verbose)
                printf("  Flushed %d stale bytes\n", flush_xfer);
        }
    }

    /* Let the device fully initialize after boot */
    usleep(2000000);

    /* XLink handshake */
    printf("  XLink PING...");
    fflush(stdout);
    if (xlink_ping() < 0) {
        fprintf(stderr, " failed\n");
        goto cleanup;
    }
    printf(" OK\n");

    /* Reset mode — send RESET_REQ and wait for device to revert to bootrom */
    if (reset_mode) {
        printf("Sending XLINK_RESET_REQ\n");
        xlink_send(XLINK_RESET_REQ, 0, NULL, 0, NULL);
        usleep(500000);
        printf("Waiting for device to reset...\n");
        libusb_release_interface(usb_dev, 0);
        libusb_close(usb_dev);
        usb_dev = NULL;

        for (int i = 0; i < 10; i++) {
            usleep(1000000);
            libusb_device **devs;
            ssize_t cnt = libusb_get_device_list(usb_ctx, &devs);
            if (cnt < 0) continue;
            int found_booted = 0, found_unbooted = 0;
            for (ssize_t d = 0; d < cnt; d++) {
                struct libusb_device_descriptor desc;
                if (libusb_get_device_descriptor(devs[d], &desc) < 0)
                    continue;
                if (desc.idVendor == NCS2_VID) {
                    if (desc.idProduct == 0x2485) found_unbooted = 1;
                    if (desc.idProduct == NCS2_PID_BOOTED) found_booted = 1;
                }
            }
            libusb_free_device_list(devs, 1);

            if (found_unbooted) {
                printf("  Device reverted to unbooted state (PID=0x2485)\n");
                libusb_exit(usb_ctx);
                return 0;
            }
            if (!found_booted) {
                printf("  Device disconnected (waiting for re-enumerate)...\n");
            } else {
                printf("  Still booted (%d/10)...\n", i + 1);
            }
        }
        fprintf(stderr, "  Device did not reset to unbooted state.\n"
                        "  Try unplugging and replugging the NCS2.\n");
        libusb_exit(usb_ctx);
        return 1;
    }

    /* Load model blob */
    printf("[2/6] Loading model: %s\n", blob_path);
    size_t blob_size;
    blob = read_file(blob_path, &blob_size);
    if (!blob) goto cleanup;
    printf("  Loaded %zu bytes (%.1f KB)\n", blob_size, blob_size / 1024.0);

    uint32_t header_size = get_blob_header_size(blob, blob_size);
    printf("  Graph header: %u bytes\n", header_size);

    size_t input_size = 0;
    if (input_path) {
        input_data = read_file(input_path, &input_size);
        if (!input_data) { free(blob); goto cleanup; }
        printf("  Input data: %zu bytes\n", input_size);
    }

    /* Open deviceMonitor stream and get capabilities */
    printf("[3/6] Querying device capabilities\n");
    uint32_t dm_stream;
    if (xlink_open_stream("deviceMonitor", CONFIG_STREAM_SIZE, &dm_stream) < 0) {
        fprintf(stderr, "Failed to open deviceMonitor stream\n");
        goto cleanup;
    }
    printf("  deviceMonitor stream ID: %u\n", dm_stream);

    device_caps_t caps;
    if (ncs2_get_capabilities(dm_stream, &caps) < 0)
        goto cleanup;

    printf("  Firmware version: %u.%u.%u.%u\n",
           caps.fw_version[0], caps.fw_version[1],
           caps.fw_version[2], caps.fw_version[3]);
    printf("  Max graphs: %u  Max FIFOs: %u  Memory: %u bytes\n",
           caps.max_graphs, caps.max_fifos, caps.max_memory);

    /* Open graphMonitor stream */
    uint32_t gm_stream;
    if (xlink_open_stream("graphMonitor", GRAPH_STREAM_SIZE, &gm_stream) < 0) {
        fprintf(stderr, "Failed to open graphMonitor stream\n");
        goto cleanup;
    }
    printf("  graphMonitor stream ID: %u\n", gm_stream);

    /* Allocate graph */
    printf("[4/6] Allocating graph on device\n");
    uint32_t graph_stream;
    tensor_desc_t in_desc, out_desc;
    uint32_t nstages;

    if (ncs2_graph_allocate(gm_stream, blob, (uint32_t)blob_size,
                             header_size, &graph_stream,
                             &in_desc, &out_desc, &nstages) < 0) {
        fprintf(stderr, "Graph allocation failed\n");
        goto cleanup;
    }

    printf("  Graph allocated successfully (%u stages)\n", nstages);
    print_tensor("Input", &in_desc);
    print_tensor("Output", &out_desc);

    /* Allocate FIFOs */
    printf("[5/6] Setting up FIFOs\n");
    uint32_t in_fifo_stream, out_fifo_stream;

    if (ncs2_fifo_allocate(gm_stream, 0, &in_desc, 1, &in_fifo_stream) < 0) {
        fprintf(stderr, "Input FIFO allocation failed\n");
        goto cleanup;
    }
    if (ncs2_fifo_allocate(gm_stream, 1, &out_desc, 0, &out_fifo_stream) < 0) {
        fprintf(stderr, "Output FIFO allocation failed\n");
        goto cleanup;
    }

    /* Prepare input */
    uint32_t tensor_size = in_desc.total_size;
    uint8_t *tensor_in = calloc(1, tensor_size);
    if (!tensor_in) {
        fprintf(stderr, "Out of memory for input tensor\n");
        goto cleanup;
    }

    if (input_data) {
        uint32_t copy_size = (input_size < tensor_size) ?
                              (uint32_t)input_size : tensor_size;
        memcpy(tensor_in, input_data, copy_size);
        if (input_size != tensor_size)
            printf("  Warning: input file is %zu bytes, tensor expects %u\n",
                   input_size, tensor_size);
    } else {
        printf("  No input file — using zeros (%u bytes)\n", tensor_size);
    }

    /* Run inference */
    printf("[6/6] Running inference\n");

    /* Write input tensor to input FIFO */
    printf("  Writing input tensor (%u bytes)\n", tensor_size);
    if (xlink_write_data(in_fifo_stream, tensor_in, tensor_size) < 0) {
        fprintf(stderr, "Failed to write input tensor\n");
        free(tensor_in);
        goto cleanup;
    }
    free(tensor_in);

    /* Trigger inference */
    printf("  Triggering inference\n");
    if (ncs2_trigger_inference(gm_stream, 0, 0, 1) < 0) {
        fprintf(stderr, "Inference trigger failed\n");
        goto cleanup;
    }

    /* Read output */
    uint32_t out_size = out_desc.total_size;
    uint8_t *tensor_out = malloc(out_size);
    if (!tensor_out) {
        fprintf(stderr, "Out of memory for output tensor\n");
        goto cleanup;
    }

    printf("  Reading output tensor\n");
    uint32_t actual_out;
    if (xlink_read_data(out_fifo_stream, tensor_out, out_size, &actual_out) < 0) {
        fprintf(stderr, "Failed to read output tensor\n");
        free(tensor_out);
        goto cleanup;
    }
    xlink_release(out_fifo_stream);

    printf("  Inference complete — %u bytes output\n", actual_out);

    /* Save or display output */
    if (output_path) {
        FILE *f = fopen(output_path, "wb");
        if (f) {
            fwrite(tensor_out, 1, actual_out, f);
            fclose(f);
            printf("  Output written to: %s\n", output_path);
        } else {
            perror(output_path);
        }
    }

    print_output(tensor_out, actual_out);
    free(tensor_out);

    printf("\nDone.\n");

    /* Cleanup */
cleanup:
    free(blob);
    free(input_data);

    for (int i = 0; i < MAX_PENDING; i++) {
        if (pending[i].valid) free(pending[i].data);
    }

    if (usb_dev) {
        libusb_release_interface(usb_dev, 0);
        libusb_close(usb_dev);
    }
    libusb_exit(usb_ctx);

    return 0;
}
