#include <string.h>

#include "ge_internal.h"

static u8 ge_clut[1024] __attribute__((aligned(4)));
static u32 ge_clut_entries;
static int ge_clut_loaded;

u32 unpack_color(u16 color, int format)
{
    u32 red;
    u32 green;
    u32 blue;
    u32 alpha;

    if (format == 0) {
        red = (color & 0x1f) * 255 / 31;
        green = ((color >> 5) & 0x3f) * 255 / 63;
        blue = ((color >> 11) & 0x1f) * 255 / 31;
        alpha = 255;
    } else if (format == 1) {
        red = (color & 0x1f) * 255 / 31;
        green = ((color >> 5) & 0x1f) * 255 / 31;
        blue = ((color >> 10) & 0x1f) * 255 / 31;
        alpha = (color & 0x8000) ? 255 : 0;
    } else {
        red = (color & 0x0f) * 17;
        green = ((color >> 4) & 0x0f) * 17;
        blue = ((color >> 8) & 0x0f) * 17;
        alpha = ((color >> 12) & 0x0f) * 17;
    }

    

    return red | (green << 8) | (blue << 16) | (alpha << 24);
}

u16 pack_color(u32 color, int format)
{
    u32 red = color & 0xff;
    u32 green = (color >> 8) & 0xff;
    u32 blue = (color >> 16) & 0xff;
    u32 alpha = color >> 24;

    if (format == 0)
        return (red >> 3) | ((green >> 2) << 5) | ((blue >> 3) << 11);

    if (format == 1)
        return
            (red >> 3) |
            ((green >> 3) << 5) |
            ((blue >> 3) << 10) |
            ((alpha >= 0x80) << 15);

    return
        (red >> 4) |
        ((green >> 4) << 4) |
        ((blue >> 4) << 8) |
        ((alpha >> 4) << 12);
}

u32 read_frame_pixel(const void *buffer, int index, int format)
{
    if (format == 3)
        return ((const u32 *)buffer)[index];

    return unpack_color(((const u16 *)buffer)[index], format);
}

void write_frame_pixel(void *buffer, int index, int format, u32 color)
{
    if (format == 3)
        ((u32 *)buffer)[index] = color;
    else
        ((u16 *)buffer)[index] = pack_color(color, format);
}

int compare_alpha(int function, int value, int reference)
{
    if (function == 0)
        return 0;
    if (function == 1)
        return 1;
    if (function == 2)
        return value == reference;
    if (function == 3)
        return value != reference;
    if (function == 4)
        return value < reference;
    if (function == 5)
        return value <= reference;
    if (function == 6)
        return value > reference;

    return value >= reference;
}

static int blend_factor(int type, u32 source, u32 destination, u32 fixed, int shift, int source_factor)
{
    int src_alpha = (source >> 24) & 0xff;
    int dst_alpha = (destination >> 24) & 0xff;
    int other = (source_factor ? destination : source) >> shift & 0xff;

    switch (type) {
    case 0: return other;
    case 1: return 255 - other;
    case 2: return src_alpha;
    case 3: return 255 - src_alpha;
    case 4: return dst_alpha;
    case 5: return 255 - dst_alpha;
    case 6: return src_alpha * 2;
    case 7: return 255 - src_alpha * 2;
    case 8: return dst_alpha * 2;
    case 9: return 255 - dst_alpha * 2;
    default: return (fixed >> shift) & 0xff;
    }
}

u32 blend_color(u32 source, u32 destination)
{
    u32 mode = ge_state.command[GE_BLENDMODE];
    int src_factor = mode & 0xf;
    int dst_factor = (mode >> 4) & 0xf;
    int operation = (mode >> 8) & 7;
    u32 result = 0;
    int shift;

    if (!ge_blend_mode_seen) {
        src_factor = 2;
        dst_factor = 3;
        operation = 0;
    }

    if (src_factor == 2 && dst_factor == 3 && operation == 0) {
        u32 alpha = source >> 24;
        u32 inverse = 255 - alpha;
        u32 red = ((source & 0xff) * alpha + (destination & 0xff) * inverse) / 255;
        u32 green = (((source >> 8) & 0xff) * alpha + ((destination >> 8) & 0xff) * inverse) / 255;
        u32 blue = (((source >> 16) & 0xff) * alpha + ((destination >> 16) & 0xff) * inverse) / 255;
        u32 output_alpha = alpha + ((destination >> 24) * inverse) / 255;
        return red | (green << 8) | (blue << 16) | (output_alpha << 24);
    }

    for (shift = 0; shift < 24; shift += 8) {
        int src = (source >> shift) & 0xff;
        int dst = (destination >> shift) & 0xff;
        int a = blend_factor(src_factor, source, destination, ge_state.command[GE_BLENDFIXEDA], shift, 1);
        int b = blend_factor(dst_factor, source, destination, ge_state.command[GE_BLENDFIXEDB], shift, 0);
        int value;

        switch (operation) {
        case 1: value = (src * a - dst * b) / 255; break;
        case 2: value = (dst * b - src * a) / 255; break;
        case 3: value = src < dst ? src : dst; break;
        case 4: value = src > dst ? src : dst; break;
        case 5: value = src > dst ? src - dst : dst - src; break;
        default: value = (src * a + dst * b) / 255; break;
        }
        if (value < 0)
            value = 0;
        if (value > 255)
            value = 255;
        result |= (u32)value << shift;
    }

    return result | (source & 0xff000000u);
}

void load_clut(u32 argument)
{
    u32 format = ge_state.command[GE_CLUTFORMAT] & 3;
    u32 entries = (argument & 0x3f) * 8;
    u32 bytes_per_entry = format == 3 ? 4 : 2;
    u32 address =
        (ge_state.command[GE_CLUTADDR] & 0x00fffff0) |
        ((ge_state.command[GE_CLUTADDRUPPER] << 8) & 0x0f000000);
    const void *source = ge_pointer(address);

    if (entries > 256)
        entries = 256;
    if (!source || !entries)
        return;

    memcpy(ge_clut, source, entries * bytes_per_entry);
    ge_clut_entries = entries;
    ge_clut_loaded = 1;
}

u32 palette_color(u32 index)
{
    u32 format = ge_state.command[GE_CLUTFORMAT] & 3;
    u32 shift = (ge_state.command[GE_CLUTFORMAT] >> 2) & 0x1f;
    u32 mask = (ge_state.command[GE_CLUTFORMAT] >> 8) & 0xff;
    u32 start = ((ge_state.command[GE_CLUTFORMAT] >> 16) & 0x1f) << 4;
    u32 address;
    const void *palette;

    index = (((index >> shift) & mask) | start) & 0xff;
    if (ge_clut_loaded) {
        if (index >= ge_clut_entries)
            return 0;
        palette = ge_clut;
    } else {
        address =
            (ge_state.command[GE_CLUTADDR] & 0x00fffff0) |
            ((ge_state.command[GE_CLUTADDRUPPER] << 8) & 0x0f000000);
        palette = ge_pointer(address);
        if (!palette)
            return 0;
    }

    if (format == 3)
        return ((const u32 *)palette)[index];

    return unpack_color(((const u16 *)palette)[index], format);
}

u32 texture_pixel(int x, int y)
{
    int format = ge_state.command[GE_TEXFORMAT] & 0xf;
    int width = 1 << (ge_state.command[GE_TEXSIZE0] & 0xf);
    int height = 1 << ((ge_state.command[GE_TEXSIZE0] >> 8) & 0xf);
    int buffer_width = ge_state.command[GE_TEXBUFWIDTH0] & 0x7ff;
    int swizzled = ge_state.command[GE_TEXMODE] & 1;
    int clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    int clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    const u8 *texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));
    int offset;

    if (!texture || !width || !height)
        return 0;

    

    if (clamp_x) {
        if (x < 0)
            x = 0;
        if (x >= width)
            x = width - 1;
    } else {
        x %= width;
        if (x < 0)
            x += width;
    }

    if (clamp_y) {
        if (y < 0)
            y = 0;
        if (y >= height)
            y = height - 1;
    } else {
        y %= height;
        if (y < 0)
            y += height;
    }

    if (!buffer_width)
        buffer_width = width;

    if (format <= 2) {
        offset = pixel_offset(x, y, buffer_width * 2, 2, swizzled);
        return unpack_color(*(const u16 *)(texture + offset), format);
    }

    if (format == 3) {
        offset = pixel_offset(x, y, buffer_width * 4, 4, swizzled);
        return *(const u32 *)(texture + offset);
    }

    if (format == 4) {
        int byte_x = x >> 1;

        if (swizzled)
            offset = pixel_offset(byte_x, y, (buffer_width + 1) >> 1, 1, 1);
        else
            offset = y * ((buffer_width + 1) >> 1) + byte_x;

        if (x & 1)
            return palette_color(texture[offset] >> 4);

        return palette_color(texture[offset] & 0x0f);
    }

    if (format == 5) {
        offset = pixel_offset(x, y, buffer_width, 1, swizzled);
        return palette_color(texture[offset]);
    }

    if (format == 6) {
        offset = pixel_offset(x, y, buffer_width * 2, 2, swizzled);
        return palette_color(*(const u16 *)(texture + offset));
    }

    if (format == 7) {
        offset = pixel_offset(x, y, buffer_width * 4, 4, swizzled);
        return palette_color(*(const u32 *)(texture + offset));
    }

    if (format >= 8 && format <= 10) {
        int bytes = format == 8 ? 8 : 16;
        int blocks_w = (buffer_width + 3) >> 2;
        const u8 *block = texture + ((y >> 2) * blocks_w + (x >> 2)) * bytes;
        const u8 *colors = block;
        u16 color0 = colors[4] | ((u16)colors[5] << 8);
        u16 color1 = colors[6] | ((u16)colors[7] << 8);
        u32 c0 = unpack_color(color0, 0);
        u32 c1 = unpack_color(color1, 0);
        u32 indices = (u32)colors[0] | ((u32)colors[1] << 8) | ((u32)colors[2] << 16) | ((u32)colors[3] << 24);
        int pos = ((y & 3) << 2) | (x & 3);
        int which = (indices >> (pos * 2)) & 3;
        u32 rgb;
        u32 a = 255;
        if (which == 0)
            rgb = c0;
        else if (which == 1)
            rgb = c1;
        else if (which == 2)
            rgb = (((c0 & 0xff) * 2 + (c1 & 0xff)) / 3) | (((((c0 >> 8) & 0xff) * 2 + ((c1 >> 8) & 0xff)) / 3) << 8) | (((((c0 >> 16) & 0xff) * 2 + ((c1 >> 16) & 0xff)) / 3) << 16);
        else
            rgb = (((c0 & 0xff) + (c1 & 0xff) * 2) / 3) | (((((c0 >> 8) & 0xff) + ((c1 >> 8) & 0xff) * 2) / 3) << 8) | (((((c0 >> 16) & 0xff) + ((c1 >> 16) & 0xff) * 2) / 3) << 16);
        if (format == 8 && color0 <= color1) {
            if (which == 2)
                rgb = (((c0 & 0xff) + (c1 & 0xff)) >> 1) | (((((c0 >> 8) & 0xff) + ((c1 >> 8) & 0xff)) >> 1) << 8) | (((((c0 >> 16) & 0xff) + ((c1 >> 16) & 0xff)) >> 1) << 16);
            if (which == 3)
                a = 0;
        } else if (format == 9) {
            a = ((block[8 + (pos >> 1)] >> ((pos & 1) * 4)) & 15) * 17;
        } else if (format == 10) {
            u8 alphas[8];
            u32 alpha_index = (u32)(block[8 + pos * 3 / 8] >> ((pos * 3) & 7));
            if (((pos * 3) & 7) > 5)
                alpha_index |= (u32)block[9 + pos * 3 / 8] << (8 - ((pos * 3) & 7));
            alpha_index &= 7;
            alphas[0] = block[14]; alphas[1] = block[15];
            if (alphas[0] > alphas[1]) {
                int i;
                for (i = 2; i < 8; i++)
                    alphas[i] = (u8)(((8 - i) * alphas[0] + (i - 1) * alphas[1]) / 7);
            } else {
                int i;
                for (i = 2; i < 6; i++)
                    alphas[i] = (u8)(((6 - i) * alphas[0] + (i - 1) * alphas[1]) / 5);
                alphas[6] = 0;
                alphas[7] = 255;
            }
            a = alphas[alpha_index];
        }
        return (rgb & 0xffffff) | (a << 24);
    }

    return 0;
}

u32 shade_pixel(u32 texture, u32 vertex)
{
    int function = ge_state.command[GE_TEXFUNC] & 7;
    int use_alpha = (ge_state.command[GE_TEXFUNC] & 0x100) != 0;
    u32 environment = ge_state.command[GE_TEXENVCOLOR];
    u32 texture_alpha = (texture >> 24) & 0xff;
    u32 vertex_alpha = (vertex >> 24) & 0xff;
    u32 output_alpha = vertex_alpha;
    u32 result = 0;
    int channel;

    if (function == 3)
        output_alpha = use_alpha ? texture_alpha : vertex_alpha;
    else if (function != 1 && use_alpha)
        output_alpha = (texture_alpha * vertex_alpha) / 255;

    for (channel = 0; channel < 3; channel++) {
        u32 shift = channel * 8;
        u32 t = (texture >> shift) & 0xff;
        u32 v = (vertex >> shift) & 0xff;
        u32 e = (environment >> shift) & 0xff;
        u32 c;

        switch (function) {
        case 1:
            c = use_alpha ? (v * (255 - texture_alpha) + t * texture_alpha) / 255 : t;
            break;
        case 2:
            c = (v * (255 - t) + e * t) / 255;
            break;
        case 3:
            c = t;
            break;
        case 4:
            c = v + t;
            if (c > 255)
                c = 255;
            break;
        default:
            c = (v * t) / 255;
            break;
        }
        result |= c << shift;
    }

    return result | (output_alpha << 24);
}

int prepare_triangle_palette(TrianglePalette *fast)
{
    int buffer_width;
    int entries;
    int blend;
    int alpha_test;
    u32 alpha_command;
    int index;

    fast->format = ge_state.command[GE_TEXFORMAT] & 0xf;

    if (fast->format != 4 && fast->format != 5)
        return 0;
    if ((ge_state.command[GE_FRAMEBUF_FORMAT] & 3) != 3)
        return 0;
    if ((ge_state.command[GE_TEXFUNC] & 7) != 0)
        return 0;

    fast->texture_width = 1 << (ge_state.command[GE_TEXSIZE0] & 0xf);
    fast->texture_height = 1 << ((ge_state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = ge_state.command[GE_TEXBUFWIDTH0] & 0x7ff;

    if (!buffer_width)
        buffer_width = fast->texture_width;

    fast->texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));
    fast->frame = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));
    fast->frame_width = ge_state.command[GE_VADDR_WIDTH] & 0x7fc;
    fast->row_bytes = fast->format == 4 ? (buffer_width + 1) >> 1 : buffer_width;
    fast->swizzled = ge_state.command[GE_TEXMODE] & 1;
    fast->clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    fast->clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;

    if (!fast->texture || !fast->frame || fast->frame_width < 64)
        return 0;

    blend = ge_state.command[GE_BLEND_ENABLE] & 1;
    fast->blend = blend;
    alpha_test = ge_state.command[GE_ALPHA_TEST_ENABLE] & 1;
    alpha_command = ge_state.command[GE_ALPHATEST];

    entries = fast->format == 4 ? 16 : 256;

    for (index = 0; index < entries; index++) {
        u32 color = shade_pixel(palette_color(index), 0xffffffffu);

        fast->visible[index] = !alpha_test || compare_alpha(
            alpha_command & 7,
            (color >> 24) & ((alpha_command >> 16) & 0xff),
            (alpha_command >> 8) & ((alpha_command >> 16) & 0xff)
        );

        fast->colors[index] = color;
    }

    return 1;
}
