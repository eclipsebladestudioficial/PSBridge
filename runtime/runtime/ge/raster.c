#include <string.h>

#include "ge_internal.h"

static void draw_pixel(int x, int y, u32 color)
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

    if ((ge_state.command[GE_ALPHA_TEST_ENABLE] & 1) && !(ge_state.command[GE_CLEARMODE] & 1)) {
        alpha_command = ge_state.command[GE_ALPHATEST];

        if (!compare_alpha(
            alpha_command & 7,
            (color >> 24) & ((alpha_command >> 16) & 0xff),
            (alpha_command >> 8) & ((alpha_command >> 16) & 0xff)
        ))
            return;
    }

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

    if ((ge_state.command[GE_TEXTURE_ENABLE] & 1) && !(ge_state.command[GE_CLEARMODE] & 1))
        return 0;

    if ((ge_state.command[GE_BLEND_ENABLE] & 1) && !(ge_state.command[GE_CLEARMODE] & 1))
        return 0;

    format = ge_state.command[GE_FRAMEBUF_FORMAT] & 3;

    if (format != 1 && format != 3)
        return 0;

    if (!fast_sprite_target(&left, &top, &right, &bottom, &frame, &width))
        return 1;

    if ((ge_state.command[GE_ALPHA_TEST_ENABLE] & 1) && !(ge_state.command[GE_CLEARMODE] & 1)) {
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
        u32 clear = ge_state.command[GE_CLEARMODE];
        int write_color = !(clear & 1) || (clear & 0x100);
        int write_alpha = !(clear & 1) || (clear & 0x200);

        if (!write_color && !write_alpha)
            return 1;

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

        for (y = top; y < bottom; y++) {
            u16 *row = pixels + y * width + left;

            for (x = left; x < right; x++)
                *row++ = color;
        }
    }

    return 1;
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
    buffer_width = ge_state.command[GE_TEXBUFWIDTH0] & 0x7ff;
    clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));

    if (!texture || !texture_width || !texture_height)
        return 0;

    if (!buffer_width)
        buffer_width = texture_width;

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
    buffer_width = ge_state.command[GE_TEXBUFWIDTH0] & 0x7ff;
    swizzled = ge_state.command[GE_TEXMODE] & 1;
    clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));

    if (!texture || !texture_width || !texture_height)
        return 0;

    if (!buffer_width)
        buffer_width = texture_width;

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
    buffer_width = ge_state.command[GE_TEXBUFWIDTH0] & 0x7ff;
    swizzled = ge_state.command[GE_TEXMODE] & 1;
    clamp_x = ge_state.command[GE_TEXWRAP] & 1;
    clamp_y = (ge_state.command[GE_TEXWRAP] >> 8) & 1;
    texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));

    if (!texture || !texture_width || !texture_height)
        return 0;

    if (!buffer_width)
        buffer_width = texture_width;

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
    int left = (int)(first.x < second.x ? first.x : second.x);
    int top = (int)(first.y < second.y ? first.y : second.y);
    int right = (int)(first.x > second.x ? first.x : second.x);
    int bottom = (int)(first.y > second.y ? first.y : second.y);
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

static void draw_triangle(GeVertex a, GeVertex b, GeVertex c)
{
    TrianglePalette fast;
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
    float red_dx;
    float green_dx;
    float blue_dx;
    float alpha_dx;
    float u_dx;
    float v_dx;
    float red_dy;
    float green_dy;
    float blue_dy;
    float alpha_dy;
    float u_dy;
    float v_dy;

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

    fast_palette = textured && a.color == 0xffffffffu &&
        b.color == 0xffffffffu && c.color == 0xffffffffu &&
        prepare_triangle_palette(&fast);

    inverse_area = 1.0f / area;

    wa_row = edge(b, c, left + 0.5f, top + 0.5f) * inverse_area;
    wb_row = edge(c, a, left + 0.5f, top + 0.5f) * inverse_area;

    wa_dx = (c.y - b.y) * inverse_area;
    wb_dx = (a.y - c.y) * inverse_area;
    wa_dy = (b.x - c.x) * inverse_area;
    wb_dy = (c.x - a.x) * inverse_area;

    u_row = c.u + wa_row * (a.u - c.u) + wb_row * (b.u - c.u);
    v_row = c.v + wa_row * (a.v - c.v) + wb_row * (b.v - c.v);

    if (fast_palette) {
        u_dx = wa_dx * (a.u - c.u) + wb_dx * (b.u - c.u);
        v_dx = wa_dx * (a.v - c.v) + wb_dx * (b.v - c.v);
        u_dy = wa_dy * (a.u - c.u) + wb_dy * (b.u - c.u);
        v_dy = wa_dy * (a.v - c.v) + wb_dy * (b.v - c.v);

        for (y = top; y <= bottom; y++) {
            float wa = wa_row;
            float wb = wb_row;
            float u = u_row;
            float v = v_row;

            u32 *row = fast.frame + y * fast.frame_width + left;

            for (x = left; x <= right; x++) {
                float wc = 1.0f - wa - wb;

                if (wa >= 0.0f && wb >= 0.0f && wc >= 0.0f) {
                    u32 color;

                    if (triangle_palette_pixel(&fast, (int)u, (int)v, &color)) {
                        u32 *destination = &row[x - left];
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

    red_row = cr + wa_row * (ar - cr) + wb_row * (br - cr);
    green_row = cg + wa_row * (ag - cg) + wb_row * (bg - cg);
    blue_row = cb + wa_row * (ab - cb) + wb_row * (bb - cb);
    alpha_row = ca + wa_row * (aa - ca) + wb_row * (ba - ca);

    red_dx = wa_dx * (ar - cr) + wb_dx * (br - cr);
    green_dx = wa_dx * (ag - cg) + wb_dx * (bg - cg);
    blue_dx = wa_dx * (ab - cb) + wb_dx * (bb - cb);
    alpha_dx = wa_dx * (aa - ca) + wb_dx * (ba - ca);
    u_dx = wa_dx * (a.u - c.u) + wb_dx * (b.u - c.u);
    v_dx = wa_dx * (a.v - c.v) + wb_dx * (b.v - c.v);

    red_dy = wa_dy * (ar - cr) + wb_dy * (br - cr);
    green_dy = wa_dy * (ag - cg) + wb_dy * (bg - cg);
    blue_dy = wa_dy * (ab - cb) + wb_dy * (bb - cb);
    alpha_dy = wa_dy * (aa - ca) + wb_dy * (ba - ca);
    u_dy = wa_dy * (a.u - c.u) + wb_dy * (b.u - c.u);
    v_dy = wa_dy * (a.v - c.v) + wb_dy * (b.v - c.v);

    for (y = top; y <= bottom; y++) {
        float wa = wa_row;
        float wb = wb_row;
        float red = red_row;
        float green = green_row;
        float blue = blue_row;
        float alpha = alpha_row;
        float u = u_row;
        float v = v_row;

        for (x = left; x <= right; x++) {
            float wc = 1.0f - wa - wb;

            if (wa >= 0.0f && wb >= 0.0f && wc >= 0.0f) {
                u32 color =
                    (u32)(int)red |
                    ((u32)(int)green << 8) |
                    ((u32)(int)blue << 16) |
                    ((u32)(int)alpha << 24);

                if (textured)
                    color = shade_pixel(
                        texture_pixel((int)u, (int)v), color
                    );

                draw_pixel(x, y, color);
            }

            wa += wa_dx;
            wb += wb_dx;
            red += red_dx;
            green += green_dx;
            blue += blue_dx;
            alpha += alpha_dx;
            u += u_dx;
            v += v_dx;
        }

        wa_row += wa_dy;
        wb_row += wb_dy;
        red_row += red_dy;
        green_row += green_dy;
        blue_row += blue_dy;
        alpha_row += alpha_dy;
        u_row += u_dy;
        v_row += v_dy;
    }
}

void draw_primitive(u32 argument)
{
    u32 type = ge_state.command[GE_VERTEXTYPE];
    int primitive = argument >> 16;
    int count = argument & 0xffff;
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
        return;
    }

    if (primitive == 3) {
        for (index = 0; index + 2 < count; index += 3)
            draw_triangle(
                read_vertex(index, type, layout),
                read_vertex(index + 1, type, layout),
                read_vertex(index + 2, type, layout)
            );
        return;
    }

    if (primitive == 4) {
        for (index = 0; index + 2 < count; index++)
            draw_triangle(
                read_vertex(index, type, layout),
                read_vertex(index + 1, type, layout),
                read_vertex(index + 2, type, layout)
            );
        return;
    }

    if (primitive == 5) {
        GeVertex first = read_vertex(0, type, layout);

        for (index = 1; index + 1 < count; index++)
            draw_triangle(
                first,
                read_vertex(index, type, layout),
                read_vertex(index + 1, type, layout)
            );
    }
}
