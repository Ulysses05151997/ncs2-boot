/*
 * ncs2_boot.c — Boot an Intel Neural Compute Stick 2 on modern Linux
 *               without OpenVINO.
 *
 * Author:  Ulysses Isa
 * License: MIT
 *
 * The NCS2 (Myriad X / MA2485) arrives on the USB bus as an unbooted
 * device (VID 0x03E7, PID 0x2485). Its internal bootrom waits for
 * firmware to be sent over USB bulk transfer. Once received, it
 * re-enumerates as PID 0xF63B.
 *
 * This tool performs that firmware upload using libusb-1.0 directly,
 * replacing the entire OpenVINO runtime for the boot step.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <libusb-1.0/libusb.h>

#define NCS2_VID            0x03E7
#define NCS2_PID_UNBOOTED   0x2485
#define NCS2_PID_BOOTED     0xF63B

#define BULK_CHUNK_SIZE     (1024 * 1024)   /* 1 MB, matches OpenVINO */
#define USB1_CHUNK_SIZE     64
#define WRITE_TIMEOUT_MS    2000
#define BOOT_WAIT_SEC       5

static uint8_t *read_firmware(const char *path, size_t *size_out)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) {
        fprintf(stderr, "Firmware file is empty or unreadable\n");
        fclose(f);
        return NULL;
    }

    uint8_t *buf = malloc((size_t)sz);
    if (!buf) {
        fprintf(stderr, "Failed to allocate %ld bytes for firmware\n", sz);
        fclose(f);
        return NULL;
    }

    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fprintf(stderr, "Failed to read firmware file\n");
        free(buf);
        fclose(f);
        return NULL;
    }

    fclose(f);
    *size_out = (size_t)sz;
    return buf;
}

static libusb_device_handle *find_ncs2(libusb_context *ctx, int pid)
{
    libusb_device **devs;
    ssize_t cnt = libusb_get_device_list(ctx, &devs);
    if (cnt < 0) {
        fprintf(stderr, "libusb_get_device_list: %s\n", libusb_strerror((int)cnt));
        return NULL;
    }

    libusb_device_handle *handle = NULL;

    for (ssize_t i = 0; i < cnt; i++) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(devs[i], &desc) < 0)
            continue;

        if (desc.idVendor == NCS2_VID && desc.idProduct == pid) {
            int rc = libusb_open(devs[i], &handle);
            if (rc < 0) {
                fprintf(stderr, "libusb_open: %s\n", libusb_strerror(rc));
                handle = NULL;
            }
            break;
        }
    }

    libusb_free_device_list(devs, 1);
    return handle;
}

static int find_bulk_out_endpoint(libusb_device_handle *handle, uint8_t *ep_out,
                                  int *chunk_size)
{
    libusb_device *dev = libusb_get_device(handle);
    struct libusb_config_descriptor *cdesc;

    int rc = libusb_get_config_descriptor(dev, 0, &cdesc);
    if (rc < 0) {
        fprintf(stderr, "get_config_descriptor: %s\n", libusb_strerror(rc));
        return -1;
    }

    /* Check USB speed to set chunk size */
    struct libusb_device_descriptor ddesc;
    libusb_get_device_descriptor(dev, &ddesc);
    *chunk_size = (ddesc.bcdUSB < 0x0200) ? USB1_CHUNK_SIZE : BULK_CHUNK_SIZE;

    const struct libusb_interface_descriptor *ifdesc =
        cdesc->interface[0].altsetting;

    for (int i = 0; i < ifdesc->bNumEndpoints; i++) {
        const struct libusb_endpoint_descriptor *ep = &ifdesc->endpoint[i];

        if ((ep->bmAttributes & LIBUSB_TRANSFER_TYPE_MASK) !=
            LIBUSB_TRANSFER_TYPE_BULK)
            continue;

        /* We want the OUT endpoint */
        if (!(ep->bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK)) {
            *ep_out = ep->bEndpointAddress;
            libusb_free_config_descriptor(cdesc);
            printf("  Bulk OUT endpoint: 0x%02X  chunk size: %d bytes\n",
                   *ep_out, *chunk_size);
            return 0;
        }
    }

    libusb_free_config_descriptor(cdesc);
    fprintf(stderr, "No BULK OUT endpoint found on device\n");
    return -1;
}

static int send_firmware(libusb_device_handle *handle, uint8_t endpoint,
                         const uint8_t *fw, size_t fw_size, int chunk_size)
{
    size_t sent = 0;
    int transferred;

    printf("  Sending %zu bytes", fw_size);
    fflush(stdout);

    while (sent < fw_size) {
        int to_send = (int)(fw_size - sent);
        if (to_send > chunk_size)
            to_send = chunk_size;

        int rc = libusb_bulk_transfer(handle, endpoint,
                                      (uint8_t *)(fw + sent),
                                      to_send, &transferred,
                                      WRITE_TIMEOUT_MS);
        if (rc != 0) {
            printf("\n");
            fprintf(stderr, "Bulk transfer failed at offset %zu: %s\n",
                    sent, libusb_strerror(rc));
            return -1;
        }

        sent += (size_t)transferred;
        printf(".");
        fflush(stdout);
    }

    printf(" done\n");
    return 0;
}

static int wait_for_boot(libusb_context *ctx)
{
    printf("  Waiting for device to re-enumerate as 0x%04X", NCS2_PID_BOOTED);
    fflush(stdout);

    for (int i = 0; i < BOOT_WAIT_SEC * 10; i++) {
        usleep(100000);   /* 100ms */
        printf(".");
        fflush(stdout);

        libusb_device_handle *h = find_ncs2(ctx, NCS2_PID_BOOTED);
        if (h) {
            libusb_close(h);
            printf(" found!\n");
            return 0;
        }
    }

    printf(" not found\n");
    return -1;
}

int main(int argc, char *argv[])
{
    /* Line-buffer stdout so progress and error messages stay in order
     * when output is piped (for example through tee into a log). */
    setvbuf(stdout, NULL, _IOLBF, 0);

    if (argc < 2) {
        fprintf(stderr,
                "Usage: %s <firmware.mvcmd>\n\n"
                "Boots an Intel NCS2 (Myriad X) by uploading firmware over USB.\n"
                "The firmware file is typically 'usb-ma2x8x.mvcmd' from an\n"
                "OpenVINO 2022.3 installation.\n",
                argv[0]);
        return 1;
    }

    const char *fw_path = argv[1];

    /* Read firmware */
    printf("[1/4] Reading firmware: %s\n", fw_path);
    size_t fw_size;
    uint8_t *fw = read_firmware(fw_path, &fw_size);
    if (!fw)
        return 1;
    printf("  Loaded %zu bytes (%.1f MB)\n", fw_size, fw_size / 1048576.0);

    /* Verify magic */
    if (fw_size < 4 || memcmp(fw, "MA2x", 4) != 0) {
        fprintf(stderr, "Warning: firmware does not start with 'MA2x' magic.\n"
                        "         This may not be a valid .mvcmd file.\n");
    }

    /* Init libusb */
    libusb_context *ctx;
    int rc = libusb_init(&ctx);
    if (rc < 0) {
        fprintf(stderr, "libusb_init: %s\n", libusb_strerror(rc));
        free(fw);
        return 1;
    }

    /* Find unbooted NCS2 */
    printf("[2/4] Searching for unbooted NCS2 (VID=0x%04X PID=0x%04X)\n",
           NCS2_VID, NCS2_PID_UNBOOTED);

    libusb_device_handle *handle = find_ncs2(ctx, NCS2_PID_UNBOOTED);
    if (!handle) {
        /* Maybe it's already booted? */
        libusb_device_handle *booted = find_ncs2(ctx, NCS2_PID_BOOTED);
        if (booted) {
            printf("  Device is already booted (PID=0x%04X). Nothing to do.\n",
                   NCS2_PID_BOOTED);
            libusb_close(booted);
            libusb_exit(ctx);
            free(fw);
            return 0;
        }
        fprintf(stderr, "  No NCS2 found on USB bus.\n"
                        "  Check that the stick is plugged in and that you have\n"
                        "  permission to access USB devices (try running with sudo,\n"
                        "  or install the udev rules).\n");
        libusb_exit(ctx);
        free(fw);
        return 1;
    }
    printf("  Found unbooted NCS2\n");

    /* Set up USB */
    rc = libusb_set_configuration(handle, 1);
    if (rc < 0) {
        fprintf(stderr, "set_configuration: %s\n", libusb_strerror(rc));
        libusb_close(handle);
        libusb_exit(ctx);
        free(fw);
        return 1;
    }

    rc = libusb_claim_interface(handle, 0);
    if (rc < 0) {
        fprintf(stderr, "claim_interface: %s\n", libusb_strerror(rc));
        libusb_close(handle);
        libusb_exit(ctx);
        free(fw);
        return 1;
    }

    uint8_t ep_out;
    int chunk_size;
    if (find_bulk_out_endpoint(handle, &ep_out, &chunk_size) < 0) {
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(ctx);
        free(fw);
        return 1;
    }

    /* Send firmware */
    printf("[3/4] Uploading firmware to NCS2\n");
    if (send_firmware(handle, ep_out, fw, fw_size, chunk_size) < 0) {
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(ctx);
        free(fw);
        return 1;
    }

    /* Clean up handle — device will disconnect during reboot */
    libusb_release_interface(handle, 0);
    libusb_close(handle);
    free(fw);

    /* Wait for device to reappear */
    printf("[4/4] Verifying boot\n");
    rc = wait_for_boot(ctx);

    libusb_exit(ctx);

    if (rc == 0) {
        printf("\nNCS2 booted successfully.\n");
        return 0;
    } else {
        fprintf(stderr, "\nDevice did not re-enumerate within %d seconds.\n"
                        "The firmware may have failed to boot, or the device\n"
                        "may need more time. Check 'lsusb' manually.\n"
                        "If the stick vanished from 'lsusb' entirely, see\n"
                        "\"Troubleshooting\" in README.md.\n",
                BOOT_WAIT_SEC);
        return 1;
    }
}
