#include <stdint.h>
#include <math.h>

#include "ge3d.h"

#define GE_WORLDMATRIXNUMBER 0x3a
#define GE_WORLDMATRIXDATA 0x3b
#define GE_VIEWMATRIXNUMBER 0x3c
#define GE_VIEWMATRIXDATA 0x3d
#define GE_PROJMATRIXNUMBER 0x3e
#define GE_PROJMATRIXDATA 0x3f
#define GE_VIEWPORTXSCALE 0x42
#define GE_VIEWPORTYSCALE 0x43
#define GE_VIEWPORTZSCALE 0x44
#define GE_VIEWPORTXCENTER 0x45
#define GE_VIEWPORTYCENTER 0x46
#define GE_VIEWPORTZCENTER 0x47
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
static float ge_clip_matrix[16];
static int ge_clip_matrix_dirty = 1;
static u32 ge_transform_revision;
static u32 ge_world_index;
static u32 ge_view_index;
static u32 ge_projection_index;
static float ge_viewport[6];
static float ge_offset[2];
static float ge_texture_transform[4] = {1.0f, 1.0f, 0.0f, 0.0f};
static float ge_texture_size[2] = {1.0f, 1.0f};
static float ge_bones[8][12] = {
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0},
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0},
    {1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0}
};
static float ge_morph[8] = {1, 0, 0, 0, 0, 0, 0, 0};
static u32 ge_bone_index;
float ge3d_morph_weight(int index)
{
    return index >= 0 && index < 8 ? ge_morph[index] : 0.0f;
}

static void ge3d_skinning_command(u32 command, u32 value)
{
    if (command == 0x2a) {
        ge_bone_index = value & 127u;
    } else {
        u32 index = ge_bone_index;
        if (index < 96u) {
            union { u32 bits; float f; } converted;
            converted.bits = value << 8;
            ge_bones[index / 12u][index % 12u] = converted.f;
        }

        ge_bone_index = (index + 1u) & 127u;
    }
}

static void ge3d_morph_command(u32 command, u32 value)
{
    union { u32 bits; float f; } converted;
    u32 index = command - 0x2cu;
    converted.bits = value << 8;
    ge_morph[index] = converted.f;
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

static int ge3d_set_float(float *element, float value)
{
    if (__builtin_memcmp(element, &value, sizeof(value)) == 0)
        return 0;

    *element = value;
    ++ge_transform_revision;
    return 1;
}

static void ge3d_set_matrix_element(float *element, u32 value)
{
    ge_clip_matrix_dirty |= ge3d_set_float(element, ge3d_float(value));
}

u32 ge3d_transform_revision(void)
{
    return ge_transform_revision;
}

static void ge3d_matrix_command(u32 command, u32 value)
{
    if (command == GE_WORLDMATRIXNUMBER)
        ge_world_index = value & 15;
    else if (command == GE_VIEWMATRIXNUMBER)
        ge_view_index = value & 15;
    else if (command == GE_PROJMATRIXNUMBER)
        ge_projection_index = value & 15;
    else if (command == GE_WORLDMATRIXDATA) {
        if (ge_world_index < 12)
            ge3d_set_matrix_element(&ge_world[ge_world_index], value);
        ge_world_index = (ge_world_index + 1) & 15;
    } else if (command == GE_VIEWMATRIXDATA) {
        if (ge_view_index < 12)
            ge3d_set_matrix_element(&ge_view[ge_view_index], value);
        ge_view_index = (ge_view_index + 1) & 15;
    } else if (command == GE_PROJMATRIXDATA) {
        ge3d_set_matrix_element(&ge_projection[ge_projection_index], value);
        ge_projection_index = (ge_projection_index + 1) & 15;
    }
}

static void ge3d_update_clip_matrix(void)
{
    for (int column = 0; column < 4; column++) {
        const float *world = &ge_world[column * 3];
        int position = column == 3;
        float x = world[0] * ge_view[0] + world[1] * ge_view[3] + world[2] * ge_view[6];
        float y = world[0] * ge_view[1] + world[1] * ge_view[4] + world[2] * ge_view[7];
        float z = world[0] * ge_view[2] + world[1] * ge_view[5] + world[2] * ge_view[8];
        if (position) {
            x += ge_view[9];
            y += ge_view[10];
            z += ge_view[11];
        }

        for (int row = 0; row < 4; row++) {
            float projected = x * ge_projection[row] + y * ge_projection[4 + row] +
                z * ge_projection[8 + row];
            ge_clip_matrix[column * 4 + row] = position ? projected + ge_projection[12 + row] : projected;
        }
    }

    ge_clip_matrix_dirty = 0;
}

void ge3d_command(u32 command, u32 value)
{
    if (command >= 0x2a && command <= 0x2b)
        ge3d_skinning_command(command, value);
    else if (command >= 0x2c && command <= 0x33)
        ge3d_morph_command(command, value);
    else if (command >= GE_WORLDMATRIXNUMBER && command <= GE_PROJMATRIXDATA)
        ge3d_matrix_command(command, value);
    else if (command >= GE_VIEWPORTXSCALE && command <= GE_VIEWPORTZCENTER)
        ge3d_set_float(&ge_viewport[command - GE_VIEWPORTXSCALE], ge3d_float(value));
    else if (command >= GE_TEXSCALEU && command <= GE_TEXOFFSETV) {
        float coordinate = ge3d_float(value);
        if (command <= GE_TEXSCALEV && coordinate == 0.0f)
            coordinate = 1.0f;
        ge3d_set_float(&ge_texture_transform[command - GE_TEXSCALEU], coordinate);
    } else if (command == GE_OFFSETX || command == GE_OFFSETY) {
        ge3d_set_float(&ge_offset[command - GE_OFFSETX], (float)(value & 0xffff) * (1.0f / 16.0f));
    } else if (command == GE_TEXSIZE0) {
        ge3d_set_float(&ge_texture_size[0], (float)(1 << (value & 0xf)));
        ge3d_set_float(&ge_texture_size[1], (float)(1 << ((value >> 8) & 0xf)));
    }
}

int ge3d_project_vertex(GeVertex *v)
{
    float x = v->clip_x;
    float y = v->clip_y;
    float z = v->clip_z;
    float w = v->clip_w;

    union { float value; u32 bits; } xb = { x }, yb = { y }, zb = { z }, wb = { w };
    v->clip_flags = 0;
    if (wb.bits < 0x33d6bf95u || wb.bits >= 0x7f800000u ||
        (xb.bits & 0x7fffffffu) > wb.bits ||
        (yb.bits & 0x7fffffffu) > wb.bits ||
        (zb.bits & 0x7fffffffu) > wb.bits) {
        if (!isfinite(x) || !isfinite(y) || !isfinite(z) || !isfinite(w)) {
            v->clip_flags = GE_CLIP_INVALID | GE_CLIP_OUTSIDE_SCREEN;
            return 0;
        }

        v->clip_flags = (unsigned)(x + w < 0.0f) |
            ((unsigned)(w - x < 0.0f) << 1) |
            ((unsigned)(y + w < 0.0f) << 2) |
            ((unsigned)(w - y < 0.0f) << 3) |
            ((unsigned)(z + w < 0.0f) << 4) |
            ((unsigned)(w - z < 0.0f) << 5);
        if (w <= 0.0f)
            v->clip_flags |= GE_CLIP_NONPOSITIVE_W;
        if (fabsf(w) < 0.0000001f) {
            v->clip_flags |= GE_CLIP_OUTSIDE_SCREEN;
            return 0;
        }
    }

    v->inv_w = 1.0f / w;
    v->x = x * v->inv_w * ge_viewport[0] + ge_viewport[3] - ge_offset[0];
    v->y = y * v->inv_w * ge_viewport[1] + ge_viewport[4] - ge_offset[1];
    v->z = z * v->inv_w * ge_viewport[2] + ge_viewport[5];
    if (v->z < 0.0f)
        v->clip_flags |= GE_CLIP_DEPTH_CLAMP | GE_CLIP_DEPTH_DISCARD;
    else if (v->z > 65535.0f) {
        v->clip_flags |= GE_CLIP_DEPTH_CLAMP;
        if (v->z >= 65536.0f)
            v->clip_flags |= GE_CLIP_DEPTH_DISCARD;
    }

    float screen_x = v->x + ge_offset[0];
    float screen_y = v->y + ge_offset[1];
    if (screen_x >= 0.0f && screen_x < 4096.0f && screen_y >= 0.0f && screen_y < 4096.0f)
        return 1;

    v->clip_flags |= GE_CLIP_OUTSIDE_SCREEN;

    return v->x > -10000.0f && v->x < 10000.0f &&
        v->y > -10000.0f && v->y < 10000.0f;
}

void ge3d_transform_vertex(GeVertex *v, u32 type,
    const float *weights, int weight_count)
{
    float x, y, z;

    if (type & (1u << 23)) {
        v->clip_x = v->x;
        v->clip_y = v->y;
        v->clip_z = v->z;
        v->clip_w = 1.0f;
        v->inv_w = 1.0f;
        v->clip_flags = 0;
        return;
    }

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

    if (weight_count > 0 && weights) {
        float sx = 0.0f;
        float sy = 0.0f;
        float sz = 0.0f;
        int i;

        for (i = 0; i < weight_count && i < 8; i++) {
            const float *bone = ge_bones[i];
            float weight = weights[i];
            if (weight == 0.0f)
                continue;

            sx += weight * (x * bone[0] + y * bone[3] + z * bone[6] + bone[9]);
            sy += weight * (x * bone[1] + y * bone[4] + z * bone[7] + bone[10]);
            sz += weight * (x * bone[2] + y * bone[5] + z * bone[8] + bone[11]);
        }

        x = sx;
        y = sy;
        z = sz;
    }

    if (ge_clip_matrix_dirty)
        ge3d_update_clip_matrix();

    v->clip_x = x * ge_clip_matrix[0] + y * ge_clip_matrix[4] + z * ge_clip_matrix[8] + ge_clip_matrix[12];
    v->clip_y = x * ge_clip_matrix[1] + y * ge_clip_matrix[5] + z * ge_clip_matrix[9] + ge_clip_matrix[13];
    v->clip_z = x * ge_clip_matrix[2] + y * ge_clip_matrix[6] + z * ge_clip_matrix[10] + ge_clip_matrix[14];
    v->clip_w = x * ge_clip_matrix[3] + y * ge_clip_matrix[7] + z * ge_clip_matrix[11] + ge_clip_matrix[15];

    if ((type & 3) != 0) {
        if ((type & 3) == 1) {
            v->u *= 1.0f / 128.0f;
            v->v *= 1.0f / 128.0f;
        } else if ((type & 3) == 2) {
            v->u *= 1.0f / 32768.0f;
            v->v *= 1.0f / 32768.0f;
        }

        v->u = (v->u * ge_texture_transform[0] + ge_texture_transform[2]) * ge_texture_size[0];
        v->v = (v->v * ge_texture_transform[1] + ge_texture_transform[3]) * ge_texture_size[1];
    }

    if (!ge3d_project_vertex(v)) {
        v->x = -100000.0f;
        v->y = -100000.0f;
    }
}
