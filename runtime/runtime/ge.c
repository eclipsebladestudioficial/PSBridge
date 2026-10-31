#include <stdint.h>
#include <string.h>
#include <kernel.h>
#include <math.h>

#include "psbridge.h"
#include "psbridge_log.h"
#include "ge3d.h"

#define GE_LIST_LIMIT 16
#define GE_STACK_LIMIT 8
#define GE_COMMAND_LIMIT 200000

#define GE_WORLDMATRIXNUMBER 0x3a
#define GE_WORLDMATRIXDATA 0x3b
#define GE_VIEWMATRIXNUMBER 0x3c
#define GE_VIEWMATRIXDATA 0x3d
#define GE_PROJMATRIXNUMBER 0x3e
#define GE_PROJMATRIXDATA 0x3f
#define GE_VIEWPORTXSCALE 0x42
#define GE_VIEWPORTYSCALE 0x43
#define GE_VIEWPORTXCENTER 0x45
#define GE_VIEWPORTYCENTER 0x46
#define GE_TEXSCALEU 0x48
#define GE_TEXSCALEV 0x49
#define GE_TEXOFFSETU 0x4a
#define GE_TEXOFFSETV 0x4b
#define GE_VADDR 0x01
#define GE_IADDR 0x02
#define GE_PRIM 0x04
#define GE_JUMP 0x08
#define GE_CALL 0x0a
#define GE_RET 0x0b
#define GE_END 0x0c
#define GE_SIGNAL 0x0e
#define GE_FINISH 0x0f
#define GE_BASE 0x10
#define GE_VERTEXTYPE 0x12
#define GE_OFFSETADDR 0x13
#define GE_ORIGIN 0x14
#define GE_TEXTURE_ENABLE 0x1e
#define GE_BLEND_ENABLE 0x21
#define GE_ALPHA_TEST_ENABLE 0x22
#define GE_VADDR_BASE 0x9c
#define GE_VADDR_WIDTH 0x9d
#define GE_TEXADDR0 0xa0
#define GE_TEXBUFWIDTH0 0xa8
#define GE_CLUTADDR 0xb0
#define GE_CLUTADDRUPPER 0xb1
#define GE_TRANSFERSRC 0xb2
#define GE_TRANSFERSRCW 0xb3
#define GE_TRANSFERDST 0xb4
#define GE_TRANSFERDSTW 0xb5
#define GE_TEXSIZE0 0xb8
#define GE_TEXMODE 0xc2
#define GE_TEXFORMAT 0xc3
#define GE_LOADCLUT 0xc4
#define GE_TEXENVCOLOR 0xca
#define GE_CLUTFORMAT 0xc5
#define GE_TEXWRAP 0xc7
#define GE_TEXFUNC 0xc9
#define GE_OFFSETX 0x4c
#define GE_OFFSETY 0x4d
#define GE_FRAMEBUF_FORMAT 0xd2
#define GE_CLEARMODE 0xd3
#define GE_SCISSOR1 0xd4
#define GE_SCISSOR2 0xd5
#define GE_ALPHATEST 0xdb
#define GE_TRANSFERSTART 0xea
#define GE_TRANSFERSRCPOS 0xeb
#define GE_TRANSFERDSTPOS 0xec
#define GE_TRANSFERSIZE 0xee

typedef struct {
    int used;
    int id;
    int done;
    u32 pc;
    u32 stall;
    u32 stack[GE_STACK_LIMIT];
    u32 stack_offset[GE_STACK_LIMIT];
    int stack_size;
    int callback_id;
    u32 finish_id;
    u32 signal_id;
    int pending_signal;
    int pending_finish;
    unsigned draws;
} GeList;

typedef struct {
    u32 command[256];
    u32 vertex_address;
    u32 index_address;
    u32 offset_address;
    float texture_u;
    float texture_v;
} GeState;

typedef struct {
    int weight;
    int texture;
    int color;
    int normal;
    int position;
    int stride;
} VertexLayout;

typedef struct {
    const u8 *texture;
    u32 *frame;
    u32 colors[256];
    u8 visible[256];
    int texture_width;
    int texture_height;
    int row_bytes;
    int frame_width;
    int format;
    int blend;
    int swizzled;
    int clamp_x;
    int clamp_y;
} TrianglePalette;

static GeList lists[GE_LIST_LIMIT];
static GeState state;
static u8 ge_clut[1024] __attribute__((aligned(4)));
static u32 ge_clut_entries;
static int ge_clut_loaded;
static int next_list_id = 1;
static int next_callback_id = 1;
typedef struct {
    int used;
    int id;
    u32 signal_function;
    u32 signal_arg;
    u32 finish_function;
    u32 finish_arg;
} GeCallback;

static GeCallback ge_callbacks[16];
static u32 diag_ge_callbacks;
static volatile u32 ge_ready_buffer;
static volatile u32 ge_ready_sequence;

u32 psbridge_ge_ready_buffer(void)
{
    return ge_ready_buffer;
}

u32 psbridge_ge_ready_sequence(void)
{
    return ge_ready_sequence;
}


static GeCallback *find_ge_callback(int id)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (ge_callbacks[i].used && ge_callbacks[i].id == id)
            return &ge_callbacks[i];
    }
    return NULL;
}

static int register_ge_callback(u32 address)
{
    const u32 *data = psbridge_game_pointer((const void *)(uintptr_t)address);
    int i;
    if (!data)
        return -1;
    for (i = 0; i < 16; i++) {
        if (!ge_callbacks[i].used) {
            GeCallback *cb = &ge_callbacks[i];
            cb->used = 1;
            cb->id = next_callback_id++;
            cb->signal_function = data[0];
            cb->signal_arg = data[1];
            cb->finish_function = data[2];
            cb->finish_arg = data[3];
            PRINT_LOG("ge cadastrou callback safada id %d sinal %08x fim %08x\n",
                cb->id, cb->signal_function, cb->finish_function);
            return cb->id;
        }
    }
    return -1;
}

static void fire_ge_callback(int callback_id, int finish, u32 data)
{
    GeCallback *cb = find_ge_callback(callback_id);
    u32 function;
    u32 argument;
    if (!cb)
        return;
    function = finish ? cb->finish_function : cb->signal_function;
    argument = finish ? cb->finish_arg : cb->signal_arg;
    if (!function)
        return;
    diag_ge_callbacks++;
    if (diag_ge_callbacks <= 16 || (diag_ge_callbacks % 128 == 0))
        PRINT_LOG("ge callback %s gozou id %d dado %08x endereco %08x vezes %u\n",
            finish ? "fim" : "sinal", callback_id, data, function, diag_ge_callbacks);
    psbridge_call_callback((void *)(uintptr_t)function, data, argument, 0);
}




static int align_value(int value, int alignment)
{
    if (alignment <= 1)
        return value;

    return (value + alignment - 1) & ~(alignment - 1);
}

static void *ge_pointer(u32 address)
{
    u32 video = (u32)(uintptr_t)psbridge_video_memory();

    address &= 0x3fffffff;

    if (address >= 0x04000000 && address < 0x04200000)
        return (u8 *)psbridge_video_memory() + address - 0x04000000;

    if (address < 0x00200000)
        return (u8 *)psbridge_video_memory() + address;

    if (address >= video && address < video + 0x00200000)
        return (void *)(uintptr_t)address;

    {
        void *result = psbridge_game_pointer((void *)(uintptr_t)address);
        if ((u32)(uintptr_t)result == address && address >= 0x00200000 && address < 0x02000000)
            return (void *)(uintptr_t)(address | 0x80000000);
        return result;
    }
}

static u32 relative_address(u32 value)
{
    u32 base = (state.command[GE_BASE] & 0x000f0000) << 8;

    return (state.offset_address + base + (value & 0x00ffffff)) & 0x0fffffff;
}

static u32 buffer_address(int address_command, int width_command)
{
    return
        (state.command[address_command] & 0x00fffff0) |
        ((state.command[width_command] << 8) & 0x0f000000);
}

static u32 unpack_color(u16 color, int format)
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

static u16 pack_color(u32 color, int format)
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

static u32 read_frame_pixel(const void *buffer, int index, int format)
{
    if (format == 3)
        return ((const u32 *)buffer)[index];

    return unpack_color(((const u16 *)buffer)[index], format);
}

static void write_frame_pixel(void *buffer, int index, int format, u32 color)
{
    if (format == 3)
        ((u32 *)buffer)[index] = color;
    else
        ((u16 *)buffer)[index] = pack_color(color, format);
}

static int compare_alpha(int function, int value, int reference)
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

static u32 blend_color(u32 source, u32 destination)
{
    u32 alpha = source >> 24;
    u32 inverse = 255 - alpha;
    u32 red;
    u32 green;
    u32 blue;
    u32 output_alpha;

    red = ((source & 0xff) * alpha + (destination & 0xff) * inverse) / 255;
    green = (((source >> 8) & 0xff) * alpha + ((destination >> 8) & 0xff) * inverse) / 255;
    blue = (((source >> 16) & 0xff) * alpha + ((destination >> 16) & 0xff) * inverse) / 255;
    output_alpha = alpha + ((destination >> 24) * inverse) / 255;

    return red | (green << 8) | (blue << 16) | (output_alpha << 24);
}

static int pixel_offset(int x, int y, int row_bytes, int bytes, int swizzled)
{
    int byte_x = x * bytes;

    if (!swizzled)
        return y * row_bytes + byte_x;

    return
        ((y >> 3) * ((row_bytes + 15) >> 4) + (byte_x >> 4)) * 128 +
        (y & 7) * 16 +
        (byte_x & 15);
}

static void load_clut(u32 argument)
{
    u32 format = state.command[GE_CLUTFORMAT] & 3;
    u32 entries = (argument & 0x3f) * 8;
    u32 bytes_per_entry = format == 3 ? 4 : 2;
    u32 address =
        (state.command[GE_CLUTADDR] & 0x00fffff0) |
        ((state.command[GE_CLUTADDRUPPER] << 8) & 0x0f000000);
    const void *source = ge_pointer(address);

    if (entries > 256)
        entries = 256;
    if (!source || !entries)
        return;

    memcpy(ge_clut, source, entries * bytes_per_entry);
    ge_clut_entries = entries;
    ge_clut_loaded = 1;
}

static u32 palette_color(u32 index)
{
    u32 format = state.command[GE_CLUTFORMAT] & 3;
    u32 shift = (state.command[GE_CLUTFORMAT] >> 2) & 0x1f;
    u32 mask = (state.command[GE_CLUTFORMAT] >> 8) & 0xff;
    u32 start = ((state.command[GE_CLUTFORMAT] >> 16) & 0x1f) << 4;
    u32 address;
    const void *palette;

    index = (((index >> shift) & mask) | start) & 0xff;
    if (ge_clut_loaded) {
        if (index >= ge_clut_entries)
            return 0;
        palette = ge_clut;
    } else {
        address =
            (state.command[GE_CLUTADDR] & 0x00fffff0) |
            ((state.command[GE_CLUTADDRUPPER] << 8) & 0x0f000000);
        palette = ge_pointer(address);
        if (!palette)
            return 0;
    }

    if (format == 3)
        return ((const u32 *)palette)[index];

    return unpack_color(((const u16 *)palette)[index], format);
}

static u32 texture_pixel(int x, int y)
{
    int format = state.command[GE_TEXFORMAT] & 0xf;
    int width = 1 << (state.command[GE_TEXSIZE0] & 0xf);
    int height = 1 << ((state.command[GE_TEXSIZE0] >> 8) & 0xf);
    int buffer_width = state.command[GE_TEXBUFWIDTH0] & 0x7ff;
    int swizzled = state.command[GE_TEXMODE] & 1;
    int clamp_x = state.command[GE_TEXWRAP] & 1;
    int clamp_y = (state.command[GE_TEXWRAP] >> 8) & 1;
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

static u32 shade_pixel(u32 texture, u32 vertex)
{
    int function = state.command[GE_TEXFUNC] & 7;
    int use_alpha = (state.command[GE_TEXFUNC] & 0x100) != 0;
    u32 environment = state.command[GE_TEXENVCOLOR];
    u32 texture_alpha = (texture >> 24) & 0xff;
    u32 vertex_alpha = (vertex >> 24) & 0xff;
    u32 output_alpha = vertex_alpha;
    u32 result = 0;
    int channel;

    if (function == 3)
        output_alpha = use_alpha ? texture_alpha : vertex_alpha;
    else if (function != 1 && use_alpha)
        output_alpha = (texture_alpha * vertex_alpha) / 255;

    if (function == 0 && (vertex & 0x00ffffff) == 0) {
    vertex = (vertex & 0xff000000) | 0x00ffffff;
}

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

static int prepare_triangle_palette(TrianglePalette *fast)
{
    int buffer_width;
    int entries;
    int use_alpha;
    int blend;
    int alpha_test;
    u32 alpha_command;
    int index;

    fast->format = state.command[GE_TEXFORMAT] & 0xf;

    if (fast->format != 4 && fast->format != 5)
        return 0;
    if ((state.command[GE_FRAMEBUF_FORMAT] & 3) != 3)
        return 0;
    if ((state.command[GE_TEXFUNC] & 7) != 0)
        return 0;

    fast->texture_width = 1 << (state.command[GE_TEXSIZE0] & 0xf);
    fast->texture_height = 1 << ((state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = state.command[GE_TEXBUFWIDTH0] & 0x7ff;

    if (!buffer_width)
        buffer_width = fast->texture_width;

    fast->texture = ge_pointer(buffer_address(GE_TEXADDR0, GE_TEXBUFWIDTH0));
    fast->frame = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));
    fast->frame_width = state.command[GE_VADDR_WIDTH] & 0x7fc;
    fast->row_bytes = fast->format == 4 ? (buffer_width + 1) >> 1 : buffer_width;
    fast->swizzled = state.command[GE_TEXMODE] & 1;
    fast->clamp_x = state.command[GE_TEXWRAP] & 1;
    fast->clamp_y = (state.command[GE_TEXWRAP] >> 8) & 1;

    if (!fast->texture || !fast->frame || fast->frame_width < 64)
        return 0;

    use_alpha = state.command[GE_TEXFUNC] & 0x100;
    blend = state.command[GE_BLEND_ENABLE] & 1;
    fast->blend = blend;
    alpha_test = state.command[GE_ALPHA_TEST_ENABLE] & 1;
    alpha_command = state.command[GE_ALPHATEST];

    entries = fast->format == 4 ? 16 : 256;

    for (index = 0; index < entries; index++) {
        u32 color = palette_color(index);

        if (!use_alpha)
            color |= 0xff000000;

        fast->visible[index] = !alpha_test || compare_alpha(
            alpha_command & 7,
            (color >> 24) & ((alpha_command >> 16) & 0xff),
            (alpha_command >> 8) & ((alpha_command >> 16) & 0xff)
        );

        fast->colors[index] = color;
    }

    return 1;
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

static void draw_pixel(int x, int y, u32 color)
{
    int width = state.command[GE_VADDR_WIDTH] & 0x7fc;
    int format = state.command[GE_FRAMEBUF_FORMAT] & 3;
    int left = state.command[GE_SCISSOR1] & 0x3ff;
    int top = (state.command[GE_SCISSOR1] >> 10) & 0x3ff;
    int right = state.command[GE_SCISSOR2] & 0x3ff;
    int bottom = (state.command[GE_SCISSOR2] >> 10) & 0x3ff;
    void *frame = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));
    int index;
    u32 alpha_command;
    u32 destination;

    if (!frame || width < 64 || width > 2048)
        return;

    if (!state.command[GE_SCISSOR2]) {
        right = 479;
        bottom = 271;
    }

    if (x < left || y < top || x > right || y > bottom)
        return;

    if (x < 0 || y < 0 || x >= width || y >= 512)
        return;

    if ((state.command[GE_ALPHA_TEST_ENABLE] & 1) && !(state.command[GE_CLEARMODE] & 1)) {
        alpha_command = state.command[GE_ALPHATEST];

        if (!compare_alpha(
            alpha_command & 7,
            (color >> 24) & ((alpha_command >> 16) & 0xff),
            (alpha_command >> 8) & ((alpha_command >> 16) & 0xff)
        ))
            return;
    }

    index = y * width + x;

    if (state.command[GE_CLEARMODE] & 1) {
        u32 clear = state.command[GE_CLEARMODE];

        destination = read_frame_pixel(frame, index, format);

        if (!(clear & 0x100))
            color = (color & 0xff000000) | (destination & 0x00ffffff);
        if (!(clear & 0x200))
            color = (color & 0x00ffffff) | (destination & 0xff000000);
    } else if (state.command[GE_BLEND_ENABLE] & 1) {
        destination = read_frame_pixel(frame, index, format);

        if (
            (state.command[GE_TEXTURE_ENABLE] & 1) &&
            ((state.command[GE_TEXFUNC] & 7) == 0)
        ) {
            color = (color & 0x00ffffff) | 0xff000000;
        }
        
        color = blend_color(color, destination);
    }

    write_frame_pixel(frame, index, format, color);
}

static VertexLayout vertex_layout(u32 type)
{
    static const int texture_size[] = {0, 2, 4, 8};
    static const int texture_align[] = {1, 1, 2, 4};
    static const int color_size[] = {0, 0, 0, 0, 2, 2, 2, 4};
    static const int color_align[] = {1, 1, 1, 1, 2, 2, 2, 4};
    static const int normal_size[] = {0, 3, 6, 12};
    static const int normal_align[] = {1, 1, 2, 4};
    static const int position_size[] = {3, 3, 6, 12};
    static const int position_align[] = {1, 1, 2, 4};
    static const int weight_size[] = {0, 1, 2, 4};
    static const int weight_align[] = {1, 1, 2, 4};
    VertexLayout layout;
    int texture = type & 3;
    int color = (type >> 2) & 7;
    int normal = (type >> 5) & 3;
    int position = (type >> 7) & 3;
    int weight = (type >> 9) & 3;
    int weight_count = ((type >> 14) & 7) + 1;
    int morph_count = ((type >> 18) & 7) + 1;
    int size = 0;
    int biggest = 1;

    memset(&layout, 0, sizeof(layout));

    if (weight) {
        layout.weight = size;
        size += weight_size[weight] * weight_count;
        biggest = weight_align[weight];
    }

    if (texture) {
        size = align_value(size, texture_align[texture]);
        layout.texture = size;
        size += texture_size[texture];
        if (texture_align[texture] > biggest)
            biggest = texture_align[texture];
    } else {
        layout.texture = -1;
    }

    if (color) {
        size = align_value(size, color_align[color]);
        layout.color = size;
        size += color_size[color];
        if (color_align[color] > biggest)
            biggest = color_align[color];
    } else {
        layout.color = -1;
    }

    if (normal) {
        size = align_value(size, normal_align[normal]);
        layout.normal = size;
        size += normal_size[normal];
        if (normal_align[normal] > biggest)
            biggest = normal_align[normal];
    }

    size = align_value(size, position_align[position]);
    layout.position = size;
    size += position_size[position];

    if (position_align[position] > biggest)
        biggest = position_align[position];

    layout.stride = align_value(size, biggest) * morph_count;

    return layout;
}

static u32 vertex_index(int number, u32 type)
{
    int index_type = (type >> 11) & 3;
    const void *indices = ge_pointer(state.index_address);

    if (!index_type || !indices)
        return number;

    if (index_type == 1)
        return ((const u8 *)indices)[number];

    if (index_type == 2)
        return ((const u16 *)indices)[number];

    return ((const u32 *)indices)[number] & 0xffff;
}

static GeVertex read_vertex(int number, u32 type, VertexLayout layout)
{
    int texture = type & 3;
    int color = (type >> 2) & 7;
    int position = (type >> 7) & 3;
    u32 index = vertex_index(number, type);
    const u8 *data = ge_pointer(state.vertex_address);
    GeVertex vertex;

    memset(&vertex, 0, sizeof(vertex));
    vertex.color = 0xffffffff;

    if (!data)
        return vertex;

    data += index * layout.stride;

    if (texture == 1) {
        vertex.u = data[layout.texture];
        vertex.v = data[layout.texture + 1];
    } else if (texture == 2) {
        vertex.u = *(const u16 *)(data + layout.texture);
        vertex.v = *(const u16 *)(data + layout.texture + 2);
    } else if (texture == 3) {
        vertex.u = *(const float *)(data + layout.texture);
        vertex.v = *(const float *)(data + layout.texture + 4);
    } else {
        vertex.u = state.texture_u;
        vertex.v = state.texture_v;
    }

    if (texture) {
        state.texture_u = vertex.u;
        state.texture_v = vertex.v;
    }

    if (color >= 4 && color <= 6)
        vertex.color = unpack_color(*(const u16 *)(data + layout.color), color - 4);
    else if (color == 7)
        vertex.color = *(const u32 *)(data + layout.color);

    if (position == 1) {
        vertex.x = *(const s8 *)(data + layout.position);
        vertex.y = *(const s8 *)(data + layout.position + 1);
        vertex.z = *(const s8 *)(data + layout.position + 2);
    } else if (position == 2) {
        vertex.x = *(const s16 *)(data + layout.position);
        vertex.y = *(const s16 *)(data + layout.position + 2);
        vertex.z = *(const s16 *)(data + layout.position + 4);
    } else if (position == 3) {
        vertex.x = *(const float *)(data + layout.position);
        vertex.y = *(const float *)(data + layout.position + 4);
        vertex.z = *(const float *)(data + layout.position + 8);
    }

    ge3d_transform_vertex(&vertex, type, state.command);
    return vertex;
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
    int scissor_left = state.command[GE_SCISSOR1] & 0x3ff;
    int scissor_top = (state.command[GE_SCISSOR1] >> 10) & 0x3ff;
    int scissor_right = state.command[GE_SCISSOR2] & 0x3ff;
    int scissor_bottom = (state.command[GE_SCISSOR2] >> 10) & 0x3ff;

    *width = state.command[GE_VADDR_WIDTH] & 0x7fc;
    *frame = ge_pointer(buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH));

    if (!*frame || *width < 64 || *width > 2048)
        return 0;

    if (!state.command[GE_SCISSOR2]) {
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

    if ((state.command[GE_TEXTURE_ENABLE] & 1) && !(state.command[GE_CLEARMODE] & 1))
        return 0;

    if ((state.command[GE_BLEND_ENABLE] & 1) && !(state.command[GE_CLEARMODE] & 1))
        return 0;

    format = state.command[GE_FRAMEBUF_FORMAT] & 3;

    if (format != 1 && format != 3)
        return 0;

    if (!fast_sprite_target(&left, &top, &right, &bottom, &frame, &width))
        return 1;

    if ((state.command[GE_ALPHA_TEST_ENABLE] & 1) && !(state.command[GE_CLEARMODE] & 1)) {
        u32 alpha = state.command[GE_ALPHATEST];
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
        u32 clear = state.command[GE_CLEARMODE];
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

    if (!(state.command[GE_TEXTURE_ENABLE] & 1) || (state.command[GE_CLEARMODE] & 1))
        return 0;

    if ((state.command[GE_TEXFORMAT] & 0xf) != 1 || (state.command[GE_TEXMODE] & 1))
        return 0;

    if ((state.command[GE_BLEND_ENABLE] & 1) ||
        ((state.command[GE_TEXFUNC] & 7) != 3 && second.color != 0xffffffff))
        return 0;

    if ((state.command[GE_FRAMEBUF_FORMAT] & 3) != 1)
        return 0;

    texture_width = 1 << (state.command[GE_TEXSIZE0] & 0xf);
    texture_height = 1 << ((state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = state.command[GE_TEXBUFWIDTH0] & 0x7ff;
    clamp_x = state.command[GE_TEXWRAP] & 1;
    clamp_y = (state.command[GE_TEXWRAP] >> 8) & 1;
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

    use_alpha = state.command[GE_TEXFUNC] & 0x100;
    alpha_test = state.command[GE_ALPHA_TEST_ENABLE] & 1;

    if (alpha_test) {
        u32 alpha = state.command[GE_ALPHATEST];
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

    if (!(state.command[GE_TEXTURE_ENABLE] & 1) || (state.command[GE_CLEARMODE] & 1))
        return 0;

    if ((state.command[GE_TEXFORMAT] & 0xf) != 4)
        return 0;

    if ((state.command[GE_FRAMEBUF_FORMAT] & 3) != 3)
        return 0;

    texture_width = 1 << (state.command[GE_TEXSIZE0] & 0xf);
    texture_height = 1 << ((state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = state.command[GE_TEXBUFWIDTH0] & 0x7ff;
    swizzled = state.command[GE_TEXMODE] & 1;
    clamp_x = state.command[GE_TEXWRAP] & 1;
    clamp_y = (state.command[GE_TEXWRAP] >> 8) & 1;
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

    blend = state.command[GE_BLEND_ENABLE] & 1;
    alpha_test = state.command[GE_ALPHA_TEST_ENABLE] & 1;
    alpha_command = state.command[GE_ALPHATEST];

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

            if (!blend || (color >> 24) == 255) {
                *row = color;
            } else if (color >> 24) {
                u32 destination = *row;
                u32 alpha = color >> 24;
                u32 inverse = 255 - alpha;
                u32 red = ((color & 0xff) * alpha + (destination & 0xff) * inverse) / 255;
                u32 green = (((color >> 8) & 0xff) * alpha + ((destination >> 8) & 0xff) * inverse) / 255;
                u32 blue = (((color >> 16) & 0xff) * alpha + ((destination >> 16) & 0xff) * inverse) / 255;
                u32 output_alpha = alpha + ((destination >> 24) * inverse) / 255;

                *row = red | (green << 8) | (blue << 16) | (output_alpha << 24);
            }

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

    if (!(state.command[GE_TEXTURE_ENABLE] & 1) || (state.command[GE_CLEARMODE] & 1))
        return 0;

    if ((state.command[GE_TEXFORMAT] & 0xf) != 3)
        return 0;

    if ((state.command[GE_FRAMEBUF_FORMAT] & 3) != 3)
        return 0;

    texture_width = 1 << (state.command[GE_TEXSIZE0] & 0xf);
    texture_height = 1 << ((state.command[GE_TEXSIZE0] >> 8) & 0xf);
    buffer_width = state.command[GE_TEXBUFWIDTH0] & 0x7ff;
    swizzled = state.command[GE_TEXMODE] & 1;
    clamp_x = state.command[GE_TEXWRAP] & 1;
    clamp_y = (state.command[GE_TEXWRAP] >> 8) & 1;
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

    blend = state.command[GE_BLEND_ENABLE] & 1;
    alpha_test = state.command[GE_ALPHA_TEST_ENABLE] & 1;
    alpha_command = state.command[GE_ALPHATEST];
    direct_color =
        (state.command[GE_TEXFUNC] & 7) == 3 ||
        ((state.command[GE_TEXFUNC] & 7) == 0 && second.color == 0xffffffff);
    use_alpha = state.command[GE_TEXFUNC] & 0x100;
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

            if (!blend || (color >> 24) == 255) {
                *row = color;
            } else if (color >> 24) {
                u32 destination = *row;
                u32 alpha = color >> 24;
                u32 inverse = 255 - alpha;
                u32 red = ((color & 0xff) * alpha + (destination & 0xff) * inverse) / 255;
                u32 green = (((color >> 8) & 0xff) * alpha + ((destination >> 8) & 0xff) * inverse) / 255;
                u32 blue = (((color >> 16) & 0xff) * alpha + ((destination >> 16) & 0xff) * inverse) / 255;
                u32 output_alpha = alpha + ((destination >> 24) * inverse) / 255;

                *row = red | (green << 8) | (blue << 16) | (output_alpha << 24);
            }

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

    texture_on = (state.command[GE_TEXTURE_ENABLE] & 1) && !(state.command[GE_CLEARMODE] & 1);

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

    int frame_width = state.command[GE_VADDR_WIDTH] & 0x7fc;
    int scissor_left = state.command[GE_SCISSOR1] & 0x3ff;
    int scissor_top = (state.command[GE_SCISSOR1] >> 10) & 0x3ff;
    int scissor_right = state.command[GE_SCISSOR2] & 0x3ff;
    int scissor_bottom = (state.command[GE_SCISSOR2] >> 10) & 0x3ff;

    int textured =
        (state.command[GE_TEXTURE_ENABLE] & 1) &&
        !(state.command[GE_CLEARMODE] & 1);

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

    if (!state.command[GE_SCISSOR2]) {
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
                        u32 alpha = color >> 24;

                         if (!fast.blend || alpha == 255)
                              *destination = color;
                         else if (alpha != 0)
                              *destination = blend_color(color, *destination);
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

static void draw_primitive(u32 argument)
{
    u32 type = state.command[GE_VERTEXTYPE];
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

static void transfer_pixels(void)
{
    int source_width = state.command[GE_TRANSFERSRCW] & 0x7f8;
    int destination_width = state.command[GE_TRANSFERDSTW] & 0x7f8;
    int source_x = state.command[GE_TRANSFERSRCPOS] & 0x3ff;
    int source_y = (state.command[GE_TRANSFERSRCPOS] >> 10) & 0x3ff;
    int destination_x = state.command[GE_TRANSFERDSTPOS] & 0x3ff;
    int destination_y = (state.command[GE_TRANSFERDSTPOS] >> 10) & 0x3ff;
    int width = (state.command[GE_TRANSFERSIZE] & 0x3ff) + 1;
    int height = ((state.command[GE_TRANSFERSIZE] >> 10) & 0x3ff) + 1;
    int bytes = (state.command[GE_TRANSFERSTART] & 1) ? 4 : 2;
    const u8 *source = ge_pointer(buffer_address(GE_TRANSFERSRC, GE_TRANSFERSRCW));
    u8 *destination = ge_pointer(buffer_address(GE_TRANSFERDST, GE_TRANSFERDSTW));
    int y;

    if (!source || !destination || !source_width || !destination_width)
        return;

    for (y = 0; y < height; y++) {
        memcpy(
            destination + ((destination_y + y) * destination_width + destination_x) * bytes,
            source + ((source_y + y) * source_width + source_x) * bytes,
            width * bytes
        );
    }
}

static GeList *find_list(int id)
{
    int index;

    for (index = 0; index < GE_LIST_LIMIT; index++) {
        if (lists[index].used && lists[index].id == id)
            return &lists[index];
    }

    return NULL;
}

static void execute_list(GeList *list)
{
    int commands = 0;

    while (!list->done && commands++ < GE_COMMAND_LIMIT) {
        const u32 *instruction;
        u32 word;
        u32 command;
        u32 argument;

        if (list->stall && list->pc == list->stall)
            break;

        if (!list->pc || (list->pc & 3)) {
            list->done = 1;
            break;
        }

        instruction = ge_pointer(list->pc);

        if (!instruction) {
            list->done = 1;
            break;
        }

        word = *instruction;
        command = word >> 24;
        argument = word & 0x00ffffff;
        list->pc += 4;
        state.command[command] = argument;
        if (command >= GE_WORLDMATRIXNUMBER && command <= GE_PROJMATRIXDATA)
            ge3d_matrix_command(command, argument);

        if (command == GE_TEXFUNC || command == GE_VERTEXTYPE || command == GE_TEXFORMAT) {

            static int ge_rgb01_logs = 0;

            if (ge_rgb01_logs < 18) {
                ge_rgb01_logs++;

                PSB_LOG(
                    GE,
                    "ge recebeu uma leitada %08x com a potencia de mil homens %06x",
                    (unsigned int)command,
                    (unsigned int)argument
                );
            }

        }


        if (command == GE_VADDR)
            state.vertex_address = relative_address(argument);
        else if (command == GE_IADDR)
            state.index_address = relative_address(argument);
        else if (command == GE_OFFSETADDR)
            state.offset_address = argument << 8;
        else if (command == GE_ORIGIN)
            state.offset_address = list->pc - 4;
        else if (command == GE_PRIM) {
            list->draws++;
            draw_primitive(argument);
        } else if (command == GE_TRANSFERSTART) {
            transfer_pixels();
        }
        else if (command == GE_LOADCLUT)
            load_clut(argument);
        else if (command == GE_JUMP)
            list->pc = relative_address(argument & 0x00fffffc);
        else if (command == GE_CALL && list->stack_size < GE_STACK_LIMIT) {
            list->stack[list->stack_size] = list->pc;
            list->stack_offset[list->stack_size] = state.offset_address;
            list->stack_size++;
            list->pc = relative_address(argument & 0x00fffffc);
        } else if (command == GE_RET && list->stack_size > 0) {
            list->stack_size--;
            list->pc = list->stack[list->stack_size];
            state.offset_address = list->stack_offset[list->stack_size];
        } else if (command == GE_SIGNAL) {
            list->signal_id = argument & 0xffff;
            list->pending_signal = 1;
        } else if (command == GE_FINISH) {
            list->finish_id = argument;
            list->pending_finish = 1;
        } else if (command == GE_END) {
            list->done = 1;
            if (list->draws) {
                u32 target = buffer_address(GE_VADDR_BASE, GE_VADDR_WIDTH);
                if (target <= 0x200000u - 0x88000u && (state.command[GE_FRAMEBUF_FORMAT] & 3u) == 3u) {
                    ge_ready_buffer = target;
                    ge_ready_sequence++;
                }
            }
            if (list->pending_signal) {
                u32 signal = list->signal_id;
                list->pending_signal = 0;
                fire_ge_callback(list->callback_id, 0, signal);
            }
            if (list->pending_finish) {
                u32 finish = list->finish_id;
                list->pending_finish = 0;
                fire_ge_callback(list->callback_id, 1, finish);
            }
        }
    }
    if (commands >= GE_COMMAND_LIMIT) {
        list->done = 1;
    }
}

static int enqueue_list(u32 address, u32 stall, int callback_id)
{
    GeList *list = NULL;
    int index;

    address &= 0x3fffffff;
    stall &= 0x3fffffff;

    if (!address || (address & 3) || (stall & 3))
        return -1;

    for (index = 0; index < GE_LIST_LIMIT; index++) {
        if (!lists[index].used || lists[index].done) {
            list = &lists[index];
            break;
        }
    }

    if (!list) {
        return -1;
    }
    memset(list, 0, sizeof(*list));
    list->used = 1;
    list->id = next_list_id++;
    list->pc = address;
    list->stall = stall;
    list->callback_id = callback_id;
    execute_list(list);
    return list->id;
}

u64 psbridge_ge(u32 nid, u32 a0, u32 a1, u32 a2, u32 a3)
{
    GeList *list;
    int index;

    if (nid == 0xe47e40e4)
        return (u32)(uintptr_t)psbridge_video_memory();

    if (nid == 0x1f6752ad) {
        static unsigned vramsinha;
        if (vramsinha++ < 4)
            PRINT_LOG("ge contou a vramsinha %u bytes\n", 0x200000u);
        return 0x200000u;
    }

    if (nid == 0xa4fc06a4)
        return (u32)register_ge_callback(a0);

    if (nid == 0x05db22ce) {
        GeCallback *cb = find_ge_callback((int)a0);
        if (!cb)
            return (u32)-1;
        memset(cb, 0, sizeof(*cb));
        return 0;
    }

    if (nid == 0xab49e76a || nid == 0x1c0d95a6)
        return enqueue_list(a0, a1, (int)a2);

    if (nid == 0xe0d68148) {
        list = find_list((int)a0);

        if (!list)
            return -1;

        list->stall = a1 & 0x3fffffff;
        execute_list(list);
        return 0;
    }

    if (nid == 0x03444eb4) {
        list = find_list((int)a0);

        if (!list)
            return -1;

        execute_list(list);
        return list->done ? 0 : 3;
    }

    if (nid == 0xb287bd61) {
        for (index = 0; index < GE_LIST_LIMIT; index++) {
            if (lists[index].used)
                execute_list(&lists[index]);
        }

        return 0;
    }

    (void)a2;
    (void)a3;
    return 0;
}
