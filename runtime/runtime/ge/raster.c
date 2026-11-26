#include <string.h>
#include <math.h>

#include "ge_internal.h"

static void draw_pixel_internal(int x, int y, u32 color, float depth, int use_depth)
{
    int width = ge_state.command[GE_VADDR_WIDTH] & 0x7fc;
    int format = ge_state.command[GE_FRAMEBUF_FORMAT] & 3;
    int left = ge_state.command[GE_SCISSOR1] & 0x3ff;
    int top = (ge_state.command[GE_SCISSOR1] >> 10) & 0x3ff;
    int right = ge_state.command[GE_SCISSOR2] & 0x3ff;
    int bottom = (ge_state.command[GE_SCISSOR2] >> 10) & 0x3ff;
    void *frame = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));
    int index;
    u32 alpha_command;
    u32 destination;
    u16 *depth_buffer = NULL;
    int depth_width = 0;
    u16 depth_value = 0;

    if (!frame || width < 64 || width > 2048)
        return;

    if (!ge_state.command[GE_SCISSOR2]) {
        right = 479;
        bottom = 271;
    }

    if (x < left || y < top || x > right || y > bottom)
        return;

    if (x < 0 || y < 0 || x >= width || y >= 512)
        return;

    if (use_depth) {
        if (depth < 0.0f)
            depth = 0.0f;
        if (depth > 65535.0f)
            depth = 65535.0f;
        depth_value = (u16)(u32)depth;
        depth_width = ge_state.command[GE_DEPTHBUF_WIDTH] & 0x7fc;
        depth_buffer = ge_pointer(buffer_address(
            GE_DEPTHBUF_BASE, GE_DEPTHBUF_WIDTH
        ));
        if (depth_width < 64 || depth_width > 2048)
            depth_buffer = NULL;
    }

    if ((ge_state.command[GE_ALPHA_TEST_ENABLE] & 1) && !(ge_state.command[GE_CLEARMODE] & 1)) {
        alpha_command = ge_state.command[GE_ALPHATEST];

        if (!compare_alpha(
            alpha_command & 7,
            (color >> 24) & ((alpha_command >> 16) & 0xff),
            (alpha_command >> 8) & ((alpha_command >> 16) & 0xff)
        ))
            return;
    }

    if (depth_buffer && !(ge_state.command[GE_CLEARMODE] & 1) &&
        (ge_state.command[GE_DEPTH_TEST_ENABLE] & 1) &&
        !compare_alpha(
            ge_state.command[GE_DEPTHTEST] & 7,
            depth_value,
            depth_buffer[y * depth_width + x]
        ))
        return;

    index = y * width + x;

    if (ge_state.command[GE_CLEARMODE] & 1) {
        u32 clear = ge_state.command[GE_CLEARMODE];

        destination = read_frame_pixel(frame, index, format);

        if (!(clear & 0x100))
            color = (color & 0xff000000) | (destination & 0x00ffffff);
        if (!(clear & 0x200))
            color = (color & 0x00ffffff) | (destination & 0xff000000);
    } else if (ge_state.command[GE_BLEND_ENABLE] & 1) {
        destination = read_frame_pixel(frame, index, format);

        color = blend_color(color, destination);
    }

    write_frame_pixel(frame, index, format, color);

    if (depth_buffer) {
        if (ge_state.command[GE_CLEARMODE] & 0x400)
            depth_buffer[y * depth_width + x] = depth_value;
        else if (!(ge_state.command[GE_CLEARMODE] & 1) &&
            !(ge_state.command[GE_DEPTH_WRITE_DISABLE] & 1))
            depth_buffer[y * depth_width + x] = depth_value;
    }
}

static void draw_pixel(int x, int y, u32 color)
{
    draw_pixel_internal(x, y, color, 0.0f, 0);
}

static int fast_sprite_target(
    int *left,
    int *top,
    int *right,
    int *bottom,
    void **frame,
    int *width
)
{
    int scissor_left = ge_state.command[GE_SCISSOR1] & 0x3ff;
    int scissor_top = (ge_state.command[GE_SCISSOR1] >> 10) & 0x3ff;
    int scissor_right = ge_state.command[GE_SCISSOR2] & 0x3ff;
    int scissor_bottom = (ge_state.command[GE_SCISSOR2] >> 10) & 0x3ff;

    *width = ge_state.command[GE_VADDR_WIDTH] & 0x7fc;
    *frame = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));

    if (!*frame || *width < 64 || *width > 2048)
        return 0;

    if (!ge_state.command[GE_SCISSOR2]) {
        scissor_right = 479;
        scissor_bottom = 271;
    }

    if (*left < scissor_left)
        *left = scissor_left;
    if (*top < scissor_top)
        *top = scissor_top;
    if (*right > scissor_right + 1)
        *right = scissor_right + 1;
    if (*bottom > scissor_bottom + 1)
        *bottom = scissor_bottom + 1;

    if (*left < 0)
        *left = 0;
    if (*top < 0)
        *top = 0;
    if (*right > *width)
        *right = *width;
    if (*bottom > 512)
        *bottom = 512;

    return *right > *left && *bottom > *top;
}

static int draw_solid_sprite(
    GeVertex second,
    int left,
    int top,
    int right,
    int bottom
)
{
    void *frame;
    int width;
    int format;
    int x;
    int y;
    u32 clear = ge_state.command[GE_CLEARMODE];
    int write_color = !(clear & 1) || (clear & 0x100);
    int write_alpha = !(clear & 1) || (clear & 0x200);

    if ((ge_state.command[GE_TEXTURE_ENABLE] & 1) && !(clear & 1))
        return 0;

    if ((ge_state.command[GE_BLEND_ENABLE] & 1) && !(clear & 1))
        return 0;

    format = ge_state.command[GE_FRAMEBUF_FORMAT] & 3;

    if (format != 1 && format != 3)
        return 0;

    if (!write_color && !write_alpha)
        return 1;

    if (!fast_sprite_target(&left, &top, &right, &bottom, &frame, &width))
        return 1;

    if ((ge_state.command[GE_ALPHA_TEST_ENABLE] & 1) && !(clear & 1)) {
        u32 alpha = ge_state.command[GE_ALPHATEST];
        u32 mask = (alpha >> 16) & 0xff;

        if (!compare_alpha(
            alpha & 7,
            (second.color >> 24) & mask,
            (alpha >> 8) & mask
        ))
            return 1;
    }

    if (format == 3) {
        u32 color = second.color;

        for (y = top; y < bottom; y++) {
            u32 *row = (u32 *)frame + y * width + left;

            if (write_color && write_alpha) {
                for (x = left; x < right; x++)
                    *row++ = color;
            } else if (write_color) {
                for (x = left; x < right; x++) {
                    *row = (*row & 0xff000000) | (color & 0x00ffffff);
                    row++;
                }
            } else {
                for (x = left; x < right; x++) {
                    *row = (*row & 0x00ffffff) | (color & 0xff000000);
                    row++;
                }
            }
        }

        return 1;
    }

    {
        u16 color = pack_color(second.color, 1);
        u16 *pixels = frame;
        u16 mask = (write_color ? 0x7fff : 0) | (write_alpha ? 0x8000 : 0);

        for (y = top; y < bottom; y++) {
            u16 *row = pixels + y * width + left;

            if (write_color && write_alpha) {
                for (x = left; x < right; x++)
                    *row++ = color;
            } else {
                for (x = left; x < right; x++) {
                    *row = (*row & ~mask) | (color & mask);
                    row++;
                }
            }
        }
    }

    return 1;
}

static void clear_depth_sprite(
    GeVertex second,
    int left,
    int top,
    int right,
    int bottom
)
{
    void *frame;
    u16 *depth;
    int frame_width;
    int depth_width;
    int x;
    int y;
    u16 value;

    if (!(ge_state.command[GE_CLEARMODE] & 0x400))
        return;

    if (!fast_sprite_target(&left, &top, &right, &bottom, &frame, &frame_width))
        return;

    depth_width = ge_state.command[GE_DEPTHBUF_WIDTH] & 0x7fc;
    depth = ge_pointer(buffer_address(GE_DEPTHBUF_BASE, GE_DEPTHBUF_WIDTH));
    if (!depth || depth_width < 64 || depth_width > 2048)
        return;

    if (right > depth_width)
        right = depth_width;
    value = second.z < 0.0f ? 0 :
        second.z > 65535.0f ? 65535 : (u16)(u32)second.z;

    for (y = top; y < bottom; y++)
        for (x = left; x < right; x++)
            depth[y * depth_width + x] = value;
}

static int draw_rgba5551_sprite(
    GeVertex first,
    GeVertex second,
    int left,
    int top,
    int right,
    int bottom
)
{
    int texture_width;
    int texture_height;
    int buffer_width;
    int clamp_x;
    int clamp_y;
    const u16 *texture;
    void *frame;
    int frame_width;
    int use_alpha;
    int alpha_test;
    int alpha_zero = 1;
    int alpha_one = 1;
    int du;
    int dv;
    int u_start;
    int v;
    int x;
    int y;

    if (!(ge_state.command[GE_TEXTURE_ENABLE] & 1) || (ge_state.command[GE_CLEARMODE] & 1))
        return 0;

    if ((ge_state.command[GE_TEXFORMAT] & 0xf) != 1 || (ge_state.command[GE_TEXMODE] & 1))
        return 0;

    if ((ge_state.command[GE_BLEND_ENABLE] & 1) ||
        ((ge_state.command[GE_TEXFUNC] & 7) != 3 && second.color != 0xffffffff))
        return 0;

    if ((ge_state.command[GE_FRAMEBUF_FORMAT] & 3) != 1)
        return 0;

    texture_width = 1 << (ge_state.command[GE_TEXSIZE0] & 0xf);
    texture_height = 1 << ((ge_state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = texture_buffer_width(1);
    clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));

    if (!texture || !texture_width || !texture_height)
        return 0;

    if (!fast_sprite_target(
        &left,
        &top,
        &right,
        &bottom,
        &frame,
        &frame_width
    ))
        return 1;

    use_alpha = ge_state.command[GE_TEXFUNC] & 0x100;
    alpha_test = ge_state.command[GE_ALPHA_TEST_ENABLE] & 1;

    if (alpha_test) {
        u32 alpha = ge_state.command[GE_ALPHATEST];
        u32 mask = (alpha >> 16) & 0xff;
        u32 reference = (alpha >> 8) & mask;

        alpha_zero = compare_alpha(alpha & 7, 0 & mask, reference);
        alpha_one = compare_alpha(alpha & 7, 255 & mask, reference);
    }

    du = (int)(
        (second.u - first.u) * 65536.0f /
        (second.x - first.x)
    );
    dv = (int)(
        (second.v - first.v) * 65536.0f /
        (second.y - first.y)
    );
    u_start =
        (int)(first.u * 65536.0f) +
        (int)((left - first.x) * du);
    v =
        (int)(first.v * 65536.0f) +
        (int)((top - first.y) * dv);

    for (y = top; y < bottom; y++) {
        int texture_y = v >> 16;
        int u = u_start;
        const u16 *texture_row;
        u16 *row = (u16 *)frame + y * frame_width + left;

        if (clamp_y) {
            if (texture_y < 0)
                texture_y = 0;
            if (texture_y >= texture_height)
                texture_y = texture_height - 1;
        } else {
            texture_y = (u32)texture_y & (texture_height - 1);
        }

        texture_row = texture + texture_y * buffer_width;

        for (x = left; x < right; x++) {
            int texture_x = u >> 16;
            u16 color;

            if (clamp_x) {
                if (texture_x < 0)
                    texture_x = 0;
                if (texture_x >= texture_width)
                    texture_x = texture_width - 1;
            } else {
                texture_x = (u32)texture_x & (texture_width - 1);
            }

            color = texture_row[texture_x];

            if (!use_alpha)
                color |= 0x8000;

            if ((color & 0x8000) ? alpha_one : alpha_zero)
                *row = color;

            row++;
            u += du;
        }

        v += dv;
    }

    return 1;
}

static int draw_clut4_sprite(
    GeVertex first,
    GeVertex second,
    int left,
    int top,
    int right,
    int bottom
)
{
    u32 colors[16];
    u8 visible[16];
    int texture_width;
    int texture_height;
    int buffer_width;
    int row_bytes;
    int swizzled;
    int clamp_x;
    int clamp_y;
    const u8 *texture;
    void *frame;
    int frame_width;
    int blend;
    int alpha_test;
    u32 alpha_command;
    int du;
    int dv;
    int u_start;
    int v;
    int index;
    int x;
    int y;

    if (!(ge_state.command[GE_TEXTURE_ENABLE] & 1) || (ge_state.command[GE_CLEARMODE] & 1))
        return 0;

    if ((ge_state.command[GE_TEXFORMAT] & 0xf) != 4)
        return 0;

    if ((ge_state.command[GE_FRAMEBUF_FORMAT] & 3) != 3)
        return 0;

    texture_width = 1 << (ge_state.command[GE_TEXSIZE0] & 0xf);
    texture_height = 1 << ((ge_state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = texture_buffer_width(4);
    swizzled = ge_state.command[GE_TEXMODE] & 1;
    clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));

    if (!texture || !texture_width || !texture_height)
        return 0;

    if (!fast_sprite_target(
        &left,
        &top,
        &right,
        &bottom,
        &frame,
        &frame_width
    ))
        return 1;

    blend = ge_state.command[GE_BLEND_ENABLE] & 1;
    alpha_test = ge_state.command[GE_ALPHA_TEST_ENABLE] & 1;
    alpha_command = ge_state.command[GE_ALPHATEST];

    for (index = 0; index < 16; index++) {
        u32 color = shade_pixel(palette_color(index), second.color);

        colors[index] = color;
        visible[index] = !alpha_test || compare_alpha(
            alpha_command & 7,
            (color >> 24) & ((alpha_command >> 16) & 0xff),
            (alpha_command >> 8) & ((alpha_command >> 16) & 0xff)
        );
    }

    du = (int)(
        (second.u - first.u) * 65536.0f /
        (second.x - first.x)
    );
    dv = (int)(
        (second.v - first.v) * 65536.0f /
        (second.y - first.y)
    );
    u_start =
        (int)(first.u * 65536.0f) +
        (int)((left - first.x) * du);
    v =
        (int)(first.v * 65536.0f) +
        (int)((top - first.y) * dv);
    row_bytes = (buffer_width + 1) >> 1;

    for (y = top; y < bottom; y++) {
        int texture_y = v >> 16;
        int u = u_start;
        u32 *row = (u32 *)frame + y * frame_width + left;

        if (clamp_y) {
            if (texture_y < 0)
                texture_y = 0;
            if (texture_y >= texture_height)
                texture_y = texture_height - 1;
        } else {
            texture_y = (u32)texture_y & (texture_height - 1);
        }

        for (x = left; x < right; x++) {
            int texture_x = u >> 16;
            int offset;
            u8 packed;
            u32 color;

            if (clamp_x) {
                if (texture_x < 0)
                    texture_x = 0;
                if (texture_x >= texture_width)
                    texture_x = texture_width - 1;
            } else {
                texture_x = (u32)texture_x & (texture_width - 1);
            }

            if (swizzled)
                offset = pixel_offset(texture_x >> 1, texture_y, row_bytes, 1, 1);
            else
                offset = texture_y * row_bytes + (texture_x >> 1);

            packed = texture[offset];
            index = (texture_x & 1) ? packed >> 4 : packed & 0x0f;

            if (!visible[index]) {
                row++;
                u += du;
                continue;
            }

            color = colors[index];

            if (blend)
                *row = blend_color(color, *row);
            else
                *row = color;

            row++;
            u += du;
        }

        v += dv;
    }

    return 1;
}

static int draw_rgba8888_sprite(
    GeVertex first,
    GeVertex second,
    int left,
    int top,
    int right,
    int bottom
)
{
    int texture_width;
    int texture_height;
    int buffer_width;
    int row_bytes;
    int swizzled;
    int clamp_x;
    int clamp_y;
    const u8 *texture;
    void *frame;
    int frame_width;
    int blend;
    int alpha_test;
    u32 alpha_command;
    int direct_color;
    int use_alpha;
    int du;
    int dv;
    int u_start;
    int v;
    int x;
    int y;

    if (!(ge_state.command[GE_TEXTURE_ENABLE] & 1) || (ge_state.command[GE_CLEARMODE] & 1))
        return 0;

    if ((ge_state.command[GE_TEXFORMAT] & 0xf) != 3)
        return 0;

    if ((ge_state.command[GE_FRAMEBUF_FORMAT] & 3) != 3)
        return 0;

    texture_width = 1 << (ge_state.command[GE_TEXSIZE0] & 0xf);
    texture_height = 1 << ((ge_state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = texture_buffer_width(3);
    swizzled = ge_state.command[GE_TEXMODE] & 1;
    clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));

    if (!texture || !texture_width || !texture_height)
        return 0;

    if (!fast_sprite_target(
        &left,
        &top,
        &right,
        &bottom,
        &frame,
        &frame_width
    ))
        return 1;

    blend = ge_state.command[GE_BLEND_ENABLE] & 1;
    alpha_test = ge_state.command[GE_ALPHA_TEST_ENABLE] & 1;
    alpha_command = ge_state.command[GE_ALPHATEST];
    direct_color =
        (ge_state.command[GE_TEXFUNC] & 7) == 3 ||
        ((ge_state.command[GE_TEXFUNC] & 7) == 0 && second.color == 0xffffffff);
    use_alpha = ge_state.command[GE_TEXFUNC] & 0x100;
    du = (int)(
        (second.u - first.u) * 65536.0f /
        (second.x - first.x)
    );
    dv = (int)(
        (second.v - first.v) * 65536.0f /
        (second.y - first.y)
    );
    u_start =
        (int)(first.u * 65536.0f) +
        (int)((left - first.x) * du);
    v =
        (int)(first.v * 65536.0f) +
        (int)((top - first.y) * dv);
    row_bytes = buffer_width * 4;

    if (direct_color && !blend && !alpha_test && !swizzled && du == 65536) {
        int texture_x = u_start >> 16;
        int span = right - left;

        if (texture_x >= 0 && texture_x + span <= buffer_width) {
            for (y = top; y < bottom; y++) {
                int texture_y = v >> 16;
                const u32 *source;
                u32 *row = (u32 *)frame + y * frame_width + left;

                if (clamp_y) {
                    if (texture_y < 0)
                        texture_y = 0;
                    if (texture_y >= texture_height)
                        texture_y = texture_height - 1;
                } else {
                    texture_y = (u32)texture_y & (texture_height - 1);
                }

                source = (const u32 *)(texture + texture_y * row_bytes) + texture_x;

                if (use_alpha) {
                    memcpy(row, source, span * sizeof(*row));
                } else {
                    u32 alpha = second.color & 0xff000000;

                    for (x = 0; x < span; x++)
                        row[x] = (source[x] & 0x00ffffff) | alpha;
                }

                v += dv;
            }

            return 1;
        }
    }

    for (y = top; y < bottom; y++) {
        int texture_y = v >> 16;
        int u = u_start;
        u32 *row = (u32 *)frame + y * frame_width + left;

        if (clamp_y) {
            if (texture_y < 0)
                texture_y = 0;
            if (texture_y >= texture_height)
                texture_y = texture_height - 1;
        } else {
            texture_y = (u32)texture_y & (texture_height - 1);
        }

        for (x = left; x < right; x++) {
            int texture_x = u >> 16;
            int offset;
            u32 color;

            if (clamp_x) {
                if (texture_x < 0)
                    texture_x = 0;
                if (texture_x >= texture_width)
                    texture_x = texture_width - 1;
            } else {
                texture_x = (u32)texture_x & (texture_width - 1);
            }

            offset = pixel_offset(texture_x, texture_y, row_bytes, 4, swizzled);
            color = *(const u32 *)(texture + offset);

            if (direct_color) {
                if (!use_alpha)
                    color = (color & 0x00ffffff) | (second.color & 0xff000000);
            } else {
                color = shade_pixel(color, second.color);
            }

            if (alpha_test && !compare_alpha(
                alpha_command & 7,
                (color >> 24) & ((alpha_command >> 16) & 0xff),
                (alpha_command >> 8) & ((alpha_command >> 16) & 0xff)
            )) {
                row++;
                u += du;
                continue;
            }

            if (blend)
                *row = blend_color(color, *row);
            else
                *row = color;

            row++;
            u += du;
        }

        v += dv;
    }

    return 1;
}

static void draw_sprite(GeVertex first, GeVertex second)
{
    if (first.x <= -99999.0f || first.y <= -99999.0f ||
        second.x <= -99999.0f || second.y <= -99999.0f)
        return;

    int left = (int)(first.x < second.x ? first.x : second.x);
    int top = (int)(first.y < second.y ? first.y : second.y);
    int right = (int)(first.x > second.x ? first.x : second.x);
    int bottom = (int)(first.y > second.y ? first.y : second.y);

    int draw_left = left;
    int draw_top = top;
    int draw_right = right;
    int draw_bottom = bottom;
    void *frame;
    int stride;
    if (fast_sprite_target(&draw_left, &draw_top, &draw_right, &draw_bottom, &frame, &stride) &&
        ge_hardware_sprite(first, second, draw_left, draw_top, draw_right, draw_bottom))
        return;

    ge_hardware_cpu_write();
    clear_depth_sprite(second, left, top, right, bottom);
    int u_start;
    int v_start;
    int texture_on;
    int du;
    int dv;
    int u;
    int v;
    int x;
    int y;
    u32 color;

    if (right <= left || bottom <= top)
        return;

    if (draw_solid_sprite(second, left, top, right, bottom))
        return;

    if (draw_rgba5551_sprite(first, second, left, top, right, bottom))
        return;

    if (draw_clut4_sprite(first, second, left, top, right, bottom))
        return;

    if (draw_rgba8888_sprite(first, second, left, top, right, bottom))
        return;

    texture_on = (ge_state.command[GE_TEXTURE_ENABLE] & 1) && !(ge_state.command[GE_CLEARMODE] & 1);

    du = (int) (
        (second.u - first.u) * 65536.0f /
        (second.x - first.x)
    );

    dv = (int) (
        (second.v - first.v) * 65536.0f /
        (second.y - first.y)
    );

    u_start = 
        (int) (first.u * 65536.0f) +
        (int) ((left - first.x) * du);

    v_start = 
        (int) (first.v * 65536.0f) +
        (int) ((top - first.y) * dv);

    v = v_start;

    for (y = top; y < bottom; y++) {
        u = u_start;

        for (x = left; x < right; x++) {
            color = second.color;

            if (texture_on) {
                color = shade_pixel(texture_pixel(u >> 16, v >> 16), color);
            }


        draw_pixel(x, y, color);
        u += du;
            
        }

        v += dv;

    
    }




}

static float edge(GeVertex a, GeVertex b, float x, float y)
{
    return (x - a.x) * (b.y - a.y) - (y - a.y) * (b.x - a.x);
}

static inline int triangle_palette_pixel(
    const TrianglePalette *fast,
    int x,
    int y,
    u32 *color
)
{
    int offset;
    u8 packed;
    int index;

    if (fast->clamp_x) {
        if (x < 0)
            x = 0;
        if (x >= fast->texture_width)
            x = fast->texture_width - 1;
    } else {
        x = (u32)x & (fast->texture_width - 1);
    }

    if (fast->clamp_y) {
        if (y < 0)
            y = 0;
        if (y >= fast->texture_height)
            y = fast->texture_height - 1;
    } else {
        y = (u32)y & (fast->texture_height - 1);
    }

    if (fast->format == 4) {
        if (fast->swizzled)
            offset = pixel_offset(x >> 1, y, fast->row_bytes, 1, 1);
        else
            offset = y * fast->row_bytes + (x >> 1);

        packed = fast->texture[offset];
        index = (x & 1) ? packed >> 4 : packed & 0x0f;
    } else {
        offset = pixel_offset(x, y, fast->row_bytes, 1, fast->swizzled);
        index = fast->texture[offset];
    }

    if (!fast->visible[index])
        return 0;

    *color = fast->colors[index];
    return 1;
}

typedef struct {
    u32 *frame;
    u16 *depth;
    int width;
    int depth_width;
    int alpha_test;
    int alpha_function;
    int alpha_reference;
    int alpha_mask;
    int depth_test;
    int depth_function;
    int depth_write;
    int clear;
    int blend;
    int blend_source_alpha;
    int blend_fixed_destination;
} TriangleTarget;

typedef struct {
    const u8 *texture;
    int width;
    int height;
    int row_bytes;
    int clamp_x;
    int clamp_y;
    int swizzled;
} TriangleRgbaTexture;

static inline __attribute__((always_inline)) u32 blend_dxt_packed(
    u32 source, u32 destination);

static int triangle_rgba_texture_setup(TriangleRgbaTexture *texture)
{
    int buffer_width = texture_buffer_width(3);

    if ((ge_state.command[GE_TEXFORMAT] & 0xf) != 3)
        return 0;

    texture->width = 1 << (ge_state.command[GE_TEXSIZE0] & 0xf);
    texture->height = 1 << ((ge_state.command[GE_TEXSIZE0] >> 8) & 0xf);
    texture->row_bytes = buffer_width * 4;
    texture->clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    texture->clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    texture->swizzled = ge_state.command[GE_TEXMODE] & 1;
    texture->texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));
    return texture->texture != NULL;
}

static inline __attribute__((always_inline)) u32 triangle_rgba_texture_pixel(
    const TriangleRgbaTexture *texture, int x, int y)
{
    int offset;

    if (texture->clamp_x) {
        if (x < 0)
            x = 0;
        if (x >= texture->width)
            x = texture->width - 1;
    } else {
        x = (u32)x & (texture->width - 1);
    }

    if (texture->clamp_y) {
        if (y < 0)
            y = 0;
        if (y >= texture->height)
            y = texture->height - 1;
    } else {
        y = (u32)y & (texture->height - 1);
    }

    offset = pixel_offset(x, y, texture->row_bytes, 4, texture->swizzled);
    return *(const u32 *)(texture->texture + offset);
}

static inline __attribute__((always_inline)) u32 raster_div255(u32 value)
{
    return (value + 1u + (value >> 8)) >> 8;
}

static inline __attribute__((always_inline)) u32 shade_pixel_cached(
    u32 texture, u32 vertex, int function, int use_alpha, u32 environment)
{
    u32 texture_alpha = texture >> 24;
    u32 vertex_alpha = vertex >> 24;
    u32 output_alpha = vertex_alpha;
    u32 result = 0;
    int channel;

    if (function == 3)
        output_alpha = use_alpha ? texture_alpha : vertex_alpha;
    else if (function != 1 && use_alpha)
        output_alpha = raster_div255(texture_alpha * vertex_alpha);

    for (channel = 0; channel < 3; channel++) {
        u32 shift = channel * 8;
        u32 t = (texture >> shift) & 0xff;
        u32 v = (vertex >> shift) & 0xff;
        u32 e = (environment >> shift) & 0xff;
        u32 color;

        if (function == 1)
            color = use_alpha ?
                raster_div255(v * (255 - texture_alpha) + t * texture_alpha) : t;
        else if (function == 2)
            color = raster_div255(v * (255 - t) + e * t);
        else if (function == 3)
            color = t;
        else if (function == 4) {
            color = v + t;
            if (color > 255)
                color = 255;
        } else {
            color = raster_div255(v * t);
        }
        result |= color << shift;
    }

    return result | (output_alpha << 24);
}

static int triangle_indexed_texture_layout(TrianglePalette *texture)
{
    int buffer_width;

    texture->format = ge_state.command[GE_TEXFORMAT] & 0xf;
    if (texture->format != 4 && texture->format != 5)
        return 0;

    texture->texture_width = 1 << (ge_state.command[GE_TEXSIZE0] & 0xf);
    texture->texture_height = 1 << ((ge_state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = texture_buffer_width(texture->format);

    texture->texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));
    if (!texture->texture)
        return 0;

    texture->row_bytes = texture->format == 4 ?
        (buffer_width + 1) >> 1 : buffer_width;
    texture->swizzled = ge_state.command[GE_TEXMODE] & 1;
    texture->clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    texture->clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    return 1;
}

static int triangle_indexed_texture_setup(TrianglePalette *texture)
{
    if (!triangle_indexed_texture_layout(texture))
        return 0;

    int entries = texture->format == 4 ? 16 : 256;
    for (int index = 0; index < entries; index++)
        texture->colors[index] = palette_color(index);

    return 1;
}

static inline __attribute__((always_inline)) u32 triangle_indexed_texture_pixel(
    const TrianglePalette *texture, int x, int y)
{
    int offset;
    u32 index;

    if (texture->clamp_x) {
        if (x < 0)
            x = 0;
        if (x >= texture->texture_width)
            x = texture->texture_width - 1;
    } else {
        x = (u32)x & (texture->texture_width - 1);
    }

    if (texture->clamp_y) {
        if (y < 0)
            y = 0;
        if (y >= texture->texture_height)
            y = texture->texture_height - 1;
    } else {
        y = (u32)y & (texture->texture_height - 1);
    }

    if (texture->format == 4) {
        u8 packed;

        if (texture->swizzled)
            offset = pixel_offset(x >> 1, y, texture->row_bytes, 1, 1);
        else
            offset = y * texture->row_bytes + (x >> 1);
        packed = texture->texture[offset];
        index = (x & 1) ? packed >> 4 : packed & 0x0f;
    } else {
        offset = pixel_offset(x, y, texture->row_bytes, 1, texture->swizzled);
        index = texture->texture[offset];
    }

    return texture->colors[index];
}

static inline __attribute__((always_inline)) int raster_compare(
    int function, int value, int reference)
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

static inline __attribute__((always_inline)) u16 triangle_depth_value(float depth)
{
    if (depth < 0.0f)
        depth = 0.0f;
    if (depth > 65535.0f)
        depth = 65535.0f;
    return (u16)(u32)depth;
}

static inline __attribute__((always_inline)) u32 blend_fixed_destination(
    u32 source, u32 destination, int subtract)
{
    unsigned alpha = source >> 24;
    u32 rb = (source & 0x00ff00ffu) * alpha;
    u32 green = ((source >> 8) & 255u) * alpha;

    if (subtract) {
        rb += 0x00fe00feu;
        green += 254;
    }
    rb = ((rb + 0x00010001u + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    u32 contribution = rb | (raster_div255(green) << 8);
    u32 result;
    if (subtract)
        __asm__("psubub %0, %1, %2" : "=r"(result) : "r"(destination), "r"(contribution));
    else
        __asm__("paddub %0, %1, %2" : "=r"(result) : "r"(destination), "r"(contribution));
    return (result & 0x00ffffffu) | (source & 0xff000000u);
}

static inline __attribute__((always_inline)) void triangle_target_store_5551(
    const TriangleTarget *target, u16 *frame, u16 *depth, u32 color, u16 z)
{
    if (target->alpha_test && !raster_compare(
        target->alpha_function, (color >> 24) & target->alpha_mask,
        target->alpha_reference))
        return;

    if (target->clear & 1) {
        u16 mask = ((target->clear & 0x100) ? 0x7fff : 0) |
            ((target->clear & 0x200) ? 0x8000 : 0);
        *frame = (pack_color_5551(color) & mask) | (*frame & ~mask);
    } else if (!target->blend) {
        *frame = pack_color_5551(color);
    } else if (target->blend_source_alpha) {
        unsigned alpha = color >> 24;
        if (alpha == 255)
            *frame = pack_color_5551(color);
        else if (alpha)
            *frame = pack_color_5551(blend_dxt_packed(color, unpack_color_5551(*frame)));
    } else {
        u32 destination = unpack_color_5551(*frame);
        if (target->blend_fixed_destination)
            color = blend_fixed_destination(color, destination, target->blend_fixed_destination < 0);
        else
            color = blend_color(color, destination);
        *frame = pack_color_5551(color);
    }

    if (depth && target->depth_write)
        *depth = z;
}

static inline __attribute__((always_inline)) void triangle_target_store(
    const TriangleTarget *target, u32 *frame, u16 *depth_buffer,
    u32 color, float depth)
{
    u16 value = 0;

    if (target->alpha_test && !raster_compare(
        target->alpha_function, (color >> 24) & target->alpha_mask,
        target->alpha_reference))
        return;

    if (depth_buffer) {
        value = triangle_depth_value(depth);
        if (target->depth_test && !raster_compare(
            target->depth_function, value, *depth_buffer))
            return;
    }

    if (target->clear & 1) {
        u32 destination = *frame;
        if (!(target->clear & 0x100))
            color = (color & 0xff000000u) | (destination & 0x00ffffffu);
        if (!(target->clear & 0x200))
            color = (color & 0x00ffffffu) | (destination & 0xff000000u);
    } else if (target->blend) {
        if (target->blend_source_alpha) {
            if ((color >> 24) == 0) {
                if (depth_buffer && target->depth_write)
                    *depth_buffer = value;
                return;
            }
            if ((color >> 24) != 255)
                color = blend_dxt_packed(color, *frame);
        } else {
            color = blend_color(color, *frame);
        }
    }

    *frame = color;
    if (depth_buffer && target->depth_write)
        *depth_buffer = value;
}

static int triangle_target_setup(TriangleTarget *target, int format)
{
    u32 alpha_command = ge_state.command[GE_ALPHATEST];
    u32 blend_mode = ge_state.command[GE_BLENDMODE] & 0x7ff;
    target->width = ge_state.command[GE_VADDR_WIDTH] & 0x7fc;
    if ((ge_state.command[GE_FRAMEBUF_FORMAT] & 3) != format ||
        target->width < 64 || target->width > 2048)
        return 0;
    target->frame = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));
    if (!target->frame)
        return 0;
    target->clear = ge_state.command[GE_CLEARMODE];
    target->blend = ge_state.command[GE_BLEND_ENABLE] & 1;
    target->blend_source_alpha = !ge_blend_mode_seen || blend_mode == 0x32;
    if (ge_blend_mode_seen && (ge_state.command[GE_BLENDFIXEDB] & 0xffffff) == 0xffffff)
        target->blend_fixed_destination = blend_mode == 0xa2 ? 1 : blend_mode == 0x2a2 ? -1 : 0;
    target->alpha_test = (ge_state.command[GE_ALPHA_TEST_ENABLE] & 1) && !(target->clear & 1);
    target->alpha_function = alpha_command & 7;
    target->alpha_mask = (alpha_command >> 16) & 0xff;
    target->alpha_reference = (alpha_command >> 8) & target->alpha_mask;
    target->depth_width = ge_state.command[GE_DEPTHBUF_WIDTH] & 0x7fc;
    target->depth = ge_pointer(buffer_address(GE_DEPTHBUF_BASE, GE_DEPTHBUF_WIDTH));
    if (target->depth_width < 64 || target->depth_width > 2048)
        target->depth = NULL;
    target->depth_test = (ge_state.command[GE_DEPTH_TEST_ENABLE] & 1) && !(target->clear & 1);
    target->depth_function = ge_state.command[GE_DEPTHTEST] & 7;
    target->depth_write = (target->clear & 0x400) ||
        (!(target->clear & 1) && !(ge_state.command[GE_DEPTH_WRITE_DISABLE] & 1));
    return 1;
}

static inline __attribute__((always_inline)) void triangle_target_pixel(
    const TriangleTarget *target, int x, int y, u32 color, float depth)
{
    int index = y * target->width + x;
    int depth_index = y * target->depth_width + x;

    triangle_target_store(
        target, target->frame + index,
        target->depth ? target->depth + depth_index : NULL,
        color, depth
    );
}

static inline void triangle_scan_span(
    GeVertex a, GeVertex b, GeVertex c, int y,
    int left, int right, int *start, int *end)
{
    float py = (float)y + 0.5f;
    float minx = 1.0e30f;
    float maxx = -1.0e30f;
    float yy[3] = {a.y, b.y, c.y};
    float xx[3] = {a.x, b.x, c.x};
    int i;

    for (i = 0; i < 3; i++) {
        int next = (i + 1) % 3;
        float y0 = yy[i];
        float y1 = yy[next];
        if ((y0 <= py && py <= y1) || (y1 <= py && py <= y0)) {
            if (y0 != y1) {
                float at = xx[i] + (py - y0) * (xx[next] - xx[i]) / (y1 - y0);
                if (at < minx) minx = at;
                if (at > maxx) maxx = at;
            }
        }
    }
    if (maxx < minx) {
        *start = right + 1;
        *end = right;
        return;
    }
    if (minx > left + 2.0f) {
        int n = (int)minx - 2;
        if (n > left) left = n;
    }
    if (maxx < right - 2.0f) {
        int n = (int)maxx + 2;
        if (n < right) right = n;
    }
    *start = left;
    *end = right;
}

static inline __attribute__((always_inline)) u32 blend_dxt_packed(u32 source, u32 destination)
{
    u32 alpha = source >> 24;
    u32 inverse = 255u - alpha;
    u32 rb = (source & 0x00ff00ffu) * alpha +
        (destination & 0x00ff00ffu) * inverse;
    u32 green = ((source >> 8) & 255u) * alpha +
        ((destination >> 8) & 255u) * inverse;
    u32 output_alpha = alpha +
        (((destination >> 24) * inverse + 1u +
        (((destination >> 24) * inverse) >> 8)) >> 8);
    rb = ((rb + 0x00010001u + ((rb >> 8) & 0x00ff00ffu)) >> 8) & 0x00ff00ffu;
    green = (green + 1u + (green >> 8)) >> 8;
    return rb | (green << 8) | (output_alpha << 24);
}

typedef struct {
    u32 revision;
    u32 clut_format;
    u32 color;
    u32 texfunc;
    u32 environment;
    u32 alpha_test;
    int blend;
} TrianglePaletteKey;

static const u32 *triangle_5551_span_colors(
    TrianglePalette *texture, const TriangleTarget *target,
    int shade_palette, u32 color, u32 texfunc, u32 environment)
{
    static struct {
        TrianglePaletteKey key;
        u32 colors[256];
    } cache[4];
    static unsigned int next;
    TrianglePaletteKey key = {
        .revision = palette_revision(),
        .clut_format = ge_state.command[GE_CLUTFORMAT],
        .color = shade_palette ? color : 0,
        .texfunc = shade_palette ? (texfunc | 0x80000000u) : texfunc & 0x100,
        .environment = shade_palette && (texfunc & 7) == 2 ?
            environment & 0xffffff : 0,
        .alpha_test = target->alpha_test ?
            0x80000000u | ge_state.command[GE_ALPHATEST] : 0,
        .blend = target->blend
    };
    u32 *colors = texture->colors;
    int index;

    if (key.revision) {
        for (index = 0; index < 4; index++)
            if (memcmp(&cache[index].key, &key, sizeof(key)) == 0)
                return cache[index].colors;

        index = next++ & 3;
        cache[index].key = key;
        colors = cache[index].colors;
    }

    for (index = 0; index < 256; index++) {
        u32 texel = palette_color(index);
        if (shade_palette)
            texel = shade_pixel_cached(texel, color, texfunc & 7, texfunc & 0x100, environment);
        else if (!(texfunc & 0x100))
            texel |= 0xff000000u;
        u32 alpha = texel >> 24;
        if ((target->alpha_test && !raster_compare(target->alpha_function,
            alpha & target->alpha_mask, target->alpha_reference)) ||
            (target->blend && !alpha))
            texel = 0;
        else if (!target->blend || alpha == 255)
            texel = 0xff000000u | pack_color_5551(texel);
        colors[index] = texel;
    }
    return colors;
}

static inline u64 repeat_pixel_5551(u16 pixel)
{
    return pixel * 0x0001000100010001ULL;
}

static void fill_span_5551(u16 *span, int count, u16 pixel)
{
    while (count && ((uintptr_t)span & 15)) {
        *span++ = pixel;
        count--;
    }
#ifdef _EE
    u64 repeated = repeat_pixel_5551(pixel);
    __asm__ volatile (
        "pcpyld %0, %0, %0" : "+r"(repeated)
    );
    while (count >= 8) {
        __asm__ volatile ("sq %1, 0(%0)" :: "r"(span), "r"(repeated) : "memory");
        span += 8;
        count -= 8;
    }
#endif
    while (count--)
        *span++ = pixel;
}

static int raster_ranges_overlap(const void *a, size_t a_size, const void *b, size_t b_size)
{
    uintptr_t start_a = (uintptr_t)a;
    uintptr_t start_b = (uintptr_t)b;

    return start_a <= start_b ? start_b - start_a < a_size : start_a - start_b < b_size;
}

static void draw_indexed_span_5551(
    u16 *frame, u16 *depth, u16 z, int count, const u8 *indices,
    const u32 *colors, const u32 *uniform_colors, u64 uniform_pixels)
{
    if (uniform_colors && !(((uintptr_t)frame | (uintptr_t)depth) & 7)) {
        const u64 repeated_depth = repeat_pixel_5551(z);

        while (count >= 4) {
            u64 previous;
            memcpy(&previous, frame, sizeof(previous));
            if (previous != uniform_pixels)
                break;

            u32 a = uniform_colors[indices[0]];
            u32 b = uniform_colors[indices[1]];
            u32 c = uniform_colors[indices[2]];
            u32 d = uniform_colors[indices[3]];
            u64 pixels = (u16)a | ((u64)(u16)b << 16) |
                ((u64)(u16)c << 32) | ((u64)(u16)d << 48);
            memcpy(frame, &pixels, sizeof(pixels));
            if (depth) {
                if (a & b & c & d & 0x10000) {
                    memcpy(depth, &repeated_depth, sizeof(repeated_depth));
                } else {
                    if (a & 0x10000)
                        depth[0] = z;
                    if (b & 0x10000)
                        depth[1] = z;
                    if (c & 0x10000)
                        depth[2] = z;
                    if (d & 0x10000)
                        depth[3] = z;
                }
                depth += 4;
            }
            frame += 4;
            indices += 4;
            count -= 4;
        }
    }

    for (int i = 0; i < count; i++) {
        u32 color = colors[indices[i]];
        if (color) {
            frame[i] = color >> 24 == 255 ? (u16)color :
                pack_color_5551(blend_dxt_packed(color, unpack_color_5551(frame[i])));
            if (depth)
                depth[i] = z;
        }
    }
}

static int draw_right_triangle_5551(
    const TriangleTarget *target, GeVertex a, GeVertex b, GeVertex c,
    int left, int top, int right, int bottom, int textured)
{
    GeVertex vertices[] = {a, b, c};
    GeVertex corner, horizontal, vertical;
    TrianglePalette texture;
    const u32 *colors = NULL;
    u32 uniform_colors[256];
    u16 *frame = (u16 *)target->frame;
    u16 *depth = target->depth_write ? target->depth : NULL;
    size_t frame_size = (bottom * target->width + right + 1) * sizeof(u16);
    size_t depth_size = (bottom * target->depth_width + right + 1) * sizeof(u16);
    int corner_index;
    int cx, cy, dx, dy, width, height, u_offset = 0, v_offset = 0, v_step = 0;
    int uniform = -1;

    if ((target->clear & 1) ||
        (target->blend && !target->blend_source_alpha) ||
        (target->depth && target->depth_test && target->depth_function != 1) ||
        a.color != b.color || b.color != c.color || a.z != b.z || b.z != c.z ||
        (depth && (right >= target->depth_width ||
            raster_ranges_overlap(frame, frame_size, depth, depth_size))))
        return 0;

    for (corner_index = 0; corner_index < 3; corner_index++) {
        corner = vertices[corner_index];
        horizontal = vertices[(corner_index + 1) % 3];
        vertical = vertices[(corner_index + 2) % 3];
        if (corner.x == horizontal.x && corner.y == vertical.y) {
            GeVertex swap = horizontal;
            horizontal = vertical;
            vertical = swap;
        }
        if (corner.y == horizontal.y && corner.x == vertical.x)
            break;
    }

    if (corner_index == 3 || !(corner.x >= -2048 && corner.x <= 2048) ||
        !(corner.y >= -2048 && corner.y <= 2048) ||
        !(horizontal.x >= -2048 && horizontal.x <= 2048) ||
        !(vertical.y >= -2048 && vertical.y <= 2048))
        return 0;

    cx = (int)corner.x;
    cy = (int)corner.y;
    dx = (int)horizontal.x - cx;
    dy = (int)vertical.y - cy;
    width = dx < 0 ? -dx : dx;
    height = dy < 0 ? -dy : dy;
    if (corner.x != cx || corner.y != cy ||
        horizontal.x != cx + dx || vertical.y != cy + dy ||
        !width || !height || width > 1024 || height > 1024 ||
        (width & (width - 1)) || (height & (height - 1)))
        return 0;

    u32 alpha = a.color >> 24;
    u16 packed = pack_color_5551(a.color);
    u16 z = depth ? triangle_depth_value(a.z) : 0;

    if (textured) {
        u32 texfunc = ge_state.command[GE_TEXFUNC];
        int shade = a.color != 0xffffffffu || ((texfunc & 7) != 0 && (texfunc & 7) != 3);
        int any_color = 0;

        if (a.inv_w != 1.0f || b.inv_w != 1.0f || c.inv_w != 1.0f ||
            !(corner.u >= 0 && corner.u <= 2048) || !(corner.v >= 0 && corner.v <= 2048) ||
            horizontal.v != corner.v || vertical.u != corner.u ||
            horizontal.u - corner.u != dx ||
            (vertical.v - corner.v != dy && vertical.v - corner.v != -dy) ||
            corner.u != (int)corner.u || corner.v != (int)corner.v ||
            (target->blend && (!target->alpha_test ||
                raster_compare(target->alpha_function, 0, target->alpha_reference))) ||
            !triangle_indexed_texture_layout(&texture) || texture.format != 5 ||
            texture.clamp_x || texture.clamp_y || texture.texture_width > texture.row_bytes ||
            texture.texture_height > 512 || texture.texture_width > 512 ||
            (texture.swizzled && (texture.row_bytes & 15)))
            return 0;

        u_offset = (int)corner.u - cx;
        v_step = vertical.v - corner.v == dy ? 1 : -1;
        v_offset = (int)corner.v - v_step * cy - (v_step < 0);
        if (left + u_offset < 0 || top * v_step + v_offset < 0 ||
            bottom * v_step + v_offset < 0)
            return 0;

        size_t source_size = texture.row_bytes *
            (texture.swizzled ? (texture.texture_height + 7) & ~7 : texture.texture_height);
        if (raster_ranges_overlap(frame, frame_size, texture.texture, source_size) ||
            (depth && raster_ranges_overlap(depth, depth_size, texture.texture, source_size)))
            return 0;

        colors = triangle_5551_span_colors(&texture, target, shade, a.color, texfunc,
            ge_state.command[GE_TEXENVCOLOR]);
        for (int i = 0; i < 256; i++)
            any_color |= colors[i] != 0;
        if (!any_color)
            return 1;
    } else {
        if (target->blend && alpha != 255 && alpha != 0)
            return 0;
        if (target->alpha_test && !raster_compare(target->alpha_function,
            alpha & target->alpha_mask, target->alpha_reference))
            return 1;
    }

    for (int y = top; y <= bottom; y++) {
        int y_from_corner = dy > 0 ? y - cy : cy - 1 - y;
        if (y_from_corner < 0 || y_from_corner >= height)
            continue;
        int span = (width * (2 * (height - y_from_corner) - 1) + height) / (2 * height);
        int start = dx > 0 ? cx : cx - span;
        int end = dx > 0 ? cx + span : cx;
        if (start < left)
            start = left;
        if (end > right + 1)
            end = right + 1;
        if (start >= end)
            continue;

        u16 *out = frame + y * target->width;
        u16 *z_out = depth ? depth + y * target->depth_width : NULL;
        if (!colors) {
            if (!target->blend || alpha)
                fill_span_5551(out + start, end - start, packed);
            if (z_out)
                fill_span_5551(z_out + start, end - start, z);
            continue;
        }

        int ty = (y * v_step + v_offset) & (texture.texture_height - 1);
        if (uniform < 0 && end - start >= 8) {
            uniform = out[(start + end) / 2];
            u32 destination = unpack_color_5551(uniform);
            for (int i = 0; i < 256; i++) {
                u32 color = colors[i];
                uniform_colors[i] = !color ? uniform : 0x10000 |
                    (color >> 24 == 255 ? (u16)color :
                    pack_color_5551(blend_dxt_packed(color, destination)));
            }
        }

        while (start < end) {
            int tx = (start + u_offset) & (texture.texture_width - 1);
            int count = texture.texture_width - tx;
            if (texture.swizzled && count > 16 - (tx & 15))
                count = 16 - (tx & 15);
            if (count > end - start)
                count = end - start;
            int offset = pixel_offset(tx, ty, texture.row_bytes, 1, texture.swizzled);
            draw_indexed_span_5551(out + start, z_out ? z_out + start : NULL, z, count,
                texture.texture + offset, colors, uniform >= 0 ? uniform_colors : NULL,
                repeat_pixel_5551(uniform));
            start += count;
        }
    }
    return 1;
}

static void draw_triangle(GeVertex a, GeVertex b, GeVertex c)
{
    TrianglePalette fast;
    GeDxtSampler dxt;
    TriangleRgbaTexture rgba_texture;
    TriangleTarget target = {0};
    int fast_target;
    int fast_5551;
    int fast_dxt;
    int affine;
    int direct_white;
    int fast_rgba;
    int fast_indexed;
    int direct_target;
    int texfunc;
    int tex_use_alpha;
    u32 tex_environment;

    if (a.x <= -99999.0f || a.y <= -99999.0f ||
        b.x <= -99999.0f || b.y <= -99999.0f ||
        c.x <= -99999.0f || c.y <= -99999.0f)
        return;

    float area = edge(a, b, c.x, c.y);
    float inverse_area;
    float wa_row;
    float wb_row;
    float wa_dx;
    float wb_dx;
    float wa_dy;
    float wb_dy;
    float red_row;
    float green_row;
    float blue_row;
    float alpha_row;
    float u_row;
    float v_row;
    float q_row;
    float z_row;
    float red_dx;
    float green_dx;
    float blue_dx;
    float alpha_dx;
    float u_dx;
    float v_dx;
    float q_dx;
    float z_dx;
    float red_dy;
    float green_dy;
    float blue_dy;
    float alpha_dy;
    float u_dy;
    float v_dy;
    float q_dy;
    float z_dy;

    float ar = a.color & 0xff;
    float ag = (a.color >> 8) & 0xff;
    float ab = (a.color >> 16) & 0xff;
    float aa = a.color >> 24;

    float br = b.color & 0xff;
    float bg = (b.color >> 8) & 0xff;
    float bb = (b.color >> 16) & 0xff;
    float ba = b.color >> 24;

    float cr = c.color & 0xff;
    float cg = (c.color >> 8) & 0xff;
    float cb = (c.color >> 16) & 0xff;
    float ca = c.color >> 24;

    int left = (int)a.x;
    int top = (int)a.y;
    int right = (int)a.x;
    int bottom = (int)a.y;

    int frame_width = ge_state.command[GE_VADDR_WIDTH] & 0x7fc;
    int scissor_left = ge_state.command[GE_SCISSOR1] & 0x3ff;
    int scissor_top = (ge_state.command[GE_SCISSOR1] >> 10) & 0x3ff;
    int scissor_right = ge_state.command[GE_SCISSOR2] & 0x3ff;
    int scissor_bottom = (ge_state.command[GE_SCISSOR2] >> 10) & 0x3ff;

    int textured =
        (ge_state.command[GE_TEXTURE_ENABLE] & 1) &&
        !(ge_state.command[GE_CLEARMODE] & 1);

    int fast_palette;
    int x;
    int y;

    if (area == 0.0f)
        return;

    if (b.x < left)
        left = (int)b.x;
    if (c.x < left)
        left = (int)c.x;
    if (b.y < top)
        top = (int)b.y;
    if (c.y < top)
        top = (int)c.y;

    if (b.x > right)
        right = (int)b.x;
    if (c.x > right)
        right = (int)c.x;
    if (b.y > bottom)
        bottom = (int)b.y;
    if (c.y > bottom)
        bottom = (int)c.y;

    if (!ge_state.command[GE_SCISSOR2]) {
        scissor_right = 479;
        scissor_bottom = 271;
    }

    if (left < scissor_left)
        left = scissor_left;
    if (top < scissor_top)
        top = scissor_top;
    if (right > scissor_right)
        right = scissor_right;
    if (bottom > scissor_bottom)
        bottom = scissor_bottom;

    if (left < 0)
        left = 0;
    if (top < 0)
        top = 0;
    if (right >= frame_width)
        right = frame_width - 1;
    if (bottom >= 512)
        bottom = 511;

    if (right < left || bottom < top)
        return;

    if (ge_hardware_triangle(a, b, c, left, top, right + 1, bottom + 1))
        return;
    ge_hardware_cpu_write();

    fast_5551 = triangle_target_setup(&target, 1);
    if (fast_5551 && (ge_state.command[GE_VERTEXTYPE] & (1 << 23)) &&
        draw_right_triangle_5551(&target, a, b, c, left, top, right, bottom, textured))
        return;
    fast_target = !fast_5551 && triangle_target_setup(&target, 3);
    affine = a.inv_w == 1.0f && b.inv_w == 1.0f && c.inv_w == 1.0f;
    texfunc = ge_state.command[GE_TEXFUNC] & 7;
    tex_use_alpha = ge_state.command[GE_TEXFUNC] & 0x100;
    tex_environment = ge_state.command[GE_TEXENVCOLOR];
    direct_white = textured &&
        a.color == 0xffffffffu &&
        b.color == 0xffffffffu &&
        c.color == 0xffffffffu &&
        (texfunc == 0 || texfunc == 3);

    fast_dxt = textured &&
        ((ge_state.command[GE_TEXFORMAT] & 15) == 8 ||
         (ge_state.command[GE_TEXFORMAT] & 15) == 9) &&
        ge_dxt_setup(&dxt);

    fast_rgba = textured && triangle_rgba_texture_setup(&rgba_texture);
    direct_target = fast_target && !target.alpha_test && !target.depth_test &&
        !(target.depth && target.depth_write) && !(target.clear & 1) &&
        (!target.blend || target.blend_source_alpha);

    fast_palette = textured && a.color == 0xffffffffu &&
        b.color == 0xffffffffu && c.color == 0xffffffffu &&
        !(ge_state.command[GE_DEPTH_TEST_ENABLE] & 1) &&
        !(ge_state.command[GE_CLEARMODE] & 0x400) &&
        prepare_triangle_palette(&fast);

    inverse_area = 1.0f / area;

    wa_row = edge(b, c, left + 0.5f, top + 0.5f) * inverse_area;
    wb_row = edge(c, a, left + 0.5f, top + 0.5f) * inverse_area;

    wa_dx = (c.y - b.y) * inverse_area;
    wb_dx = (a.y - c.y) * inverse_area;
    wa_dy = (b.x - c.x) * inverse_area;
    wb_dy = (c.x - a.x) * inverse_area;

    u_row = c.u * c.inv_w +
        wa_row * (a.u * a.inv_w - c.u * c.inv_w) +
        wb_row * (b.u * b.inv_w - c.u * c.inv_w);
    v_row = c.v * c.inv_w +
        wa_row * (a.v * a.inv_w - c.v * c.inv_w) +
        wb_row * (b.v * b.inv_w - c.v * c.inv_w);
    q_row = c.inv_w + wa_row * (a.inv_w - c.inv_w) +
        wb_row * (b.inv_w - c.inv_w);
    z_row = c.z + wa_row * (a.z - c.z) + wb_row * (b.z - c.z);

    if (fast_palette && a.inv_w == b.inv_w && b.inv_w == c.inv_w) {
        u_row /= a.inv_w;
        v_row /= a.inv_w;
        u_dx = wa_dx * (a.u - c.u) + wb_dx * (b.u - c.u);
        v_dx = wa_dx * (a.v - c.v) + wb_dx * (b.v - c.v);
        u_dy = wa_dy * (a.u - c.u) + wb_dy * (b.u - c.u);
        v_dy = wa_dy * (a.v - c.v) + wb_dy * (b.v - c.v);

        for (y = top; y <= bottom; y++) {
            int xstart;
            int xend;
            float wa;
            float wb;
            float u;
            float v;
            u32 *row;
            float skip;

            triangle_scan_span(a, b, c, y, left, right, &xstart, &xend);
            skip = (float)(xstart - left);
            wa = wa_row + skip * wa_dx;
            wb = wb_row + skip * wb_dx;
            u = u_row + skip * u_dx;
            v = v_row + skip * v_dx;
            row = fast.frame + y * fast.frame_width + xstart;

            for (x = xstart; x <= xend; x++) {
                float wc = 1.0f - wa - wb;

                if (wa >= 0.0f && wb >= 0.0f && wc >= 0.0f) {
                    u32 color;

                    if (triangle_palette_pixel(&fast, (int)u, (int)v, &color)) {
                        u32 *destination = &row[x - xstart];
                        if (fast.blend)
                            *destination = blend_color(color, *destination);
                        else
                            *destination = color;
                    }
                }

                wa += wa_dx;
                wb += wb_dx;
                u += u_dx;
                v += v_dx;
            }

            wa_row += wa_dy;
            wb_row += wb_dy;
            u_row += u_dy;
            v_row += v_dy;
        }

        return;
    }

    if (direct_white && fast_dxt && fast_target &&
        a.inv_w == b.inv_w && b.inv_w == c.inv_w && a.inv_w != 0.0f) {
        float inv_q = 1.0f / a.inv_w;
        float tex_u_row = u_row * inv_q;
        float tex_v_row = v_row * inv_q;
        float tex_u_dx = wa_dx * (a.u - c.u) + wb_dx * (b.u - c.u);
        float tex_v_dx = wa_dx * (a.v - c.v) + wb_dx * (b.v - c.v);
        float tex_u_dy = wa_dy * (a.u - c.u) + wb_dy * (b.u - c.u);
        float tex_v_dy = wa_dy * (a.v - c.v) + wb_dy * (b.v - c.v);
        int tex_alpha = ge_state.command[GE_TEXFUNC] & 0x100;
        int direct_target = !target.alpha_test && !target.depth_test &&
            !(target.depth && target.depth_write) && !(target.clear & 1) &&
            (!target.blend || target.blend_source_alpha);

        z_dx = wa_dx * (a.z - c.z) + wb_dx * (b.z - c.z);
        z_dy = wa_dy * (a.z - c.z) + wb_dy * (b.z - c.z);

        for (y = top; y <= bottom; y++) {
            int xstart, xend;
            float skip;
            float wa, wb, u, v, z;

            triangle_scan_span(a, b, c, y, left, right, &xstart, &xend);
            skip = (float)(xstart - left);
            wa = wa_row + skip * wa_dx;
            wb = wb_row + skip * wb_dx;
            u = tex_u_row + skip * tex_u_dx;
            v = tex_v_row + skip * tex_v_dx;
            z = z_row + skip * z_dx;

            while (xstart <= xend &&
                (wa < 0.0f || wb < 0.0f || 1.0f - wa - wb < 0.0f)) {
                xstart++;
                wa += wa_dx;
                wb += wb_dx;
                u += tex_u_dx;
                v += tex_v_dx;
                z += z_dx;
            }
            if (xstart <= xend) {
                float wa_end = wa + (float)(xend - xstart) * wa_dx;
                float wb_end = wb + (float)(xend - xstart) * wb_dx;
                while (xend >= xstart &&
                    (wa_end < 0.0f || wb_end < 0.0f ||
                     1.0f - wa_end - wb_end < 0.0f)) {
                    xend--;
                    wa_end -= wa_dx;
                    wb_end -= wb_dx;
                }
                if (direct_target && dxt.ready_complete) {
                    u32 *out = target.frame + y * target.width + xstart;
                    int count = xend - xstart + 1;
                    int iu = (int)u;
                    int iv = (int)v;
                    float du_error = (tex_u_dx - 1.0f) * (float)(count - 1);
                    float dv_error = tex_v_dx * (float)(count - 1);
                    float uf = u - (float)iu;
                    float vf = v - (float)iv;
                    int linear = count > 0 && iu >= 0 && iv >= 0 &&
                        iu + count <= dxt.width && iv < dxt.height &&
                        du_error > -0.04f && du_error < 0.04f &&
                        dv_error > -0.04f && dv_error < 0.04f &&
                        uf > 0.08f && uf < 0.92f &&
                        vf > 0.08f && vf < 0.92f;
                    if (linear) {
                        const u32 *pixels = dxt.ready_pixels + iv * dxt.width + iu;
                        if (dxt.ready_opaque || (!target.blend && tex_alpha)) {
                            memcpy(out, pixels, count * sizeof(u32));
                        } else {
                            x = 0;
                            while (x < count) {
                                u32 color = pixels[x];
                                u32 alpha = tex_alpha ? color >> 24 : 255u;
                                if (alpha == 255u) {
                                    int begin = x;
                                    do {
                                        x++;
                                    } while (x < count &&
                                        (!tex_alpha || (pixels[x] >> 24) == 255u));
                                    if (tex_alpha)
                                        memcpy(out + begin, pixels + begin, (x - begin) * sizeof(u32));
                                    else {
                                        int j;
                                        for (j = begin; j < x; j++)
                                            out[j] = pixels[j] | 0xff000000u;
                                    }
                                } else if (alpha == 0u) {
                                    x++;
                                } else {
                                    out[x] = blend_dxt_packed(color, out[x]);
                                    x++;
                                }
                            }
                        }
                    } else {
                        for (x = xstart; x <= xend; x++) {
                            u32 color = ge_dxt_ready_pixel(&dxt, (int)u, (int)v);
                            if (!tex_alpha)
                                color |= 0xff000000u;
                            if (!target.blend || (color >> 24) == 255)
                                *out = color;
                            else if (color >> 24)
                                *out = blend_dxt_packed(color, *out);
                            out++;
                            u += tex_u_dx;
                            v += tex_v_dx;
                        }
                    }
                } else if (direct_target) {
                    u32 *out = target.frame + y * target.width + xstart;
                    for (x = xstart; x <= xend; x++) {
                        u32 color = ge_dxt_cached_pixel(&dxt, (int)u, (int)v);
                        if (!tex_alpha)
                            color |= 0xff000000u;
                        if (!target.blend || (color >> 24) == 255)
                            *out = color;
                        else if (color >> 24)
                            *out = blend_dxt_packed(color, *out);
                        out++;
                        u += tex_u_dx;
                        v += tex_v_dx;
                    }
                } else if (dxt.ready_complete) {
                    for (x = xstart; x <= xend; x++) {
                        u32 color = ge_dxt_ready_pixel(&dxt, (int)u, (int)v);
                        if (!tex_alpha)
                            color |= 0xff000000u;
                        triangle_target_pixel(&target, x, y, color, z);
                        u += tex_u_dx;
                        v += tex_v_dx;
                        z += z_dx;
                    }
                } else {
                    for (x = xstart; x <= xend; x++) {
                        u32 color = ge_dxt_cached_pixel(&dxt, (int)u, (int)v);
                        if (!tex_alpha)
                            color |= 0xff000000u;
                        triangle_target_pixel(&target, x, y, color, z);
                        u += tex_u_dx;
                        v += tex_v_dx;
                        z += z_dx;
                    }
                }
            }
            wa_row += wa_dy;
            wb_row += wb_dy;
            tex_u_row += tex_u_dy;
            tex_v_row += tex_v_dy;
            z_row += z_dy;
        }
        return;
    }

    fast_indexed = textured && triangle_indexed_texture_setup(&fast);

    red_row = cr + wa_row * (ar - cr) + wb_row * (br - cr);
    green_row = cg + wa_row * (ag - cg) + wb_row * (bg - cg);
    blue_row = cb + wa_row * (ab - cb) + wb_row * (bb - cb);
    alpha_row = ca + wa_row * (aa - ca) + wb_row * (ba - ca);

    red_dx = wa_dx * (ar - cr) + wb_dx * (br - cr);
    green_dx = wa_dx * (ag - cg) + wb_dx * (bg - cg);
    blue_dx = wa_dx * (ab - cb) + wb_dx * (bb - cb);
    alpha_dx = wa_dx * (aa - ca) + wb_dx * (ba - ca);
    u_dx = wa_dx * (a.u * a.inv_w - c.u * c.inv_w) +
        wb_dx * (b.u * b.inv_w - c.u * c.inv_w);
    v_dx = wa_dx * (a.v * a.inv_w - c.v * c.inv_w) +
        wb_dx * (b.v * b.inv_w - c.v * c.inv_w);
    q_dx = wa_dx * (a.inv_w - c.inv_w) +
        wb_dx * (b.inv_w - c.inv_w);
    z_dx = wa_dx * (a.z - c.z) + wb_dx * (b.z - c.z);

    red_dy = wa_dy * (ar - cr) + wb_dy * (br - cr);
    green_dy = wa_dy * (ag - cg) + wb_dy * (bg - cg);
    blue_dy = wa_dy * (ab - cb) + wb_dy * (bb - cb);
    alpha_dy = wa_dy * (aa - ca) + wb_dy * (ba - ca);
    u_dy = wa_dy * (a.u * a.inv_w - c.u * c.inv_w) +
        wb_dy * (b.u * b.inv_w - c.u * c.inv_w);
    v_dy = wa_dy * (a.v * a.inv_w - c.v * c.inv_w) +
        wb_dy * (b.v * b.inv_w - c.v * c.inv_w);
    q_dy = wa_dy * (a.inv_w - c.inv_w) +
        wb_dy * (b.inv_w - c.inv_w);
    z_dy = wa_dy * (a.z - c.z) + wb_dy * (b.z - c.z);

    for (y = top; y <= bottom; y++) {
        int xstart;
        int xend;
        float skip;
        float wa;
        float wb;
        float red;
        float green;
        float blue;
        float alpha;
        float u;
        float v;
        float q;
        float z;
        u32 *target_frame_row;
        u16 *target_frame_5551_row;
        u16 *target_depth_row;

        triangle_scan_span(a, b, c, y, left, right, &xstart, &xend);
        skip = (float)(xstart - left);
        wa = wa_row + skip * wa_dx;
        wb = wb_row + skip * wb_dx;
        red = red_row + skip * red_dx;
        green = green_row + skip * green_dx;
        blue = blue_row + skip * blue_dx;
        alpha = alpha_row + skip * alpha_dx;
        u = u_row + skip * u_dx;
        v = v_row + skip * v_dx;
        q = q_row + skip * q_dx;
        z = z_row + skip * z_dx;
        target_frame_row = fast_target ? target.frame + y * target.width : NULL;
        target_frame_5551_row = fast_5551 ? (u16 *)target.frame + y * target.width : NULL;
        target_depth_row = (fast_target || fast_5551) && target.depth ?
            target.depth + y * target.depth_width : NULL;

        for (x = xstart; x <= xend; x++) {
            float wc = 1.0f - wa - wb;

            if (wa >= 0.0f && wb >= 0.0f && wc >= 0.0f) {
                u16 *depth = target_depth_row ? target_depth_row + x : NULL;
                u16 z_value = fast_5551 && depth ? triangle_depth_value(z) : 0;
                if (fast_5551 && depth && target.depth_test &&
                    !raster_compare(target.depth_function, z_value, *depth))
                    goto next_pixel;

                u32 color =
                    (u32)(int)red |
                    ((u32)(int)green << 8) |
                    ((u32)(int)blue << 16) |
                    ((u32)(int)alpha << 24);

                if (textured && q != 0.0f) {
                    float invq = affine ? 1.0f : 1.0f / q;
                    int tx = (int)(u * invq);
                    int ty = (int)(v * invq);
                    u32 texel = fast_dxt ? ge_dxt_cached_pixel(&dxt, tx, ty) :
                        fast_rgba ? triangle_rgba_texture_pixel(&rgba_texture, tx, ty) :
                        fast_indexed ? triangle_indexed_texture_pixel(&fast, tx, ty) :
                        texture_pixel(tx, ty);
                    if (direct_white) {
                        color = texel;
                        if (!tex_use_alpha)
                            color |= 0xff000000u;
                    } else {
                        color = shade_pixel_cached(
                            texel, color, texfunc, tex_use_alpha, tex_environment
                        );
                    }
                }

                if (direct_target) {
                    u32 *destination = target.frame + y * target.width + x;
                    u32 alpha = color >> 24;

                    if (!target.blend || alpha == 255)
                        *destination = color;
                    else if (alpha)
                        *destination = blend_dxt_packed(color, *destination);
                } else if (fast_target) {
                    triangle_target_store(&target, target_frame_row + x, depth, color, z);
                } else if (fast_5551) {
                    triangle_target_store_5551(&target, target_frame_5551_row + x, depth, color, z_value);
                } else {
                    draw_pixel_internal(x, y, color, z, 1);
                }
            }

next_pixel:
            wa += wa_dx;
            wb += wb_dx;
            red += red_dx;
            green += green_dx;
            blue += blue_dx;
            alpha += alpha_dx;
            u += u_dx;
            v += v_dx;
            q += q_dx;
            z += z_dx;
        }

        wa_row += wa_dy;
        wb_row += wb_dy;
        red_row += red_dy;
        green_row += green_dy;
        blue_row += blue_dy;
        alpha_row += alpha_dy;
        u_row += u_dy;
        v_row += v_dy;
        q_row += q_dy;
        z_row += z_dy;
    }
}

static float clip_distance(GeVertex v, int plane)
{
    if (plane == 0)
        return v.clip_x + v.clip_w;
    if (plane == 1)
        return v.clip_w - v.clip_x;
    if (plane == 2)
        return v.clip_y + v.clip_w;
    if (plane == 3)
        return v.clip_w - v.clip_y;
    if (plane == 4)
        return v.clip_z + v.clip_w;
    return v.clip_w - v.clip_z;
}

static GeVertex clip_vertex(GeVertex a, GeVertex b, float t)
{
    GeVertex out;
    float ar = a.color & 0xff;
    float ag = (a.color >> 8) & 0xff;
    float ab = (a.color >> 16) & 0xff;
    float aa = a.color >> 24;
    float br = b.color & 0xff;
    float bg = (b.color >> 8) & 0xff;
    float bb = (b.color >> 16) & 0xff;
    float ba = b.color >> 24;

    out = a;
    out.clip_x = a.clip_x + (b.clip_x - a.clip_x) * t;
    out.clip_y = a.clip_y + (b.clip_y - a.clip_y) * t;
    out.clip_z = a.clip_z + (b.clip_z - a.clip_z) * t;
    out.clip_w = a.clip_w + (b.clip_w - a.clip_w) * t;
    out.u = a.u + (b.u - a.u) * t;
    out.v = a.v + (b.v - a.v) * t;
    out.color =
        (u32)(ar + (br - ar) * t) |
        ((u32)(ag + (bg - ag) * t) << 8) |
        ((u32)(ab + (bb - ab) * t) << 16) |
        ((u32)(aa + (ba - aa) * t) << 24);
    return out;
}

static int ge3d_emit_triangle(GeVertex a, GeVertex b, GeVertex c, u32 type, int flip)
{
    float area = edge(a, b, c.x, c.y);
    (void)type;
    int enabled = (ge_state.command[0x1d] & 1u) &&
        !(ge_state.command[GE_CLEARMODE] & 1u);
    int winding = ge_state.command[0x9b] & 1u;
    int visible = !enabled || !area ||
        (((area > 0.0f) ^ !!flip) == !winding);

    if (visible)
        draw_triangle(a, b, c);
    return visible;
}

static int ge3d_outside_screen(GeVertex v, const u32 *commands)
{
    float x = v.x + (float)(commands[GE_OFFSETX] & 0xffffu) * (1.0f / 16.0f);
    float y = v.y + (float)(commands[GE_OFFSETY] & 0xffffu) * (1.0f / 16.0f);

    return !isfinite(x) || !isfinite(y) || x < 0.0f || y < 0.0f ||
        x >= 4096.0f || y >= 4096.0f;
}

static void draw_clipped_triangle(GeVertex a, GeVertex b, GeVertex c, u32 type, int flip)
{
    GeVertex polygon[5];
    GeVertex clipped[5];
    int count = 3;
    int clipping_enabled = ge_state.command[0x1c] & 1u;
    int plane;
    int i;
    int outside_near = 0;

    if (type & (1u << 23)) {
        ge3d_emit_triangle(a, b, c, type, flip);
        return;
    }

    if (!isfinite(a.clip_x) || !isfinite(a.clip_y) || !isfinite(a.clip_z) || !isfinite(a.clip_w) ||
        !isfinite(b.clip_x) || !isfinite(b.clip_y) || !isfinite(b.clip_z) || !isfinite(b.clip_w) ||
        !isfinite(c.clip_x) || !isfinite(c.clip_y) || !isfinite(c.clip_z) || !isfinite(c.clip_w)) {
        return;
    }

    for (plane = 0; plane < 6; plane++) {
        if (clip_distance(a, plane) < 0.0f &&
            clip_distance(b, plane) < 0.0f &&
            clip_distance(c, plane) < 0.0f) {
            return;
        }
    }

    if (!clipping_enabled && (a.clip_w <= 0.0f || b.clip_w <= 0.0f || c.clip_w <= 0.0f)) {
        return;
    }

    polygon[0] = a;
    polygon[1] = b;
    polygon[2] = c;
    if (clip_distance(a, 4) < 0.0f) outside_near++;
    if (clip_distance(b, 4) < 0.0f) outside_near++;
    if (clip_distance(c, 4) < 0.0f) outside_near++;

    if (clipping_enabled && outside_near) {
        int output_count = 0;
        GeVertex previous = polygon[2];
        float previous_distance = clip_distance(previous, 4);
        int previous_inside = previous_distance >= 0.0f;

        for (i = 0; i < 3; i++) {
            GeVertex current = polygon[i];
            float current_distance = clip_distance(current, 4);
            int current_inside = current_distance >= 0.0f;

            if (current_inside != previous_inside) {
                float t = previous_distance / (previous_distance - current_distance);
                clipped[output_count++] = clip_vertex(previous, current, t);
            }
            if (current_inside)
                clipped[output_count++] = current;

            previous = current;
            previous_distance = current_distance;
            previous_inside = current_inside;
        }
        count = output_count;
        for (i = 0; i < count; i++)
            polygon[i] = clipped[i];
    }

    if (!count)
        return;
    for (i = 0; i < count; i++) {
        if (!ge3d_project_vertex(&polygon[i], ge_state.command)) {
            return;
        }
        if (ge3d_outside_screen(polygon[i], ge_state.command)) {
            return;
        }
        if (clipping_enabled) {
            if (polygon[i].z < 0.0f) polygon[i].z = 0.0f;
            if (polygon[i].z > 65535.0f) polygon[i].z = 65535.0f;
        } else if (polygon[i].z < 0.0f || polygon[i].z >= 65536.0f) {
            return;
        }
    }

    for (i = 1; i + 1 < count; i++)
        ge3d_emit_triangle(polygon[0], polygon[i], polygon[i + 1], type, flip);
}

void draw_primitive(u32 argument)
{
    u32 type = ge_state.command[GE_VERTEXTYPE];
    int primitive = argument >> 16;
    int count = argument & 0xffff;
    int index_type = (type >> 11) & 3;
    VertexLayout layout;
    int index;

    if (count <= 0)
        return;

    layout = vertex_layout(type);
    if (primitive == 6) {
        for (index = 0; index + 1 < count; index += 2)
            draw_sprite(
                read_vertex(index, type, layout),
                read_vertex(index + 1, type, layout)
            );
    } else if (primitive == 3) {
        for (index = 0; index + 2 < count; index += 3)
            draw_clipped_triangle(
                read_vertex(index, type, layout),
                read_vertex(index + 1, type, layout),
                read_vertex(index + 2, type, layout),
                type, 0
            );
    } else if (primitive == 4) {
        for (index = 0; index + 2 < count; index++)
            draw_clipped_triangle(
                read_vertex(index, type, layout),
                read_vertex(index + 1, type, layout),
                read_vertex(index + 2, type, layout),
                type, index & 1
            );
    } else if (primitive == 5) {
        GeVertex first = read_vertex(0, type, layout);

        for (index = 1; index + 1 < count; index++)
            draw_clipped_triangle(
                first,
                read_vertex(index, type, layout),
                read_vertex(index + 1, type, layout),
                type, 0
            );
    }

    if (index_type)
        ge_state.index_address += (u32)count << (index_type - 1);
    else
        ge_state.vertex_address += (u32)count * layout.stride;
}
