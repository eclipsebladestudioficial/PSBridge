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
} GeHardwareTextureKey;

typedef struct {
    GeHardwareTextureKey key;
    u64 tex0;
    int binary_alpha;
} GeHardwareTexture;

static GeHardwareBatch batch;
static GeHardwareTexture textures[16];
static unsigned texture_count;
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
            u32 allocation = psbridge_graphics_alloc_texture(depth_size + 8191 + GS_GE_STRIDE * 16 * 2);
            if (allocation == GSKIT_ALLOC_ERROR)
                return 0;
            depth_vram = (allocation + 8191) & ~8191u;
        }
    }

    if (color != batch.color || format != batch.format ||
        (depth && batch.depth && depth != batch.depth) || batch.count + 24 >= GS_GE_PACKET_SIZE)
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
    }

    if (depth && !batch.depth)
        batch.depth = depth;
    if (top < batch.top)
        batch.top = top;
    if (bottom > batch.bottom)
        batch.bottom = bottom;
    return 1;
}

void ge_hardware_read(const void *source)
{
    if (batch.count &&
        (uintptr_t)source - (uintptr_t)psbridge_video_memory() < 0x200000)
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
}

static const GeHardwareTexture *load_texture(u32 vertex_color)
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
    unsigned rows = compressed ? (height + 3) >> 2 : swizzled ? (height + 7) & ~7 : height;
    unsigned source_size = row_bytes * rows;
    const u8 *pixels = psbridge_ram_pointer(address, source_size);
    if (!pixels)
        return NULL;

    int indexed = format == 4 || format == 5;
    u32 revision = indexed ? palette_revision() : 0;
    if (indexed && !revision)
        return NULL;

    GeHardwareTextureKey key = {
        pixels, revision, indexed ? ge_state.command[GE_CLUTFORMAT] : 0,
        (row_bytes << 5) | (swizzled << 4) | format, size,
        ge_state.command[GE_TEXFUNC], ge_state.command[GE_TEXENVCOLOR], vertex_color,
    };
    for (unsigned i = 0; i < texture_count; i++) {
        if (!memcmp(&key, &textures[i].key, sizeof(key)))
            return &textures[i];
    }

    int tbw = (width + 63) / 64;
    unsigned bytes = gsKit_texture_size(tbw * 64, height, GS_PSM_CT32);
    if (bytes > GS_GE_TEXTURE_BYTES)
        return NULL;
    if (!texture_vram) {
        u32 allocation = psbridge_graphics_alloc_texture(GS_GE_TEXTURE_BYTES);
        if (allocation == GSKIT_ALLOC_ERROR)
            return NULL;
        texture_vram = allocation;
    }

    if (bytes > GS_GE_TEXTURE_BYTES - texture_used ||
        texture_count == sizeof(textures) / sizeof(textures[0]))
        ge_hardware_texture_dirty();

    u32 vram = texture_vram + texture_used;
    u32 palette[256];
    if (indexed) {
        int entries = format == 4 ? 16 : 256;
        for (int i = 0; i < entries; i++)
            palette[i] = shade_pixel(palette_color(i), vertex_color);
    }
    GeDxtSampler dxt;
    if (compressed) {
        dxt = (GeDxtSampler){
            .texture = pixels, .width = width, .height = height,
            .blocks_w = row_bytes / block_bytes, .bytes = block_bytes, .format = format,
        };
    }

    int binary_alpha = 1;
    gs_transfer(vram, GS_PSM_CT32, 0, height, tbw, width, 0);
    for (int y = 0; y < height; y += GS_GE_TEXTURE_ROWS) {
        int rows = height - y < GS_GE_TEXTURE_ROWS ? height - y : GS_GE_TEXTURE_ROWS;
        u32 *out = decoded;
        for (int row = y; row < y + rows; row++) {
            for (int x = 0; x < width; x++) {
                u32 color;
                if (format == 4) {
                    u8 indices = pixels[pixel_offset(x >> 1, row, row_bytes, 1, swizzled)];
                    color = palette[(indices >> ((x & 1) * 4)) & 15];
                } else if (format == 5) {
                    color = palette[pixels[pixel_offset(x, row, row_bytes, 1, swizzled)]];
                } else if (compressed) {
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
                *out++ = color;
                unsigned alpha = color >> 24;
                binary_alpha &= alpha == 0 || alpha == 255;
            }
        }
        upload_rows((u8 *)decoded, rows * width / 4);
    }

    texture_used += bytes;
    GeHardwareTexture *entry = &textures[texture_count++];
    *entry = (GeHardwareTexture){
        .key = key,
        .tex0 = GS_SETREG_TEX0(vram / 256, tbw, GS_PSM_CT32, width_shift, height_shift,
            1, 1, 0, 0, 0, 0, 0),
        .binary_alpha = binary_alpha,
    };
    return entry;
}

static void draw_state(int primitive, int left, int top, int right, int bottom,
    u32 mask, int depth_write, int depth_test, int alpha_test, int alpha_ref,
    const GeHardwareTexture *texture)
{
    gs_reg(GS_FRAME_2, GS_SETREG_FRAME(batch.vram / 8192, GS_GE_STRIDE / 64,
        batch.format == 3 ? GS_PSM_CT32 : GS_PSM_CT16, mask));
    gs_reg(GS_ZBUF_2, GS_SETREG_ZBUF(depth_vram / 8192, GS_PSMZ_16, !depth_write));
    static const u8 gs_alpha_func[] = {0, 1, 4, 7, 2, 3, 6, 5};
    gs_reg(GS_TEST_2, GS_SETREG_TEST(alpha_test != 1, gs_alpha_func[alpha_test],
        alpha_ref, 0, 0, 0, 1, depth_test));
    batch.depth_written |= depth_write;
    gs_reg(GS_SCISSOR_2, GS_SETREG_SCISSOR(left, right - 1, top, bottom - 1));
    if (texture) {
        u32 wrap = ge_state.command[GE_TEXWRAP];
        gs_reg(GS_TEXFLUSH, 0);
        gs_reg(GS_TEX0_2, texture->tex0);
        gs_reg(GS_TEX1_2, GS_SETREG_TEX1(1, 0, 0, 0, 0, 0, 0));
        gs_reg(GS_CLAMP_2, GS_SETREG_CLAMP(wrap & 1, (wrap >> 8) & 1, 0, 0, 0, 0));
    }
    gs_reg(GS_PRIM, GS_SETREG_PRIM(primitive, primitive == 3, texture != NULL, 0, 0, 0,
        primitive == 6, 1, 0));
}

static u32 float_bits(float value)
{
    u32 bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static void emit_vertex(GeVertex vertex, const GeHardwareTexture *texture)
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
        texture = load_texture(second.color);
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

    if (batch.format == 1)
        mask = pack_color_5551(mask) * 0x10001u;

    draw_state(6, left, top, right, bottom, mask, depth != NULL, 1,
        alpha_test, alpha_ref, texture);
    second.x = left;
    second.y = top;
    if (texture)
        gs_reg(GS_UV, GS_SETREG_UV((u32)(u * 16), (u32)(v * 16)));
    emit_vertex(second, NULL);
    second.x = right;
    second.y = bottom;
    if (texture)
        gs_reg(GS_UV, GS_SETREG_UV((u32)((u + right - left) * 16),
            (u32)((v + bottom - top) * 16)));
    emit_vertex(second, NULL);
    return 1;
}

static int vertex_in_gs(GeVertex vertex)
{
    return vertex.x >= -2048.0f && vertex.x < 2048.0f &&
        vertex.y >= -2048.0f && vertex.y < 2048.0f;
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
    if (blend && (!textured || (ge_blend_mode_seen &&
        (ge_state.command[GE_BLENDMODE] & 0x7ff) != 0x32) ||
        compare_alpha(alpha_test, 0, alpha_ref)))
        return 0;

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
    if (textured) {
        if (a.color != b.color || b.color != c.color ||
            !(a.inv_w > 0.0f) || !(b.inv_w > 0.0f) || !(c.inv_w > 0.0f) ||
            !(a.u >= 0.0f && a.u <= 4096.0f && a.v >= 0.0f && a.v <= 4096.0f) ||
            !(b.u >= 0.0f && b.u <= 4096.0f && b.v >= 0.0f && b.v <= 4096.0f) ||
            !(c.u >= 0.0f && c.u <= 4096.0f && c.v >= 0.0f && c.v <= 4096.0f))
            return 0;
        int format = ge_state.command[GE_FRAMEBUF_FORMAT] & 3;
        int stride = ge_state.command[GE_VADDR_WIDTH] & 0x7fc;
        if ((format != 1 && format != 3) || stride != GS_GE_STRIDE || bottom > GS_GE_HEIGHT)
            return 0;

        texture = load_texture(a.color);
        if (!texture || (blend && !texture->binary_alpha))
            return 0;
    }

    if (!begin_draw(top, bottom, depth, 0, 0))
        return 0;

    draw_state(3, left, top, right, bottom, 0, depth_write, depth_test,
        alpha_test, alpha_ref, texture);
    emit_vertex(a, texture);
    emit_vertex(b, texture);
    emit_vertex(c, texture);
    return 1;
}
