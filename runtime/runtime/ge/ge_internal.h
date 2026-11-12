#ifndef PSBRIDGE_GE_INTERNAL_H
#define PSBRIDGE_GE_INTERNAL_H

#include <stdint.h>

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
#define GE_DEPTH_TEST_ENABLE 0x23
#define GE_VADDR_BASE 0x9c
#define GE_VADDR_WIDTH 0x9d
#define GE_DEPTHBUF_BASE 0x9e
#define GE_DEPTHBUF_WIDTH 0x9f
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
#define GE_DEPTHTEST 0xde
#define GE_BLENDMODE 0xdf
#define GE_BLENDFIXEDA 0xe0
#define GE_BLENDFIXEDB 0xe1
#define GE_DEPTH_WRITE_DISABLE 0xe7
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

typedef struct {
    const u8 *block;
    u32 colors[4];
    u32 indices;
} GeDxtBlockCache;

typedef struct {
    const u8 *texture;
    const u8 *block;
    u32 colors[4];
    u32 indices;
    int width;
    int height;
    int blocks_w;
    int bytes;
    int format;
    int clamp_x;
    int clamp_y;
    GeDxtBlockCache cache[32];
    u32 *ready_pixels;
    int ready_complete;
    int ready_opaque;
} GeDxtSampler;

int ge_dxt_setup(GeDxtSampler *sampler);
void ge_dxt_new_list(void);
void ge_dxt_dirty(void);
static inline u32 ge_dxt_unpack_inline(u16 color)
{
    u32 red = ((color >> 11) & 0x1f) * 255 / 31;
    u32 green = ((color >> 5) & 0x3f) * 255 / 63;
    u32 blue = (color & 0x1f) * 255 / 31;
    return red | (green << 8) | (blue << 16) | 0xff000000u;
}

static inline __attribute__((always_inline)) u32 ge_dxt_pixel(GeDxtSampler *sampler, int x, int y)
{
    const u8 *block;
    u32 color;
    int index;
    int pos;
    int block_index;
    if (sampler->clamp_x) {
        if (x < 0) x = 0;
        if (x >= sampler->width) x = sampler->width - 1;
    } else {
        x = (u32)x & (sampler->width - 1);
    }
    if (sampler->clamp_y) {
        if (y < 0) y = 0;
        if (y >= sampler->height) y = sampler->height - 1;
    } else {
        y = (u32)y & (sampler->height - 1);
    }
    block_index = (y >> 2) * sampler->blocks_w + (x >> 2);
    block = sampler->texture + block_index * sampler->bytes;
    if (block != sampler->block) {
        GeDxtBlockCache *cached = &sampler->cache[block_index & 31];
        if (cached->block == block) {
            sampler->colors[0] = cached->colors[0];
            sampler->colors[1] = cached->colors[1];
            sampler->colors[2] = cached->colors[2];
            sampler->colors[3] = cached->colors[3];
            sampler->indices = cached->indices;
            sampler->block = block;
        } else {
        u16 color0 = block[4] | ((u16)block[5] << 8);
        u16 color1 = block[6] | ((u16)block[7] << 8);
        u32 c0 = ge_dxt_unpack_inline(color0);
        u32 c1 = ge_dxt_unpack_inline(color1);
        sampler->colors[0] = c0;
        sampler->colors[1] = c1;
        sampler->colors[2] =
            (((c0 & 0xff) * 2 + (c1 & 0xff)) / 3) |
            (((((c0 >> 8) & 0xff) * 2 + ((c1 >> 8) & 0xff)) / 3) << 8) |
            (((((c0 >> 16) & 0xff) * 2 + ((c1 >> 16) & 0xff)) / 3) << 16) |
            0xff000000u;
        sampler->colors[3] =
            (((c0 & 0xff) + (c1 & 0xff) * 2) / 3) |
            (((((c0 >> 8) & 0xff) + ((c1 >> 8) & 0xff) * 2) / 3) << 8) |
            (((((c0 >> 16) & 0xff) + ((c1 >> 16) & 0xff) * 2) / 3) << 16) |
            0xff000000u;
        if (sampler->format == 8 && color0 <= color1) {
            sampler->colors[2] =
                (((c0 & 0xff) + (c1 & 0xff)) >> 1) |
                (((((c0 >> 8) & 0xff) + ((c1 >> 8) & 0xff)) >> 1) << 8) |
                (((((c0 >> 16) & 0xff) + ((c1 >> 16) & 0xff)) >> 1) << 16) |
                0xff000000u;
            sampler->colors[3] &= 0x00ffffff;
        }
        sampler->indices = (u32)block[0] | ((u32)block[1] << 8) |
            ((u32)block[2] << 16) | ((u32)block[3] << 24);
        sampler->block = block;
        cached->block = block;
        cached->colors[0] = sampler->colors[0];
        cached->colors[1] = sampler->colors[1];
        cached->colors[2] = sampler->colors[2];
        cached->colors[3] = sampler->colors[3];
        cached->indices = sampler->indices;
        }
    }
    pos = ((y & 3) << 2) | (x & 3);
    index = (sampler->indices >> (pos * 2)) & 3;
    color = sampler->colors[index];
    if (sampler->format == 9) {
        u32 alpha = ((block[8 + (pos >> 1)] >> ((pos & 1) * 4)) & 15) * 17;
        color = (color & 0x00ffffff) | (alpha << 24);
    }
    return color;
}


static inline __attribute__((always_inline)) u32 ge_dxt_ready_pixel(const GeDxtSampler *sampler, int x, int y)
{
    if (sampler->clamp_x) {
        if (x < 0) x = 0;
        if (x >= sampler->width) x = sampler->width - 1;
    } else x = (u32)x & (sampler->width - 1);
    if (sampler->clamp_y) {
        if (y < 0) y = 0;
        if (y >= sampler->height) y = sampler->height - 1;
    } else y = (u32)y & (sampler->height - 1);
    return sampler->ready_pixels[y * sampler->width + x];
}

static inline __attribute__((always_inline)) u32 ge_dxt_cached_pixel(GeDxtSampler *sampler, int x, int y)
{
    if (!sampler->ready_pixels)
        return ge_dxt_pixel(sampler, x, y);
    return ge_dxt_ready_pixel(sampler, x, y);
}


typedef struct {
    int used;
    int id;
    u32 signal_function;
    u32 signal_arg;
    u32 finish_function;
    u32 finish_arg;
} GeCallback;


extern GeList ge_lists[GE_LIST_LIMIT];
extern GeState ge_state;
extern int ge_blend_mode_seen;
extern volatile u32 ge_ready_buffer;
extern volatile u32 ge_ready_sequence;

GeCallback *find_ge_callback(int id);
int register_ge_callback(u32 address);
void *ge_pointer(u32 address);
u32 relative_address(u32 value);
u32 buffer_address(int address_command, int width_command);

u32 unpack_color(u16 color, int format);
u16 pack_color(u32 color, int format);
u32 read_frame_pixel(const void *buffer, int index, int format);
void write_frame_pixel(void *buffer, int index, int format, u32 color);
int compare_alpha(int function, int value, int reference);
u32 blend_color(u32 source, u32 destination);
void load_clut(u32 argument);
u32 palette_color(u32 index);
u32 texture_pixel(int x, int y);
u32 shade_pixel(u32 texture, u32 vertex);
int prepare_triangle_palette(TrianglePalette *fast);

static inline int pixel_offset(int x, int y, int row_bytes, int bytes, int swizzled)
{
    int byte_x = x * bytes;

    if (!swizzled)
        return y * row_bytes + byte_x;

    return
        ((y >> 3) * ((row_bytes + 15) >> 4) + (byte_x >> 4)) * 128 +
        (y & 7) * 16 +
        (byte_x & 15);
}

VertexLayout vertex_layout(u32 type);
GeVertex read_vertex(int number, u32 type, VertexLayout layout);
void draw_primitive(u32 argument);

GeList *find_list(int id);
void execute_list(GeList *list);
int enqueue_list(u32 address, u32 stall, int callback_id);

#endif
