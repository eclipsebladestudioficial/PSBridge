#ifndef PSBRIDGE_GE3D_H
#define PSBRIDGE_GE3D_H

#include <tamtypes.h>

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
} GeVertex;

void ge3d_matrix_command(u32 command, u32 value);
void ge3d_command(u32 command, u32 value);
void ge3d_transform_vertex(GeVertex *vertex, u32 type, const u32 *commands);
int ge3d_project_vertex(GeVertex *vertex, const u32 *commands);
void ge3d_log_matrices(void);

#endif
