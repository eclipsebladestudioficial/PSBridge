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
#define GS_GE_TEXTURE_BYTES (2560 * 1024)
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
    GE_GS_STATE_FBA,
    GE_GS_STATE_PRIM,
    GE_GS_STATE_COUNT,
} GeGsDrawState;

enum {
    GE_GS_SOURCE_ALPHA_LOW = 1,
    GE_GS_SOURCE_ALPHA_HIGH = 2,
};

typedef struct {
    u8 *color;
    u8 *depth;
    u64 scissor;
    GeGsBlend blend;
    int format;
    int depth_test;
    int depth_write;
    int alpha_test;
    int alpha_ref;
    int blend_enabled;
    int textured;
    int indexed;
    int modulate;
    int texture_alpha;
    int triangle_result;
} GeHardwareState;

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
    unsigned ad_start;
    unsigned vertex_start;
    unsigned vertex_count;
    unsigned vertex_fields;
    unsigned state_valid;
    u64 state[GE_GS_STATE_COUNT];
    u64 draw_scissor;
    u64 draw_texture;
    u32 draw_mask;
    u32 draw_flags;
    u32 draw_depth_vram;
} GeHardwareBatch;

typedef struct {
    u8 *pixels;
    u32 vram;
    int psm;
    int top;
    int bottom;
    int dirty_top;
    int dirty_bottom;
} GeHardwareSurface;

typedef struct {
    u32 address;
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
    float s_scale;
    float t_scale;
    unsigned image_index;
    unsigned source_alpha_classes;
} GeHardwareTexture;

typedef struct {
    const u8 *pixels;
    u32 layout;
    u32 size;
    u32 vram;
    int top;
    int bottom;
    int complete;
} GeHardwareImage;

static GeHardwareBatch batch;
static GeHardwareState draw_config;
unsigned ge_hardware_config_dirty = 1;
static GeHardwareSurface resident_color;
static GeHardwareSurface resident_depth;
static GeHardwareTexture textures[32];
static const GeHardwareTexture *bound_texture;
static GeHardwareImage images[32];
static unsigned texture_count;
static unsigned image_count;
static u32 texture_vram;
static unsigned texture_used;
static u32 decoded[GS_GE_STRIDE * GS_GE_TEXTURE_ROWS] __attribute__((aligned(64)));
static u32 depth_vram;
static union {
    GeGifRegister qwords[GS_GE_PACKET_SIZE + 1];
    u64 registers[2 * (GS_GE_PACKET_SIZE + 1)];
} packet __attribute__((aligned(64)));
static GeGifRegister upload_chain[8] __attribute__((aligned(64)));

static void close_vertex_tag(int end)
{
    if (!batch.vertex_count)
        return;

    unsigned fields = batch.vertex_fields;
    unsigned values = batch.vertex_count * fields;
    u64 registers = fields == 3 ? GS_ST | (GS_RGBAQ << 4) | (GS_XYZ2 << 8) :
        GS_RGBAQ | (GS_XYZ2 << 4);
    u64 tag = GIF_TAG(batch.vertex_count, end, 0, 0, GSKIT_GIF_FLG_REGLIST, fields);
    packet.qwords[batch.vertex_start - 1] = (GeGifRegister){ tag, registers };
    if (values & 1)
        packet.registers[2 * batch.count - 1] = 0;

    batch.vertex_count = 0;
}

static void close_ad_tag(int end)
{
    if (!batch.ad_start)
        return;

    unsigned registers = batch.count - batch.ad_start;
    u64 tag = GIF_TAG(registers, end, 0, 0, GSKIT_GIF_FLG_PACKED, 1);
    packet.qwords[batch.ad_start - 1] = (GeGifRegister){ tag, GIF_AD };
    batch.ad_start = 0;
}

static void gs_reg(u64 address, u64 data)
{
    if (!batch.ad_start) {
        close_vertex_tag(0);
        batch.ad_start = ++batch.count;
    }

    packet.qwords[batch.count++] = (GeGifRegister){ data, address };
}

static void send_gif(const void *data, unsigned qwc)
{
    dmaKit_send(DMA_CHANNEL_GIF, (void *)data, qwc);
    dmaKit_wait(DMA_CHANNEL_GIF, 0);
}

static void setup_transfer(GeGifRegister *transfer, u32 vram, int psm,
    int y, int height, int tbw, int width, int download)
{
    u64 tag = GIF_TAG_AD(4);
    u64 buffer = download ? GS_SETREG_BITBLTBUF(vram / 256, tbw, psm, 0, 0, 0) :
        GS_SETREG_BITBLTBUF(0, 0, 0, vram / 256, tbw, psm);
    u64 position = download ? GS_SETREG_TRXPOS(0, y, 0, 0, 0) : GS_SETREG_TRXPOS(0, 0, 0, y, 0);

    transfer[0] = (GeGifRegister){ tag, GIF_AD };
    transfer[1] = (GeGifRegister){ buffer, GS_BITBLTBUF };
    transfer[2] = (GeGifRegister){ position, GS_TRXPOS };
    transfer[3] = (GeGifRegister){ GS_SETREG_TRXREG(width, height), GS_TRXREG };
    transfer[4] = (GeGifRegister){ download, GS_TRXDIR };
}

static void gs_download(u32 vram, int psm, int y, int height, int tbw, int width)
{
    GeGifRegister transfer[5] __attribute__((aligned(64)));
    setup_transfer(transfer, vram, psm, y, height, tbw, width, 1);
    send_gif(transfer, 5);
}

static void upload_rows(const u8 *pixels, unsigned qwc, const GeGifRegister *transfer)
{
    static GeGifRegister *chain;
    if (!chain) {
        SyncDCache(upload_chain, upload_chain + 8);
        chain = UCAB_SEG((uintptr_t)upload_chain & 0x1fffffffu);
    }

    unsigned setup = 0;
    if (transfer) {
        memcpy(chain + 1, transfer, 5 * sizeof(*chain));
        setup = 5;
    }

    SyncDCache((void *)pixels, (void *)(pixels + qwc * 16));
    while (qwc) {
        unsigned chunk = qwc > 0x7fff ? 0x7fff : qwc;
        u32 address = (uintptr_t)pixels & 0x1fffffffu;
        chain[0] = (GeGifRegister){ DMA_TAG(setup + 1, 0, DMA_CNT, 0, 0, 0), 0 };
        chain[setup + 1].data = GIF_TAG(chunk, 1, 0, 0, GSKIT_GIF_FLG_IMAGE, 0);
        chain[setup + 1].address = 0;
        chain[setup + 2] = (GeGifRegister){ DMA_TAG(chunk, 0, DMA_REFE, 0, address, 0), 0 };
        __asm__ volatile("sync.l" ::: "memory");
        dmaKit_send_chain_ucab(DMA_CHANNEL_GIF, chain);
        dmaKit_wait(DMA_CHANNEL_GIF, 0);
        setup = 0;
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

void ge_hardware_invalidate_surfaces(void)
{
    resident_color.pixels = NULL;
    resident_depth.pixels = NULL;
}

static int surface_matches(const GeHardwareSurface *surface, const u8 *pixels, u32 vram, int psm)
{
    return surface->pixels == pixels && surface->vram == vram && surface->psm == psm;
}

static void read_surface(GeHardwareSurface *surface)
{
    int top = surface->dirty_top;
    int rows = surface->dirty_bottom - top;
    if (!rows)
        return;

    unsigned row_bytes = GS_GE_STRIDE * (surface->psm == GS_PSM_CT32 ? 4 : 2);
    gs_download(surface->vram, surface->psm, top, rows, GS_GE_STRIDE / 64, GS_GE_STRIDE);
    download_rows(surface->pixels + top * row_bytes, rows * row_bytes / 16);
    surface->dirty_top = 0;
    surface->dirty_bottom = 0;
}

static void read_surfaces(void)
{
    if (!resident_color.dirty_bottom && !resident_depth.dirty_bottom)
        return;

    GeGifRegister finish[] __attribute__((aligned(64))) = {
        { 0, GIF_AD },
        { 0, GS_FINISH },
    };
    finish[0].data = GIF_TAG_AD(1);
    GS_SETREG_CSR_FINISH(1);
    send_gif(finish, 2);

    while (!(GS_CSR_FINISH))
        __asm__ volatile("nop");

    int full_color = !resident_color.dirty_top && resident_color.dirty_bottom == GS_GE_HEIGHT;
    read_surface(&resident_color);
    read_surface(&resident_depth);
    if (full_color)
        psbridge_graphics_draw_complete(resident_color.pixels, resident_color.psm == GS_PSM_CT32 ? 3 : 1);
}

static void prepare_surface_rows(u8 *pixels, u32 vram, int psm, int top, int bottom, int replace)
{
    int rows = bottom - top;
    unsigned row_bytes = GS_GE_STRIDE * (psm == GS_PSM_CT32 ? 4 : 2);
    u8 *first = pixels + top * row_bytes;
    if (replace) {
        SyncDCache(first, first + rows * row_bytes);
    } else {
        GeGifRegister transfer[5];
        setup_transfer(transfer, vram, psm, top, rows, GS_GE_STRIDE / 64, GS_GE_STRIDE, 0);
        upload_rows(first, rows * row_bytes / 16, transfer);
    }
}

static void prepare_surface(GeHardwareSurface *surface, u8 *pixels, u32 vram, int psm,
    int replace, int write)
{
    if (!surface_matches(surface, pixels, vram, psm)) {
        prepare_surface_rows(pixels, vram, psm, batch.top, batch.bottom, replace);
        *surface = (GeHardwareSurface){
            .pixels = pixels, .vram = vram, .psm = psm, .top = batch.top, .bottom = batch.bottom,
        };
    } else {
        if (batch.top < surface->top) {
            prepare_surface_rows(pixels, vram, psm, batch.top, surface->top, 0);
            surface->top = batch.top;
        }
        if (batch.bottom > surface->bottom) {
            prepare_surface_rows(pixels, vram, psm, surface->bottom, batch.bottom, 0);
            surface->bottom = batch.bottom;
        }
    }

    if (write) {
        if (!surface->dirty_bottom || batch.top < surface->dirty_top)
            surface->dirty_top = batch.top;
        if (batch.bottom > surface->dirty_bottom)
            surface->dirty_bottom = batch.bottom;
    }
}

static void submit_batch(void)
{
    if (!batch.count)
        return;

    int psm = batch.format == 3 ? GS_PSM_CT32 : GS_PSM_CT16;
    int depth_psm = 0x30 | GS_PSMZ_16;
    if ((resident_color.dirty_bottom &&
         !surface_matches(&resident_color, batch.color, batch.vram, psm)) ||
        (resident_depth.dirty_bottom && batch.depth &&
         !surface_matches(&resident_depth, batch.depth, depth_vram, depth_psm))) {
        read_surfaces();
        psbridge_graphics_draw_invalidate();
    }

    prepare_surface(&resident_color, batch.color, batch.vram, psm, batch.replace_color, 1);
    if (batch.depth)
        prepare_surface(&resident_depth, batch.depth, depth_vram, depth_psm,
            batch.replace_depth, batch.depth_written);

    close_vertex_tag(1);
    close_ad_tag(1);
    send_gif(packet.qwords, batch.count);
    batch.count = 0;
}

void ge_hardware_flush(void)
{
    if (batch.count)
        submit_batch();
    if (resident_color.dirty_bottom || resident_depth.dirty_bottom)
        read_surfaces();
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

static void prepare_draw_config(void)
{
    const u32 *command = ge_state.command;
    GeHardwareState state = { .depth_test = 1, .alpha_test = 1 };
    state.format = command[GE_FRAMEBUF_FORMAT] & 3;
    state.textured = command[GE_TEXTURE_ENABLE] & 1;
    state.blend_enabled = command[GE_BLEND_ENABLE] & 1;
    state.modulate = (command[GE_TEXFUNC] & 7) == 0;
    state.texture_alpha = (command[GE_TEXFUNC] >> 8) & 1;

    unsigned format = command[GE_TEXFORMAT] & 15;
    state.indexed = format == 4 || format == 5;

    unsigned color_bytes = GS_GE_STRIDE * GS_GE_HEIGHT * (state.format == 3 ? 4 : 2);
    uintptr_t video_base = (uintptr_t)psbridge_video_memory();
    uintptr_t color_offset = 0x200000;
    if ((state.format == 1 || state.format == 3) &&
        (command[GE_VADDR_WIDTH] & 0x7fc) == GS_GE_STRIDE) {
        u8 *color = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));
        color_offset = (uintptr_t)color - video_base;
        if (!(color_offset & 63) && color_offset <= 0x200000 - color_bytes)
            state.color = color;
    }

    u32 first = command[GE_SCISSOR1];
    u32 last = command[GE_SCISSOR2];
    unsigned right = last ? last & 0x3ff : 479;
    unsigned bottom = last ? (last >> 10) & 0x3ff : 271;
    if (right >= GS_GE_STRIDE)
        right = GS_GE_STRIDE - 1;
    if (bottom >= GS_GE_HEIGHT)
        bottom = GS_GE_HEIGHT - 1;

    state.scissor = GS_SETREG_SCISSOR(first & 0x3ff, right, (first >> 10) & 0x3ff, bottom);
    if (command[GE_CLEARMODE] & 1)
        goto done;

    if (command[GE_ALPHA_TEST_ENABLE] & 1) {
        u32 setting = command[GE_ALPHATEST];
        state.alpha_test = setting & 7;
        state.alpha_ref = (setting >> 8) & 255;
        if (state.alpha_test != 1 && (setting >> 16) != 255)
            goto done;
        if (!state.alpha_test) {
            state.triangle_result = 1;
            goto done;
        }
    }

    if (state.blend_enabled) {
        u32 mode = ge_blend_mode_seen ? command[GE_BLENDMODE] & 0x7ff : 0x32;
        if (mode == 0x32) {
            if (!state.textured)
                state.blend = GE_GS_BLEND_SOURCE_ALPHA;
            else if (compare_alpha(state.alpha_test, 0, state.alpha_ref))
                goto done;
            else if (state.indexed && state.format == 1 && state.alpha_test == 6)
                state.blend = GE_GS_BLEND_SOURCE_ALPHA;
        } else if (state.textured && command[GE_BLENDFIXEDB] == 0xffffff &&
                   (mode == 0x0a2 || mode == 0x2a2)) {
            state.blend = mode == 0x0a2 ? GE_GS_BLEND_ADD : GE_GS_BLEND_REVERSE_SUBTRACT;
        } else {
            goto done;
        }
    }

    int depth_stride = command[GE_DEPTHBUF_WIDTH] & 0x7fc;
    if (depth_stride >= 64 && depth_stride <= 2048) {
        if (command[GE_DEPTH_TEST_ENABLE] & 1) {
            int test = command[GE_DEPTHTEST] & 7;
            if (!test) {
                state.triangle_result = 1;
                goto done;
            }
            if (test != 1 && test != 6 && test != 7)
                goto done;

            state.depth_test = test == 6 ? 3 : test == 7 ? 2 : 1;
        }

        state.depth_write = !(command[GE_DEPTH_WRITE_DISABLE] & 1);
        if (state.depth_write || state.depth_test != 1) {
            if (depth_stride != GS_GE_STRIDE)
                goto done;

            u8 *depth = ge_pointer(buffer_address(GE_DEPTHBUF_BASE, GE_DEPTHBUF_WIDTH));
            uintptr_t offset = (uintptr_t)depth - video_base;
            unsigned depth_size = GS_GE_STRIDE * GS_GE_HEIGHT * 2;
            if (!depth || (offset & 63) || offset > 0x200000 - depth_size ||
                (offset < color_offset + color_bytes && color_offset < offset + depth_size))
                goto done;

            state.depth = depth;
        }
    }

    if (state.color)
        state.triangle_result = 2;

done:
    draw_config = state;
    ge_hardware_config_dirty = 0;
}

static int begin_draw(int top, int bottom, u8 *depth, int replace_color, int replace_depth)
{
    if (ge_hardware_config_dirty)
        prepare_draw_config();

    u8 *color = draw_config.color;
    int format = draw_config.format;
    if (!color || top < 0 || bottom > GS_GE_HEIGHT || top >= bottom)
        return 0;

    if (depth && !depth_vram) {
        unsigned gs_depth_size = GS_GE_STRIDE * ((GS_GE_HEIGHT + 63) & ~63u) * 2;
        u32 allocation = psbridge_graphics_alloc_texture(gs_depth_size + 8191);
        if (allocation == GSKIT_ALLOC_ERROR)
            return 0;

        depth_vram = (allocation + 8191) & ~8191u;
    }

    if (color != batch.color || format != batch.format ||
        (depth && batch.depth && depth != batch.depth))
        ge_hardware_flush();
    else if (batch.count + 40 >= GS_GE_PACKET_SIZE)
        submit_batch();

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

static int reads_rows(const void *source, unsigned size, const u8 *target,
    unsigned row_bytes, int top, int bottom)
{
    uintptr_t start = (uintptr_t)target + top * row_bytes;
    uintptr_t read = (uintptr_t)source;
    return read >= start ? read - start < (bottom - top) * row_bytes :
        start - read < size;
}

static int reads_surface(const void *source, unsigned size, const GeHardwareSurface *surface)
{
    return surface->dirty_bottom && reads_rows(source, size, surface->pixels,
        GS_GE_STRIDE * (surface->psm == GS_PSM_CT32 ? 4 : 2),
        surface->dirty_top, surface->dirty_bottom);
}

void ge_hardware_read(const void *source, unsigned size)
{
    if (!source || !size)
        return;

    if ((batch.count &&
         (reads_rows(source, size, batch.color, GS_GE_STRIDE * (batch.format == 3 ? 4 : 2),
              batch.top, batch.bottom) ||
          (batch.depth_written && reads_rows(source, size, batch.depth, GS_GE_STRIDE * 2,
              batch.top, batch.bottom)))) ||
        reads_surface(source, size, &resident_color) || reads_surface(source, size, &resident_depth))
        ge_hardware_flush();
}

void ge_hardware_cpu_write(void)
{
    ge_hardware_flush();
    ge_hardware_invalidate_surfaces();
    psbridge_graphics_draw_invalidate();
}

void ge_hardware_texture_dirty(void)
{
    if (!batch.count)
        texture_used = 0;
    texture_count = 0;
    image_count = 0;
    bound_texture = NULL;
}

void ge_hardware_texture_unbind(void)
{
    bound_texture = NULL;
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

static void unswizzle_indexed_tiles(
    u8 *output, const u8 *pixels, int rows, int width, int stride)
{
    for (int y = 0; y < rows; y += 8) {
        const u8 *source = pixels + y * stride;
        u8 *destination = output + y * width;
        for (int x = 0; x < width; x += 16, source += 128, destination += 16) {
            u128 first, second, third, fourth;
            u8 *row = destination;
            __asm__ volatile (
                "lq %[first], 0(%[source])\n\t"
                "lq %[second], 16(%[source])\n\t"
                "lq %[third], 32(%[source])\n\t"
                "lq %[fourth], 48(%[source])\n\t"
                "sq %[first], 0(%[row])\n\t"
                "addu %[row], %[row], %[stride]\n\t"
                "sq %[second], 0(%[row])\n\t"
                "addu %[row], %[row], %[stride]\n\t"
                "sq %[third], 0(%[row])\n\t"
                "addu %[row], %[row], %[stride]\n\t"
                "sq %[fourth], 0(%[row])\n\t"
                "addu %[row], %[row], %[stride]\n\t"
                "lq %[first], 64(%[source])\n\t"
                "lq %[second], 80(%[source])\n\t"
                "lq %[third], 96(%[source])\n\t"
                "lq %[fourth], 112(%[source])\n\t"
                "sq %[first], 0(%[row])\n\t"
                "addu %[row], %[row], %[stride]\n\t"
                "sq %[second], 0(%[row])\n\t"
                "addu %[row], %[row], %[stride]\n\t"
                "sq %[third], 0(%[row])\n\t"
                "addu %[row], %[row], %[stride]\n\t"
                "sq %[fourth], 0(%[row])"
                : [first] "=&r"(first), [second] "=&r"(second),
                  [third] "=&r"(third), [fourth] "=&r"(fourth), [row] "+&r"(row)
                : [source] "r"(source), [stride] "r"(width)
                : "memory"
            );
        }
    }
}

static void upload_indexed_texture(
    const u8 *pixels, int format, int swizzled, int row_bytes,
    int width, int first, int end, int tbw, int psm, u32 vram)
{
    int indices_per_byte = format == 4 ? 2 : 1;
    int source_width = width / indices_per_byte;
    int transfer_bytes = source_width < 16 ? 16 : source_width;
    int height = end - first;
    int aligned = !((uintptr_t)pixels & 15);
    GeGifRegister transfer[5];
    setup_transfer(transfer, vram, psm, first, height, tbw, transfer_bytes * indices_per_byte, 0);

    if (!swizzled && row_bytes == transfer_bytes && source_width == transfer_bytes && aligned) {
        upload_rows(pixels + first * row_bytes, height * transfer_bytes / 16, transfer);
        return;
    }

    int chunk_rows = sizeof(decoded) / transfer_bytes;
    int tiled = swizzled && aligned && !(source_width & 15) && !(height & 7);
    for (int y = first; y < end; y += chunk_rows) {
        int rows = end - y < chunk_rows ? end - y : chunk_rows;
        u8 *out = (u8 *)decoded;
        if (tiled) {
            unswizzle_indexed_tiles(out, pixels + y * row_bytes, rows, source_width, row_bytes);
        } else {
            for (int row = y; row < y + rows; row++, out += transfer_bytes) {
                const u8 *source = pixels + pixel_offset(0, row, row_bytes, 1, swizzled);
                if (swizzled) {
                    for (int x = 0; x < source_width; x += 16, source += 128) {
                        int bytes = source_width - x < 16 ? source_width - x : 16;
                        memcpy(out + x, source, bytes);
                    }
                } else {
                    memcpy(out, source, source_width);
                }

                if (transfer_bytes > source_width)
                    memset(out + source_width, 0, transfer_bytes - source_width);
            }
        }

        upload_rows((u8 *)decoded, rows * transfer_bytes / 16, y == first ? transfer : NULL);
    }
}

static void ensure_image_rows(GeHardwareImage *image, float min_v, float max_v)
{
    if (image->complete)
        return;

    int height = 1 << (image->size >> 8);
    if (image->bottom && min_v >= (float)(image->top + 2) &&
        max_v < (float)(image->bottom - 2))
        return;

    int first = 0;
    int end = height;
    if (height >= 16 && min_v >= 2.0f && max_v < height - 2.0f) {
        first = ((int)min_v - 2) & ~7;
        end = ((int)max_v + 10) & ~7;
        if (end > height)
            end = height;
    }

    if (image->top <= first && image->bottom >= end)
        return;

    int width = 1 << (image->size & 15);
    int format = image->layout & 15;
    int swizzled = (image->layout >> 4) & 1;
    int stride = image->layout >> 5;
    int row_bytes = format == 4 ? (stride + 1) / 2 : stride;
    int tbw = 2 * ((width + 127) / 128);
    int psm = format == 4 ? GS_PSM_T4 : GS_PSM_T8;

    batch.state_valid &= ~(1u << GE_GS_STATE_TEXTURE);
    batch.draw_flags = 0;
    if (image->bottom == 0) {
        image->top = end;
        image->bottom = end;
    }
    if (first < image->top) {
        upload_indexed_texture(image->pixels, format, swizzled, row_bytes,
            width, first, image->top, tbw, psm, image->vram);
        image->top = first;
    }
    if (end > image->bottom) {
        upload_indexed_texture(image->pixels, format, swizzled, row_bytes,
            width, image->bottom, end, tbw, psm, image->vram);
        image->bottom = end;
    }
    image->complete = !image->top && image->bottom == height;
}

static int texture_key_equal(const GeHardwareTextureKey *a, const GeHardwareTextureKey *b)
{
    return a->address == b->address &&
        a->color == b->color &&
        a->revision == b->revision &&
        a->blend == b->blend &&
        a->clut == b->clut &&
        a->layout == b->layout &&
        a->size == b->size &&
        a->function == b->function &&
        a->environment == b->environment &&
        a->binary_alpha_required == b->binary_alpha_required &&
        a->texture_function == b->texture_function &&
        a->blend_alpha_ref == b->blend_alpha_ref;
}

static const GeHardwareTexture *use_texture(const GeHardwareTexture *texture, float min_v, float max_v)
{
    bound_texture = texture;
    if (texture->image_index)
        ensure_image_rows(&images[texture->image_index - 1], min_v, max_v);

    return texture->tex0 ? texture : NULL;
}

static inline const GeHardwareTexture *matching_bound_texture(
    u32 vertex_color, GeGsBlend blend, int binary_alpha_required,
    GeGsTextureFunction texture_function, int blend_alpha_ref)
{
    if (bound_texture && bound_texture->key.color == vertex_color &&
        bound_texture->key.blend == blend &&
        bound_texture->key.binary_alpha_required == binary_alpha_required &&
        bound_texture->key.texture_function == texture_function &&
        bound_texture->key.blend_alpha_ref == blend_alpha_ref)
        return bound_texture;

    return NULL;
}

static const GeHardwareTexture *load_texture(
    u32 vertex_color, GeGsBlend blend, int binary_alpha_required,
    GeGsTextureFunction texture_function, int blend_alpha_ref, float min_v, float max_v)
{
    const GeHardwareTexture *bound = matching_bound_texture(vertex_color, blend,
        binary_alpha_required, texture_function, blend_alpha_ref);
    if (bound)
        return use_texture(bound, min_v, max_v);

    int format = ge_state.command[GE_TEXFORMAT] & 15;
    int swizzled = ge_state.command[GE_TEXMODE] & 1;
    u32 size = ge_state.command[GE_TEXSIZE0] & 0xf0f;
    u32 width_shift = size & 15;
    u32 height_shift = size >> 8;
    int width = 1 << width_shift;
    int height = 1 << height_shift;
    int compressed = format == 8 || format == 9;
    int indexed = format == 4 || format == 5;
    if (format > 5 && !compressed)
        return NULL;

    u32 revision = indexed ? palette_revision() : 0;
    if (indexed && !revision)
        return NULL;

    int stride = texture_buffer_width(format);
    u32 address = buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0);
    u32 function = ge_state.command[GE_TEXFUNC] & 0x107;
    GeHardwareTextureKey key = {
        address,
        revision,
        indexed ? ge_state.command[GE_CLUTFORMAT] : 0,
        (stride << 5) | (swizzled << 4) | format,
        size,
        function,
        (function & 7) == 2 ? ge_state.command[GE_TEXENVCOLOR] & 0xffffff : 0,
        vertex_color,
        blend,
        binary_alpha_required,
        texture_function,
        blend_alpha_ref,
    };

    for (unsigned i = texture_count; i > 0; i--) {
        const GeHardwareTexture *cached = &textures[i - 1];
        if (texture_key_equal(&key, &cached->key))
            return use_texture(cached, min_v, max_v);
    }

    if (width < 4 || width > 512 || height > 512 || width > stride)
        return NULL;

    static const int bits[] = { 16, 16, 16, 32, 4, 8 };
    int block_bytes = format == 8 ? 8 : 16;
    int row_bytes = compressed ? ((stride + 3) >> 2) * block_bytes :
        (stride * bits[format] + 7) / 8;
    if (!compressed && swizzled && (row_bytes & 15))
        return NULL;

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

    GeHardwareImage *image = NULL;
    if (indexed) {
        for (unsigned i = 0; i < image_count; i++) {
            if (images[i].pixels == pixels &&
                images[i].layout == key.layout &&
                images[i].size == size) {
                image = &images[i];
                break;
            }
        }
    }

    if ((image ? palette_bytes : texture_bytes + palette_bytes) >
            GS_GE_TEXTURE_BYTES - texture_used ||
        texture_count == sizeof(textures) / sizeof(textures[0])) {
        submit_batch();
        ge_hardware_texture_dirty();
        image = NULL;
    }

    u32 palette[256] __attribute__((aligned(64)));
    int new_image = image == NULL;
    int binary_alpha = 1;
    unsigned source_alpha_classes = 0;
    if (indexed) {
        int entries = format == 4 ? 16 : 256;
        int copy_palette = (function & 7) == 0 && vertex_color == 0xffffffffu;
        u32 alpha_mask = function & 0x100 ? 0 : 0xff000000u;
        for (int i = 0; i < entries; i++) {
            u32 color = palette_color(i);
            color = copy_palette ? color | alpha_mask : shade_pixel(color, vertex_color);
            unsigned alpha = color >> 24;
            binary_alpha &= alpha == 0 || alpha == 255;
            int slot = format == 4 ? i : (i & ~0x18) | ((i & 8) << 1) | ((i & 16) >> 1);

            if (texture_function == GE_GS_TEXTURE_MODULATE_RGBA) {
                color = (color & 0xffffffu) | (((alpha + (alpha == 255)) >> 1) << 24);
            } else if (blend == GE_GS_BLEND_SOURCE_ALPHA) {
                unsigned weight = alpha > (unsigned)blend_alpha_ref ?
                    (alpha + (alpha == 1 || alpha == 255)) >> 1 : 0;
                if (weight)
                    source_alpha_classes |= weight < 64 ?
                        GE_GS_SOURCE_ALPHA_LOW : GE_GS_SOURCE_ALPHA_HIGH;
                color = (color & 0xffffffu) | (weight << 24);
            } else if (blend != GE_GS_BLEND_NONE) {
                color = premultiply_for_gs(color, blend);
            }

            palette[slot] = color;
        }

        if (binary_alpha_required && !binary_alpha) {
            GeHardwareTexture *entry = &textures[texture_count++];
            *entry = (GeHardwareTexture){
                .key = key,
            };
            bound_texture = entry;
            return NULL;
        }
    }

    u32 free_vram = texture_vram + texture_used;
    u32 vram = image ? image->vram : free_vram;
    u32 clut = 0;
    GeGifRegister transfer[5];

    if (indexed) {
        if (!image) {
            image = &images[image_count++];
            *image = (GeHardwareImage){
                .pixels = pixels, .layout = key.layout, .size = size, .vram = vram,
            };
            clut = free_vram + texture_bytes;
        } else {
            clut = free_vram;
        }
        ensure_image_rows(image, min_v, max_v);
        setup_transfer(transfer, clut, GS_PSM_CT32, 0, format == 4 ? 2 : 16,
            1, format == 4 ? 8 : 16, 0);
        upload_rows((u8 *)palette, (format == 4 ? 16 : 256) * sizeof(*palette) / 16, transfer);
    } else {
        GeDxtSampler dxt = {
            .texture = pixels, .width = width, .height = height,
            .blocks_w = row_bytes / block_bytes, .bytes = block_bytes,
            .format = format,
        };
        setup_transfer(transfer, vram, GS_PSM_CT32, 0, height, tbw, width, 0);

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

            upload_rows((u8 *)decoded, rows * width / 4, y == 0 ? transfer : NULL);
        }
    }

    texture_used += (new_image ? texture_bytes : 0) + palette_bytes;
    GeHardwareTexture *entry = &textures[texture_count++];
    *entry = (GeHardwareTexture){
        .key = key,
    };
    bound_texture = entry;
    if (binary_alpha_required && !binary_alpha)
        return NULL;

    entry->tex0 = GS_SETREG_TEX0(vram / 256, tbw, psm, width_shift, height_shift,
        1, texture_function == GE_GS_TEXTURE_DECAL, clut / 256, GS_PSM_CT32, 0, 0, indexed);
    if (image)
        entry->image_index = image - images + 1;
    entry->source_alpha_classes = source_alpha_classes;
    entry->s_scale = 1.0f / width;
    entry->t_scale = 1.0f / height;
    return entry;
}

static void draw_state(int primitive, u64 scissor,
    u32 mask, int depth_write, int depth_test, int alpha_test, int alpha_ref, int force_alpha,
    const GeHardwareTexture *texture, GeGsBlend blend)
{
    u64 tex0 = texture ? texture->tex0 : 0;
    u32 wrap = texture ? ge_state.command[GE_TEXWRAP] : 0;
    int weight = texture && texture->key.texture_function == GE_GS_TEXTURE_MODULATE_RGBA ? 0 : 2;
    u32 flags = primitive | (depth_write << 3) | (depth_test << 4) | (alpha_test << 6) |
        (alpha_ref << 9) | (blend << 17) | (weight << 19) |
        ((wrap & 1) << 21) | (((wrap >> 8) & 1) << 22) | (force_alpha << 23);
    if (batch.draw_flags == flags && batch.draw_scissor == scissor && batch.draw_texture == tex0 &&
        batch.draw_mask == mask && batch.draw_depth_vram == depth_vram)
        return;

    batch.draw_flags = flags;
    batch.draw_scissor = scissor;
    batch.draw_texture = tex0;
    batch.draw_mask = mask;
    batch.draw_depth_vram = depth_vram;

    gs_draw_reg(GE_GS_STATE_FRAME, GS_FRAME_2, GS_SETREG_FRAME(batch.vram / 8192, GS_GE_STRIDE / 64,
        batch.format == 3 ? GS_PSM_CT32 : GS_PSM_CT16, mask));
    gs_draw_reg(GE_GS_STATE_ZBUF, GS_ZBUF_2, GS_SETREG_ZBUF(depth_vram / 8192, GS_PSMZ_16, !depth_write));
    static const u8 gs_alpha_func[] = {0, 1, 4, 7, 2, 3, 6, 5};
    gs_draw_reg(GE_GS_STATE_TEST, GS_TEST_2, GS_SETREG_TEST(alpha_test != 1, gs_alpha_func[alpha_test],
        alpha_ref, 0, 0, 0, 1, depth_test));
    batch.depth_written |= depth_write;
    gs_draw_reg(GE_GS_STATE_SCISSOR, GS_SCISSOR_2, scissor);
    gs_draw_reg(GE_GS_STATE_FBA, GS_FBA_2, force_alpha);

    if (texture) {
        if (gs_state_changed(GE_GS_STATE_TEXTURE, tex0)) {
            gs_reg(GS_TEXFLUSH, 0);
            gs_reg(GS_TEX0_2, tex0);
        }
        gs_draw_reg(GE_GS_STATE_CLAMP, GS_CLAMP_2, GS_SETREG_CLAMP(wrap & 1, (wrap >> 8) & 1, 0, 0, 0, 0));
    }

    if (blend == GE_GS_BLEND_ADD)
        gs_draw_reg(GE_GS_STATE_ALPHA, GS_ALPHA_2, GS_SETREG_ALPHA(0, 2, weight, 1, 128));
    else if (blend == GE_GS_BLEND_REVERSE_SUBTRACT)
        gs_draw_reg(GE_GS_STATE_ALPHA, GS_ALPHA_2, GS_SETREG_ALPHA(2, 0, weight, 1, 128));
    else if (blend == GE_GS_BLEND_SOURCE_ALPHA)
        gs_draw_reg(GE_GS_STATE_ALPHA, GS_ALPHA_2, GS_SETREG_ALPHA(0, 1, 0, 1, 0));

    gs_draw_reg(GE_GS_STATE_PRIM, GS_PRIM, GS_SETREG_PRIM(primitive, primitive != 6, texture != NULL, 0,
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
    u32 rgb = ((color >> 1) & 0x7f7f7fu) + (color & 0x010101u);
    return result | rgb;
}

static u64 vertex_xyz(const GeVertex *vertex)
{
    float depth = vertex->z;
    u32 z = !(depth >= 0.0f) ? 0 : depth >= 65535.0f ? 65535 : (int)depth;
    return GS_SETREG_XYZ((u32)(int)(vertex->x * 16 + GS_GE_OFFSET),
        (u32)(int)(vertex->y * 16 + GS_GE_OFFSET), z);
}

static inline void emit_vertex(u64 *output, const GeVertex *vertex,
    const GeHardwareTexture *texture, int full_alpha)
{
    u32 q = 0;
    u32 color = vertex->color;
    if (texture) {
        float s = vertex->u * vertex->inv_w * texture->s_scale;
        float t = vertex->v * vertex->inv_w * texture->t_scale;
        *output++ = GS_SETREG_STQ(float_bits(s), float_bits(t));
        q = float_bits(vertex->inv_w);
        if (texture->key.texture_function != GE_GS_TEXTURE_DECAL)
            color = texture_vertex_color(color, texture->key.texture_function, full_alpha);
    }

    output[0] = color | ((u64)q << 32);
    output[1] = vertex_xyz(vertex);
}

static u64 *reserve_vertices(unsigned fields, unsigned count)
{
    if (batch.vertex_count && batch.vertex_fields != fields)
        close_vertex_tag(0);
    if (!batch.vertex_count) {
        close_ad_tag(0);
        batch.vertex_start = ++batch.count;
        batch.vertex_fields = fields;
    }

    unsigned start = 2 * batch.vertex_start + batch.vertex_count * fields;
    batch.vertex_count += count;
    batch.count = (start + count * fields + 1) / 2;
    return &packet.registers[start];
}

static u64 *emit_triangle(const GeVertex *a, const GeVertex *b, const GeVertex *c,
    const GeHardwareTexture *texture, int full_alpha)
{
    unsigned fields = texture ? 3 : 2;
    u64 *output = reserve_vertices(fields, 3);
    emit_vertex(output, a, texture, full_alpha);
    emit_vertex(output + fields, b, texture, full_alpha);
    emit_vertex(output + 2 * fields, c, texture, full_alpha);
    return output;
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

        texture = load_texture(second.color, GE_GS_BLEND_NONE, 0, GE_GS_TEXTURE_DECAL, 0,
            v, v + bottom - top);
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

    if (depth) {
        if (ge_hardware_config_dirty)
            prepare_draw_config();

        uintptr_t offset = (uintptr_t)depth - (uintptr_t)psbridge_video_memory();
        uintptr_t color = (uintptr_t)draw_config.color - (uintptr_t)psbridge_video_memory();
        unsigned color_size = GS_GE_STRIDE * GS_GE_HEIGHT * (draw_config.format == 3 ? 4 : 2);
        unsigned depth_size = GS_GE_STRIDE * GS_GE_HEIGHT * 2;
        if ((offset & 63) || offset > 0x200000 - depth_size ||
            (offset < color + color_size && color < offset + depth_size))
            return 0;
    }

    int full = left == 0 && top == 0 && right == GS_GE_STRIDE && bottom == GS_GE_HEIGHT;
    if (!begin_draw(top, bottom, depth, full && !mask && !texture, full && depth))
        return 0;

    u64 scissor = GS_SETREG_SCISSOR(left, right - 1, top, bottom - 1);
    draw_state(6, scissor, mask, depth != NULL, 1,
        alpha_test, alpha_ref, 0, texture, GE_GS_BLEND_NONE);
    second.x = left;
    second.y = top;
    if (texture)
        gs_reg(GS_UV, GS_SETREG_UV((u32)(u * 16), (u32)(v * 16)));
    gs_reg(GS_RGBAQ, second.color);
    gs_reg(GS_XYZ2, vertex_xyz(&second));
    second.x = right;
    second.y = bottom;
    if (texture)
        gs_reg(GS_UV, GS_SETREG_UV((u32)((u + right - left) * 16),
            (u32)((v + bottom - top) * 16)));
    gs_reg(GS_RGBAQ, second.color);
    gs_reg(GS_XYZ2, vertex_xyz(&second));
    return 1;
}

static int vertex_in_gs(const GeVertex *vertex)
{
    return vertex->x >= -2048.0f && vertex->x < 2048.0f &&
        vertex->y >= -2048.0f && vertex->y < 2048.0f;
}

static int texture_coords_in_gs(const GeVertex *vertex)
{
    return vertex->u >= -4096.0f && vertex->u <= 4096.0f &&
        vertex->v >= -4096.0f && vertex->v <= 4096.0f;
}

static inline __attribute__((always_inline)) int draw_hardware_triangles(
    const GeVertex *a, const GeVertex *b, const GeVertex *c, unsigned strip_count,
    int top, int bottom)
{
    if (!vertex_in_gs(a) || !vertex_in_gs(b) || !vertex_in_gs(c))
        return 0;

    if (ge_hardware_config_dirty)
        prepare_draw_config();
    const GeHardwareState *config = &draw_config;
    if (config->triangle_result != 2)
        return config->triangle_result;

    int alpha_test = config->alpha_test;
    int alpha_ref = config->alpha_ref;
    int depth_test = config->depth_test;
    int depth_write = config->depth_write;
    GeGsBlend gs_blend = config->blend;
    u8 *depth = config->depth;
    if (depth &&
        (!(a->z >= 0.0f && a->z <= 65535.0f) ||
         !(b->z >= 0.0f && b->z <= 65535.0f) ||
         !(c->z >= 0.0f && c->z <= 65535.0f)))
        return 0;

    for (unsigned i = 3; i < strip_count; i++) {
        const GeVertex *vertex = &a[i];
        if (!vertex_in_gs(vertex) || vertex->color != a->color ||
            (depth && !(vertex->z >= 0.0f && vertex->z <= 65535.0f)))
            return 0;
    }

    const GeHardwareTexture *texture = NULL;
    int varying_source_alpha = 0;
    u32 mask = 0;
    if (!config->textured && gs_blend == GE_GS_BLEND_SOURCE_ALPHA) {
        unsigned alpha = a->color >> 24;
        if (alpha != b->color >> 24 || alpha != c->color >> 24)
            return 0;
        if (!compare_alpha(alpha_test, alpha, alpha_ref))
            return 1;
        if (alpha != 255)
            return 0;

        gs_blend = GE_GS_BLEND_NONE;
        alpha_test = 1;
    }

    if (config->textured) {
        if (!(a->inv_w > 0.0f) || !(b->inv_w > 0.0f) || !(c->inv_w > 0.0f))
            return 0;
        if (!texture_coords_in_gs(a) || !texture_coords_in_gs(b) || !texture_coords_in_gs(c))
            return 0;

        if (bottom > GS_GE_HEIGHT)
            return 0;

        GeGsTextureFunction function = GE_GS_TEXTURE_DECAL;
        u32 palette_color = a->color;
        if (a->color != b->color || b->color != c->color) {
            if (strip_count)
                return 0;

            if (!config->indexed || gs_blend == GE_GS_BLEND_NONE || !config->modulate)
                return 0;

            if (((a->color ^ b->color) | (b->color ^ c->color)) >> 24) {
                varying_source_alpha = gs_blend == GE_GS_BLEND_SOURCE_ALPHA &&
                    alpha_ref < 128 && config->texture_alpha &&
                    (a->color & b->color & c->color & 0xffffffu) == 0xffffffu;
                if (config->format != 1 || (alpha_test != 1 && !varying_source_alpha))
                    return 0;

                function = GE_GS_TEXTURE_MODULATE_RGBA;
                palette_color = 0xffffffffu;
            } else {
                function = GE_GS_TEXTURE_MODULATE_RGB;
                palette_color |= 0xffffffu;
            }
        }

        for (unsigned i = 3; i < strip_count; i++) {
            if (!(a[i].inv_w > 0.0f) || !texture_coords_in_gs(&a[i]))
                return 0;
        }

        int blend_ref = gs_blend == GE_GS_BLEND_SOURCE_ALPHA && !varying_source_alpha ?
            alpha_ref : 0;
        int binary_alpha = config->blend_enabled && gs_blend == GE_GS_BLEND_NONE;
        texture = matching_bound_texture(palette_color, gs_blend, binary_alpha, function, blend_ref);
        if (!texture || (texture->image_index && !images[texture->image_index - 1].complete)) {
            float min_v = a->v < b->v ? a->v : b->v;
            float max_v = a->v > b->v ? a->v : b->v;
            if (c->v < min_v)
                min_v = c->v;
            if (c->v > max_v)
                max_v = c->v;

            if (!(a->inv_w >= 0.00001f && a->inv_w <= 100000.0f &&
                  b->inv_w >= 0.00001f && b->inv_w <= 100000.0f &&
                  c->inv_w >= 0.00001f && c->inv_w <= 100000.0f))
                min_v = 0.0f;

            for (unsigned i = 3; i < strip_count; i++) {
                if (a[i].v < min_v)
                    min_v = a[i].v;
                if (a[i].v > max_v)
                    max_v = a[i].v;
                if (!(a[i].inv_w >= 0.00001f && a[i].inv_w <= 100000.0f))
                    min_v = 0.0f;
            }

            texture = texture ? use_texture(texture, min_v, max_v) :
                load_texture(palette_color, gs_blend, binary_alpha, function, blend_ref, min_v, max_v);
        }
        if (!texture || !texture->tex0)
            return 0;
    }

    if (strip_count && gs_blend == GE_GS_BLEND_SOURCE_ALPHA &&
        texture->source_alpha_classes == (GE_GS_SOURCE_ALPHA_LOW | GE_GS_SOURCE_ALPHA_HIGH))
        return 0;

    if (!begin_draw(top, bottom, depth, 0, 0))
        return 0;

    u64 scissor = config->scissor;
    const u64 *first_pass = NULL;
    int force_alpha = 0;
    if (varying_source_alpha) {
        if ((a->color | b->color | c->color) >> 31) {
            draw_state(3, scissor, 0x00ffffffu, 0, depth_test,
                7, 128, 0, texture, GE_GS_BLEND_NONE);
            emit_triangle(a, b, c, texture, 1);
        }

        mask = 0x80000000u;
        alpha_ref >>= 1;
    } else if (texture && texture->key.texture_function == GE_GS_TEXTURE_MODULATE_RGBA) {
        draw_state(3, scissor, 0x00ffffffu, 0, depth_test,
            1, 0, 0, texture, GE_GS_BLEND_NONE);
        emit_triangle(a, b, c, texture, 1);
        mask = 0x80000000u;
    } else if (gs_blend == GE_GS_BLEND_SOURCE_ALPHA) {
        unsigned classes = texture->source_alpha_classes;
        if (texture->key.texture_function != GE_GS_TEXTURE_MODULATE_RGBA) {
            force_alpha = classes == GE_GS_SOURCE_ALPHA_HIGH;
            if (classes == (GE_GS_SOURCE_ALPHA_LOW | GE_GS_SOURCE_ALPHA_HIGH)) {
                draw_state(3, scissor, 0x00ffffffu, 0, depth_test,
                    7, 64, 1, texture, GE_GS_BLEND_NONE);
                first_pass = emit_triangle(a, b, c, texture, 0);
            }
        }
        mask = force_alpha ? 0 : 0x80000000u;
        alpha_ref = 0;
    }

    if (strip_count) {
        int reset_strip = (batch.state_valid & (1u << GE_GS_STATE_PRIM)) &&
            (batch.state[GE_GS_STATE_PRIM] & 7) == 4;
        draw_state(4, scissor, mask, depth_write, depth_test,
            alpha_test, alpha_ref, force_alpha, texture, gs_blend);
        if (reset_strip)
            gs_reg(GS_PRIM, batch.state[GE_GS_STATE_PRIM]);

        unsigned fields = texture ? 3 : 2;
        u64 *output = reserve_vertices(fields, strip_count);
        for (unsigned i = 0; i < strip_count; i++)
            emit_vertex(output + i * fields, &a[i], texture, 0);
        return 1;
    }

    draw_state(3, scissor, mask, depth_write, depth_test,
        alpha_test, alpha_ref, force_alpha, texture, gs_blend);
    if (first_pass)
        memcpy(reserve_vertices(3, 3), first_pass, 9 * sizeof(*first_pass));
    else
        emit_triangle(a, b, c, texture, 0);
    return 1;
}

int __attribute__((noinline)) ge_hardware_triangle(const GeVertex *a, const GeVertex *b, const GeVertex *c,
    int top, int bottom)
{
    return draw_hardware_triangles(a, b, c, 0, top, bottom);
}

int ge_hardware_strip(const GeVertex *vertices, unsigned count, int top, int bottom)
{
    return draw_hardware_triangles(vertices, vertices + 1, vertices + 2, count, top, bottom);
}
