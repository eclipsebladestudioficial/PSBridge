#include <stdint.h>

#include "ge3d.h"

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
#define GE_OFFSETX 0x4c
#define GE_OFFSETY 0x4d
#define GE_TEXSIZE0 0xb8

static float ge_world[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
static float ge_view[12] = {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0};
static float ge_projection[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
static u32 ge_world_index;
static u32 ge_view_index;
static u32 ge_projection_index;

static void ge3d_skinning_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_morph_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_patch_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_texture_generation_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_viewport_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_lighting_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_material_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_clipping_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_culling_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_depth_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_stencil_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static void ge3d_fog_command(u32 command, u32 value)
{
    (void)command;
    (void)value;
}

static float ge3d_float(u32 argument)
{
    union {
        u32 bits;
        float value;
    } v;
    v.bits = argument << 8;
    return v.value;
}

void ge3d_matrix_command(u32 command, u32 value)
{
    if (command == GE_WORLDMATRIXNUMBER)
        ge_world_index = value & 15;
    else if (command == GE_VIEWMATRIXNUMBER)
        ge_view_index = value & 15;
    else if (command == GE_PROJMATRIXNUMBER)
        ge_projection_index = value & 15;
    else if (command == GE_WORLDMATRIXDATA) {
        if (ge_world_index < 12)
            ge_world[ge_world_index] = ge3d_float(value);
        ge_world_index = (ge_world_index + 1) & 15;
    } else if (command == GE_VIEWMATRIXDATA) {
        if (ge_view_index < 12)
            ge_view[ge_view_index] = ge3d_float(value);
        ge_view_index = (ge_view_index + 1) & 15;
    } else if (command == GE_PROJMATRIXDATA) {
        if (ge_projection_index < 16)
            ge_projection[ge_projection_index] = ge3d_float(value);
        ge_projection_index = (ge_projection_index + 1) & 15;
    }
}

void ge3d_command(u32 command, u32 value)
{
    if (command == 0x05 || command == 0x06 ||
        (command >= 0x36 && command <= 0x38))
        ge3d_patch_command(command, value);
    else if (command >= 0x2a && command <= 0x2b)
        ge3d_skinning_command(command, value);
    else if (command >= 0x2c && command <= 0x33)
        ge3d_morph_command(command, value);
    else if (command >= 0x3a && command <= 0x3f)
        ge3d_matrix_command(command, value);
    else if (command == 0x40 || command == 0x41 ||
        command == 0xc0 || command == 0xc1)
        ge3d_texture_generation_command(command, value);
    else if (command >= 0x42 && command <= 0x4d)
        ge3d_viewport_command(command, value);
    else if ((command >= 0x17 && command <= 0x1b) ||
        (command >= 0x5f && command <= 0x9a))
        ge3d_lighting_command(command, value);
    else if (command >= 0x53 && command <= 0x5e)
        ge3d_material_command(command, value);
    else if (command == 0x1c || command == 0xd6 || command == 0xd7)
        ge3d_clipping_command(command, value);
    else if (command == 0x1d || command == 0x9b)
        ge3d_culling_command(command, value);
    else if (command == 0x23 || command == 0x9e || command == 0x9f ||
        command == 0xde || command == 0xe7)
        ge3d_depth_command(command, value);
    else if (command == 0x24 || command == 0xdc || command == 0xdd)
        ge3d_stencil_command(command, value);
    else if (command == 0x1f ||
        (command >= 0xcd && command <= 0xcf))
        ge3d_fog_command(command, value);
}

void ge3d_transform_vertex(GeVertex *v, u32 type, const u32 *commands)
{
    float x, y, z, wx, wy, wz, vx, vy, vz, cx, cy, cw;
    float sx, sy, ox, oy;

    if (type & (1u << 23))
        return;

    x = v->x;
    y = v->y;
    z = v->z;

    if (((type >> 7) & 3) == 1) {
        x *= 1.0f / 128.0f;
        y *= 1.0f / 128.0f;
        z *= 1.0f / 128.0f;
    } else if (((type >> 7) & 3) == 2) {
        x *= 1.0f / 32768.0f;
        y *= 1.0f / 32768.0f;
        z *= 1.0f / 32768.0f;
    }

    wx = x * ge_world[0] + y * ge_world[3] + z * ge_world[6] + ge_world[9];
    wy = x * ge_world[1] + y * ge_world[4] + z * ge_world[7] + ge_world[10];
    wz = x * ge_world[2] + y * ge_world[5] + z * ge_world[8] + ge_world[11];

    vx = wx * ge_view[0] + wy * ge_view[3] + wz * ge_view[6] + ge_view[9];
    vy = wx * ge_view[1] + wy * ge_view[4] + wz * ge_view[7] + ge_view[10];
    vz = wx * ge_view[2] + wy * ge_view[5] + wz * ge_view[8] + ge_view[11];

    cx = vx * ge_projection[0] + vy * ge_projection[4] + vz * ge_projection[8] + ge_projection[12];
    cy = vx * ge_projection[1] + vy * ge_projection[5] + vz * ge_projection[9] + ge_projection[13];
    cw = vx * ge_projection[3] + vy * ge_projection[7] + vz * ge_projection[11] + ge_projection[15];

    if (!(cw > 0.00001f)) {
        v->x = -100000.0f;
        v->y = -100000.0f;
        return;
    }

    sx = ge3d_float(commands[GE_VIEWPORTXSCALE]);
    sy = ge3d_float(commands[GE_VIEWPORTYSCALE]);
    ox = ge3d_float(commands[GE_VIEWPORTXCENTER]);
    oy = ge3d_float(commands[GE_VIEWPORTYCENTER]);

    v->x = cx / cw * sx + ox - (float)(commands[GE_OFFSETX] & 0xffff) * (1.0f / 16.0f);
    v->y = cy / cw * sy + oy - (float)(commands[GE_OFFSETY] & 0xffff) * (1.0f / 16.0f);

    if (!(v->x > -10000 && v->x < 10000 && v->y > -10000 && v->y < 10000)) {
        v->x = -100000.0f;
        v->y = -100000.0f;
    }

    if ((type & 3) != 0) {
        float tu = ge3d_float(commands[GE_TEXSCALEU]);
        float tv = ge3d_float(commands[GE_TEXSCALEV]);

        if (!tu)
            tu = 1.0f;
        if (!tv)
            tv = 1.0f;

        if ((type & 3) == 1) {
            v->u *= 1.0f / 128.0f;
            v->v *= 1.0f / 128.0f;
        } else if ((type & 3) == 2) {
            v->u *= 1.0f / 32768.0f;
            v->v *= 1.0f / 32768.0f;
        }

        v->u = v->u * tu + ge3d_float(commands[GE_TEXOFFSETU]);
        v->v = v->v * tv + ge3d_float(commands[GE_TEXOFFSETV]);
        v->u *= (float)(1 << (commands[GE_TEXSIZE0] & 0xf));
        v->v *= (float)(1 << ((commands[GE_TEXSIZE0] >> 8) & 0xf));
    }
}
