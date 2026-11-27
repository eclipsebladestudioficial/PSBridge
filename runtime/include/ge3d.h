#ifndef PSBRIDGE_GE3D_H
#define PSBRIDGE_GE3D_H

#include <tamtypes.h>

enum {
    GE_CLIP_NEAR = 1u << 4,
    GE_CLIP_PLANES = 0x3f,
    GE_CLIP_INVALID = 1u << 6,
    GE_CLIP_OUTSIDE_SCREEN = 1u << 7,
    GE_CLIP_NONPOSITIVE_W = 1u << 8,
    GE_CLIP_DEPTH_CLAMP = 1u << 9,
    GE_CLIP_DEPTH_DISCARD = 1u << 10,
};

typedef struct {
    float x;
    float y;
    float u;
    float v;
    float z;
    float clip_x;
    float clip_y;
    float clip_z;
    float clip_w;
    float inv_w;
    u32 color;
    unsigned clip_flags;
} GeVertex;

void ge3d_command(u32 command, u32 value);
u32 ge3d_transform_revision(void);
float ge3d_morph_weight(int index);
void ge3d_transform_vertex(GeVertex *vertex, u32 type,
    const float *weights, int weight_count);
int ge3d_project_vertex(GeVertex *vertex);

#endif
