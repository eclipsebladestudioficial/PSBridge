#include <dmaKit.h>
#include <ee_regs.h>
#include <gsKit.h>
#include <kernel.h>
#include <string.h>
#include <vif_registers.h>

#include "ge_internal.h"

#define GS_GE_STRIDE 512
#define GS_GE_HEIGHT 272
#define GS_GE_OFFSET 32768
#define GS_GE_PACKET_SIZE 4096
#define GS_GE_TEXTURE_BYTES (1024 * 1024)
#define GS_GE_TEXTURE_ROWS 8

extern u8 __psbridge_heap_start[];
extern u8 __psbridge_heap_end[];

typedef enum {
    GE_GS_BLEND_NONE,
    GE_GS_BLEND_ADD,
    GE_GS_BLEND_REVERSE_SUBTRACT,
    GE_GS_BLEND_SOURCE_ALPHA,
} GeGsBlend;

typedef enum {
    GE_GS_TEXTURE_DECAL,
    GE_GS_TEXTURE_MODULATE_RGB,
    GE_GS_TEXTURE_MODULATE_RGBA,
} GeGsTextureFunction;

typedef enum {
    GE_GS_STATE_FRAME,
    GE_GS_STATE_ZBUF,
    GE_GS_STATE_TEST,
    GE_GS_STATE_SCISSOR,
    GE_GS_STATE_TEXTURE,
    GE_GS_STATE_CLAMP,
    GE_GS_STATE_ALPHA,
    GE_GS_STATE_COUNT,
} GeGsDrawState;

typedef struct {
    u64 data;
    u64 address;
} GeGifRegister;

typedef struct {
    u8 *color;
    u8 *depth;
    u32 vram;
    int format;
    int depth_written;
    int replace_color;
    int replace_depth;
    int top;
    int bottom;
    unsigned count;
    unsigned state_valid;
    u64 state[GE_GS_STATE_COUNT];
} GeHardwareBatch;

typedef struct {
    const u8 *pixels;
    u32 revision;
    u32 clut;
    u32 layout;
    u32 size;
    u32 function;
    u32 environment;
    u32 color;
    GeGsBlend blend;
    int binary_alpha_required;
    GeGsTextureFunction texture_function;
    int blend_alpha_ref;
} GeHardwareTextureKey;

typedef struct {
    GeHardwareTextureKey key;
    u64 tex0;
} GeHardwareTexture;

typedef struct {
    const u8 *pixels;
    u32 layout;
    u32 size;
    u32 vram;
} GeHardwareImage;

static GeHardwareBatch batch;
static GeHardwareTexture textures[16];
static GeHardwareImage images[16];
static unsigned texture_count;
static unsigned image_count;
static u32 texture_vram;
static unsigned texture_used;
static u32 decoded[GS_GE_STRIDE * GS_GE_TEXTURE_ROWS] __attribute__((aligned(64)));
static u32 depth_vram;
static GeGifRegister packet[GS_GE_PACKET_SIZE + 1] __attribute__((aligned(64)));

static void send_gif(const void *data, unsigned qwc)
{
    SyncDCache((void *)data, (u8 *)data + qwc * 16);
    dmaKit_send(DMA_CHANNEL_GIF, (void *)data, qwc);
    dmaKit_wait(DMA_CHANNEL_GIF, 0);
}

static void gs_transfer(u32 vram, int psm, int y, int height, int tbw, int width, int download)
{
    GeGifRegister transfer[] __attribute__((aligned(64))) = {
        { 0, GIF_AD },
        { download ? GS_SETREG_BITBLTBUF(vram / 256, tbw, psm, 0, 0, 0) :
            GS_SETREG_BITBLTBUF(0, 0, 0, vram / 256, tbw, psm), GS_BITBLTBUF },
        { download ? GS_SETREG_TRXPOS(0, y, 0, 0, 0) :
            GS_SETREG_TRXPOS(0, 0, 0, y, 0), GS_TRXPOS },
        { GS_SETREG_TRXREG(width, height), GS_TRXREG },
        { download, GS_TRXDIR },
    };

    transfer[0].data = GIF_TAG_AD(4);
    send_gif(transfer, sizeof(transfer) / sizeof(transfer[0]));
}

static void upload_rows(u8 *pixels, unsigned qwc)
{
    GeGifRegister image __attribute__((aligned(64))) = { 0, 0 };

    SyncDCache(pixels, pixels + qwc * 16);
    while (qwc) {
        unsigned chunk = qwc > 0x7fff ? 0x7fff : qwc;
        image.data = GIF_TAG(chunk, 1, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        send_gif(&image, 1);
        dmaKit_send(DMA_CHANNEL_GIF, pixels, chunk);
        dmaKit_wait(DMA_CHANNEL_GIF, 0);
        pixels += chunk * 16;
        qwc -= chunk;
    }
}

static void download_rows(u8 *pixels, unsigned qwc)
{
    VIF1_STAT = 1u << 23;
    GS_SETREG_BUSDIR(1);
    __asm__ volatile("sync.p" ::: "memory");
    *DMA_REG_STAT = 1u << DMA_CHANNEL_VIF1;
    *R_EE_D1_QWC = qwc;
    *R_EE_D1_MADR = DMA_SET_MADR((u32)(uintptr_t)pixels, 0);
    *R_EE_D1_CHCR = DMA_SET_CHCR(0, 0, 0, 0, 0, 1, 0);
    while (*R_EE_D1_CHCR & (1u << 8))
        __asm__ volatile("nop");
    VIF1_STAT = 0;
    GS_SETREG_BUSDIR(0);
    __asm__ volatile("sync.p" ::: "memory");
    InvalidDCache(pixels, pixels + qwc * 16);
}

void ge_hardware_flush(void)
{
    if (!batch.count)
        return;

    int psm = batch.format == 3 ? GS_PSM_CT32 : GS_PSM_CT16;
    unsigned row_bytes = GS_GE_STRIDE * (batch.format == 3 ? 4 : 2);
    int rows = batch.bottom - batch.top;
    unsigned qwc = rows * row_bytes / 16;
    u8 *pixels = batch.color + batch.top * row_bytes;

    u8 *depth = batch.depth ? batch.depth + batch.top * GS_GE_STRIDE * 2 : NULL;
    unsigned depth_qwc = rows * GS_GE_STRIDE * 2 / 16;

    if (!batch.replace_color) {
        gs_transfer(batch.vram, psm, batch.top, rows, GS_GE_STRIDE / 64, GS_GE_STRIDE, 0);
        upload_rows(pixels, qwc);
    } else {
        SyncDCache(pixels, pixels + qwc * 16);
    }
    if (depth && !batch.replace_depth) {
        gs_transfer(depth_vram, 0x30 | GS_PSMZ_16, batch.top, rows, GS_GE_STRIDE / 64, GS_GE_STRIDE, 0);
        upload_rows(depth, depth_qwc);
    } else if (depth) {
        SyncDCache(depth, depth + depth_qwc * 16);
    }

    GS_SETREG_CSR_FINISH(1);
    packet[++batch.count] = (GeGifRegister){ 0, GS_FINISH };
    packet[0].data = GIF_TAG_AD(batch.count);
    packet[0].address = GIF_AD;
    send_gif(packet, batch.count + 1);
    while (!(GS_CSR_FINISH))
        __asm__ volatile("nop");

    gs_transfer(batch.vram, psm, batch.top, rows, GS_GE_STRIDE / 64, GS_GE_STRIDE, 1);
    download_rows(pixels, qwc);
    if (batch.depth_written) {
        gs_transfer(depth_vram, 0x30 | GS_PSMZ_16, batch.top, rows, GS_GE_STRIDE / 64, GS_GE_STRIDE, 1);
        download_rows(depth, depth_qwc);
    }
    if (rows == GS_GE_HEIGHT)
        psbridge_graphics_draw_complete(batch.color, batch.format);
    batch.count = 0;
}

static void gs_reg(u64 address, u64 data)
{
    packet[++batch.count] = (GeGifRegister){ data, address };
}

static int gs_state_changed(GeGsDrawState field, u64 data)
{
    unsigned bit = 1u << field;
    if ((batch.state_valid & bit) && batch.state[field] == data)
        return 0;
    batch.state_valid |= bit;
    batch.state[field] = data;
    return 1;
}

static void gs_draw_reg(GeGsDrawState field, u64 address, u64 data)
{
    if (gs_state_changed(field, data))
        gs_reg(address, data);
}

static int begin_draw(int top, int bottom, u8 *depth, int replace_color, int replace_depth)
{
    int format = ge_state.command[GE_FRAMEBUF_FORMAT] & 3;
    int stride = ge_state.command[GE_VADDR_WIDTH] & 0x7fc;
    if ((format != 1 && format != 3) || stride != GS_GE_STRIDE ||
        top < 0 || bottom > GS_GE_HEIGHT || top >= bottom)
        return 0;

    u8 *color = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));
    uintptr_t offset = (uintptr_t)color - (uintptr_t)psbridge_video_memory();
    unsigned size = GS_GE_STRIDE * GS_GE_HEIGHT * (format == 3 ? 4 : 2);
    if ((offset & 63) || offset > 0x200000 - size)
        return 0;

    if (depth) {
        uintptr_t depth_offset = (uintptr_t)depth - (uintptr_t)psbridge_video_memory();
        unsigned depth_size = GS_GE_STRIDE * GS_GE_HEIGHT * 2;
        if ((depth_offset & 63) || depth_offset > 0x200000 - depth_size ||
            (depth_offset < offset + size && offset < depth_offset + depth_size))
            return 0;
        if (!depth_vram) {
            unsigned gs_depth_size = GS_GE_STRIDE * ((GS_GE_HEIGHT + 63) & ~63u) * 2;
            u32 allocation = psbridge_graphics_alloc_texture(gs_depth_size + 8191);
            if (allocation == GSKIT_ALLOC_ERROR)
                return 0;
            depth_vram = (allocation + 8191) & ~8191u;
        }
    }

    if (color != batch.color || format != batch.format ||
        (depth && batch.depth && depth != batch.depth) || batch.count + 40 >= GS_GE_PACKET_SIZE)
        ge_hardware_flush();

    if (!batch.count) {
        u32 vram;
        if (psbridge_graphics_draw_target(&vram) < 0)
            return 0;

        batch = (GeHardwareBatch){
            .color = color, .depth = depth, .vram = vram, .format = format,
            .replace_color = replace_color, .replace_depth = replace_depth,
            .top = top, .bottom = bottom,
        };
        gs_reg(GS_PRMODECONT, 1);
        gs_reg(GS_DTHE, 0);
        gs_reg(GS_PABE, 0);
        gs_reg(GS_FBA_2, 0);
        gs_reg(GS_COLCLAMP, 1);
        gs_reg(GS_XYOFFSET_2, GS_SETREG_XYOFFSET(GS_GE_OFFSET, GS_GE_OFFSET));
        gs_reg(GS_TEX1_2, GS_SETREG_TEX1(1, 0, 0, 0, 0, 0, 0));
    }

    if (depth && !batch.depth)
        batch.depth = depth;
    if (top < batch.top)
        batch.top = top;
    if (bottom > batch.bottom)
        batch.bottom = bottom;
    return 1;
}

static int reads_batch_rows(const void *source, unsigned size, const u8 *target,
    unsigned row_bytes)
{
    uintptr_t start = (uintptr_t)target + batch.top * row_bytes;
    uintptr_t read = (uintptr_t)source;
    return read >= start ? read - start < (batch.bottom - batch.top) * row_bytes :
        start - read < size;
}

void ge_hardware_read(const void *source, unsigned size)
{
    if (batch.count && source && size &&
        (reads_batch_rows(source, size, batch.color, GS_GE_STRIDE * (batch.format == 3 ? 4 : 2)) ||
         (batch.depth_written && reads_batch_rows(source, size, batch.depth, GS_GE_STRIDE * 2))))
        ge_hardware_flush();
}

void ge_hardware_cpu_write(void)
{
    ge_hardware_flush();
    psbridge_graphics_draw_invalidate();
}

void ge_hardware_texture_dirty(void)
{
    if (texture_count)
        ge_hardware_flush();
    texture_used = 0;
    texture_count = 0;
    image_count = 0;
}

static const u8 *texture_source(u32 address, unsigned size)
{
    const u8 *pixels = psbridge_ram_pointer(address, size);
    if (pixels)
        return pixels;

    uintptr_t start = (uintptr_t)__psbridge_heap_start & 0x1fffffffu;
    uintptr_t end = (uintptr_t)__psbridge_heap_end & 0x1fffffffu;
    if (address >= start && address < end && size <= end - address)
        return (const u8 *)(uintptr_t)(address | 0x80000000u);
    return NULL;
}

static u32 premultiply_for_gs(u32 color, GeGsBlend blend)
{
    unsigned alpha = color >> 24;
    unsigned bias = blend == GE_GS_BLEND_REVERSE_SUBTRACT ? 254 : 0;
    u32 result = color & 0xff000000u;

    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned product = ((color >> shift) & 255) * alpha + bias;
        result |= (product / 255) << shift;
    }
    return result;
}

static void upload_indexed_texture(
    const u8 *pixels, int format, int swizzled, int row_bytes,
    int width, int height, int tbw, int psm, u32 vram)
{
    int indices_per_byte = format == 4 ? 2 : 1;
    int source_width = width / indices_per_byte;
    int transfer_bytes = source_width < 16 ? 16 : source_width;
    gs_transfer(vram, psm, 0, height, tbw, transfer_bytes * indices_per_byte, 0);
    if (!swizzled && row_bytes == transfer_bytes && source_width == transfer_bytes &&
        !((uintptr_t)pixels & 15)) {
        upload_rows((u8 *)pixels, height * transfer_bytes / 16);
    } else {
        int chunk_rows = sizeof(decoded) / transfer_bytes;
        for (int y = 0; y < height; y += chunk_rows) {
            int rows = height - y < chunk_rows ? height - y : chunk_rows;
            u8 *out = (u8 *)decoded;
            for (int row = y; row < y + rows; row++, out += transfer_bytes) {
                const u8 *source = pixels + pixel_offset(0, row, row_bytes, 1, swizzled);
                if (swizzled) {
                    int x = 0;
                    for (; x + 16 <= source_width; x += 16, source += 128)
                        memcpy(out + x, source, 16);
                    if (x < source_width)
                        memcpy(out + x, source, source_width - x);
                } else {
                    memcpy(out, source, source_width);
                }
                if (transfer_bytes > source_width)
                    memset(out + source_width, 0, transfer_bytes - source_width);
            }
            upload_rows((u8 *)decoded, rows * transfer_bytes / 16);
        }
    }
}

static const GeHardwareTexture *load_texture(
    u32 vertex_color, GeGsBlend blend, int binary_alpha_required,
    GeGsTextureFunction texture_function, int blend_alpha_ref)
{
    int format = ge_state.command[GE_TEXFORMAT] & 15;
    int swizzled = ge_state.command[GE_TEXMODE] & 1;
    u32 size = ge_state.command[GE_TEXSIZE0] & 0xf0f;
    u32 width_shift = size & 15;
    u32 height_shift = size >> 8;
    int width = 1 << width_shift;
    int height = 1 << height_shift;
    int compressed = format == 8 || format == 9;
    if (format > 5 && !compressed)
        return NULL;
    int stride = texture_buffer_width(format);
    static const int bits[] = { 16, 16, 16, 32, 4, 8 };
    if (width < 4 || width > 512 || height > 512 || width > stride)
        return NULL;

    int block_bytes = format == 8 ? 8 : 16;
    int row_bytes = compressed ? ((stride + 3) >> 2) * block_bytes :
        (stride * bits[format] + 7) / 8;
    if (!compressed && swizzled && (row_bytes & 15))
        return NULL;

    u32 address = buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0);
    unsigned source_size;
    if (compressed) {
        source_size = ((height - 1) >> 2) * row_bytes + ((width + 3) >> 2) * block_bytes;
    } else {
        int used_bytes = (width * bits[format] + 7) / 8;
        source_size = pixel_offset(used_bytes - 1, height - 1, row_bytes, 1, swizzled) + 1;
    }
    const u8 *pixels = texture_source(address, source_size);
    if (!pixels)
        return NULL;
    ge_hardware_read(pixels, source_size);

    int indexed = format == 4 || format == 5;
    u32 revision = indexed ? palette_revision() : 0;
    if (indexed && !revision)
        return NULL;

    GeHardwareTextureKey key = {
        pixels, revision, indexed ? ge_state.command[GE_CLUTFORMAT] : 0,
        (row_bytes << 5) | (swizzled << 4) | format, size,
        ge_state.command[GE_TEXFUNC], ge_state.command[GE_TEXENVCOLOR], vertex_color,
        blend, binary_alpha_required, texture_function, blend_alpha_ref,
    };
    for (unsigned i = 0; i < texture_count; i++) {
        if (!memcmp(&key, &textures[i].key, sizeof(key)))
            return textures[i].tex0 ? &textures[i] : NULL;
    }

    int tbw = indexed ? 2 * ((width + 127) / 128) : (width + 63) / 64;
    int psm = format == 4 ? GS_PSM_T4 : format == 5 ? GS_PSM_T8 : GS_PSM_CT32;
    unsigned texture_bytes = gsKit_texture_size(tbw * 64, height, psm);
    unsigned palette_bytes = format == 4 ? 256 : format == 5 ? 1024 : 0;
    if (texture_bytes + palette_bytes > GS_GE_TEXTURE_BYTES)
        return NULL;
    if (!texture_vram) {
        u32 allocation = psbridge_graphics_alloc_texture(GS_GE_TEXTURE_BYTES);
        if (allocation == GSKIT_ALLOC_ERROR)
            return NULL;
        texture_vram = allocation;
    }

    const GeHardwareImage *image = NULL;
    if (indexed) {
        for (unsigned i = 0; i < image_count; i++) {
            if (images[i].pixels == pixels && images[i].layout == key.layout && images[i].size == size) {
                image = &images[i];
                break;
            }
        }
    }

    if ((image ? palette_bytes : texture_bytes + palette_bytes) > GS_GE_TEXTURE_BYTES - texture_used ||
        texture_count == sizeof(textures) / sizeof(textures[0])) {
        ge_hardware_texture_dirty();
        image = NULL;
    }

    u32 palette[256] __attribute__((aligned(64)));
    int binary_alpha = 1;
    if (indexed) {
        int entries = format == 4 ? 16 : 256;
        for (int i = 0; i < entries; i++) {
            u32 color = shade_pixel(palette_color(i), vertex_color);
            unsigned alpha = color >> 24;
            binary_alpha &= alpha == 0 || alpha == 255;
            int slot = format == 4 ? i : (i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1);
            if (texture_function == GE_GS_TEXTURE_MODULATE_RGBA) {
                color = (color & 0xffffffu) | (((alpha + (alpha == 255)) >> 1) << 24);
            } else if (blend == GE_GS_BLEND_SOURCE_ALPHA) {
                unsigned weight = alpha > (unsigned)blend_alpha_ref ?
                    (alpha + (alpha == 1 || alpha == 255)) >> 1 : 0;
                color = (color & 0xffffffu) | (weight << 24);
            } else if (blend != GE_GS_BLEND_NONE) {
                color = premultiply_for_gs(color, blend);
            }
            palette[slot] = color;
        }
        if (binary_alpha_required && !binary_alpha) {
            textures[texture_count++] = (GeHardwareTexture){ .key = key };
            return NULL;
        }
    }

    u32 free_vram = texture_vram + texture_used;
    u32 vram = image ? image->vram : free_vram;
    u32 clut = 0;
    if (indexed) {
        if (!image) {
            upload_indexed_texture(pixels, format, swizzled, row_bytes, width, height, tbw, psm, vram);
            images[image_count++] = (GeHardwareImage){ pixels, key.layout, size, vram };
        }
        clut = free_vram + (image ? 0 : texture_bytes);
        gs_transfer(clut, GS_PSM_CT32, 0, format == 4 ? 2 : 16, 1, format == 4 ? 8 : 16, 0);
        upload_rows((u8 *)palette, (format == 4 ? 16 : 256) * sizeof(*palette) / 16);
    } else {
        GeDxtSampler dxt = {
            .texture = pixels, .width = width, .height = height,
            .blocks_w = row_bytes / block_bytes, .bytes = block_bytes,
            .format = format,
        };
        gs_transfer(vram, GS_PSM_CT32, 0, height, tbw, width, 0);
        for (int y = 0; y < height; y += GS_GE_TEXTURE_ROWS) {
            int rows = height - y < GS_GE_TEXTURE_ROWS ? height - y : GS_GE_TEXTURE_ROWS;
            u32 *out = decoded;
            for (int row = y; row < y + rows; row++) {
                for (int x = 0; x < width; x++) {
                    u32 color;
                    if (compressed) {
                        color = shade_pixel(ge_dxt_pixel(&dxt, x, row), vertex_color);
                    } else if (format == 3) {
                        u32 sample;
                        memcpy(&sample, pixels + pixel_offset(x, row, row_bytes, 4, swizzled), 4);
                        color = shade_pixel(sample, vertex_color);
                    } else {
                        u16 sample;
                        memcpy(&sample, pixels + pixel_offset(x, row, row_bytes, 2, swizzled), 2);
                        color = shade_pixel(unpack_color(sample, format), vertex_color);
                    }
                    if (blend != GE_GS_BLEND_NONE)
                        color = premultiply_for_gs(color, blend);
                    *out++ = color;
                    unsigned alpha = color >> 24;
                    binary_alpha &= alpha == 0 || alpha == 255;
                }
            }
            upload_rows((u8 *)decoded, rows * width / 4);
        }
    }

    texture_used += (image ? 0 : texture_bytes) + palette_bytes;
    GeHardwareTexture *entry = &textures[texture_count++];
    *entry = (GeHardwareTexture){ .key = key };
    if (binary_alpha_required && !binary_alpha)
        return NULL;
    entry->tex0 = GS_SETREG_TEX0(vram / 256, tbw, psm, width_shift, height_shift,
        1, texture_function == GE_GS_TEXTURE_DECAL, clut / 256, GS_PSM_CT32, 0, 0, indexed);
    return entry;
}

static void draw_state(int primitive, int left, int top, int right, int bottom,
    u32 mask, int depth_write, int depth_test, int alpha_test, int alpha_ref,
    const GeHardwareTexture *texture, GeGsBlend blend)
{
    gs_draw_reg(GE_GS_STATE_FRAME, GS_FRAME_2, GS_SETREG_FRAME(batch.vram / 8192, GS_GE_STRIDE / 64,
        batch.format == 3 ? GS_PSM_CT32 : GS_PSM_CT16, mask));
    gs_draw_reg(GE_GS_STATE_ZBUF, GS_ZBUF_2, GS_SETREG_ZBUF(depth_vram / 8192, GS_PSMZ_16, !depth_write));
    static const u8 gs_alpha_func[] = {0, 1, 4, 7, 2, 3, 6, 5};
    gs_draw_reg(GE_GS_STATE_TEST, GS_TEST_2, GS_SETREG_TEST(alpha_test != 1, gs_alpha_func[alpha_test],
        alpha_ref, 0, 0, 0, 1, depth_test));
    batch.depth_written |= depth_write;
    gs_draw_reg(GE_GS_STATE_SCISSOR, GS_SCISSOR_2, GS_SETREG_SCISSOR(left, right - 1, top, bottom - 1));
    if (texture) {
        u32 wrap = ge_state.command[GE_TEXWRAP];
        if (gs_state_changed(GE_GS_STATE_TEXTURE, texture->tex0)) {
            gs_reg(GS_TEXFLUSH, 0);
            gs_reg(GS_TEX0_2, texture->tex0);
        }
        gs_draw_reg(GE_GS_STATE_CLAMP, GS_CLAMP_2, GS_SETREG_CLAMP(wrap & 1, (wrap >> 8) & 1, 0, 0, 0, 0));
    }
    int weight = texture && texture->key.texture_function == GE_GS_TEXTURE_MODULATE_RGBA ? 0 : 2;
    if (blend == GE_GS_BLEND_ADD)
        gs_draw_reg(GE_GS_STATE_ALPHA, GS_ALPHA_2, GS_SETREG_ALPHA(0, 2, weight, 1, 128));
    else if (blend == GE_GS_BLEND_REVERSE_SUBTRACT)
        gs_draw_reg(GE_GS_STATE_ALPHA, GS_ALPHA_2, GS_SETREG_ALPHA(2, 0, weight, 1, 128));
    else if (blend == GE_GS_BLEND_SOURCE_ALPHA)
        gs_draw_reg(GE_GS_STATE_ALPHA, GS_ALPHA_2, GS_SETREG_ALPHA(0, 1, 0, 1, 0));
    gs_reg(GS_PRIM, GS_SETREG_PRIM(primitive, primitive == 3, texture != NULL, 0,
        blend != GE_GS_BLEND_NONE, 0,
        primitive == 6, 1, 0));
}

static u32 float_bits(float value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static u32 texture_vertex_color(u32 color, GeGsTextureFunction function, int full_alpha)
{
    u32 result = function == GE_GS_TEXTURE_MODULATE_RGBA ?
        (full_alpha ? color & 0xff000000u : (color >> 25) << 24) : 0x80000000u;
    for (unsigned shift = 0; shift < 24; shift += 8) {
        unsigned channel = (color >> shift) & 255;
        result |= ((channel + 1) >> 1) << shift;
    }
    return result;
}

static void emit_vertex(GeVertex vertex, const GeHardwareTexture *texture, int full_alpha)
{
    float depth = vertex.z;
    u32 z = !(depth >= 0.0f) ? 0 : depth >= 65535.0f ? 65535 : (u32)depth;
    u32 q = 0;
    if (texture) {
        u32 size = texture->key.size;
        float s = vertex.u * vertex.inv_w / (1 << (size & 15));
        float t = vertex.v * vertex.inv_w / (1 << (size >> 8));
        gs_reg(GS_ST, GS_SETREG_STQ(float_bits(s), float_bits(t)));
        q = float_bits(vertex.inv_w);
        if (texture->key.texture_function != GE_GS_TEXTURE_DECAL)
            vertex.color = texture_vertex_color(vertex.color, texture->key.texture_function, full_alpha);
    }
    gs_reg(GS_RGBAQ, vertex.color | ((u64)q << 32));
    gs_reg(GS_XYZ2, GS_SETREG_XYZ((u32)(vertex.x * 16 + GS_GE_OFFSET),
        (u32)(vertex.y * 16 + GS_GE_OFFSET), z));
}

int ge_hardware_sprite(GeVertex first, GeVertex second, int left, int top, int right, int bottom)
{
    u32 clear = ge_state.command[GE_CLEARMODE];
    u32 alpha = ge_state.command[GE_ALPHATEST];
    int textured = !(clear & 1) && (ge_state.command[GE_TEXTURE_ENABLE] & 1);
    int alpha_test = 1;
    int alpha_ref = 0;
    const GeHardwareTexture *texture = NULL;
    float u = 0.0f;
    float v = 0.0f;

    if (!(clear & 1) && (ge_state.command[GE_BLEND_ENABLE] & 1))
        return 0;

    int depth_stride = ge_state.command[GE_DEPTHBUF_WIDTH] & 0x7fc;
    if ((clear & 0x400) && depth_stride >= 64 && depth_stride != GS_GE_STRIDE)
        return 0;
    u8 *depth = (clear & 0x400) && depth_stride == GS_GE_STRIDE ?
        ge_pointer(buffer_address(GE_DEPTHBUF_BASE, GE_DEPTHBUF_WIDTH)) : NULL;

    if (!(clear & 1) && (ge_state.command[GE_ALPHA_TEST_ENABLE] & 1)) {
        unsigned mask = (alpha >> 16) & 255;
        if (textured) {
            if (mask != 255)
                return 0;
            alpha_test = alpha & 7;
            alpha_ref = (alpha >> 8) & 255;
        } else if (!compare_alpha(alpha & 7, (second.color >> 24) & mask, (alpha >> 8) & mask)) {
            return 1;
        }
    }

    if (textured) {
        if (second.u - first.u != second.x - first.x ||
            second.v - first.v != second.y - first.y)
            return 0;
        u = first.u + (left - first.x);
        v = first.v + (top - first.y);
        if (!(u >= 0.0f && u + right - left < 1024.0f &&
              v >= 0.0f && v + bottom - top < 1024.0f) ||
            u != (int)u || v != (int)v)
            return 0;
        texture = load_texture(second.color, GE_GS_BLEND_NONE, 0, GE_GS_TEXTURE_DECAL, 0);
        if (!texture)
            return 0;
    }

    u32 mask = 0;
    if (clear & 1) {
        if (!(clear & 0x100))
            mask |= 0x00ffffffu;
        if (!(clear & 0x200))
            mask |= 0xff000000u;
    }
    if (mask == 0xffffffffu && !depth)
        return 1;

    int full = left == 0 && top == 0 && right == GS_GE_STRIDE && bottom == GS_GE_HEIGHT;
    if (!begin_draw(top, bottom, depth, full && !mask && !texture, full && depth))
        return 0;

    draw_state(6, left, top, right, bottom, mask, depth != NULL, 1,
        alpha_test, alpha_ref, texture, GE_GS_BLEND_NONE);
    second.x = left;
    second.y = top;
    if (texture)
        gs_reg(GS_UV, GS_SETREG_UV((u32)(u * 16), (u32)(v * 16)));
    emit_vertex(second, NULL, 0);
    second.x = right;
    second.y = bottom;
    if (texture)
        gs_reg(GS_UV, GS_SETREG_UV((u32)((u + right - left) * 16),
            (u32)((v + bottom - top) * 16)));
    emit_vertex(second, NULL, 0);
    return 1;
}

static int vertex_in_gs(GeVertex vertex)
{
    return vertex.x >= -2048.0f && vertex.x < 2048.0f &&
        vertex.y >= -2048.0f && vertex.y < 2048.0f;
}

static int texture_coords_in_gs(GeVertex vertex)
{
    return vertex.u >= -4096.0f && vertex.u <= 4096.0f &&
        vertex.v >= -4096.0f && vertex.v <= 4096.0f;
}

int ge_hardware_triangle(GeVertex a, GeVertex b, GeVertex c, int left, int top, int right, int bottom)
{
    if (ge_state.command[GE_CLEARMODE] & 1)
        return 0;

    if (!vertex_in_gs(a) || !vertex_in_gs(b) || !vertex_in_gs(c))
        return 0;

    int textured = ge_state.command[GE_TEXTURE_ENABLE] & 1;
    int blend = ge_state.command[GE_BLEND_ENABLE] & 1;
    int alpha_test = 1;
    int alpha_ref = 0;
    if (ge_state.command[GE_ALPHA_TEST_ENABLE] & 1) {
        u32 setting = ge_state.command[GE_ALPHATEST];
        alpha_test = setting & 7;
        alpha_ref = (setting >> 8) & 255;
        if (alpha_test != 1 && ((setting >> 16) & 255) != 255)
            return 0;
        if (!alpha_test)
            return 1;
    }
    GeGsBlend gs_blend = GE_GS_BLEND_NONE;
    if (blend) {
        u32 mode = ge_blend_mode_seen ? ge_state.command[GE_BLENDMODE] & 0x7ff : 0x32;
        if (!textured)
            return 0;
        if (mode == 0x32) {
            if (compare_alpha(alpha_test, 0, alpha_ref))
                return 0;
        } else if ((ge_state.command[GE_BLENDFIXEDB] & 0xffffff) == 0xffffff &&
            (mode == 0x0a2 || mode == 0x2a2)) {
            gs_blend = mode == 0x0a2 ? GE_GS_BLEND_ADD : GE_GS_BLEND_REVERSE_SUBTRACT;
        } else {
            return 0;
        }
    }

    int depth_test = 1;
    int depth_write = 0;
    u8 *depth = NULL;
    int depth_stride = ge_state.command[GE_DEPTHBUF_WIDTH] & 0x7fc;
    if (depth_stride >= 64 && depth_stride <= 2048) {
        if (ge_state.command[GE_DEPTH_TEST_ENABLE] & 1) {
            int test = ge_state.command[GE_DEPTHTEST] & 7;
            if (!test)
                return 1;
            if (test != 1 && test != 6 && test != 7)
                return 0;
            depth_test = test == 6 ? 3 : test == 7 ? 2 : 1;
        }
        depth_write = !(ge_state.command[GE_DEPTH_WRITE_DISABLE] & 1);
        if (depth_write || depth_test != 1) {
            if (depth_stride != GS_GE_STRIDE ||
                !(a.z >= 0.0f && a.z <= 65535.0f) ||
                !(b.z >= 0.0f && b.z <= 65535.0f) ||
                !(c.z >= 0.0f && c.z <= 65535.0f))
                return 0;
            depth = ge_pointer(buffer_address(GE_DEPTHBUF_BASE, GE_DEPTHBUF_WIDTH));
            if (!depth)
                return 0;
        }
    }

    const GeHardwareTexture *texture = NULL;
    u32 mask = 0;
    if (textured) {
        if (!(a.inv_w > 0.0f) || !(b.inv_w > 0.0f) || !(c.inv_w > 0.0f))
            return 0;
        if (!texture_coords_in_gs(a) || !texture_coords_in_gs(b) || !texture_coords_in_gs(c))
            return 0;
        int format = ge_state.command[GE_FRAMEBUF_FORMAT] & 3;
        int stride = ge_state.command[GE_VADDR_WIDTH] & 0x7fc;
        if ((format != 1 && format != 3) || stride != GS_GE_STRIDE || bottom > GS_GE_HEIGHT)
            return 0;

        int tex_format = ge_state.command[GE_TEXFORMAT] & 15;
        int indexed = tex_format == 4 || tex_format == 5;
        GeGsTextureFunction function = GE_GS_TEXTURE_DECAL;
        u32 palette_color = a.color;
        if (blend && gs_blend == GE_GS_BLEND_NONE &&
            indexed && format == 1 && alpha_test == 6)
            gs_blend = GE_GS_BLEND_SOURCE_ALPHA;

        if (a.color != b.color || b.color != c.color) {
            if (!indexed || gs_blend == GE_GS_BLEND_NONE || (ge_state.command[GE_TEXFUNC] & 7) != 0)
                return 0;
            if (((a.color ^ b.color) | (b.color ^ c.color)) >> 24) {
                if (format != 1 || alpha_test != 1)
                    return 0;
                function = GE_GS_TEXTURE_MODULATE_RGBA;
                palette_color = 0xffffffffu;
            } else {
                function = GE_GS_TEXTURE_MODULATE_RGB;
                palette_color |= 0xffffffu;
            }
        }

        int blend_ref = gs_blend == GE_GS_BLEND_SOURCE_ALPHA ? alpha_ref : 0;
        texture = load_texture(palette_color, gs_blend, blend && gs_blend == GE_GS_BLEND_NONE,
            function, blend_ref);
        if (!texture)
            return 0;
    }

    if (!begin_draw(top, bottom, depth, 0, 0))
        return 0;

    if (texture && texture->key.texture_function == GE_GS_TEXTURE_MODULATE_RGBA &&
        (a.color | b.color | c.color) >> 31) {
        draw_state(3, left, top, right, bottom, 0x00ffffffu, 0, depth_test,
            1, 0, texture, GE_GS_BLEND_NONE);
        emit_vertex(a, texture, 1);
        emit_vertex(b, texture, 1);
        emit_vertex(c, texture, 1);
        mask = 0x80000000u;
    } else if (gs_blend == GE_GS_BLEND_SOURCE_ALPHA) {
        gs_reg(GS_FBA_2, 1);
        draw_state(3, left, top, right, bottom, 0x00ffffffu, 0, depth_test,
            7, 64, texture, GE_GS_BLEND_NONE);
        emit_vertex(a, texture, 0);
        emit_vertex(b, texture, 0);
        emit_vertex(c, texture, 0);
        gs_reg(GS_FBA_2, 0);
        mask = 0x80000000u;
        alpha_ref = 0;
    }

    draw_state(3, left, top, right, bottom, mask, depth_write, depth_test,
        alpha_test, alpha_ref, texture, gs_blend);
    emit_vertex(a, texture, 0);
    emit_vertex(b, texture, 0);
    emit_vertex(c, texture, 0);
    return 1;
}
