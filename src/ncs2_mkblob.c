/*
 * ncs2_mkblob.c — Generate a minimal .blob file for the NCS2.
 *
 * Author:  Ulysses Isa
 * License: MIT
 *
 * Produces a structurally valid blob that the Myriad X firmware can
 * parse during GRAPH_ALLOCATE.  The "model" is a no-op — it exists
 * only to exercise the full inference protocol pipeline.
 *
 * Binary format reverse-engineered from OpenVINO 2022.3:
 *   graph_transformer/include/vpu/backend/blob_format.hpp
 *   graph_transformer/src/backend/serialize.cpp
 *   graph_transformer/src/model/data.cpp
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#define BLOB_MAGIC    9709
#define BLOB_VER_MAJ  6
#define BLOB_VER_MIN  0

#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

/* DataType enum from data_desc.hpp */
#define DT_FP16  0

/* DimsOrder codes from data_desc.cpp */
#define ORDER_NCHW  0x4321
#define ORDER_NC    0x21

/* Location enum from data.hpp */
#define LOC_NONE   0
#define LOC_INPUT  1
#define LOC_OUTPUT 2
#define LOC_BLOB   3
#define LOC_BSS    4

#pragma pack(push, 1)

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} elf_hdr_t;    /* 52 bytes */

typedef struct {
    uint32_t magic_number;
    uint32_t file_size;
    uint32_t blob_ver_major;
    uint32_t blob_ver_minor;
    uint32_t inputs_count;
    uint32_t outputs_count;
    uint32_t stages_count;
    uint32_t inputs_size;
    uint32_t outputs_size;
    uint32_t batch_size;
    uint32_t bss_mem_size;
    uint32_t number_of_cmx_slices;
    uint32_t number_of_shaves;
    uint32_t has_hw_stage;
    uint32_t has_shave_stage;
    uint32_t has_dma_stage;
    uint32_t input_info_section_offset;
    uint32_t output_info_section_offset;
    uint32_t stage_section_offset;
    uint32_t const_data_section_offset;
} blob_hdr_t;   /* 80 bytes */

#pragma pack(pop)

/* Helper: append uint32_t to buffer */
static int buf_u32(uint8_t *buf, int off, uint32_t val)
{
    memcpy(buf + off, &val, 4);
    return off + 4;
}

/* Helper: append string (padded to align 16) */
static int buf_str(uint8_t *buf, int off, const char *s)
{
    uint32_t len = (uint32_t)strlen(s);
    uint32_t padded = ALIGN_UP(len + 1, 16);
    off = buf_u32(buf, off, padded);
    memcpy(buf + off, s, len);
    memset(buf + off + len, 0, padded - len);
    return off + (int)padded;
}

/*
 * Write one IO info entry matching the format from:
 *   DataNode::serializeIOInfo + DataNode::serializeDescImpl
 *
 * Format:
 *   u32 ioIdx
 *   u32 ioBufferOffset
 *   u32 nameLength (aligned to 16)
 *   char name[nameLength]
 *   u32 dataType
 *   u32 dimsOrder
 *   u32 numDims
 *   u32 dimsLocation
 *   u32 dimsOffset       (offset in blob where dims are stored)
 *   u32 stridesLocation
 *   u32 stridesOffset     (offset in blob where strides are stored)
 */
static int write_io_entry(uint8_t *buf, int idx, const char *name,
                           uint32_t data_type, uint32_t dims_order,
                           uint32_t num_dims, uint32_t dims_offset,
                           uint32_t strides_offset)
{
    int off = 0;
    off = buf_u32(buf, off, (uint32_t)idx);       /* ioIdx */
    off = buf_u32(buf, off, 0);                    /* ioBufferOffset */
    off = buf_str(buf, off, name);                 /* name */
    off = buf_u32(buf, off, data_type);            /* dataType */
    off = buf_u32(buf, off, dims_order);           /* dimsOrder */
    off = buf_u32(buf, off, num_dims);             /* numDims */
    off = buf_u32(buf, off, LOC_BLOB);             /* dimsLocation */
    off = buf_u32(buf, off, dims_offset);          /* dimsOffset */
    off = buf_u32(buf, off, LOC_BLOB);             /* stridesLocation */
    off = buf_u32(buf, off, strides_offset);       /* stridesOffset */
    return off;
}

int main(int argc, char *argv[])
{
    const char *output_path = "test.blob";
    if (argc > 1) output_path = argv[1];

    /* Tensor dimensions — small enough to not run out of device memory */
    uint32_t in_n = 1, in_c = 3, in_h = 8, in_w = 8;
    uint32_t out_n = 1, out_c = 1, out_h = 1, out_w = 1;
    uint32_t in_total  = in_n * in_c * in_h * in_w * 2;   /* FP16 */
    uint32_t out_total = out_n * out_c * out_h * out_w * 2;

    uint32_t hdr_size = ALIGN_UP(sizeof(elf_hdr_t) + sizeof(blob_hdr_t), 64);

    /* Build IO info sections */
    uint8_t in_info[256], out_info[256];
    memset(in_info, 0, sizeof(in_info));
    memset(out_info, 0, sizeof(out_info));

    /* We'll store dims and strides in the const_data section.
     * Dims: 4 x uint32_t per tensor = 16 bytes each
     * Strides: 4 x uint32_t per tensor = 16 bytes each
     * Layout in const data section:
     *   [in_dims 16B] [in_strides 16B] [out_dims 16B] [out_strides 16B]
     */
    uint32_t in_info_size  = ALIGN_UP(128, 64);
    uint32_t out_info_size = ALIGN_UP(128, 64);
    uint32_t stage_size    = 64;  /* empty stage section */
    uint32_t const_size    = 64;  /* dims + strides data */

    uint32_t in_info_off   = hdr_size;
    uint32_t out_info_off  = in_info_off + in_info_size;
    uint32_t stage_off     = out_info_off + out_info_size;
    uint32_t const_off     = stage_off + stage_size;
    uint32_t file_size     = const_off + const_size;

    /* Offsets within const_data section for dims/strides arrays */
    uint32_t in_dims_off     = const_off;
    uint32_t in_strides_off  = const_off + 16;
    uint32_t out_dims_off    = const_off + 32;
    uint32_t out_strides_off = const_off + 48;

    int in_len  = write_io_entry(in_info, 0, "input", DT_FP16, ORDER_NCHW,
                                  4, in_dims_off, in_strides_off);
    int out_len = write_io_entry(out_info, 0, "output", DT_FP16, ORDER_NCHW,
                                  4, out_dims_off, out_strides_off);
    (void)in_len;
    (void)out_len;

    /* Build const data section with actual dims and strides */
    uint8_t const_data[64];
    memset(const_data, 0, sizeof(const_data));
    int p = 0;
    /* Input dims: N=1 C=3 H=8 W=8 */
    p = buf_u32(const_data, p, in_n);
    p = buf_u32(const_data, p, in_c);
    p = buf_u32(const_data, p, in_h);
    p = buf_u32(const_data, p, in_w);
    /* Input strides (FP16, NCHW): W=2, H=W*w=16, C=H*h=128, N=C*c=384 */
    p = buf_u32(const_data, p, in_c * in_h * in_w * 2); /* N stride */
    p = buf_u32(const_data, p, in_h * in_w * 2);         /* C stride */
    p = buf_u32(const_data, p, in_w * 2);                 /* H stride */
    p = buf_u32(const_data, p, 2);                         /* W stride */
    /* Output dims: N=1 C=1 H=1 W=1 */
    p = buf_u32(const_data, p, out_n);
    p = buf_u32(const_data, p, out_c);
    p = buf_u32(const_data, p, out_h);
    p = buf_u32(const_data, p, out_w);
    /* Output strides (FP16, NCHW) */
    p = buf_u32(const_data, p, out_c * out_h * out_w * 2);
    p = buf_u32(const_data, p, out_h * out_w * 2);
    p = buf_u32(const_data, p, out_w * 2);
    p = buf_u32(const_data, p, 2);
    (void)p;

    /* Build headers */
    elf_hdr_t elf;
    memset(&elf, 0, sizeof(elf));
    elf.e_ident[0] = 0x7f;
    elf.e_ident[1] = 'E';
    elf.e_ident[2] = 'L';
    elf.e_ident[3] = 'F';
    elf.e_ident[4] = 1;
    elf.e_ident[5] = 1;
    elf.e_ident[6] = 1;
    elf.e_type    = 1;
    elf.e_machine = 2;
    elf.e_version = 2;
    elf.e_ehsize  = sizeof(elf) * 8;

    blob_hdr_t blob;
    memset(&blob, 0, sizeof(blob));
    blob.magic_number              = BLOB_MAGIC;
    blob.file_size                 = file_size;
    blob.blob_ver_major            = BLOB_VER_MAJ;
    blob.blob_ver_minor            = BLOB_VER_MIN;
    blob.inputs_count              = 1;
    blob.outputs_count             = 1;
    blob.stages_count              = 0;
    blob.inputs_size               = in_total;
    blob.outputs_size              = out_total;
    blob.batch_size                = 1;
    blob.bss_mem_size              = 0;
    blob.number_of_cmx_slices      = 16;
    blob.number_of_shaves          = 16;
    blob.has_hw_stage              = 0;
    blob.has_shave_stage           = 0;
    blob.has_dma_stage             = 0;
    blob.input_info_section_offset = in_info_off;
    blob.output_info_section_offset = out_info_off;
    blob.stage_section_offset      = stage_off;
    blob.const_data_section_offset = const_off;

    /* Write file */
    FILE *f = fopen(output_path, "wb");
    if (!f) { perror(output_path); return 1; }

    uint8_t zeros[256];
    memset(zeros, 0, sizeof(zeros));

    /* Header (padded to hdr_size) */
    fwrite(&elf, sizeof(elf), 1, f);
    fwrite(&blob, sizeof(blob), 1, f);
    uint32_t written = sizeof(elf) + sizeof(blob);
    if (written < hdr_size)
        fwrite(zeros, hdr_size - written, 1, f);

    /* Input info section (padded to in_info_size) */
    fwrite(in_info, 128, 1, f);
    if (128 < in_info_size)
        fwrite(zeros, in_info_size - 128, 1, f);

    /* Output info section (padded to out_info_size) */
    fwrite(out_info, 128, 1, f);
    if (128 < out_info_size)
        fwrite(zeros, out_info_size - 128, 1, f);

    /* Stage section (empty, padded) */
    fwrite(zeros, stage_size, 1, f);

    /* Const data section (dims + strides) */
    fwrite(const_data, sizeof(const_data), 1, f);

    fclose(f);

    printf("Created %s (%u bytes)\n", output_path, file_size);
    printf("  Input tensor:  %ux%ux%ux%u FP16 = %u bytes\n",
           in_n, in_c, in_h, in_w, in_total);
    printf("  Output tensor: %ux%ux%ux%u FP16 = %u bytes\n",
           out_n, out_c, out_h, out_w, out_total);

    return 0;
}
