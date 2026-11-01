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
